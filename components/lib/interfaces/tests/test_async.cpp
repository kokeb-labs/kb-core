// The asynchronous contract (ADR-0016): AsyncPoll, AsyncFromSync over the I2C and SPI fakes
// (start, one completion, Busy, an expired deadline, a late completion, cancel), and the host
// asynchronous fakes with every injected fault: latency in polls, a hang ended by the deadline, a
// NACK, a completion after the deadline, the cancel race, buffers untouched until completion.
// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The kb-core authors
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "kb/interfaces/host/host.hpp"
#include "kb/interfaces/interfaces.hpp"
#include "kb_test.hpp"

namespace h = kb::iface::host;
namespace i = kb::iface;
using kb::Instant;
using kb::Microseconds;
using kb::Status;
using kb::utils::Deadline;
using kb_test::Rt;

namespace {

/// A clock the test moves; `step_us` passes on every read (a transfer that takes time).
struct StepClock {
  mutable uint64_t us{1000U};
  uint64_t step_us{0U};
  [[nodiscard]] Instant Now() const noexcept {
    const Instant now{us};
    us += step_us;
    return now;
  }
};
static_assert(i::MonotonicClock<StepClock>);

using SyncI2c = i::AsyncFromSync<h::FakeI2cBus, StepClock>;
using SyncSpi = i::AsyncFromSync<h::FakeSpiDevice, StepClock>;
static_assert(i::I2cAsync<SyncI2c> && i::I2cBus<SyncI2c>);
static_assert(i::SpiAsync<SyncSpi> && i::SpiDevice<SyncSpi>);
static_assert(!i::NonBlocking<SyncI2c> && !i::NonBlocking<SyncSpi>);
static_assert(i::NonBlocking<h::FakeAsyncI2c> && i::NonBlocking<h::FakeAsyncSpi>);
static_assert(!i::I2cAsync<h::FakeI2cBus> && !i::SpiAsync<h::FakeSpiDevice>);
static_assert(!i::I2cAsync<h::FakeAsyncSpi> && !i::SpiAsync<h::FakeAsyncI2c>);

/// A type with Poll and Cancel but the wrong flag type is still an operation; one without the flag
/// is not.
struct NoFlag {
  [[nodiscard]] static i::AsyncPoll Poll(Instant now) noexcept {
    static_cast<void>(now);
    return i::AsyncPoll{};
  }
  [[nodiscard]] static Status Cancel() noexcept {
    return Status::Ok;
  }
};
static_assert(!i::AsyncOperation<NoFlag>);

Deadline After(uint64_t now_us, uint32_t us) {
  return Deadline::After(Instant{now_us}, Microseconds{us});
}

/// An I2C fake with one device at 0x40 whose register 0x10 holds 0xA5, 0x5A.
struct I2cRig {
  h::FakeI2cBus bus{};
  I2cRig() {
    static_cast<void>(bus.AddDevice(0x40U));
    static_cast<void>(bus.EnsureReady());
    bus.Device(0x40U)->regs[0x10U] = 0xA5U;
    bus.Device(0x40U)->regs[0x11U] = 0x5AU;
  }
};

} // namespace

// The state values are part of the contract every port and log shares.
static_assert((static_cast<uint8_t>(i::AsyncState::Idle) == 0U) &&
              (static_cast<uint8_t>(i::AsyncState::Pending) == 1U) &&
              (static_cast<uint8_t>(i::AsyncState::Done) == 2U));

KB_TEST("async poll: a default poll is Idle with no outcome") {
  const i::AsyncPoll d{};
  KB_CHECK(d.state == i::AsyncState::Idle);
  KB_CHECK_EQ(d.status, Status::NotReady);
  KB_CHECK_EQ(d.detail, 0U);
  KB_CHECK_EQ(d.count, 0U);
  const i::AsyncPoll pending = i::AsyncPoll::InState(i::AsyncState::Pending);
  KB_CHECK_EQ(pending.detail, 0U);
  KB_CHECK_EQ(pending.count, 0U);
}

