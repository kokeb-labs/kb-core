// kb::handlers::RequestChannel on the none backend: every slot state transition (post, take,
// complete, wait, abandon while posted or running), Busy and InvalidArgument, the counters, and a
// completion that arrives while the caller waits (a fake-time timer plays the owner task).
// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The kb-core authors
#include <cstdint>

#include "handler_test_support.hpp"
#include "kb/rtos/time.hpp"
#include "kb_test.hpp"

using kb::Milliseconds;
using kb::Status;
using kbt::Command;
using kbt::Reply;
namespace h = kb::handlers;

namespace {

using Channel = h::RequestChannel<Command, Reply, 2U>;

kb::Result<Reply> Ok(uint32_t value) noexcept {
  return kb::Result<Reply>::Success(Reply{value});
}

/// Takes the next request and completes it with op + arg, from the fake-time timer.
struct Owner {
  Channel* channel{nullptr};
  uint32_t completed{0U};
  static void Run(void* context) noexcept {
    auto* self = static_cast<Owner*>(context);
    h::TakenRequest<Command> taken{};
    if (self->channel->Next(taken)) {
      self->channel->Complete(taken.slot, Ok(taken.command.op + taken.command.arg));
      ++self->completed;
    }
  }
};

} // namespace

KB_TEST("channel: Create once; a second Create is AlreadyStarted") {
  Channel ch{};
  KB_CHECK(ch.Create().IsOk());
  KB_CHECK_EQ(ch.Create().Error(), Status::AlreadyStarted);
  KB_CHECK_EQ(Channel::Capacity(), 2U);
  KB_CHECK_EQ((h::RequestChannel<Command, Reply, 1U>::Capacity()), 1U);
  KB_CHECK_EQ((h::RequestChannel<Command, Reply, 24U>::Capacity()), 24U);
}

KB_TEST("channel: without Create every post is refused Busy and no slot is kept") {
  Channel ch{};
  const kb::Result<uint32_t> slot = ch.Post(h::RequestKind::Command, Command{.op = 1U, .arg = 2U});
  KB_CHECK_EQ(slot.Error(), Status::Busy);
  KB_CHECK_EQ(ch.StateOf(0U), h::SlotState::Free);
  KB_CHECK_EQ(ch.StateOf(1U), h::SlotState::Free);
  const h::ChannelStats st = ch.Stats();
  KB_CHECK_EQ(st.posted, 0U);
  KB_CHECK_EQ(st.busy, 1U);
  h::TakenRequest<Command> taken{};
  KB_CHECK(!ch.Next(taken));
}

KB_TEST("channel: post, take, complete and wait deliver the reply and free the slot") {
  Channel ch{};
  KB_REQUIRE(ch.Create().IsOk());
  const kb::Result<uint32_t> slot = ch.Post(h::RequestKind::Sample, Command{.op = 3U, .arg = 4U});
  KB_REQUIRE(slot.IsOk());
  KB_CHECK_EQ(slot.Value(), 0U);
  KB_CHECK_EQ(ch.StateOf(0U), h::SlotState::Posted);
  h::TakenRequest<Command> taken{};
  KB_REQUIRE(ch.Next(taken));
  KB_CHECK_EQ(taken.slot, 0U);
  KB_CHECK_EQ(taken.kind, h::RequestKind::Sample);
  KB_CHECK_EQ(taken.command.op, 3U);
  KB_CHECK_EQ(taken.command.arg, 4U);
  KB_CHECK_EQ(ch.StateOf(0U), h::SlotState::Running);
  KB_CHECK(!ch.Next(taken)); // nothing else waiting
  ch.Complete(0U, Ok(9U));
  KB_CHECK_EQ(ch.StateOf(0U), h::SlotState::Done);
  const kb::Instant before = kb::rtos::Now();
  const kb::Result<Reply> reply = ch.Wait(0U, Milliseconds{10U});
  KB_REQUIRE(reply.IsOk());
  KB_CHECK_EQ(reply.Value().value, 9U);
  KB_CHECK(kb::rtos::Now() == before); // completed: no wait
  KB_CHECK_EQ(ch.StateOf(0U), h::SlotState::Free);
  const h::ChannelStats st = ch.Stats();
  KB_CHECK_EQ(st.posted, 1U);
  KB_CHECK_EQ(st.completed, 1U);
  KB_CHECK_EQ(st.timeouts, 0U);
  KB_CHECK_EQ(st.skipped, 0U);
  KB_CHECK_EQ(st.busy, 0U);
}

