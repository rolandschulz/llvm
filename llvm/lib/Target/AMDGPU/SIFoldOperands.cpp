//===-- SIFoldOperands.cpp - Fold operands --- ----------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
/// \file
//===----------------------------------------------------------------------===//
//

#include "AMDGPU.h"
#include "AMDGPUSubtarget.h"
#include "SIInstrInfo.h"
#include "SIMachineFunctionInfo.h"
#include "MCTargetDesc/AMDGPUMCTargetDesc.h"
#include "llvm/ADT/DepthFirstIterator.h"
#include "llvm/CodeGen/LiveIntervals.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Target/TargetMachine.h"

#define DEBUG_TYPE "si-fold-operands"
using namespace llvm;

namespace {

struct FoldCandidate {
  MachineInstr *UseMI;
  union {
    MachineOperand *OpToFold;
    uint64_t ImmToFold;
    int FrameIndexToFold;
  };
  int ShrinkOpcode;
  unsigned char UseOpNo;
  MachineOperand::MachineOperandType Kind;
  bool Commuted;

  FoldCandidate(MachineInstr *MI, unsigned OpNo, MachineOperand *FoldOp,
                bool Commuted_ = false,
                int ShrinkOp = -1) :
    UseMI(MI), OpToFold(nullptr), ShrinkOpcode(ShrinkOp), UseOpNo(OpNo),
    Kind(FoldOp->getType()),
    Commuted(Commuted_) {
    if (FoldOp->isImm()) {
      ImmToFold = FoldOp->getImm();
    } else if (FoldOp->isFI()) {
      FrameIndexToFold = FoldOp->getIndex();
    } else {
      assert(FoldOp->isReg() || FoldOp->isGlobal());
      OpToFold = FoldOp;
    }
  }

  bool isFI() const {
    return Kind == MachineOperand::MO_FrameIndex;
  }

  bool isImm() const {
    return Kind == MachineOperand::MO_Immediate;
  }

  bool isReg() const {
    return Kind == MachineOperand::MO_Register;
  }

  bool isGlobal() const { return Kind == MachineOperand::MO_GlobalAddress; }

  bool isCommuted() const {
    return Commuted;
  }

  bool needsShrink() const {
    return ShrinkOpcode != -1;
  }

  int getShrinkOpcode() const {
    return ShrinkOpcode;
  }
};

class SIFoldOperands : public MachineFunctionPass {
public:
  static char ID;
  MachineRegisterInfo *MRI;
  const SIInstrInfo *TII;
  const SIRegisterInfo *TRI;
  const GCNSubtarget *ST;
  const SIMachineFunctionInfo *MFI;

  void foldOperand(MachineOperand &OpToFold,
                   MachineInstr *UseMI,
                   int UseOpIdx,
                   SmallVectorImpl<FoldCandidate> &FoldList,
                   SmallVectorImpl<MachineInstr *> &CopiesToReplace) const;

  void foldInstOperand(MachineInstr &MI, MachineOperand &OpToFold) const;

  const MachineOperand *isClamp(const MachineInstr &MI) const;
  bool tryFoldClamp(MachineInstr &MI);

  std::pair<const MachineOperand *, int> isOMod(const MachineInstr &MI) const;
  bool tryFoldOMod(MachineInstr &MI);

public:
  SIFoldOperands() : MachineFunctionPass(ID) {
    initializeSIFoldOperandsPass(*PassRegistry::getPassRegistry());
  }

  bool runOnMachineFunction(MachineFunction &MF) override;

  StringRef getPassName() const override { return "SI Fold Operands"; }

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.setPreservesCFG();
    MachineFunctionPass::getAnalysisUsage(AU);
  }
};

} // End anonymous namespace.

INITIALIZE_PASS(SIFoldOperands, DEBUG_TYPE,
                "SI Fold Operands", false, false)

char SIFoldOperands::ID = 0;

char &llvm::SIFoldOperandsID = SIFoldOperands::ID;

// Wrapper around isInlineConstant that understands special cases when
// instruction types are replaced during operand folding.
static bool isInlineConstantIfFolded(const SIInstrInfo *TII,
                                     const MachineInstr &UseMI,
                                     unsigned OpNo,
                                     const MachineOperand &OpToFold) {
  if (TII->isInlineConstant(UseMI, OpNo, OpToFold))
    return true;

  unsigned Opc = UseMI.getOpcode();
  switch (Opc) {
  case AMDGPU::V_MAC_F32_e64:
  case AMDGPU::V_MAC_F16_e64:
  case AMDGPU::V_FMAC_F32_e64:
  case AMDGPU::V_FMAC_F16_e64: {
    // Special case for mac. Since this is replaced with mad when folded into
    // src2, we need to check the legality for the final instruction.
    int Src2Idx = AMDGPU::getNamedOperandIdx(Opc, AMDGPU::OpName::src2);
    if (static_cast<int>(OpNo) == Src2Idx) {
      bool IsFMA = Opc == AMDGPU::V_FMAC_F32_e64 ||
                   Opc == AMDGPU::V_FMAC_F16_e64;
      bool IsF32 = Opc == AMDGPU::V_MAC_F32_e64 ||
                   Opc == AMDGPU::V_FMAC_F32_e64;

      unsigned Opc = IsFMA ?
        (IsF32 ? AMDGPU::V_FMA_F32 : AMDGPU::V_FMA_F16_gfx9) :
        (IsF32 ? AMDGPU::V_MAD_F32 : AMDGPU::V_MAD_F16);
      const MCInstrDesc &MadDesc = TII->get(Opc);
      return TII->isInlineConstant(OpToFold, MadDesc.OpInfo[OpNo].OperandType);
    }
    return false;
  }
  default:
    return false;
  }
}

// TODO: Add heuristic that the frame index might not fit in the addressing mode
// immediate offset to avoid materializing in loops.
static bool frameIndexMayFold(const SIInstrInfo *TII,
                              const MachineInstr &UseMI,
                              int OpNo,
                              const MachineOperand &OpToFold) {
  return OpToFold.isFI() &&
    (TII->isMUBUF(UseMI) || TII->isFLATScratch(UseMI)) &&
    OpNo == AMDGPU::getNamedOperandIdx(UseMI.getOpcode(), AMDGPU::OpName::vaddr);
}

FunctionPass *llvm::createSIFoldOperandsPass() {
  return new SIFoldOperands();
}