KB_TEST("async poll: states, completion results and the count kept only on success") {
  const i::AsyncPoll idle = i::AsyncPoll::InState(i::AsyncState::Idle);
  KB_CHECK(!idle.IsPending());
  KB_CHECK_EQ(idle.Completion().Error(), Status::NotReady);
  const i::AsyncPoll pending = i::AsyncPoll::InState(i::AsyncState::Pending);
  KB_CHECK(pending.IsPending());
  KB_CHECK_EQ(pending.Completion().Error(), Status::NotReady);
  const i::AsyncPoll ok = i::AsyncPoll::Completed(Status::Ok, Rt<std::size_t>(3U));
  KB_CHECK(!ok.IsPending());
  KB_REQUIRE(ok.Completion().IsOk());
  KB_CHECK_EQ(ok.Completion().Value(), 3U);
  KB_CHECK_EQ(ok.detail, 0U);
  const i::AsyncPoll nack = i::AsyncPoll::Completed(Status::Nack, Rt<std::size_t>(3U), 0x107U);
  KB_CHECK_EQ(nack.count, 0U);
  KB_CHECK_EQ(nack.Completion().Error(), Status::Nack);
  KB_CHECK_EQ(nack.Completion().Detail(), 0x107U);
}

KB_TEST("async from sync, I2C: the transfer runs in Start, Poll reports it once") {
  I2cRig rig{};
  StepClock clock{};
  SyncI2c a{rig.bus, clock};
  KB_CHECK_EQ(a.EnsureReady(), Status::Ok);
  const std::array<uint8_t, 1> reg{0x10U};
  std::array<uint8_t, 2> rx{};
  KB_CHECK(a.Poll(Instant{1000U}).state == i::AsyncState::Idle);
  KB_CHECK_EQ(a.StartWriteRead(0x40U, reg, rx, After(1000U, 500U)), Status::Ok);
  KB_CHECK_EQ(rx[0], 0xA5U);                                             // the transfer already ran
  KB_CHECK_EQ(a.StartRead(0x40U, rx, After(1000U, 500U)), Status::Busy); // not yet reported
  const i::AsyncPoll p = a.Poll(Instant{1001U});
  KB_CHECK(p.state == i::AsyncState::Done);
  KB_CHECK_EQ(p.Completion().Value(), 2U);
  KB_CHECK(a.Poll(Instant{1002U}).state == i::AsyncState::Idle);

  const std::array<uint8_t, 2> w{0x20U, 0x77U};
  KB_CHECK_EQ(a.StartWrite(0x40U, w, After(1000U, 500U)), Status::Ok);
  KB_CHECK_EQ(a.Poll(Instant{1003U}).Completion().Value(), 2U);
  KB_CHECK_EQ(rig.bus.Device(0x40U)->regs[0x20U], 0x77U);
  std::array<uint8_t, 1> one{};
  KB_CHECK_EQ(a.StartRead(0x40U, one, After(1000U, 500U)), Status::Ok);
  KB_CHECK_EQ(a.Poll(Instant{1004U}).Completion().Value(), 1U);
  KB_CHECK_EQ(a.StartRead(0x41U, one, After(1000U, 500U)), Status::Ok); // nobody at 0x41
  KB_CHECK_EQ(a.Poll(Instant{1005U}).Completion().Error(), Status::NoDevice);
  KB_CHECK(a.LastFailure() == i::I2cFailure::AddressNack);
  KB_CHECK_EQ(a.Write(0x40U, w), Status::Ok);
  KB_CHECK_EQ(a.WriteRead(0x40U, reg, rx), Status::Ok);
  KB_CHECK_EQ(a.Read(0x40U, rx), Status::Ok);
}

KB_TEST("async from sync: an expired deadline starts nothing; a late transfer is Timeout") {
  I2cRig rig{};
  StepClock clock{};
  SyncI2c a{rig.bus, clock};
  const std::array<uint8_t, 2> w{0x20U, 0x01U};
  const uint32_t before = rig.bus.Transfers();
  KB_CHECK_EQ(a.StartWrite(0x40U, w, After(0U, 1000U)), Status::Timeout); // now 1000: due at 1000
  KB_CHECK_EQ(rig.bus.Transfers(), before);
  KB_CHECK(a.Poll(Instant{1000U}).state == i::AsyncState::Idle);
  clock.step_us = 600U; // the transfer takes 600 us
  KB_CHECK_EQ(a.StartWrite(0x40U, w, After(clock.us, 500U)), Status::Ok);
  KB_CHECK_EQ(a.Poll(Instant{0U}).Completion().Error(), Status::Timeout);
  // cppcheck-suppress redundantAssignment
  clock.step_us = 400U;
  KB_CHECK_EQ(a.StartWrite(0x40U, w, After(clock.us, 500U)), Status::Ok);
  KB_CHECK(a.Poll(Instant{0U}).Completion().IsOk()); // within its deadline
  // cppcheck-suppress redundantAssignment
  // cppcheck-suppress unreadVariable
  clock.step_us = 500U; // ends exactly at the deadline: expired
  KB_CHECK_EQ(a.StartWrite(0x40U, w, After(clock.us, 500U)), Status::Ok);
  KB_CHECK_EQ(a.Poll(Instant{0U}).Completion().Error(), Status::Timeout);
}

