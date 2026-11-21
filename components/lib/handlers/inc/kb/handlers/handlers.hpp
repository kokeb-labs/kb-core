/**
 * @file handlers.hpp
 * @brief Everything of kb::handlers that is not a binding: the handler core, the start-up and
 * health types, the bus domains and holds, both access facades, the hook bases, the liveness feed,
 * the asynchronous poller and the health journal.
 * Bindings are included one by one (`kb/handlers/bindings/<chip>.hpp`).
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 The kb-core authors
 */
#ifndef KB_HANDLERS_HANDLERS_HPP
#define KB_HANDLERS_HANDLERS_HPP

#include "kb/handlers/common/async_poller.hpp"
#include "kb/handlers/common/bus_domain.hpp"
#include "kb/handlers/common/driver_result.hpp"
#include "kb/handlers/common/facade.hpp"
#include "kb/handlers/common/guarded.hpp"
#include "kb/handlers/common/handler_core.hpp"
#include "kb/handlers/common/health.hpp"
#include "kb/handlers/common/health_journal.hpp"
#include "kb/handlers/common/hooks.hpp"
#include "kb/handlers/common/liveness.hpp"
#include "kb/handlers/common/owner_access.hpp"
#include "kb/handlers/common/request_channel.hpp"
#include "kb/handlers/common/seqlock.hpp"
#include "kb/handlers/common/service_key.hpp"
#include "kb/handlers/common/types.hpp"

#endif // KB_HANDLERS_HANDLERS_HPP