static bool updateOperand(FoldCandidate &Fold,
                          const SIInstrInfo &TII,
                          const TargetRegisterInfo &TRI,
                          const GCNSubtarget &ST) {
  MachineInstr *MI = Fold.UseMI;
  MachineOperand &Old = MI->getOperand(Fold.UseOpNo);
  assert(Old.isReg());

  if (Fold.isImm()) {
    if (MI->getDesc().TSFlags & SIInstrFlags::IsPacked &&
        !(MI->getDesc().TSFlags & SIInstrFlags::IsMAI) &&
        AMDGPU::isInlinableLiteralV216(static_cast<uint16_t>(Fold.ImmToFold),
                                       ST.hasInv2PiInlineImm())) {
      // Set op_sel/op_sel_hi on this operand or bail out if op_sel is
      // already set.
      unsigned Opcode = MI->getOpcode();
      int OpNo = MI->getOperandNo(&Old);
      int ModIdx = -1;
      if (OpNo == AMDGPU::getNamedOperandIdx(Opcode, AMDGPU::OpName::src0))
        ModIdx = AMDGPU::OpName::src0_modifiers;
      else if (OpNo == AMDGPU::getNamedOperandIdx(Opcode, AMDGPU::OpName::src1))
        ModIdx = AMDGPU::OpName::src1_modifiers;
      else if (OpNo == AMDGPU::getNamedOperandIdx(Opcode, AMDGPU::OpName::src2))
        ModIdx = AMDGPU::OpName::src2_modifiers;
      assert(ModIdx != -1);
      ModIdx = AMDGPU::getNamedOperandIdx(Opcode, ModIdx);
      MachineOperand &Mod = MI->getOperand(ModIdx);
      unsigned Val = Mod.getImm();
      if ((Val & SISrcMods::OP_SEL_0) || !(Val & SISrcMods::OP_SEL_1))
        return false;
      // Only apply the following transformation if that operand requries
      // a packed immediate.
      switch (TII.get(Opcode).OpInfo[OpNo].OperandType) {
      case AMDGPU::OPERAND_REG_IMM_V2FP16:
      case AMDGPU::OPERAND_REG_IMM_V2INT16:
      case AMDGPU::OPERAND_REG_INLINE_C_V2FP16:
      case AMDGPU::OPERAND_REG_INLINE_C_V2INT16:
        // If upper part is all zero we do not need op_sel_hi.
        if (!isUInt<16>(Fold.ImmToFold)) {
          if (!(Fold.ImmToFold & 0xffff)) {
            Mod.setImm(Mod.getImm() | SISrcMods::OP_SEL_0);
            Mod.setImm(Mod.getImm() & ~SISrcMods::OP_SEL_1);
            Old.ChangeToImmediate((Fold.ImmToFold >> 16) & 0xffff);
            return true;
          }
          Mod.setImm(Mod.getImm() & ~SISrcMods::OP_SEL_1);
          Old.ChangeToImmediate(Fold.ImmToFold & 0xffff);
          return true;
        }
        break;
      default:
        break;
      }
    }
  }

  if ((Fold.isImm() || Fold.isFI() || Fold.isGlobal()) && Fold.needsShrink()) {
    MachineBasicBlock *MBB = MI->getParent();
    auto Liveness = MBB->computeRegisterLiveness(&TRI, AMDGPU::VCC, MI, 16);
    if (Liveness != MachineBasicBlock::LQR_Dead) {
      LLVM_DEBUG(dbgs() << "Not shrinking " << MI << " due to vcc liveness\n");
      return false;
    }

    MachineRegisterInfo &MRI = MBB->getParent()->getRegInfo();
    int Op32 = Fold.getShrinkOpcode();
    MachineOperand &Dst0 = MI->getOperand(0);
    MachineOperand &Dst1 = MI->getOperand(1);
    assert(Dst0.isDef() && Dst1.isDef());

    bool HaveNonDbgCarryUse = !MRI.use_nodbg_empty(Dst1.getReg());

    const TargetRegisterClass *Dst0RC = MRI.getRegClass(Dst0.getReg());
    Register NewReg0 = MRI.createVirtualRegister(Dst0RC);

    MachineInstr *Inst32 = TII.buildShrunkInst(*MI, Op32);

    if (HaveNonDbgCarryUse) {
      BuildMI(*MBB, MI, MI->getDebugLoc(), TII.get(AMDGPU::COPY), Dst1.getReg())
        .addReg(AMDGPU::VCC, RegState::Kill);
    }

    // Keep the old instruction around to avoid breaking iterators, but
    // replace it with a dummy instruction to remove uses.
    //
    // FIXME: We should not invert how this pass looks at operands to avoid
    // this. Should track set of foldable movs instead of looking for uses
    // when looking at a use.
    Dst0.setReg(NewReg0);
    for (unsigned I = MI->getNumOperands() - 1; I > 0; --I)
      MI->RemoveOperand(I);
    MI->setDesc(TII.get(AMDGPU::IMPLICIT_DEF));

    if (Fold.isCommuted())
      TII.commuteInstruction(*Inst32, false);
    return true;
  }

  assert(!Fold.needsShrink() && "not handled");

  if (Fold.isImm()) {
    Old.ChangeToImmediate(Fold.ImmToFold);
    return true;
  }

  if (Fold.isGlobal()) {
    Old.ChangeToGA(Fold.OpToFold->getGlobal(), Fold.OpToFold->getOffset(),
                   Fold.OpToFold->getTargetFlags());
    return true;
  }

  if (Fold.isFI()) {
    Old.ChangeToFrameIndex(Fold.FrameIndexToFold);
    return true;
  }

  MachineOperand *New = Fold.OpToFold;
  Old.substVirtReg(New->getReg(), New->getSubReg(), TRI);
  Old.setIsUndef(New->isUndef());
  return true;
}

static bool isUseMIInFoldList(ArrayRef<FoldCandidate> FoldList,
                              const MachineInstr *MI) {
  for (auto Candidate : FoldList) {
    if (Candidate.UseMI == MI)
      return true;
  }
  return false;
}

static bool tryAddToFoldList(SmallVectorImpl<FoldCandidate> &FoldList,
                             MachineInstr *MI, unsigned OpNo,
                             MachineOperand *OpToFold,
                             const SIInstrInfo *TII) {
  if (!TII->isOperandLegal(*MI, OpNo, OpToFold)) {
    // Special case for v_mac_{f16, f32}_e64 if we are trying to fold into src2
    unsigned Opc = MI->getOpcode();
    if ((Opc == AMDGPU::V_MAC_F32_e64 || Opc == AMDGPU::V_MAC_F16_e64 ||
         Opc == AMDGPU::V_FMAC_F32_e64 || Opc == AMDGPU::V_FMAC_F16_e64) &&
        (int)OpNo == AMDGPU::getNamedOperandIdx(Opc, AMDGPU::OpName::src2)) {
      bool IsFMA = Opc == AMDGPU::V_FMAC_F32_e64 ||
                   Opc == AMDGPU::V_FMAC_F16_e64;
      bool IsF32 = Opc == AMDGPU::V_MAC_F32_e64 ||
                   Opc == AMDGPU::V_FMAC_F32_e64;
      unsigned NewOpc = IsFMA ?
        (IsF32 ? AMDGPU::V_FMA_F32 : AMDGPU::V_FMA_F16_gfx9) :
        (IsF32 ? AMDGPU::V_MAD_F32 : AMDGPU::V_MAD_F16);

      // Check if changing this to a v_mad_{f16, f32} instruction will allow us
      // to fold the operand.
      MI->setDesc(TII->get(NewOpc));
      bool FoldAsMAD = tryAddToFoldList(FoldList, MI, OpNo, OpToFold, TII);
      if (FoldAsMAD) {
        MI->untieRegOperand(OpNo);
        return true;
      }
      MI->setDesc(TII->get(Opc));
    }

    // Special case for s_setreg_b32
    if (Opc == AMDGPU::S_SETREG_B32 && OpToFold->isImm()) {
      MI->setDesc(TII->get(AMDGPU::S_SETREG_IMM32_B32));
      FoldList.push_back(FoldCandidate(MI, OpNo, OpToFold));
      return true;
    }

    // If we are already folding into another operand of MI, then
    // we can't commute the instruction, otherwise we risk making the
    // other fold illegal.
    if (isUseMIInFoldList(FoldList, MI))
      return false;

    unsigned CommuteOpNo = OpNo;

    // Operand is not legal, so try to commute the instruction to
    // see if this makes it possible to fold.
    unsigned CommuteIdx0 = TargetInstrInfo::CommuteAnyOperandIndex;
    unsigned CommuteIdx1 = TargetInstrInfo::CommuteAnyOperandIndex;
    bool CanCommute = TII->findCommutedOpIndices(*MI, CommuteIdx0, CommuteIdx1);

    if (CanCommute) {
      if (CommuteIdx0 == OpNo)
        CommuteOpNo = CommuteIdx1;
      else if (CommuteIdx1 == OpNo)
        CommuteOpNo = CommuteIdx0;
    }


    // One of operands might be an Imm operand, and OpNo may refer to it after
    // the call of commuteInstruction() below. Such situations are avoided
    // here explicitly as OpNo must be a register operand to be a candidate
    // for memory folding.
    if (CanCommute && (!MI->getOperand(CommuteIdx0).isReg() ||
                       !MI->getOperand(CommuteIdx1).isReg()))
      return false;

    if (!CanCommute ||
        !TII->commuteInstruction(*MI, false, CommuteIdx0, CommuteIdx1))
      return false;

    if (!TII->isOperandLegal(*MI, CommuteOpNo, OpToFold)) {
      if ((Opc == AMDGPU::V_ADD_I32_e64 ||
           Opc == AMDGPU::V_SUB_I32_e64 ||
           Opc == AMDGPU::V_SUBREV_I32_e64) && // FIXME
          (OpToFold->isImm() || OpToFold->isFI() || OpToFold->isGlobal())) {
        MachineRegisterInfo &MRI = MI->getParent()->getParent()->getRegInfo();

        // Verify the other operand is a VGPR, otherwise we would violate the
        // constant bus restriction.
        unsigned OtherIdx = CommuteOpNo == CommuteIdx0 ? CommuteIdx1 : CommuteIdx0;
        MachineOperand &OtherOp = MI->getOperand(OtherIdx);
        if (!OtherOp.isReg() ||
            !TII->getRegisterInfo().isVGPR(MRI, OtherOp.getReg()))
          return false;

        assert(MI->getOperand(1).isDef());

        // Make sure to get the 32-bit version of the commuted opcode.
        unsigned MaybeCommutedOpc = MI->getOpcode();
        int Op32 = AMDGPU::getVOPe32(MaybeCommutedOpc);

        FoldList.push_back(FoldCandidate(MI, CommuteOpNo, OpToFold, true,
                                         Op32));
        return true;
      }

      TII->commuteInstruction(*MI, false, CommuteIdx0, CommuteIdx1);
      return false;
    }

    FoldList.push_back(FoldCandidate(MI, CommuteOpNo, OpToFold, true));
    return true;
  }

  FoldList.push_back(FoldCandidate(MI, OpNo, OpToFold));
  return true;
}

