// kb::handlers::AsyncPoller (ADR-0016) over the host asynchronous fakes and AsyncFromSync: starts,
// one non-blocking step per pass, completion once with the received bytes, refusals, deadlines,
// late completions, NACKs, cancel and the destructor's cancel, the counters; and an owner-pass
// access that samples a register through the poller, where no pass waits on the bus.
// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The kb-core authors
#include <array>
#include <cstdint>
#include <limits>
#include <span>

#include "handler_test_support.hpp"
#include "kb/handlers/common/async_poller.hpp"
#include "kb/rtos/none/control.hpp"
#include "kb/rtos/time.hpp"
#include "kb/utils/endian.hpp"
#include "kb_test.hpp"

using kb::Instant;
using kb::Microseconds;
using kb::Milliseconds;
using kb::Status;
namespace h = kb::handlers;
namespace host = kb::iface::host;
namespace i = kb::iface;

namespace {

/// An I2C fake with a sensor at 0x48 whose register 0x00 holds 0x12, 0x34.
struct Sensor {
  host::FakeI2cBus bus{};
  Instant now{1000U};
  host::FakeAsyncI2c async{bus, now};
  Sensor() {
    static_cast<void>(bus.AddDevice(0x48U));
    static_cast<void>(bus.EnsureReady());
    bus.Device(0x48U)->regs[0x00U] = 0x12U;
    bus.Device(0x48U)->regs[0x01U] = 0x34U;
  }
};

constexpr std::array<uint8_t, 1> kReg{0x00U};

using I2cPoller = h::AsyncPoller<host::FakeAsyncI2c, 4U>;
using SpiPoller = h::AsyncPoller<host::FakeAsyncSpi, 4U>;

/// kb::rtos::Now() as a clock (the none backend's fake time).
struct RtosClock {
  [[nodiscard]] static Instant Now() noexcept {
    return kb::rtos::Now();
  }
};

/// An owner-pass access: each pass starts a read when idle and publishes it when done.
struct Sampler {
  I2cPoller& poller;
  uint32_t published{0U};
  uint16_t last{0U};
  uint64_t longest_pass_us{0U};

  [[nodiscard]] bool Service(const h::ServiceKey& key, Instant now) noexcept {
    static_cast<void>(key);
    const Instant begin = kb::rtos::Now();
    if (!poller.InFlight()) {
      static_cast<void>(poller.StartWriteRead(0x48U, kReg, 2U, now, Microseconds{3000U}));
    }
    const i::AsyncPoll p = poller.Step(now);
    if (p.Completion().IsOk()) {
      const std::span<const uint8_t> b = poller.Received();
      last = kb::utils::LoadBe16(b.first<2U>());
      ++published;
    }
    const uint64_t spent = kb::utils::ElapsedUs(kb::rtos::Now(), begin);
    longest_pass_us = (spent > longest_pass_us) ? spent : longest_pass_us;
    return true;
  }
};

} // namespace

KB_TEST("async poller: start, pending steps, one completion with the bytes, then idle") {
  Sensor s{};
  s.async.Script().CompleteAfterPolls(2U);
  I2cPoller poller{s.async};
  KB_CHECK(!poller.InFlight());
  KB_CHECK(poller.Step(Instant{1000U}).state == i::AsyncState::Idle);
  KB_CHECK_EQ(s.async.Script().Counters().polls, 0U); // idle: the port is not polled
  KB_CHECK_EQ(poller.StartWriteRead(0x48U, kReg, 2U, Instant{1000U}, Microseconds{500U}),
              Status::Ok);
  KB_CHECK(poller.InFlight());
  KB_CHECK(poller.Received().empty());
  KB_CHECK_EQ(poller.StartWriteRead(0x48U, kReg, 2U, Instant{1000U}, Microseconds{500U}),
              Status::Busy);
  KB_CHECK(poller.Step(Instant{1100U}).IsPending());
  const i::AsyncPoll done = poller.Step(Instant{1250U});
  KB_CHECK_EQ(done.Completion().Value(), 2U);
  KB_CHECK(!poller.InFlight());
  KB_REQUIRE(poller.Received().size() == 2U);
  KB_CHECK_EQ(poller.Received()[0], 0x12U);
  KB_CHECK_EQ(poller.Received()[1], 0x34U);
  KB_CHECK(poller.Step(Instant{1300U}).state == i::AsyncState::Idle);
  const h::AsyncStats st = poller.Stats();
  KB_CHECK_EQ(st.started, 1U);
  KB_CHECK_EQ(st.refused, 1U);
  KB_CHECK_EQ(st.ok, 1U);
  KB_CHECK_EQ(st.max_latency_us, 250U);
  KB_CHECK_EQ(poller.Cancel(), Status::Ok); // nothing in flight
  KB_CHECK_EQ(poller.Stats().cancels, 0U);
}