KB_TEST("async from sync: Cancel drops a completion not yet reported") {
  I2cRig rig{};
  StepClock clock{};
  SyncI2c a{rig.bus, clock};
  const std::array<uint8_t, 2> w{0x20U, 0x01U};
  KB_CHECK_EQ(a.Cancel(), Status::Ok); // nothing in flight
  KB_CHECK_EQ(a.StartWrite(0x40U, w, After(1000U, 500U)), Status::Ok);
  KB_CHECK_EQ(a.Cancel(), Status::Ok);
  KB_CHECK(a.Poll(Instant{1001U}).state == i::AsyncState::Idle);
  KB_CHECK_EQ(a.StartWrite(0x40U, w, After(1000U, 500U)), Status::Ok); // free again
}

KB_TEST("async from sync, SPI: full duplex in Start; the count is the longer span") {
  h::FakeSpiDevice dev{};
  StepClock clock{};
  SyncSpi a{dev, clock};
  KB_CHECK_EQ(a.EnsureReady(), Status::Ok);
  const std::array<uint8_t, 3> answer{1U, 2U, 3U};
  KB_REQUIRE(dev.Respond(answer));
  const std::array<uint8_t, 3> tx{9U, 8U, 7U};
  std::array<uint8_t, 3> rx{};
  KB_CHECK_EQ(a.StartTransfer(tx, rx, After(1000U, 500U)), Status::Ok);
  KB_CHECK_EQ(a.Poll(Instant{1000U}).Completion().Value(), 3U);
  KB_CHECK(rx == answer);
  KB_CHECK_EQ(a.StartTransfer({}, rx, After(1000U, 500U)), Status::Ok); // read only
  KB_CHECK_EQ(a.Poll(Instant{1000U}).Completion().Value(), 3U);
  KB_CHECK_EQ(a.StartTransfer(tx, {}, After(1000U, 500U)), Status::Ok); // write only
  KB_CHECK_EQ(a.Poll(Instant{1000U}).Completion().Value(), 3U);
  KB_CHECK_EQ(a.Transfer(tx, rx), Status::Ok);
  dev.FailNext(Status::BusError);
  KB_CHECK_EQ(a.StartTransfer(tx, rx, After(1000U, 500U)), Status::Ok);
  KB_CHECK_EQ(a.Poll(Instant{1000U}).Completion().Error(), Status::BusError);
}

KB_TEST("fake async I2C: completes on the n-th poll; buffers untouched until then") {
  I2cRig rig{};
  Instant now{1000U};
  h::FakeAsyncI2c a{rig.bus, now};
  KB_CHECK_EQ(a.EnsureReady(), Status::Ok);
  a.Script().CompleteAfterPolls(3U);
  const std::array<uint8_t, 1> reg{0x10U};
  std::array<uint8_t, 2> rx{0xEEU, 0xEEU};
  KB_CHECK_EQ(a.StartWriteRead(0x40U, reg, rx, After(1000U, 900U)), Status::Ok);
  KB_CHECK(a.Script().InFlight());
  KB_CHECK_EQ(a.StartRead(0x40U, rx, After(1000U, 900U)), Status::Busy);
  KB_CHECK(a.Poll(Instant{1100U}).IsPending());
  KB_CHECK(a.Poll(Instant{1200U}).IsPending());
  KB_CHECK_EQ(rx[0], 0xEEU);
  const i::AsyncPoll p = a.Poll(Instant{1300U});
  KB_CHECK_EQ(p.Completion().Value(), 2U);
  KB_CHECK_EQ(rx[0], 0xA5U);
  KB_CHECK_EQ(rx[1], 0x5AU);
  KB_CHECK(a.Poll(Instant{1400U}).state == i::AsyncState::Idle);
  const h::AsyncCounters& c = a.Script().Counters();
  KB_CHECK_EQ(c.started, 1U);
  KB_CHECK_EQ(c.refused, 1U);
  KB_CHECK_EQ(c.completed, 1U);
  KB_CHECK_EQ(c.polls, 4U);
  a.Script().CompleteAfterPolls(0U); // taken as 1
  const std::array<uint8_t, 2> w{0x30U, 0x42U};
  KB_CHECK_EQ(a.StartWrite(0x40U, w, After(1000U, 900U)), Status::Ok);
  KB_CHECK_EQ(a.Poll(Instant{1001U}).Completion().Value(), 2U);
  KB_CHECK_EQ(rig.bus.Device(0x40U)->regs[0x30U], 0x42U);
  std::array<uint8_t, 1> one{};
  KB_CHECK_EQ(a.StartRead(0x40U, one, After(1000U, 900U)), Status::Ok);
  KB_CHECK_EQ(a.Poll(Instant{1001U}).Completion().Value(), 1U);
  KB_CHECK_EQ(a.Write(0x40U, w), Status::Ok);
  KB_CHECK_EQ(a.WriteRead(0x40U, reg, rx), Status::Ok);
  KB_CHECK_EQ(a.Read(0x40U, rx), Status::Ok);
  KB_CHECK(a.LastFailure() == i::I2cFailure::None);
}