// If the use operand doesn't care about the value, this may be an operand only
// used for register indexing, in which case it is unsafe to fold.
static bool isUseSafeToFold(const SIInstrInfo *TII,
                            const MachineInstr &MI,
                            const MachineOperand &UseMO) {
  return !UseMO.isUndef() && !TII->isSDWA(MI);
  //return !MI.hasRegisterImplicitUseOperand(UseMO.getReg());
}

static bool tryToFoldACImm(const SIInstrInfo *TII,
                           const MachineOperand &OpToFold,
                           MachineInstr *UseMI,
                           unsigned UseOpIdx,
                           SmallVectorImpl<FoldCandidate> &FoldList) {
  const MCInstrDesc &Desc = UseMI->getDesc();
  const MCOperandInfo *OpInfo = Desc.OpInfo;
  if (!OpInfo || UseOpIdx >= Desc.getNumOperands())
    return false;

  uint8_t OpTy = OpInfo[UseOpIdx].OperandType;
  if (OpTy < AMDGPU::OPERAND_REG_INLINE_AC_FIRST ||
      OpTy > AMDGPU::OPERAND_REG_INLINE_AC_LAST)
    return false;

  if (OpToFold.isImm() && TII->isInlineConstant(OpToFold, OpTy) &&
      TII->isOperandLegal(*UseMI, UseOpIdx, &OpToFold)) {
    UseMI->getOperand(UseOpIdx).ChangeToImmediate(OpToFold.getImm());
    return true;
  }

  if (!OpToFold.isReg())
    return false;

  Register UseReg = OpToFold.getReg();
  if (!Register::isVirtualRegister(UseReg))
    return false;

  if (llvm::find_if(FoldList, [UseMI](const FoldCandidate &FC) {
        return FC.UseMI == UseMI; }) != FoldList.end())
    return false;

  MachineRegisterInfo &MRI = UseMI->getParent()->getParent()->getRegInfo();
  const MachineInstr *Def = MRI.getUniqueVRegDef(UseReg);
  if (!Def || !Def->isRegSequence())
    return false;

  int64_t Imm;
  MachineOperand *Op;
  for (unsigned I = 1, E = Def->getNumExplicitOperands(); I < E; I += 2) {
    const MachineOperand &Sub = Def->getOperand(I);
    if (!Sub.isReg() || Sub.getSubReg())
      return false;
    MachineInstr *SubDef = MRI.getUniqueVRegDef(Sub.getReg());
    while (SubDef && !SubDef->isMoveImmediate() &&
           !SubDef->getOperand(1).isImm() && TII->isFoldableCopy(*SubDef))
      SubDef = MRI.getUniqueVRegDef(SubDef->getOperand(1).getReg());
    if (!SubDef || !SubDef->isMoveImmediate() || !SubDef->getOperand(1).isImm())
      return false;
    Op = &SubDef->getOperand(1);
    auto SubImm = Op->getImm();
    if (I == 1) {
      if (!TII->isInlineConstant(SubDef->getOperand(1), OpTy))
        return false;

      Imm = SubImm;
      continue;
    }
    if (Imm != SubImm)
      return false; // Can only fold splat constants
  }

  if (!TII->isOperandLegal(*UseMI, UseOpIdx, Op))
    return false;

  FoldList.push_back(FoldCandidate(UseMI, UseOpIdx, Op));
  return true;
}