KB_TEST("async poller: a start while one is in flight leaves the transfer's buffers alone") {
  Sensor s{};
  s.async.Script().CompleteAfterPolls(2U);
  I2cPoller poller{s.async};
  KB_REQUIRE(poller.StartWriteRead(0x48U, kReg, 2U, Instant{1000U}, Microseconds{500U}) ==
             Status::Ok);
  const std::array<uint8_t, 1> other{0x01U}; // another register: it must not replace the first
  KB_CHECK_EQ(poller.StartWriteRead(0x48U, other, 1U, Instant{1000U}, Microseconds{500U}),
              Status::Busy);
  KB_CHECK(poller.Step(Instant{1001U}).IsPending());
  KB_REQUIRE(poller.Step(Instant{1002U}).Completion().IsOk());
  KB_REQUIRE(poller.Received().size() == 2U);
  KB_CHECK_EQ(poller.Received()[0], 0x12U); // register 0x00, as started
  KB_CHECK_EQ(poller.Received()[1], 0x34U);
}

KB_TEST("async poller: lengths above the buffer are refused before the port sees them") {
  Sensor s{};
  I2cPoller poller{s.async};
  const std::array<uint8_t, 5> big{};
  KB_CHECK_EQ(poller.StartWriteRead(0x48U, big, 1U, Instant{1000U}, Microseconds{100U}),
              Status::InvalidArgument);
  KB_CHECK_EQ(poller.StartWriteRead(0x48U, kReg, 5U, Instant{1000U}, Microseconds{100U}),
              Status::InvalidArgument);
  KB_CHECK_EQ(poller.StartWriteRead(0x48U, kReg, 4U, Instant{1000U}, Microseconds{100U}),
              Status::Ok); // exactly the buffer
  KB_CHECK_EQ(s.async.Script().Counters().started, 1U);
  KB_CHECK_EQ(poller.Stats().refused, 2U);
}

KB_TEST("async poller: the port's refusal is counted and nothing is in flight") {
  Sensor s{};
  I2cPoller poller{s.async};
  KB_CHECK_EQ(poller.StartWriteRead(0x07U, kReg, 2U, Instant{1000U}, Microseconds{100U}),
              Status::InvalidArgument);
  KB_CHECK(!poller.InFlight());
  KB_CHECK_EQ(poller.Stats().refused, 1U);
}

KB_TEST("async poller: a hang ends at the deadline; a late completion and a NACK are failures") {
  Sensor s{};
  I2cPoller poller{s.async};
  s.async.Script().Hang(true);
  KB_REQUIRE(poller.StartWriteRead(0x48U, kReg, 2U, Instant{1000U}, Microseconds{500U}) ==
             Status::Ok);
  KB_CHECK(poller.Step(Instant{1499U}).IsPending());
  KB_CHECK_EQ(poller.Step(Instant{1500U}).Completion().Error(), Status::Timeout);
  KB_CHECK(poller.Received().empty());
  s.async.Script().Hang(false);
  s.async.Script().CompleteLate(true);
  s.now = Instant{2000U};
  KB_REQUIRE(poller.StartWriteRead(0x48U, kReg, 2U, Instant{2000U}, Microseconds{500U}) ==
             Status::Ok);
  KB_CHECK_EQ(poller.Step(Instant{2600U}).Completion().Error(), Status::Timeout);
  KB_CHECK(poller.Received().empty()); // late data is not received
  s.async.Script().CompleteLate(false);
  s.bus.FailNext(i::I2cFailure::DataNack);
  // cppcheck-suppress redundantAssignment
  // cppcheck-suppress unreadVariable
  s.now = Instant{3000U};
  KB_REQUIRE(poller.StartWriteRead(0x48U, kReg, 2U, Instant{3000U}, Microseconds{500U}) ==
             Status::Ok);
  KB_CHECK_EQ(poller.Step(Instant{3001U}).Completion().Error(), Status::Nack);
  const h::AsyncStats st = poller.Stats();
  KB_CHECK_EQ(st.timeouts, 2U);
  KB_CHECK_EQ(st.failed, 1U);
  KB_CHECK_EQ(st.ok, 0U);
  KB_CHECK_EQ(st.max_latency_us, 600U);
}

KB_TEST("async poller: a new transfer empties the received bytes until it completes") {
  Sensor s{};
  I2cPoller poller{s.async};
  KB_REQUIRE(poller.StartWriteRead(0x48U, kReg, 2U, Instant{1000U}, Microseconds{500U}) ==
             Status::Ok);
  KB_REQUIRE(poller.Step(Instant{1001U}).Completion().IsOk());
  KB_CHECK_EQ(poller.Received().size(), 2U);
  // cppcheck-suppress unreadVariable
  s.now = Instant{1002U};
  KB_REQUIRE(poller.StartWriteRead(0x48U, kReg, 1U, Instant{1002U}, Microseconds{500U}) ==
             Status::Ok);
  KB_CHECK(poller.Received().empty());
  KB_REQUIRE(poller.Step(Instant{1003U}).Completion().IsOk());
  KB_CHECK_EQ(poller.Received().size(), 1U);
}