KB_TEST("fake async I2C: refusals: an expired deadline, a bad address; NACK as the completion") {
  I2cRig rig{};
  Instant now{5000U};
  h::FakeAsyncI2c a{rig.bus, now};
  const std::array<uint8_t, 1> reg{0x10U};
  std::array<uint8_t, 2> rx{};
  KB_CHECK_EQ(a.StartWriteRead(0x40U, reg, rx, After(1000U, 4000U)), Status::Timeout);
  KB_CHECK_EQ(a.StartWriteRead(0x07U, reg, rx, After(5000U, 100U)), Status::InvalidArgument);
  KB_CHECK_EQ(a.StartWriteRead(0x78U, reg, rx, After(5000U, 100U)), Status::InvalidArgument);
  KB_CHECK_EQ(a.StartWriteRead(0x08U, reg, rx, After(5000U, 100U)), Status::Ok); // lowest valid
  KB_CHECK_EQ(a.Poll(Instant{5001U}).Completion().Error(), Status::NoDevice);
  KB_CHECK_EQ(a.StartWriteRead(0x77U, reg, rx, After(5000U, 100U)), Status::Ok); // highest
  KB_CHECK_EQ(a.Poll(Instant{5001U}).Completion().Error(), Status::NoDevice);
  rig.bus.FailNext(i::I2cFailure::DataNack);
  KB_CHECK_EQ(a.StartWriteRead(0x40U, reg, rx, After(5000U, 100U)), Status::Ok);
  const i::AsyncPoll nack = a.Poll(Instant{5001U});
  KB_CHECK_EQ(nack.Completion().Error(), Status::Nack);
  KB_CHECK_EQ(nack.detail, static_cast<uint32_t>(Status::Nack));
  KB_CHECK_EQ(a.Script().Counters().refused, 3U);
}

KB_TEST("fake async: a hang ends at the deadline as Timeout, without touching the buffers") {
  I2cRig rig{};
  Instant now{0U};
  h::FakeAsyncI2c a{rig.bus, now};
  a.Script().Hang(true);
  const std::array<uint8_t, 1> reg{0x10U};
  std::array<uint8_t, 2> rx{0xEEU, 0xEEU};
  KB_CHECK_EQ(a.StartWriteRead(0x40U, reg, rx, After(0U, 1000U)), Status::Ok);
  KB_CHECK(a.Poll(Instant{999U}).IsPending());
  const i::AsyncPoll p = a.Poll(Instant{1000U});
  KB_CHECK_EQ(p.Completion().Error(), Status::Timeout);
  KB_CHECK_EQ(rx[0], 0xEEU);
  KB_CHECK_EQ(a.Script().Counters().timeouts, 1U);
  KB_CHECK_EQ(a.Script().Counters().late, 0U);
  KB_CHECK(!a.Script().InFlight());
  a.Script().Hang(false);
}