void SIFoldOperands::foldOperand(
  MachineOperand &OpToFold,
  MachineInstr *UseMI,
  int UseOpIdx,
  SmallVectorImpl<FoldCandidate> &FoldList,
  SmallVectorImpl<MachineInstr *> &CopiesToReplace) const {
  const MachineOperand &UseOp = UseMI->getOperand(UseOpIdx);

  if (!isUseSafeToFold(TII, *UseMI, UseOp))
    return;

  // FIXME: Fold operands with subregs.
  if (UseOp.isReg() && OpToFold.isReg()) {
    if (UseOp.isImplicit() || UseOp.getSubReg() != AMDGPU::NoSubRegister)
      return;
  }

  // Special case for REG_SEQUENCE: We can't fold literals into
  // REG_SEQUENCE instructions, so we have to fold them into the
  // uses of REG_SEQUENCE.
  if (UseMI->isRegSequence()) {
    Register RegSeqDstReg = UseMI->getOperand(0).getReg();
    unsigned RegSeqDstSubReg = UseMI->getOperand(UseOpIdx + 1).getImm();

    MachineRegisterInfo::use_iterator Next;
    for (MachineRegisterInfo::use_iterator
           RSUse = MRI->use_begin(RegSeqDstReg), RSE = MRI->use_end();
         RSUse != RSE; RSUse = Next) {
      Next = std::next(RSUse);

      MachineInstr *RSUseMI = RSUse->getParent();

      if (tryToFoldACImm(TII, UseMI->getOperand(0), RSUseMI,
                         RSUse.getOperandNo(), FoldList))
        continue;

      if (RSUse->getSubReg() != RegSeqDstSubReg)
        continue;

      foldOperand(OpToFold, RSUseMI, RSUse.getOperandNo(), FoldList,
                  CopiesToReplace);
    }

    return;
  }

  if (tryToFoldACImm(TII, OpToFold, UseMI, UseOpIdx, FoldList))
    return;

  if (frameIndexMayFold(TII, *UseMI, UseOpIdx, OpToFold)) {
    // Sanity check that this is a stack access.
    // FIXME: Should probably use stack pseudos before frame lowering.
    MachineOperand *SOff = TII->getNamedOperand(*UseMI, AMDGPU::OpName::soffset);
    if (!SOff->isReg() || (SOff->getReg() != MFI->getScratchWaveOffsetReg() &&
                           SOff->getReg() != MFI->getStackPtrOffsetReg()))
      return;

    if (TII->getNamedOperand(*UseMI, AMDGPU::OpName::srsrc)->getReg() !=
        MFI->getScratchRSrcReg())
      return;

    // A frame index will resolve to a positive constant, so it should always be
    // safe to fold the addressing mode, even pre-GFX9.
    UseMI->getOperand(UseOpIdx).ChangeToFrameIndex(OpToFold.getIndex());
    SOff->setReg(MFI->getStackPtrOffsetReg());
    return;
  }

  bool FoldingImmLike =
      OpToFold.isImm() || OpToFold.isFI() || OpToFold.isGlobal();

  if (FoldingImmLike && UseMI->isCopy()) {
    Register DestReg = UseMI->getOperand(0).getReg();

    // Don't fold into a copy to a physical register. Doing so would interfere
    // with the register coalescer's logic which would avoid redundant
    // initalizations.
    if (DestReg.isPhysical())
      return;

    const TargetRegisterClass *DestRC =  MRI->getRegClass(DestReg);

    Register SrcReg = UseMI->getOperand(1).getReg();
    if (SrcReg.isVirtual()) { // XXX - This can be an assert?
      const TargetRegisterClass * SrcRC = MRI->getRegClass(SrcReg);
      if (TRI->isSGPRClass(SrcRC) && TRI->hasVectorRegisters(DestRC)) {
        MachineRegisterInfo::use_iterator NextUse;
        SmallVector<FoldCandidate, 4> CopyUses;
        for (MachineRegisterInfo::use_iterator
          Use = MRI->use_begin(DestReg), E = MRI->use_end();
          Use != E; Use = NextUse) {
          NextUse = std::next(Use);
          FoldCandidate FC = FoldCandidate(Use->getParent(),
           Use.getOperandNo(), &UseMI->getOperand(1));
          CopyUses.push_back(FC);
       }
        for (auto & F : CopyUses) {
          foldOperand(*F.OpToFold, F.UseMI, F.UseOpNo,
           FoldList, CopiesToReplace);
        }
      }
    }

    if (DestRC == &AMDGPU::AGPR_32RegClass &&
        TII->isInlineConstant(OpToFold, AMDGPU::OPERAND_REG_INLINE_C_INT32)) {
      UseMI->setDesc(TII->get(AMDGPU::V_ACCVGPR_WRITE_B32));
      UseMI->getOperand(1).ChangeToImmediate(OpToFold.getImm());
      CopiesToReplace.push_back(UseMI);
      return;
    }

    // In order to fold immediates into copies, we need to change the
    // copy to a MOV.

    unsigned MovOp = TII->getMovOpcode(DestRC);
    if (MovOp == AMDGPU::COPY)
      return;

    UseMI->setDesc(TII->get(MovOp));
    MachineInstr::mop_iterator ImpOpI = UseMI->implicit_operands().begin();
    MachineInstr::mop_iterator ImpOpE = UseMI->implicit_operands().end();
    while (ImpOpI != ImpOpE) {
      MachineInstr::mop_iterator Tmp = ImpOpI;
      ImpOpI++;
      UseMI->RemoveOperand(UseMI->getOperandNo(Tmp));
    }
    CopiesToReplace.push_back(UseMI);
  } else {
    if (UseMI->isCopy() && OpToFold.isReg() &&
        Register::isVirtualRegister(UseMI->getOperand(0).getReg()) &&
        TRI->isVectorRegister(*MRI, UseMI->getOperand(0).getReg()) &&
        TRI->isVectorRegister(*MRI, UseMI->getOperand(1).getReg()) &&
        !UseMI->getOperand(1).getSubReg()) {
      unsigned Size = TII->getOpSize(*UseMI, 1);
      UseMI->getOperand(1).setReg(OpToFold.getReg());
      UseMI->getOperand(1).setSubReg(OpToFold.getSubReg());
      UseMI->getOperand(1).setIsKill(false);
      CopiesToReplace.push_back(UseMI);
      OpToFold.setIsKill(false);
      if (Size != 4)
        return;
      if (TRI->isAGPR(*MRI, UseMI->getOperand(0).getReg()) &&
          TRI->isVGPR(*MRI, UseMI->getOperand(1).getReg()))
        UseMI->setDesc(TII->get(AMDGPU::V_ACCVGPR_WRITE_B32));
      else if (TRI->isVGPR(*MRI, UseMI->getOperand(0).getReg()) &&
               TRI->isAGPR(*MRI, UseMI->getOperand(1).getReg()))
        UseMI->setDesc(TII->get(AMDGPU::V_ACCVGPR_READ_B32));
      return;
    }

    unsigned UseOpc = UseMI->getOpcode();
    if (UseOpc == AMDGPU::V_READFIRSTLANE_B32 ||
        (UseOpc == AMDGPU::V_READLANE_B32 &&
         (int)UseOpIdx ==
         AMDGPU::getNamedOperandIdx(UseOpc, AMDGPU::OpName::src0))) {
      // %vgpr = V_MOV_B32 imm
      // %sgpr = V_READFIRSTLANE_B32 %vgpr
      // =>
      // %sgpr = S_MOV_B32 imm
      if (FoldingImmLike) {
        if (execMayBeModifiedBeforeUse(*MRI,
                                       UseMI->getOperand(UseOpIdx).getReg(),
                                       *OpToFold.getParent(),
                                       *UseMI))
          return;

        UseMI->setDesc(TII->get(AMDGPU::S_MOV_B32));

        // FIXME: ChangeToImmediate should clear subreg
        UseMI->getOperand(1).setSubReg(0);
        if (OpToFold.isImm())
          UseMI->getOperand(1).ChangeToImmediate(OpToFold.getImm());
        else
          UseMI->getOperand(1).ChangeToFrameIndex(OpToFold.getIndex());
        UseMI->RemoveOperand(2); // Remove exec read (or src1 for readlane)
        return;
      }

      if (OpToFold.isReg() && TRI->isSGPRReg(*MRI, OpToFold.getReg())) {
        if (execMayBeModifiedBeforeUse(*MRI,
                                       UseMI->getOperand(UseOpIdx).getReg(),
                                       *OpToFold.getParent(),
                                       *UseMI))
          return;

        // %vgpr = COPY %sgpr0
        // %sgpr1 = V_READFIRSTLANE_B32 %vgpr
        // =>
        // %sgpr1 = COPY %sgpr0
        UseMI->setDesc(TII->get(AMDGPU::COPY));
        UseMI->getOperand(1).setReg(OpToFold.getReg());
        UseMI->getOperand(1).setSubReg(OpToFold.getSubReg());
        UseMI->getOperand(1).setIsKill(false);
        UseMI->RemoveOperand(2); // Remove exec read (or src1 for readlane)
        return;
      }
    }

    const MCInstrDesc &UseDesc = UseMI->getDesc();

    // Don't fold into target independent nodes.  Target independent opcodes
    // don't have defined register classes.
    if (UseDesc.isVariadic() ||
        UseOp.isImplicit() ||
        UseDesc.OpInfo[UseOpIdx].RegClass == -1)
      return;
  }

  if (!FoldingImmLike) {
    tryAddToFoldList(FoldList, UseMI, UseOpIdx, &OpToFold, TII);

    // FIXME: We could try to change the instruction from 64-bit to 32-bit
    // to enable more folding opportunites.  The shrink operands pass
    // already does this.
    return;
  }


  const MCInstrDesc &FoldDesc = OpToFold.getParent()->getDesc();
  const TargetRegisterClass *FoldRC =
    TRI->getRegClass(FoldDesc.OpInfo[0].RegClass);

  // Split 64-bit constants into 32-bits for folding.
  if (UseOp.getSubReg() && AMDGPU::getRegBitWidth(FoldRC->getID()) == 64) {
    Register UseReg = UseOp.getReg();
    const TargetRegisterClass *UseRC = MRI->getRegClass(UseReg);

    if (AMDGPU::getRegBitWidth(UseRC->getID()) != 64)
      return;

    APInt Imm(64, OpToFold.getImm());
    if (UseOp.getSubReg() == AMDGPU::sub0) {
      Imm = Imm.getLoBits(32);
    } else {
      assert(UseOp.getSubReg() == AMDGPU::sub1);
      Imm = Imm.getHiBits(32);
    }

    MachineOperand ImmOp = MachineOperand::CreateImm(Imm.getSExtValue());
    tryAddToFoldList(FoldList, UseMI, UseOpIdx, &ImmOp, TII);
    return;
  }



  tryAddToFoldList(FoldList, UseMI, UseOpIdx, &OpToFold, TII);
}