KB_TEST("async poller: Cancel and the destructor end a transfer in flight") {
  Sensor s{};
  s.async.Script().CompleteAfterPolls(10U);
  {
    I2cPoller poller{s.async};
    KB_REQUIRE(poller.StartWriteRead(0x48U, kReg, 2U, Instant{1000U}, Microseconds{500U}) ==
               Status::Ok);
    KB_CHECK_EQ(poller.Cancel(), Status::Ok);
    KB_CHECK(!poller.InFlight());
    KB_CHECK_EQ(poller.Stats().cancels, 1U);
    KB_CHECK(poller.Step(Instant{1001U}).state == i::AsyncState::Idle);
    KB_REQUIRE(poller.StartWriteRead(0x48U, kReg, 2U, Instant{1000U}, Microseconds{500U}) ==
               Status::Ok);
  } // destroyed with a transfer in flight
  KB_CHECK(!s.async.Script().InFlight());
  KB_CHECK_EQ(s.async.Script().Counters().cancels, 2U);
}

KB_TEST("async poller, SPI: a frame, a read-only frame, and a mismatched length refused") {
  host::FakeSpiDevice dev{};
  Instant now{0U};
  host::FakeAsyncSpi async{dev, now};
  KB_REQUIRE(async.EnsureReady() == Status::Ok);
  const std::array<uint8_t, 2> answer{0xABU, 0xCDU};
  KB_REQUIRE(dev.Respond(answer));
  SpiPoller poller{async};
  const std::array<uint8_t, 2> tx{0x80U, 0x00U};
  KB_CHECK_EQ(poller.StartTransfer(tx, 3U, Instant{0U}, Microseconds{100U}),
              Status::InvalidArgument);
  KB_CHECK_EQ(poller.StartTransfer(tx, 2U, Instant{0U}, Microseconds{100U}), Status::Ok);
  KB_REQUIRE(poller.Step(Instant{1U}).Completion().IsOk());
  KB_CHECK_EQ(poller.Received()[0], 0xABU);
  KB_CHECK_EQ(poller.Received()[1], 0xCDU);
  KB_CHECK_EQ(poller.StartTransfer({}, 2U, Instant{0U}, Microseconds{100U}), Status::Ok);
  KB_REQUIRE(poller.Step(Instant{1U}).Completion().IsOk());
  KB_CHECK_EQ(poller.Received().size(), 2U);
  KB_CHECK_EQ(poller.StartTransfer({}, 5U, Instant{0U}, Microseconds{100U}),
              Status::InvalidArgument);
}

KB_TEST("async poller: a synchronous port compiles only as MayBlock and still reports once") {
  host::FakeI2cBus bus{};
  static_cast<void>(bus.AddDevice(0x48U));
  static_cast<void>(bus.EnsureReady());
  RtosClock clock{};
  using Adapter = i::AsyncFromSync<host::FakeI2cBus, RtosClock>;
  Adapter adapter{bus, clock};
  h::AsyncPoller<Adapter, 2U, h::AsyncUse::MayBlock> poller{adapter};
  KB_CHECK_EQ(poller.StartWriteRead(0x48U, kReg, 2U, kb::rtos::Now(), Microseconds{1000U}),
              Status::Ok);
  KB_CHECK(poller.Step(kb::rtos::Now()).Completion().IsOk());
  KB_CHECK(poller.Step(kb::rtos::Now()).state == i::AsyncState::Idle);
}

KB_TEST("async poller: the latency counter saturates") {
  Sensor s{};
  // cppcheck-suppress unreadVariable
  s.now = Instant{0U};
  s.async.Script().CompleteLate(true);
  I2cPoller poller{s.async};
  KB_REQUIRE(poller.StartWriteRead(0x48U, kReg, 2U, Instant{0U}, Microseconds{1U}) == Status::Ok);
  static_cast<void>(poller.Step(Instant{std::numeric_limits<uint64_t>::max()}));
  KB_CHECK_EQ(poller.Stats().max_latency_us, std::numeric_limits<uint32_t>::max());
  KB_REQUIRE(poller.StartWriteRead(0x48U, kReg, 2U, Instant{0U}, Microseconds{1U}) == Status::Ok);
  static_cast<void>(poller.Step(Instant{5U}));
  KB_CHECK_EQ(poller.Stats().max_latency_us, std::numeric_limits<uint32_t>::max()); // kept
}

KB_TEST("async poller: an owner pass samples through the poller and never waits on the bus") {
  Sensor s{};
  s.async.Script().CompleteAfterPolls(3U); // the transfer spans three passes
  I2cPoller poller{s.async};
  Sampler sampler{.poller = poller};
  h::BusDomain<host::FakeAsyncI2c, h::ThreadModel::Owner> domain{s.async};
  for (uint32_t pass = 0U; pass < 9U; ++pass) {
    s.now = kb::rtos::Now();
    KB_REQUIRE(domain.Service(s.now, sampler).IsOk());
    kb::rtos::none::AdvanceTicks(1U);
  }
  KB_CHECK_EQ(sampler.published, 3U);
  KB_CHECK_EQ(sampler.last, 0x1234U);
  KB_CHECK_EQ(sampler.longest_pass_us, 0U); // no pass let time pass: nothing waited
  KB_CHECK_EQ(poller.Stats().ok, 3U);
  KB_CHECK_EQ(poller.Stats().max_latency_us, 2000U);
}
