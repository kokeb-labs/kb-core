# The ThreadX backend's boundary file (deviations/boundary-files.yml) and where the kernel comes from
# (docs/parts/rtos-threadx.md). ThreadX headers come in with -isystem:
#   KB_RTOS=THREADX, KB_THREADX_INCLUDE_DIRS     a kernel the product builds (an STM32Cube or vendor SDK copy): its
#                                                common/inc, port inc and tx_user.h folders, KB_THREADX_DEFINITIONS
#                                                (the kernel's own TX_* defines, e.g. TX_INCLUDE_USER_DEFINE_FILE)
#                                                and KB_THREADX_TICK_HZ (required); the product links the kernel
#   KB_RTOS=THREADX, KB_THREADX_PORT=<port>      the pinned submodule third_party/threadx (KB_THREADX_DIR), built
#                                                here for <port> (linux, cortex_m0, cortex_m4, cortex_m7, ...) with
#                                                the gnu toolchain and KB_THREADX_USER_FILE (default kb_tx_user.h)
#   neither                                      the configure stops: the backend never builds without a kernel
#   KB_BUILD_TESTS                               kb_rtos_backend_threadx_fake over tests/fakes/threadx, always
#   KB_THREADX_HOST_TESTS=ON (Linux host)        the pinned kernel's Linux port too, for components/lib/rtos/tests on ThreadX
# KB_THREADX_TICK_HZ is the tick both the kernel and the backend are built for; the backend fails to compile when the
# kernel's TX_TIMER_TICKS_PER_SECOND differs. KB_THREADX_QUEUE_WORD_BYTES is sizeof(ULONG) of the kernel (default 4;
# the backend fails to compile when it is wrong). KB_THREADX_THREAD_BYTES, when set, is sizeof(TX_THREAD) in the
# product's kernel build: the backend then checks that it sees the same control-block layout.
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The kb-core authors
set(_kb_threadx_sources threadx_boundary.cpp threadx_isr.cpp)
set(KB_THREADX_DIR "${KB_ROOT}/third_party/threadx" CACHE PATH "The ThreadX checkout (the pinned submodule)")
# kb_tx_user.h lives outside the backend directory: it is build configuration for the kernel, not target code, and
# the MISRA scan covers this directory.
set(_kb_threadx_default_user "${KB_ROOT}/components/lib/rtos/threadx-config/kb_tx_user.h")
set(KB_THREADX_USER_FILE "${_kb_threadx_default_user}" CACHE FILEPATH
    "tx_user.h the pinned kernel is built with")
set(KB_THREADX_THREAD_BYTES "" CACHE STRING "sizeof(TX_THREAD) in the product's kernel build (optional check)")

# Builds the pinned kernel once, as target `threadx`, for `port` (gnu toolchain) at KB_THREADX_TICK_HZ.
function(kb_threadx_kernel port)
  if(TARGET threadx)
    return()
  endif()
  if(NOT EXISTS "${KB_THREADX_DIR}/common/inc/tx_api.h")
    message(FATAL_ERROR "[kb] The ThreadX kernel is not checked out at ${KB_THREADX_DIR}. Run "
                        "'git submodule update --init third_party/threadx' or set KB_THREADX_INCLUDE_DIRS.")
  endif()
  if(NOT EXISTS "${KB_THREADX_DIR}/ports/${port}/gnu/CMakeLists.txt")
    message(FATAL_ERROR "[kb] KB_THREADX_PORT=${port}: ThreadX has no kernel port directory ${port}/gnu")
  endif()
  set(THREADX_ARCH "${port}")
  set(THREADX_TOOLCHAIN gnu)
  set(TX_USER_FILE "${KB_THREADX_USER_FILE}")
  add_subdirectory("${KB_THREADX_DIR}" "${CMAKE_BINARY_DIR}/third_party/threadx" EXCLUDE_FROM_ALL)
  target_compile_definitions(threadx PUBLIC "KB_THREADX_TICK_HZ=${KB_THREADX_TICK_HZ}")
endfunction()