static bool evalBinaryInstruction(unsigned Opcode, int32_t &Result,
                                  uint32_t LHS, uint32_t RHS) {
  switch (Opcode) {
  case AMDGPU::V_AND_B32_e64:
  case AMDGPU::V_AND_B32_e32:
  case AMDGPU::S_AND_B32:
    Result = LHS & RHS;
    return true;
  case AMDGPU::V_OR_B32_e64:
  case AMDGPU::V_OR_B32_e32:
  case AMDGPU::S_OR_B32:
    Result = LHS | RHS;
    return true;
  case AMDGPU::V_XOR_B32_e64:
  case AMDGPU::V_XOR_B32_e32:
  case AMDGPU::S_XOR_B32:
    Result = LHS ^ RHS;
    return true;
  case AMDGPU::V_LSHL_B32_e64:
  case AMDGPU::V_LSHL_B32_e32:
  case AMDGPU::S_LSHL_B32:
    // The instruction ignores the high bits for out of bounds shifts.
    Result = LHS << (RHS & 31);
    return true;
  case AMDGPU::V_LSHLREV_B32_e64:
  case AMDGPU::V_LSHLREV_B32_e32:
    Result = RHS << (LHS & 31);
    return true;
  case AMDGPU::V_LSHR_B32_e64:
  case AMDGPU::V_LSHR_B32_e32:
  case AMDGPU::S_LSHR_B32:
    Result = LHS >> (RHS & 31);
    return true;
  case AMDGPU::V_LSHRREV_B32_e64:
  case AMDGPU::V_LSHRREV_B32_e32:
    Result = RHS >> (LHS & 31);
    return true;
  case AMDGPU::V_ASHR_I32_e64:
  case AMDGPU::V_ASHR_I32_e32:
  case AMDGPU::S_ASHR_I32:
    Result = static_cast<int32_t>(LHS) >> (RHS & 31);
    return true;
  case AMDGPU::V_ASHRREV_I32_e64:
  case AMDGPU::V_ASHRREV_I32_e32:
    Result = static_cast<int32_t>(RHS) >> (LHS & 31);
    return true;
  default:
    return false;
  }
}

static unsigned getMovOpc(bool IsScalar) {
  return IsScalar ? AMDGPU::S_MOV_B32 : AMDGPU::V_MOV_B32_e32;
}

/// Remove any leftover implicit operands from mutating the instruction. e.g.
/// if we replace an s_and_b32 with a copy, we don't need the implicit scc def
/// anymore.
static void stripExtraCopyOperands(MachineInstr &MI) {
  const MCInstrDesc &Desc = MI.getDesc();
  unsigned NumOps = Desc.getNumOperands() +
                    Desc.getNumImplicitUses() +
                    Desc.getNumImplicitDefs();

  for (unsigned I = MI.getNumOperands() - 1; I >= NumOps; --I)
    MI.RemoveOperand(I);
}

static void mutateCopyOp(MachineInstr &MI, const MCInstrDesc &NewDesc) {
  MI.setDesc(NewDesc);
  stripExtraCopyOperands(MI);
}

static MachineOperand *getImmOrMaterializedImm(MachineRegisterInfo &MRI,
                                               MachineOperand &Op) {
  if (Op.isReg()) {
    // If this has a subregister, it obviously is a register source.
    if (Op.getSubReg() != AMDGPU::NoSubRegister ||
        !Register::isVirtualRegister(Op.getReg()))
      return &Op;

    MachineInstr *Def = MRI.getVRegDef(Op.getReg());
    if (Def && Def->isMoveImmediate()) {
      MachineOperand &ImmSrc = Def->getOperand(1);
      if (ImmSrc.isImm())
        return &ImmSrc;
    }
  }

  return &Op;
}