KB_TEST("fake async: a completion after the deadline is Timeout, never Ok") {
  I2cRig rig{};
  Instant now{0U};
  h::FakeAsyncI2c a{rig.bus, now};
  a.Script().CompleteLate(true);
  const std::array<uint8_t, 1> reg{0x10U};
  std::array<uint8_t, 2> rx{};
  KB_CHECK_EQ(a.StartWriteRead(0x40U, reg, rx, After(0U, 1000U)), Status::Ok);
  KB_CHECK(a.Poll(Instant{500U}).IsPending()); // not before the deadline
  const i::AsyncPoll p = a.Poll(Instant{1500U});
  KB_CHECK_EQ(p.Completion().Error(), Status::Timeout);
  KB_CHECK_EQ(p.detail, static_cast<uint32_t>(Status::Ok)); // the transfer itself worked
  KB_CHECK_EQ(a.Script().Counters().late, 1U);
  KB_CHECK_EQ(a.Script().Counters().timeouts, 1U);
}

KB_TEST("fake async: Cancel ends a transfer; a racing completion is dropped; Poll is Idle") {
  I2cRig rig{};
  Instant now{0U};
  h::FakeAsyncI2c a{rig.bus, now};
  a.Script().CompleteAfterPolls(5U);
  const std::array<uint8_t, 2> w{0x30U, 0x11U};
  KB_CHECK_EQ(a.Cancel(), Status::Ok); // nothing in flight: nothing counted
  KB_CHECK_EQ(a.Script().Counters().cancels, 0U);
  KB_CHECK_EQ(a.StartWrite(0x40U, w, After(0U, 1000U)), Status::Ok);
  KB_CHECK(a.Poll(Instant{1U}).IsPending());
  KB_CHECK_EQ(a.Cancel(), Status::Ok);
  KB_CHECK(a.Poll(Instant{2U}).state == i::AsyncState::Idle);
  KB_CHECK_EQ(rig.bus.Device(0x40U)->regs[0x30U], 0U); // never sent
  KB_CHECK_EQ(a.Script().Counters().cancels, 1U);

  a.Script().CompleteDuringCancel(true);
  KB_CHECK_EQ(a.StartWrite(0x40U, w, After(0U, 1000U)), Status::Ok);
  KB_CHECK_EQ(a.Cancel(), Status::Ok);
  KB_CHECK(a.Poll(Instant{3U}).state == i::AsyncState::Idle);
  KB_CHECK_EQ(rig.bus.Device(0x40U)->regs[0x30U], 0x11U); // it did complete on the wire
  KB_CHECK_EQ(a.Script().Counters().dropped, 1U);
  KB_CHECK_EQ(a.Script().Counters().completed, 0U);
}

KB_TEST("fake async SPI: frames complete on poll; bad spans refused; failures reported") {
  h::FakeSpiDevice dev{};
  Instant now{0U};
  h::FakeAsyncSpi a{dev, now};
  KB_CHECK_EQ(a.EnsureReady(), Status::Ok);
  const std::array<uint8_t, 2> answer{0x12U, 0x34U};
  KB_REQUIRE(dev.Respond(answer));
  const std::array<uint8_t, 2> tx{0x80U, 0x00U};
  std::array<uint8_t, 2> rx{};
  std::array<uint8_t, 3> wrong{};
  KB_CHECK_EQ(a.StartTransfer(tx, wrong, After(0U, 100U)), Status::InvalidArgument);
  KB_CHECK_EQ(a.StartTransfer(tx, rx, After(0U, 100U)), Status::Ok);
  KB_CHECK_EQ(rx[0], 0U);
  KB_CHECK_EQ(a.Poll(Instant{10U}).Completion().Value(), 2U);
  KB_CHECK(rx == answer);
  KB_CHECK_EQ(a.StartTransfer({}, rx, After(0U, 100U)), Status::Ok);
  KB_CHECK_EQ(a.Poll(Instant{10U}).Completion().Value(), 2U);
  KB_CHECK_EQ(a.StartTransfer(tx, {}, After(0U, 100U)), Status::Ok);
  KB_CHECK_EQ(a.Poll(Instant{10U}).Completion().Value(), 2U);
  dev.FailNext(Status::BusError);
  KB_CHECK_EQ(a.StartTransfer(tx, rx, After(0U, 100U)), Status::Ok);
  KB_CHECK_EQ(a.Poll(Instant{10U}).Completion().Error(), Status::BusError);
  a.Script().CompleteAfterPolls(4U);
  KB_CHECK_EQ(a.StartTransfer(tx, rx, After(0U, 100U)), Status::Ok);
  KB_CHECK_EQ(a.Cancel(), Status::Ok);
  KB_CHECK_EQ(a.Transfer(tx, rx), Status::Ok);
}