KB_TEST("channel: a failure reply carries its status and detail") {
  Channel ch{};
  KB_REQUIRE(ch.Create().IsOk());
  const kb::Result<uint32_t> slot = ch.Post(h::RequestKind::Command, Command{});
  KB_REQUIRE(slot.IsOk());
  h::TakenRequest<Command> taken{};
  KB_REQUIRE(ch.Next(taken));
  ch.Complete(taken.slot, kb::Result<Reply>::Failure(Status::Nack, 12U));
  const kb::Result<Reply> reply = ch.Wait(slot.Value(), Milliseconds{1U});
  KB_CHECK_EQ(reply.Error(), Status::Nack);
  KB_CHECK_EQ(reply.Detail(), 12U);
  KB_CHECK_EQ(ch.Stats().completed, 1U);
}

KB_TEST("channel: Busy when every slot is in use; requests are taken in posting order") {
  Channel ch{};
  KB_REQUIRE(ch.Create().IsOk());
  const kb::Result<uint32_t> a = ch.Post(h::RequestKind::Command, Command{.op = 1U, .arg = 0U});
  const kb::Result<uint32_t> b = ch.Post(h::RequestKind::Stop, Command{.op = 2U, .arg = 0U});
  KB_REQUIRE(a.IsOk() && b.IsOk());
  KB_CHECK_EQ(b.Value(), 1U);
  KB_CHECK_EQ(ch.Post(h::RequestKind::Command, Command{}).Error(), Status::Busy);
  KB_CHECK_EQ(ch.Stats().busy, 1U);
  KB_CHECK_EQ(ch.Stats().posted, 2U);
  h::TakenRequest<Command> taken{};
  KB_REQUIRE(ch.Next(taken));
  KB_CHECK_EQ(taken.command.op, 1U);
  KB_REQUIRE(ch.Next(taken));
  KB_CHECK_EQ(taken.command.op, 2U);
  KB_CHECK_EQ(taken.kind, h::RequestKind::Stop);
  // Slot 0 done and read: free for the next post while slot 1 still runs.
  ch.Complete(0U, Ok(1U));
  KB_CHECK(ch.Wait(0U, Milliseconds{1U}).IsOk());
  const kb::Result<uint32_t> c = ch.Post(h::RequestKind::Command, Command{});
  KB_REQUIRE(c.IsOk());
  KB_CHECK_EQ(c.Value(), 0U);
}

KB_TEST("channel: a bad slot is InvalidArgument for Wait, ignored by Complete, Free for StateOf") {
  Channel ch{};
  KB_REQUIRE(ch.Create().IsOk());
  KB_CHECK_EQ(ch.Wait(2U, Milliseconds{1U}).Error(), Status::InvalidArgument);
  KB_CHECK_EQ(ch.Wait(0xFFFFFFFFU, Milliseconds{1U}).Error(), Status::InvalidArgument);
  ch.Complete(2U, Ok(1U));
  KB_CHECK_EQ(ch.StateOf(2U), h::SlotState::Free);
  const h::ChannelStats st = ch.Stats();
  KB_CHECK_EQ(st.timeouts + st.completed, 0U);
}