# The tick: the default user file takes it from KB_THREADX_TICK_HZ (default 1000); any other kernel must say it.
if(KB_RTOS STREQUAL "THREADX" OR KB_THREADX_HOST_TESTS)
  if(NOT KB_THREADX_TICK_HZ)
    if(KB_THREADX_INCLUDE_DIRS OR NOT KB_THREADX_USER_FILE STREQUAL "${_kb_threadx_default_user}")
      message(FATAL_ERROR "[kb] KB_RTOS=THREADX with the product's kernel or tx_user.h needs KB_THREADX_TICK_HZ: "
                          "the TX_TIMER_TICKS_PER_SECOND that kernel is built with (tx_api.h silently defaults to 100)")
    endif()
    set(KB_THREADX_TICK_HZ 1000)
  endif()
  set_property(GLOBAL PROPERTY KB_THREADX_TICK_HZ "${KB_THREADX_TICK_HZ}") # for the tests' timing checks
endif()

if(KB_RTOS STREQUAL "THREADX")
  add_library(kb_rtos_backend_threadx STATIC ${_kb_threadx_sources})
  kb_strict_target(kb_rtos_backend_threadx)
  target_link_libraries(kb_rtos_backend_threadx PUBLIC kb_rtos_api)
  if(KB_THREADX_THREAD_BYTES)
    target_compile_definitions(kb_rtos_backend_threadx PRIVATE "KB_THREADX_THREAD_BYTES=${KB_THREADX_THREAD_BYTES}")
  endif()
  if(KB_THREADX_INCLUDE_DIRS)
    target_include_directories(kb_rtos_backend_threadx SYSTEM PRIVATE ${KB_THREADX_INCLUDE_DIRS})
    target_compile_definitions(kb_rtos_backend_threadx PRIVATE ${KB_THREADX_DEFINITIONS}
      "KB_THREADX_TICK_HZ=${KB_THREADX_TICK_HZ}")
  elseif(KB_THREADX_PORT)
    # Headers and options of the kernel only: the image links `threadx` itself, so the installed backend does
    # not carry a target kb-core does not install. Same defines as the kernel, so the control blocks agree.
    kb_threadx_kernel("${KB_THREADX_PORT}")
    target_include_directories(kb_rtos_backend_threadx SYSTEM PRIVATE
      "$<TARGET_PROPERTY:threadx,INTERFACE_INCLUDE_DIRECTORIES>")
    target_compile_definitions(kb_rtos_backend_threadx PRIVATE
      "$<TARGET_PROPERTY:threadx,INTERFACE_COMPILE_DEFINITIONS>")
  else()
    message(FATAL_ERROR "[kb] KB_RTOS=THREADX needs a kernel: KB_THREADX_INCLUDE_DIRS (the product's ThreadX "
                        "headers and tx_user.h) or KB_THREADX_PORT (build the pinned third_party/threadx for that "
                        "port, e.g. cortex_m4)")
  endif()
endif()

if(KB_BUILD_TESTS)
  add_library(kb_fake_threadx STATIC "${KB_ROOT}/tests/fakes/threadx/fake_threadx.cpp")
  kb_strict_target(kb_fake_threadx)
  target_include_directories(kb_fake_threadx SYSTEM PUBLIC "${KB_ROOT}/tests/fakes/threadx")
  add_library(kb_rtos_backend_threadx_fake STATIC ${_kb_threadx_sources})
  kb_strict_target(kb_rtos_backend_threadx_fake)
  target_link_libraries(kb_rtos_backend_threadx_fake PUBLIC kb_rtos_api kb_fake_threadx)
  if(KB_THREADX_HOST_TESTS)
    if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR CMAKE_CROSSCOMPILING)
      message(FATAL_ERROR "[kb] KB_THREADX_HOST_TESTS needs a Linux host: the ThreadX Linux port uses Linux "
                          "scheduling and signals (run it in the CI container image elsewhere)")
    endif()
    kb_threadx_kernel(linux)
    add_library(kb_rtos_backend_threadx_linux STATIC ${_kb_threadx_sources})
    kb_strict_target(kb_rtos_backend_threadx_linux)
    target_link_libraries(kb_rtos_backend_threadx_linux PUBLIC kb_rtos_api threadx)
  endif()
endif()
