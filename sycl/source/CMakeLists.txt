#To-Do:
#1. Figure out why CMP0057 has to be set. Should have been taken care of earlier in the build
#2. Use AddLLVM to modify the build and access config options
#cmake_policy(SET CMP0057 NEW)
#include(AddLLVM)

function(add_sycl_rt_library LIB_NAME)

  add_library(${LIB_NAME} SHARED ${ARGN})

  add_dependencies(${LIB_NAME}
    ocl-icd
    ocl-headers
    sycl-headers
  )

  set_target_properties(${LIB_NAME} PROPERTIES LINKER_LANGUAGE CXX)

  if (MSVC)
      target_compile_definitions(${LIB_NAME} PRIVATE __SYCL_BUILD_SYCL_DLL )
  endif()
  target_include_directories(${LIB_NAME} PRIVATE "${sycl_inc_dir}")
  target_link_libraries(${LIB_NAME}
      PRIVATE OpenCL::Headers
      PRIVATE ${OpenCL_LIBRARIES}
  )
  if (SYCL_USE_LIBCXX)
      if ((CMAKE_CXX_COMPILER_ID STREQUAL "GNU") OR
          (CMAKE_CXX_COMPILER_ID STREQUAL "Clang"))
          target_compile_options(${LIB_NAME} PRIVATE -nostdinc++)
          if ((NOT (DEFINED SYCL_LIBCXX_INCLUDE_PATH)) OR (NOT (DEFINED SYCL_LIBCXX_LIBRARY_PATH)))
              message(FATAL_ERROR "When building with libc++ SYCL_LIBCXX_INCLUDE_PATHS and"
                                  "SYCL_LIBCXX_LIBRARY_PATH should be set")
          endif()
          target_include_directories(${LIB_NAME} PRIVATE "${SYCL_LIBCXX_INCLUDE_PATH}")
          target_link_libraries(${LIB_NAME} PRIVATE "-L${SYCL_LIBCXX_LIBRARY_PATH}" -nodefaultlibs -lc++ -lc++abi -lm -lc -lgcc_s -lgcc)
      else()
          message(FATAL_ERROR "Build with libc++ is not yet supported for this compiler")
      endif()
  else()

# Workaround for bug in GCC version 5 and higher.
# More information https://bugs.launchpad.net/ubuntu/+source/gcc-5/+bug/1568899
    if (CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND
        CMAKE_CXX_COMPILER_VERSION VERSION_GREATER 5.0)
    target_link_libraries(${LIB_NAME} PRIVATE gcc_s gcc)
    endif()

  endif()
endfunction(add_sycl_rt_library)

set(SYCL_SOURCES
    "${sycl_inc_dir}/CL/sycl.hpp"
    "detail/builtins_common.cpp"
    "detail/builtins_geometric.cpp"
    "detail/builtins_integer.cpp"
    "detail/builtins_math.cpp"
    "detail/builtins_relational.cpp"
    "detail/pi.cpp"
    "detail/pi_opencl.cpp"
    "detail/common.cpp"
    "detail/context_impl.cpp"
    "detail/device_impl.cpp"
    "detail/device_info.cpp"
    "detail/event_impl.cpp"
    "detail/force_device.cpp"
    "detail/helpers.cpp"
    "detail/image_accessor_util.cpp"
    "detail/image_impl.cpp"
    "detail/kernel_impl.cpp"
    "detail/kernel_info.cpp"
    "detail/memory_manager.cpp"
    "detail/platform_impl.cpp"
    "detail/platform_info.cpp"
    "detail/program_impl.cpp"
    "detail/program_manager/program_manager.cpp"
    "detail/queue_impl.cpp"
    "detail/os_util.cpp"
    "detail/platform_util.cpp"
    "detail/sampler_impl.cpp"
    "detail/stream_impl.cpp"
    "detail/scheduler/commands.cpp"
    "detail/scheduler/scheduler.cpp"
    "detail/scheduler/graph_processor.cpp"
    "detail/scheduler/graph_builder.cpp"
    "detail/usm/clusm.cpp"
    "detail/usm/usm_dispatch.cpp"
    "detail/usm/usm_impl.cpp"
    "detail/util.cpp"
    "context.cpp"
    "device.cpp"
    "device_selector.cpp"
    "event.cpp"
    "exception.cpp"
    "exception_list.cpp"
    "half_type.cpp"
    "kernel.cpp"
    "platform.cpp"
    "queue.cpp"
    "ordered_queue.cpp"
    "sampler.cpp"
    "stream.cpp"
    "spirv_ops.cpp"
)

add_sycl_rt_library(sycl ${SYCL_SOURCES})

if (MSVC)
  # MSVC provides two incompatible build variants for its CRT: release and debug
  # To avoid potential issues in user code we also need to provide two kinds
  # of SYCL Runtime Library for release and debug configurations.
  set(SYCL_CXX_FLAGS "")
  if (CMAKE_BUILD_TYPE MATCHES "Debug")
    set(SYCL_CXX_FLAGS "${CMAKE_CXX_FLAGS_DEBUG}")
    string(REPLACE "/MDd" "" SYCL_CXX_FLAGS "${SYCL_CXX_FLAGS}")
    string(REPLACE "/MTd" "" SYCL_CXX_FLAGS "${SYCL_CXX_FLAGS}")
  else()
    if (CMAKE_BUILD_TYPE MATCHES "Release")
      set(SYCL_CXX_FLAGS "${CMAKE_CXX_FLAGS_RELEASE}")
    elseif (CMAKE_BUILD_TYPE MATCHES "RelWithDebInfo")
      set(SYCL_CXX_FLAGS "${CMAKE_CXX_FLAGS_MINSIZEREL}")
    elseif (CMAKE_BUILD_TYPE MATCHES "MinSizeRel")
      set(SYCL_CXX_FLAGS "${CMAKE_CXX_FLAGS_RELWITHDEBINFO}")
    endif()
    string(REPLACE "/MD" "" SYCL_CXX_FLAGS "${SYCL_CXX_FLAGS}")
    string(REPLACE "/MT" "" SYCL_CXX_FLAGS "${SYCL_CXX_FLAGS}")
  endif()

  # target_compile_options requires list of options, not a string
  string(REPLACE " " ";" SYCL_CXX_FLAGS "${SYCL_CXX_FLAGS}")

  set(SYCL_CXX_FLAGS_RELEASE "${SYCL_CXX_FLAGS};/MD")
  set(SYCL_CXX_FLAGS_DEBUG "${SYCL_CXX_FLAGS};/MDd")

  # CMake automatically applies these flags to all targets. To override this
  # behavior, options lists are reset.
  set(CMAKE_CXX_FLAGS_RELEASE "")
  set(CMAKE_CXX_FLAGS_MINSIZEREL "")
  set(CMAKE_CXX_FLAGS_RELWITHDEBINFO "")
  set(CMAKE_CXX_FLAGS_DEBUG "")

  target_compile_options(sycl PUBLIC ${SYCL_CXX_FLAGS_RELEASE})

  add_sycl_rt_library(sycld ${SYCL_SOURCES})
  target_compile_options(sycld PUBLIC ${SYCL_CXX_FLAGS_DEBUG})
endif()

install(TARGETS ${SYCL_RT_LIBS}
    ARCHIVE DESTINATION "lib" COMPONENT sycl
    LIBRARY DESTINATION "lib" COMPONENT sycl
    RUNTIME DESTINATION "bin" COMPONENT sycl)