// Try to simplify operations with a constant that may appear after instruction
// selection.
// TODO: See if a frame index with a fixed offset can fold.
static bool tryConstantFoldOp(MachineRegisterInfo &MRI,
                              const SIInstrInfo *TII,
                              MachineInstr *MI,
                              MachineOperand *ImmOp) {
  unsigned Opc = MI->getOpcode();
  if (Opc == AMDGPU::V_NOT_B32_e64 || Opc == AMDGPU::V_NOT_B32_e32 ||
      Opc == AMDGPU::S_NOT_B32) {
    MI->getOperand(1).ChangeToImmediate(~ImmOp->getImm());
    mutateCopyOp(*MI, TII->get(getMovOpc(Opc == AMDGPU::S_NOT_B32)));
    return true;
  }

  int Src1Idx = AMDGPU::getNamedOperandIdx(Opc, AMDGPU::OpName::src1);
  if (Src1Idx == -1)
    return false;

  int Src0Idx = AMDGPU::getNamedOperandIdx(Opc, AMDGPU::OpName::src0);
  MachineOperand *Src0 = getImmOrMaterializedImm(MRI, MI->getOperand(Src0Idx));
  MachineOperand *Src1 = getImmOrMaterializedImm(MRI, MI->getOperand(Src1Idx));

  if (!Src0->isImm() && !Src1->isImm())
    return false;

  if (MI->getOpcode() == AMDGPU::V_LSHL_OR_B32) {
    if (Src0->isImm() && Src0->getImm() == 0) {
      // v_lshl_or_b32 0, X, Y -> copy Y
      // v_lshl_or_b32 0, X, K -> v_mov_b32 K
      bool UseCopy = TII->getNamedOperand(*MI, AMDGPU::OpName::src2)->isReg();
      MI->RemoveOperand(Src1Idx);
      MI->RemoveOperand(Src0Idx);

      MI->setDesc(TII->get(UseCopy ? AMDGPU::COPY : AMDGPU::V_MOV_B32_e32));
      return true;
    }
  }

  // and k0, k1 -> v_mov_b32 (k0 & k1)
  // or k0, k1 -> v_mov_b32 (k0 | k1)
  // xor k0, k1 -> v_mov_b32 (k0 ^ k1)
  if (Src0->isImm() && Src1->isImm()) {
    int32_t NewImm;
    if (!evalBinaryInstruction(Opc, NewImm, Src0->getImm(), Src1->getImm()))
      return false;

    const SIRegisterInfo &TRI = TII->getRegisterInfo();
    bool IsSGPR = TRI.isSGPRReg(MRI, MI->getOperand(0).getReg());

    // Be careful to change the right operand, src0 may belong to a different
    // instruction.
    MI->getOperand(Src0Idx).ChangeToImmediate(NewImm);
    MI->RemoveOperand(Src1Idx);
    mutateCopyOp(*MI, TII->get(getMovOpc(IsSGPR)));
    return true;
  }

  if (!MI->isCommutable())
    return false;

  if (Src0->isImm() && !Src1->isImm()) {
    std::swap(Src0, Src1);
    std::swap(Src0Idx, Src1Idx);
  }

  int32_t Src1Val = static_cast<int32_t>(Src1->getImm());
  if (Opc == AMDGPU::V_OR_B32_e64 ||
      Opc == AMDGPU::V_OR_B32_e32 ||
      Opc == AMDGPU::S_OR_B32) {
    if (Src1Val == 0) {
      // y = or x, 0 => y = copy x
      MI->RemoveOperand(Src1Idx);
      mutateCopyOp(*MI, TII->get(AMDGPU::COPY));
    } else if (Src1Val == -1) {
      // y = or x, -1 => y = v_mov_b32 -1
      MI->RemoveOperand(Src1Idx);
      mutateCopyOp(*MI, TII->get(getMovOpc(Opc == AMDGPU::S_OR_B32)));
    } else
      return false;

    return true;
  }

  if (MI->getOpcode() == AMDGPU::V_AND_B32_e64 ||
      MI->getOpcode() == AMDGPU::V_AND_B32_e32 ||
      MI->getOpcode() == AMDGPU::S_AND_B32) {
    if (Src1Val == 0) {
      // y = and x, 0 => y = v_mov_b32 0
      MI->RemoveOperand(Src0Idx);
      mutateCopyOp(*MI, TII->get(getMovOpc(Opc == AMDGPU::S_AND_B32)));
    } else if (Src1Val == -1) {
      // y = and x, -1 => y = copy x
      MI->RemoveOperand(Src1Idx);
      mutateCopyOp(*MI, TII->get(AMDGPU::COPY));
      stripExtraCopyOperands(*MI);
    } else
      return false;

    return true;
  }

  if (MI->getOpcode() == AMDGPU::V_XOR_B32_e64 ||
      MI->getOpcode() == AMDGPU::V_XOR_B32_e32 ||
      MI->getOpcode() == AMDGPU::S_XOR_B32) {
    if (Src1Val == 0) {
      // y = xor x, 0 => y = copy x
      MI->RemoveOperand(Src1Idx);
      mutateCopyOp(*MI, TII->get(AMDGPU::COPY));
      return true;
    }
  }

  return false;
}

// Try to fold an instruction into a simpler one
static bool tryFoldInst(const SIInstrInfo *TII,
                        MachineInstr *MI) {
  unsigned Opc = MI->getOpcode();

  if (Opc == AMDGPU::V_CNDMASK_B32_e32    ||
      Opc == AMDGPU::V_CNDMASK_B32_e64    ||
      Opc == AMDGPU::V_CNDMASK_B64_PSEUDO) {
    const MachineOperand *Src0 = TII->getNamedOperand(*MI, AMDGPU::OpName::src0);
    const MachineOperand *Src1 = TII->getNamedOperand(*MI, AMDGPU::OpName::src1);
    int Src1ModIdx = AMDGPU::getNamedOperandIdx(Opc, AMDGPU::OpName::src1_modifiers);
    int Src0ModIdx = AMDGPU::getNamedOperandIdx(Opc, AMDGPU::OpName::src0_modifiers);
    if (Src1->isIdenticalTo(*Src0) &&
        (Src1ModIdx == -1 || !MI->getOperand(Src1ModIdx).getImm()) &&
        (Src0ModIdx == -1 || !MI->getOperand(Src0ModIdx).getImm())) {
      LLVM_DEBUG(dbgs() << "Folded " << *MI << " into ");
      auto &NewDesc =
          TII->get(Src0->isReg() ? (unsigned)AMDGPU::COPY : getMovOpc(false));
      int Src2Idx = AMDGPU::getNamedOperandIdx(Opc, AMDGPU::OpName::src2);
      if (Src2Idx != -1)
        MI->RemoveOperand(Src2Idx);
      MI->RemoveOperand(AMDGPU::getNamedOperandIdx(Opc, AMDGPU::OpName::src1));
      if (Src1ModIdx != -1)
        MI->RemoveOperand(Src1ModIdx);
      if (Src0ModIdx != -1)
        MI->RemoveOperand(Src0ModIdx);
      mutateCopyOp(*MI, NewDesc);
      LLVM_DEBUG(dbgs() << *MI << '\n');
      return true;
    }
  }

  return false;
}

