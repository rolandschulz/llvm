#include <spirv/spirv.h>

__attribute__((convergent)) int __ockl_wgred_or_i32(int a);

_CLC_DEF _CLC_OVERLOAD _CLC_CONVERGENT bool __spirv_GroupAny(uint scope,
                                                             bool predicate) {
  return __ockl_wgred_or_i32(predicate);
}