KB_TEST("channel: abandoned while Posted, nothing runs; the owner skips and frees it") {
  Channel ch{};
  KB_REQUIRE(ch.Create().IsOk());
  const kb::Result<uint32_t> a = ch.Post(h::RequestKind::Command, Command{.op = 1U, .arg = 0U});
  KB_REQUIRE(a.IsOk());
  const kb::Instant before = kb::rtos::Now();
  KB_CHECK_EQ(ch.Wait(a.Value(), Milliseconds{2U}).Error(), Status::Timeout);
  KB_CHECK(kb::utils::ElapsedUs(kb::rtos::Now(), before) >= 2000U);
  KB_CHECK_EQ(ch.StateOf(a.Value()), h::SlotState::Abandoned);
  KB_CHECK_EQ(ch.Stats().timeouts, 1U);
  const kb::Result<uint32_t> b = ch.Post(h::RequestKind::Command, Command{.op = 2U, .arg = 0U});
  KB_REQUIRE(b.IsOk());
  KB_CHECK_EQ(b.Value(), 1U); // the abandoned slot is not reused before the owner frees it
  h::TakenRequest<Command> taken{};
  KB_REQUIRE(ch.Next(taken)); // skips the abandoned request, takes the next one
  KB_CHECK_EQ(taken.command.op, 2U);
  KB_CHECK_EQ(ch.StateOf(a.Value()), h::SlotState::Free);
  KB_CHECK_EQ(ch.Stats().skipped, 1U);

  const kb::Result<uint32_t> c = ch.Post(h::RequestKind::Command, Command{.op = 3U, .arg = 0U});
  KB_REQUIRE(c.IsOk());
  KB_CHECK_EQ(ch.Wait(c.Value(), Milliseconds{0U}).Error(), Status::Timeout);
  KB_CHECK(!ch.Next(taken)); // only an abandoned request was queued
  KB_CHECK_EQ(ch.Stats().skipped, 2U);
  KB_CHECK_EQ(ch.StateOf(c.Value()), h::SlotState::Free);
}

KB_TEST("channel: abandoned while Running, the late completion frees the slot without a reply") {
  Channel ch{};
  KB_REQUIRE(ch.Create().IsOk());
  const kb::Result<uint32_t> a = ch.Post(h::RequestKind::Command, Command{});
  KB_REQUIRE(a.IsOk());
  h::TakenRequest<Command> taken{};
  KB_REQUIRE(ch.Next(taken));
  KB_CHECK_EQ(ch.Wait(a.Value(), Milliseconds{1U}).Error(), Status::Timeout);
  KB_CHECK_EQ(ch.StateOf(a.Value()), h::SlotState::Abandoned);
  ch.Complete(taken.slot, Ok(5U));
  KB_CHECK_EQ(ch.StateOf(a.Value()), h::SlotState::Free);
  const h::ChannelStats st = ch.Stats();
  KB_CHECK_EQ(st.completed, 0U);
  KB_CHECK_EQ(st.timeouts, 1U);
  KB_CHECK_EQ(st.skipped, 0U);
  // The slot is reused normally afterwards.
  const kb::Result<uint32_t> b = ch.Post(h::RequestKind::Command, Command{});
  KB_REQUIRE(b.IsOk());
  KB_CHECK_EQ(b.Value(), a.Value());
}

KB_TEST("channel: a completion that arrives while the caller waits is delivered") {
  Channel ch{};
  KB_REQUIRE(ch.Create().IsOk());
  Owner owner{};
  owner.channel = &ch;
  const kbt::TickerScope ticker{&Owner::Run, &owner};
  const kb::Result<uint32_t> a = ch.Post(h::RequestKind::Command, Command{.op = 40U, .arg = 2U});
  KB_REQUIRE(a.IsOk());
  const kb::Result<Reply> reply = ch.Wait(a.Value(), Milliseconds{5U});
  KB_REQUIRE(reply.IsOk());
  KB_CHECK_EQ(reply.Value().value, 42U);
  KB_CHECK_EQ(owner.completed, 1U);
  KB_CHECK_EQ(ch.StateOf(a.Value()), h::SlotState::Free);
  KB_CHECK_EQ(ch.Stats().completed, 1U);
  KB_CHECK_EQ(ch.Stats().timeouts, 0U);
}

KB_TEST("channel: waiting on a slot that was never posted gives Fault and leaves it Free") {
  Channel ch{};
  KB_REQUIRE(ch.Create().IsOk());
  KB_CHECK_EQ(ch.Wait(1U, Milliseconds{1U}).Error(), Status::Fault);
  KB_CHECK_EQ(ch.StateOf(1U), h::SlotState::Free);
  const h::ChannelStats st = ch.Stats();
  KB_CHECK_EQ(st.timeouts, 0U);
  KB_CHECK_EQ(st.completed, 0U);
}
