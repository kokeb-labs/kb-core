// Fuzz target: kb::handlers::RequestChannel follows a model of its slot protocol for any sequence
// of caller and owner operations (components/lib/handlers/fuzz/README.md).
// Single context, none backend: waits use a zero timeout.
// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The kb-core authors
// Loops stay in the plain form of the target code.
// cppcheck-suppress-file useStlAlgorithm
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>

#include "kb/handlers/common/request_channel.hpp"

namespace {

void Require(bool condition) {
  if (!condition) {
    std::abort();
  }
}

struct Cmd {
  uint8_t value{0U};
};
struct Rep {
  uint16_t value{0U};
};

constexpr std::size_t kDepth = 4U;
using Channel = kb::handlers::RequestChannel<Cmd, Rep, kDepth>;

// What the model knows of each slot.
enum class M : uint8_t { Free, Posted, Running, Done, GoneQueued, GoneRunning };

struct Model {
  std::array<M, kDepth> state{};
  std::array<uint8_t, kDepth> cmd{};
  std::array<bool, kDepth> failed{};
  [[nodiscard]] std::size_t InUse() const {
    std::size_t n = 0U;
    for (const M s : state) {
      n += (s != M::Free) ? 1U : 0U;
    }
    return n;
  }
};

uint16_t ReplyOf(uint8_t c) {
  return static_cast<uint16_t>(c + 1000U);
}

void Post(Channel& ch, Model& m, uint8_t value) {
  Cmd c{};
  c.value = value;
  const bool full = m.InUse() == kDepth;
  const kb::Result<uint32_t> slot = ch.Post(kb::handlers::RequestKind::Command, c);
  Require(slot.IsOk() == !full);
  if (slot.IsOk()) {
    Require(m.state[slot.Value()] == M::Free);
    m.state[slot.Value()] = M::Posted;
    m.cmd[slot.Value()] = value;
  } else {
    Require(slot.Error() == kb::Status::Busy);
  }
}

// The owner takes the next request: abandoned queued ones are freed on the way.
bool Take(Channel& ch, Model& m, kb::handlers::TakenRequest<Cmd>& out) {
  const bool got = ch.Next(out);
  if (got) {
    Require(m.state[out.slot] == M::Posted);
    Require(out.command.value == m.cmd[out.slot]);
    m.state[out.slot] = M::Running;
  }
  for (std::size_t i = 0U; i < kDepth; ++i) {
    if ((m.state[i] == M::GoneQueued) &&
        (ch.StateOf(static_cast<uint32_t>(i)) == kb::handlers::SlotState::Free)) {
      m.state[i] = M::Free;
    }
  }
  return got;
}

void Complete(Channel& ch, Model& m, uint32_t slot, bool fail) {
  kb::Result<Rep> r = kb::Result<Rep>::Failure(kb::Status::Fault, 7U);
  if (!fail) {
    Rep rep{};
    rep.value = ReplyOf(m.cmd[slot]);
    r = kb::Result<Rep>::Success(rep);
  }
  ch.Complete(slot, r);
  if (m.state[slot] == M::Running) {
    m.state[slot] = M::Done;
    m.failed[slot] = fail;
  } else {
    Require(m.state[slot] == M::GoneRunning);
    m.state[slot] = M::Free;
  }
}

void Wait(Channel& ch, Model& m, uint32_t slot) {
  const kb::Result<Rep> r = ch.Wait(slot, kb::Milliseconds{0U});
  const M s = m.state[slot];
  if (s == M::Done) {
    Require(r.IsOk() == !m.failed[slot]);
    if (r.IsOk()) {
      Require(r.Value().value == ReplyOf(m.cmd[slot]));
    } else {
      Require((r.Error() == kb::Status::Fault) && (r.Detail() == 7U));
    }
    m.state[slot] = M::Free;
  } else {
    Require(r.Error() == kb::Status::Timeout);
    Require((s == M::Posted) || (s == M::Running));
    m.state[slot] = (s == M::Posted) ? M::GoneQueued : M::GoneRunning;
  }
}

// A slot the caller still waits on (Posted, Running or Done), the n-th one found.
bool Waiting(const Model& m, uint8_t n, uint32_t& slot) {
  bool found = false;
  for (std::size_t k = 0U; k < kDepth; ++k) {
    const std::size_t i = (k + n) % kDepth;
    const M s = m.state[i];
    if ((!found) && ((s == M::Posted) || (s == M::Running) || (s == M::Done))) {
      slot = static_cast<uint32_t>(i);
      found = true;
    }
  }
  return found;
}

bool RunningSlot(const Model& m, uint8_t n, uint32_t& slot) {
  bool found = false;
  for (std::size_t k = 0U; k < kDepth; ++k) {
    const std::size_t i = (k + n) % kDepth;
    if ((!found) && ((m.state[i] == M::Running) || (m.state[i] == M::GoneRunning))) {
      slot = static_cast<uint32_t>(i);
      found = true;
    }
  }
  return found;
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, std::size_t size) {
  static Channel* channel = nullptr;
  static Channel storage{};
  if (channel == nullptr) {
    Require(storage.Create().IsOk());
    channel = &storage;
  }
  // Every input ends with all slots free (checked below), so each one starts from a free channel.
  Model m{};
  for (std::size_t i = 0U; i < kDepth; ++i) {
    Require(channel->StateOf(static_cast<uint32_t>(i)) == kb::handlers::SlotState::Free);
  }
  Channel& ch = *channel;
  for (std::size_t i = 0U; (i + 1U) < size; i += 2U) {
    const auto op = static_cast<uint8_t>(data[i] % 5U);
    const uint8_t arg = data[i + 1U];
    uint32_t slot = 0U;
    kb::handlers::TakenRequest<Cmd> taken{};
    switch (op) {
    case 0U:
      Post(ch, m, arg);
      break;
    case 1U:
      if (Take(ch, m, taken)) {
        Complete(ch, m, taken.slot, (arg & 1U) != 0U);
      }
      break;
    case 2U:
      static_cast<void>(Take(ch, m, taken)); // left running: completed later
      break;
    case 3U:
      if (RunningSlot(m, arg, slot)) {
        Complete(ch, m, slot, (arg & 1U) != 0U);
      }
      break;
    default:
      if (Waiting(m, arg, slot)) {
        Wait(ch, m, slot);
      }
      break;
    }
  }
  // Finish everything: the owner completes all it took, callers collect or give up, the owner skips
  // the rest.
  uint32_t slot = 0U;
  while (RunningSlot(m, 0U, slot)) {
    Complete(ch, m, slot, false);
  }
  while (Waiting(m, 0U, slot)) {
    Wait(ch, m, slot);
  }
  kb::handlers::TakenRequest<Cmd> taken{};
  while (Take(ch, m, taken)) {
    Complete(ch, m, taken.slot, false);
  }
  while (Waiting(m, 0U, slot)) {
    Wait(ch, m, slot);
  }
  while (RunningSlot(m, 0U, slot)) {
    Complete(ch, m, slot, false);
  }
  for (std::size_t i = 0U; i < kDepth; ++i) {
    Require(ch.StateOf(static_cast<uint32_t>(i)) == kb::handlers::SlotState::Free);
    Require(m.state[i] == M::Free);
  }
  return 0;
}