void SIFoldOperands::foldInstOperand(MachineInstr &MI,
                                     MachineOperand &OpToFold) const {
  // We need mutate the operands of new mov instructions to add implicit
  // uses of EXEC, but adding them invalidates the use_iterator, so defer
  // this.
  SmallVector<MachineInstr *, 4> CopiesToReplace;
  SmallVector<FoldCandidate, 4> FoldList;
  MachineOperand &Dst = MI.getOperand(0);

  bool FoldingImm = OpToFold.isImm() || OpToFold.isFI() || OpToFold.isGlobal();
  if (FoldingImm) {
    unsigned NumLiteralUses = 0;
    MachineOperand *NonInlineUse = nullptr;
    int NonInlineUseOpNo = -1;

    MachineRegisterInfo::use_iterator NextUse;
    for (MachineRegisterInfo::use_iterator
           Use = MRI->use_begin(Dst.getReg()), E = MRI->use_end();
         Use != E; Use = NextUse) {
      NextUse = std::next(Use);
      MachineInstr *UseMI = Use->getParent();
      unsigned OpNo = Use.getOperandNo();

      // Folding the immediate may reveal operations that can be constant
      // folded or replaced with a copy. This can happen for example after
      // frame indices are lowered to constants or from splitting 64-bit
      // constants.
      //
      // We may also encounter cases where one or both operands are
      // immediates materialized into a register, which would ordinarily not
      // be folded due to multiple uses or operand constraints.

      if (OpToFold.isImm() && tryConstantFoldOp(*MRI, TII, UseMI, &OpToFold)) {
        LLVM_DEBUG(dbgs() << "Constant folded " << *UseMI << '\n');

        // Some constant folding cases change the same immediate's use to a new
        // instruction, e.g. and x, 0 -> 0. Make sure we re-visit the user
        // again. The same constant folded instruction could also have a second
        // use operand.
        NextUse = MRI->use_begin(Dst.getReg());
        FoldList.clear();
        continue;
      }

      // Try to fold any inline immediate uses, and then only fold other
      // constants if they have one use.
      //
      // The legality of the inline immediate must be checked based on the use
      // operand, not the defining instruction, because 32-bit instructions
      // with 32-bit inline immediate sources may be used to materialize
      // constants used in 16-bit operands.
      //
      // e.g. it is unsafe to fold:
      //  s_mov_b32 s0, 1.0    // materializes 0x3f800000
      //  v_add_f16 v0, v1, s0 // 1.0 f16 inline immediate sees 0x00003c00

      // Folding immediates with more than one use will increase program size.
      // FIXME: This will also reduce register usage, which may be better
      // in some cases. A better heuristic is needed.
      if (isInlineConstantIfFolded(TII, *UseMI, OpNo, OpToFold)) {
        foldOperand(OpToFold, UseMI, OpNo, FoldList, CopiesToReplace);
      } else if (frameIndexMayFold(TII, *UseMI, OpNo, OpToFold)) {
        foldOperand(OpToFold, UseMI, OpNo, FoldList,
                    CopiesToReplace);
      } else {
        if (++NumLiteralUses == 1) {
          NonInlineUse = &*Use;
          NonInlineUseOpNo = OpNo;
        }
      }
    }

    if (NumLiteralUses == 1) {
      MachineInstr *UseMI = NonInlineUse->getParent();
      foldOperand(OpToFold, UseMI, NonInlineUseOpNo, FoldList, CopiesToReplace);
    }
  } else {
    // Folding register.
    SmallVector <MachineRegisterInfo::use_iterator, 4> UsesToProcess;
    for (MachineRegisterInfo::use_iterator
           Use = MRI->use_begin(Dst.getReg()), E = MRI->use_end();
         Use != E; ++Use) {
      UsesToProcess.push_back(Use);
    }
    for (auto U : UsesToProcess) {
      MachineInstr *UseMI = U->getParent();

      foldOperand(OpToFold, UseMI, U.getOperandNo(),
        FoldList, CopiesToReplace);
    }
  }

  MachineFunction *MF = MI.getParent()->getParent();
  // Make sure we add EXEC uses to any new v_mov instructions created.
  for (MachineInstr *Copy : CopiesToReplace)
    Copy->addImplicitDefUseOperands(*MF);

  for (FoldCandidate &Fold : FoldList) {
    if (Fold.isReg() && Register::isVirtualRegister(Fold.OpToFold->getReg())) {
      Register Reg = Fold.OpToFold->getReg();
      MachineInstr *DefMI = Fold.OpToFold->getParent();
      if (DefMI->readsRegister(AMDGPU::EXEC, TRI) &&
          execMayBeModifiedBeforeUse(*MRI, Reg, *DefMI, *Fold.UseMI))
        continue;
    }
    if (updateOperand(Fold, *TII, *TRI, *ST)) {
      // Clear kill flags.
      if (Fold.isReg()) {
        assert(Fold.OpToFold && Fold.OpToFold->isReg());
        // FIXME: Probably shouldn't bother trying to fold if not an
        // SGPR. PeepholeOptimizer can eliminate redundant VGPR->VGPR
        // copies.
        MRI->clearKillFlags(Fold.OpToFold->getReg());
      }
      LLVM_DEBUG(dbgs() << "Folded source from " << MI << " into OpNo "
                        << static_cast<int>(Fold.UseOpNo) << " of "
                        << *Fold.UseMI << '\n');
      tryFoldInst(TII, Fold.UseMI);
    } else if (Fold.isCommuted()) {
      // Restoring instruction's original operand order if fold has failed.
      TII->commuteInstruction(*Fold.UseMI, false);
    }
  }
}

// Clamp patterns are canonically selected to v_max_* instructions, so only
// handle them.
const MachineOperand *SIFoldOperands::isClamp(const MachineInstr &MI) const {
  unsigned Op = MI.getOpcode();
  switch (Op) {
  case AMDGPU::V_MAX_F32_e64:
  case AMDGPU::V_MAX_F16_e64:
  case AMDGPU::V_MAX_F64:
  case AMDGPU::V_PK_MAX_F16: {
    if (!TII->getNamedOperand(MI, AMDGPU::OpName::clamp)->getImm())
      return nullptr;

    // Make sure sources are identical.
    const MachineOperand *Src0 = TII->getNamedOperand(MI, AMDGPU::OpName::src0);
    const MachineOperand *Src1 = TII->getNamedOperand(MI, AMDGPU::OpName::src1);
    if (!Src0->isReg() || !Src1->isReg() ||
        Src0->getReg() != Src1->getReg() ||
        Src0->getSubReg() != Src1->getSubReg() ||
        Src0->getSubReg() != AMDGPU::NoSubRegister)
      return nullptr;

    // Can't fold up if we have modifiers.
    if (TII->hasModifiersSet(MI, AMDGPU::OpName::omod))
      return nullptr;

    unsigned Src0Mods
      = TII->getNamedOperand(MI, AMDGPU::OpName::src0_modifiers)->getImm();
    unsigned Src1Mods
      = TII->getNamedOperand(MI, AMDGPU::OpName::src1_modifiers)->getImm();

    // Having a 0 op_sel_hi would require swizzling the output in the source
    // instruction, which we can't do.
    unsigned UnsetMods = (Op == AMDGPU::V_PK_MAX_F16) ? SISrcMods::OP_SEL_1
                                                      : 0u;
    if (Src0Mods != UnsetMods && Src1Mods != UnsetMods)
      return nullptr;
    return Src0;
  }
  default:
    return nullptr;
  }
}

// We obviously have multiple uses in a clamp since the register is used twice
// in the same instruction.
static bool hasOneNonDBGUseInst(const MachineRegisterInfo &MRI, unsigned Reg) {
  int Count = 0;
  for (auto I = MRI.use_instr_nodbg_begin(Reg), E = MRI.use_instr_nodbg_end();
       I != E; ++I) {
    if (++Count > 1)
      return false;
  }

  return true;
}

// FIXME: Clamp for v_mad_mixhi_f16 handled during isel.
bool SIFoldOperands::tryFoldClamp(MachineInstr &MI) {
  const MachineOperand *ClampSrc = isClamp(MI);
  if (!ClampSrc || !hasOneNonDBGUseInst(*MRI, ClampSrc->getReg()))
    return false;

  MachineInstr *Def = MRI->getVRegDef(ClampSrc->getReg());

  // The type of clamp must be compatible.
  if (TII->getClampMask(*Def) != TII->getClampMask(MI))
    return false;

  MachineOperand *DefClamp = TII->getNamedOperand(*Def, AMDGPU::OpName::clamp);
  if (!DefClamp)
    return false;

  LLVM_DEBUG(dbgs() << "Folding clamp " << *DefClamp << " into " << *Def
                    << '\n');

  // Clamp is applied after omod, so it is OK if omod is set.
  DefClamp->setImm(1);
  MRI->replaceRegWith(MI.getOperand(0).getReg(), Def->getOperand(0).getReg());
  MI.eraseFromParent();
  return true;
}

static int getOModValue(unsigned Opc, int64_t Val) {
  switch (Opc) {
  case AMDGPU::V_MUL_F32_e64: {
    switch (static_cast<uint32_t>(Val)) {
    case 0x3f000000: // 0.5
      return SIOutMods::DIV2;
    case 0x40000000: // 2.0
      return SIOutMods::MUL2;
    case 0x40800000: // 4.0
      return SIOutMods::MUL4;
    default:
      return SIOutMods::NONE;
    }
  }
  case AMDGPU::V_MUL_F16_e64: {
    switch (static_cast<uint16_t>(Val)) {
    case 0x3800: // 0.5
      return SIOutMods::DIV2;
    case 0x4000: // 2.0
      return SIOutMods::MUL2;
    case 0x4400: // 4.0
      return SIOutMods::MUL4;
    default:
      return SIOutMods::NONE;
    }
  }
  default:
    llvm_unreachable("invalid mul opcode");
  }
}

