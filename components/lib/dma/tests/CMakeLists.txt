# Host tests of kb::dma (components/lib/dma/tests/README.md): vocabulary and state machine, the stream, the host
# model, and the conformance suite T1-T18 against a good model and against one broken model per case.
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The kb-core authors
kb_add_unit_test(kb_dma_tests
  SOURCES test_vocab.cpp test_assignment.cpp test_stream.cpp test_conformance.cpp
  LIBS kb_dma kb_dma_host)
find_package(Threads)
if(Threads_FOUND)
  kb_add_unit_test(kb_dma_threads SOURCES test_threads.cpp LIBS kb_dma Threads::Threads)
  # Coverage builds: the threads share the instrumented code, so its counters must be updated atomically.
  if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND "${CMAKE_CXX_FLAGS}" MATCHES "--coverage|-fprofile-arcs")
    target_compile_options(kb_dma_threads PRIVATE -fprofile-update=atomic)
  endif()
endif()
kb_add_compile_fail_tests(dma "${CMAKE_CURRENT_SOURCE_DIR}/compile_fail"
  SOURCES lease_copy.cpp lease_assign.cpp portkey_forged.cpp buffer_copy.cpp buffer_zero.cpp region_bad_line.cpp
          region_align_below_line.cpp region_unreachable.cpp frame_too_large.cpp size_not_whole_units.cpp
          element_size_mismatch.cpp stream_one_frame.cpp stream_frame_not_trivial.cpp stream_move.cpp
          frameview_copy.cpp cacheops_missing.cpp
          notify_above_rtos_limit.cpp bus_frame_too_large.cpp shape_frames_mismatch.cpp
          streams_priority_differ.cpp
  LIBS kb_dma)
