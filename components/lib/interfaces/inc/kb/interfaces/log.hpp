/**
 * @file log.hpp
 * @brief The log contract: `kb::iface::LogSink` (bounded, no allocation) and `LogSinkRef`.
 *
 * `void Write(LogLevel level, std::string_view text) noexcept` writes one line; a sink may cut long
 * text and may drop lines under load, but never blocks without bound and never allocates.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 The kb-core authors
 */
#ifndef KB_INTERFACES_LOG_HPP
#define KB_INTERFACES_LOG_HPP

#include <concepts>
#include <string_view>

#include "kb/interfaces/types.hpp"

namespace kb::iface {

/** @brief Where log lines go. */
template <typename L>
concept LogSink = requires(L& sink, LogLevel level, std::string_view text) {
  { sink.Write(level, text) } noexcept -> std::same_as<void>;
};

} // namespace kb::iface

#endif // KB_INTERFACES_LOG_HPP
