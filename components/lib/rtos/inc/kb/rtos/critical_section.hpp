/**
 * @file critical_section.hpp
 * @brief kb::rtos::CriticalSection: masks interrupts and preemption for one short scope (RAII,
 * nests).
 *
 * Keep the scope to a few instructions (copying a value shared with an interrupt handler); never
 * block, sleep or call a kb-core function that may wait inside it.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 The kb-core authors
 */
#ifndef KB_RTOS_CRITICAL_SECTION_HPP
#define KB_RTOS_CRITICAL_SECTION_HPP

#include "kb/rtos/backend.hpp"

namespace kb::rtos {

/** @brief Enters a critical section on construction and leaves it on destruction. */
class CriticalSection {
public:
  CriticalSection() noexcept {
    backend::EnterCritical();
  }
  CriticalSection(const CriticalSection&) = delete;
  CriticalSection& operator=(const CriticalSection&) = delete;
  CriticalSection(CriticalSection&&) = delete;
  CriticalSection& operator=(CriticalSection&&) = delete;
  ~CriticalSection() {
    backend::ExitCritical();
  }
};

} // namespace kb::rtos

#endif // KB_RTOS_CRITICAL_SECTION_HPP