// FIXME: Does this really not support denormals with f16?
// FIXME: Does this need to check IEEE mode bit? SNaNs are generally not
// handled, so will anything other than that break?
std::pair<const MachineOperand *, int>
SIFoldOperands::isOMod(const MachineInstr &MI) const {
  unsigned Op = MI.getOpcode();
  switch (Op) {
  case AMDGPU::V_MUL_F32_e64:
  case AMDGPU::V_MUL_F16_e64: {
    // If output denormals are enabled, omod is ignored.
    if ((Op == AMDGPU::V_MUL_F32_e64 && ST->hasFP32Denormals()) ||
        (Op == AMDGPU::V_MUL_F16_e64 && ST->hasFP16Denormals()))
      return std::make_pair(nullptr, SIOutMods::NONE);

    const MachineOperand *RegOp = nullptr;
    const MachineOperand *ImmOp = nullptr;
    const MachineOperand *Src0 = TII->getNamedOperand(MI, AMDGPU::OpName::src0);
    const MachineOperand *Src1 = TII->getNamedOperand(MI, AMDGPU::OpName::src1);
    if (Src0->isImm()) {
      ImmOp = Src0;
      RegOp = Src1;
    } else if (Src1->isImm()) {
      ImmOp = Src1;
      RegOp = Src0;
    } else
      return std::make_pair(nullptr, SIOutMods::NONE);

    int OMod = getOModValue(Op, ImmOp->getImm());
    if (OMod == SIOutMods::NONE ||
        TII->hasModifiersSet(MI, AMDGPU::OpName::src0_modifiers) ||
        TII->hasModifiersSet(MI, AMDGPU::OpName::src1_modifiers) ||
        TII->hasModifiersSet(MI, AMDGPU::OpName::omod) ||
        TII->hasModifiersSet(MI, AMDGPU::OpName::clamp))
      return std::make_pair(nullptr, SIOutMods::NONE);

    return std::make_pair(RegOp, OMod);
  }
  case AMDGPU::V_ADD_F32_e64:
  case AMDGPU::V_ADD_F16_e64: {
    // If output denormals are enabled, omod is ignored.
    if ((Op == AMDGPU::V_ADD_F32_e64 && ST->hasFP32Denormals()) ||
        (Op == AMDGPU::V_ADD_F16_e64 && ST->hasFP16Denormals()))
      return std::make_pair(nullptr, SIOutMods::NONE);

    // Look through the DAGCombiner canonicalization fmul x, 2 -> fadd x, x
    const MachineOperand *Src0 = TII->getNamedOperand(MI, AMDGPU::OpName::src0);
    const MachineOperand *Src1 = TII->getNamedOperand(MI, AMDGPU::OpName::src1);

    if (Src0->isReg() && Src1->isReg() && Src0->getReg() == Src1->getReg() &&
        Src0->getSubReg() == Src1->getSubReg() &&
        !TII->hasModifiersSet(MI, AMDGPU::OpName::src0_modifiers) &&
        !TII->hasModifiersSet(MI, AMDGPU::OpName::src1_modifiers) &&
        !TII->hasModifiersSet(MI, AMDGPU::OpName::clamp) &&
        !TII->hasModifiersSet(MI, AMDGPU::OpName::omod))
      return std::make_pair(Src0, SIOutMods::MUL2);

    return std::make_pair(nullptr, SIOutMods::NONE);
  }
  default:
    return std::make_pair(nullptr, SIOutMods::NONE);
  }
}

// FIXME: Does this need to check IEEE bit on function?
bool SIFoldOperands::tryFoldOMod(MachineInstr &MI) {
  const MachineOperand *RegOp;
  int OMod;
  std::tie(RegOp, OMod) = isOMod(MI);
  if (OMod == SIOutMods::NONE || !RegOp->isReg() ||
      RegOp->getSubReg() != AMDGPU::NoSubRegister ||
      !hasOneNonDBGUseInst(*MRI, RegOp->getReg()))
    return false;

  MachineInstr *Def = MRI->getVRegDef(RegOp->getReg());
  MachineOperand *DefOMod = TII->getNamedOperand(*Def, AMDGPU::OpName::omod);
  if (!DefOMod || DefOMod->getImm() != SIOutMods::NONE)
    return false;

  // Clamp is applied after omod. If the source already has clamp set, don't
  // fold it.
  if (TII->hasModifiersSet(*Def, AMDGPU::OpName::clamp))
    return false;

  LLVM_DEBUG(dbgs() << "Folding omod " << MI << " into " << *Def << '\n');

  DefOMod->setImm(OMod);
  MRI->replaceRegWith(MI.getOperand(0).getReg(), Def->getOperand(0).getReg());
  MI.eraseFromParent();
  return true;
}

bool SIFoldOperands::runOnMachineFunction(MachineFunction &MF) {
  if (skipFunction(MF.getFunction()))
    return false;

  MRI = &MF.getRegInfo();
  ST = &MF.getSubtarget<GCNSubtarget>();
  TII = ST->getInstrInfo();
  TRI = &TII->getRegisterInfo();
  MFI = MF.getInfo<SIMachineFunctionInfo>();

  // omod is ignored by hardware if IEEE bit is enabled. omod also does not
  // correctly handle signed zeros.
  //
  // FIXME: Also need to check strictfp
  bool IsIEEEMode = MFI->getMode().IEEE;
  bool HasNSZ = MFI->hasNoSignedZerosFPMath();

  for (MachineBasicBlock *MBB : depth_first(&MF)) {
    MachineBasicBlock::iterator I, Next;

    MachineOperand *CurrentKnownM0Val = nullptr;
    for (I = MBB->begin(); I != MBB->end(); I = Next) {
      Next = std::next(I);
      MachineInstr &MI = *I;

      tryFoldInst(TII, &MI);

      if (!TII->isFoldableCopy(MI)) {
        // TODO: Omod might be OK if there is NSZ only on the source
        // instruction, and not the omod multiply.
        if (IsIEEEMode || (!HasNSZ && !MI.getFlag(MachineInstr::FmNsz)) ||
            !tryFoldOMod(MI))
          tryFoldClamp(MI);

        // Saw an unknown clobber of m0, so we no longer know what it is.
        if (CurrentKnownM0Val && MI.modifiesRegister(AMDGPU::M0, TRI))
          CurrentKnownM0Val = nullptr;
        continue;
      }

      // Specially track simple redefs of m0 to the same value in a block, so we
      // can erase the later ones.
      if (MI.getOperand(0).getReg() == AMDGPU::M0) {
        MachineOperand &NewM0Val = MI.getOperand(1);
        if (CurrentKnownM0Val && CurrentKnownM0Val->isIdenticalTo(NewM0Val)) {
          MI.eraseFromParent();
          continue;
        }

        // We aren't tracking other physical registers
        CurrentKnownM0Val = (NewM0Val.isReg() && NewM0Val.getReg().isPhysical()) ?
          nullptr : &NewM0Val;
        continue;
      }

      MachineOperand &OpToFold = MI.getOperand(1);
      bool FoldingImm =
          OpToFold.isImm() || OpToFold.isFI() || OpToFold.isGlobal();

      // FIXME: We could also be folding things like TargetIndexes.
      if (!FoldingImm && !OpToFold.isReg())
        continue;

      if (OpToFold.isReg() && !Register::isVirtualRegister(OpToFold.getReg()))
        continue;

      // Prevent folding operands backwards in the function. For example,
      // the COPY opcode must not be replaced by 1 in this example:
      //
      //    %3 = COPY %vgpr0; VGPR_32:%3
      //    ...
      //    %vgpr0 = V_MOV_B32_e32 1, implicit %exec
      MachineOperand &Dst = MI.getOperand(0);
      if (Dst.isReg() && !Register::isVirtualRegister(Dst.getReg()))
        continue;

      foldInstOperand(MI, OpToFold);
    }
  }
  return false;
}
