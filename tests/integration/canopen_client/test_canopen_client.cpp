// canopen_client through kb::handlers::canopen_client: start-up, the app API, the published
// consumer snapshot, error mapping to the health record and recovery, on the owner model only (the
// binding requires an owner), over a kb::iface::CanBus backed by the driver's own simulated bus and
// nodes (components/drivers/driver-canopen-client/tests/ mock_canopen_client.hpp). The simulated
// clock follows the RTOS clock; a mutex keeps the test thread and the owner task off the simulation
// at the same time. SPDX-License-Identifier: MIT Copyright (c) 2026 The kb-core authors
#include <cstdint>
#include <mutex>
#include <optional>

#include "binding_support.hpp"
#include "kb/handlers/bindings/canopen_client.hpp"
#include "kb_test.hpp"
#include "mock_canopen_client.hpp"

namespace {

using kb::handlers::HealthState;
namespace co = kb::handlers::canopen_client;
using canopen_client_test::SimBus;

/// kb::iface::CanBus over the driver's simulated bus; nodes 5 and 6, pre-operational.
class ModelCan {
public:
  ModelCan() : base_us_(kb::rtos::Now().Us()) {
    static_cast<void>(bus_.AddNode(5U));
    static_cast<void>(bus_.AddNode(6U));
  }

  /// Runs `f(bus)` with the simulation held.
  template <typename F>
  void With(F f) {
    const std::lock_guard<std::mutex> hold{mutex_};
    sync();
    f(bus_);
  }

  [[nodiscard]] kb::Status EnsureReady() noexcept {
    const std::lock_guard<std::mutex> hold{mutex_};
    ++bus_.init_calls;
    return bus_.init_ok ? kb::Status::Ok : kb::Status::NotReady;
  }
  [[nodiscard]] kb::Status Send(const kb::iface::CanFrame& frame,
                                kb::Milliseconds timeout) noexcept {
    static_cast<void>(timeout);
    const std::lock_guard<std::mutex> hold{mutex_};
    sync();
    const int index = bus_.sends++;
    if (!bus_.send_ok || (index == bus_.fail_send_index)) {
      return kb::Status::BusError;
    }
    canopen_client::CanFrame f{};
    f.id = static_cast<uint16_t>(frame.id);
    f.dlc = frame.dlc;
    f.rtr = frame.remote;
    f.data = frame.data;
    bus_.FromClient(f);
    return kb::Status::Ok;
  }
  [[nodiscard]] kb::Status Receive(kb::iface::CanFrame& frame, kb::Milliseconds timeout) noexcept {
    const uint64_t end = kb::rtos::Now().Us() + (static_cast<uint64_t>(timeout.Count()) * 1000U);
    kb::Status s = kb::Status::Timeout;
    bool waiting = true;
    while (waiting) {
      {
        const std::lock_guard<std::mutex> hold{mutex_};
        sync();
        if (bus_.receive_error) {
          s = kb::Status::BusError;
          waiting = false;
        } else if (!bus_.rx.empty() && (bus_.rx.front().at_us <= bus_.now_us)) {
          const canopen_client_test::Queued q = bus_.rx.front();
          bus_.rx.erase(bus_.rx.begin());
          frame = co::ToKbFrame(q.frame);
          s = kb::Status::Ok;
          waiting = false;
        } else {
          // nothing due yet
        }
      }
      if (waiting && (kb::rtos::Now().Us() >= end)) {
        waiting = false;
      } else if (waiting) {
        kb::rtos::SleepFor(kb::Microseconds{200U});
      } else {
        // done
      }
    }
    return s;
  }

private:
  void sync() noexcept {
    const uint64_t now = 1'000'000U + (kb::rtos::Now().Us() - base_us_);
    if (now > bus_.now_us) {
      bus_.now_us = now;
    }
    bus_.PumpHeartbeats(bus_.now_us);
  }
  SimBus bus_{};
  std::mutex mutex_{};
  uint64_t base_us_;
};
static_assert(kb::iface::CanBus<ModelCan>);

/// The simulated node `id` (the first one when none has it, so the reference is never null).
canopen_client_test::SimNode& NodeAt(SimBus& bus, uint8_t id) {
  canopen_client_test::SimNode* n = bus.Node(id);
  return (n != nullptr) ? *n : bus.nodes.front();
}

using Handler = co::Handler<ModelCan>;
using OwnerBus = kb::handlers::BusDomain<ModelCan, kb::handlers::ThreadModel::Owner>;
using OwnedIo = kb::handlers::OwnerAccess<Handler, OwnerBus>;

constexpr kb::Milliseconds kWait{3000U};

co::Config TwoNodes() {
  co::Config c{};
  c.chip = canopen_client::FastBusPreset();
  c.chip.sdo_timeout_us = 20'000U;
  c.chip.drive_poll_interval_us = 1'000U;
  c.nodes[0] = co::NodeWatch{5U, 100'000U};
  c.nodes[1] = co::NodeWatch{6U, 100'000U};
  c.node_count = 2U;
  c.tpdos[0] = co::TpdoWatch{0x185U, 2U};
  c.tpdo_count = 1U;
  c.handler.health.fault_after = 3U;
  c.handler.recovery.backoff = kb::Milliseconds{1U};
  return c;
}

/// An owner bus with its owner task running.
struct OwnerRig {
  kb_it::Timing t{};
  ModelCan can{};
  OwnerBus domain{can};
  OwnedIo io;
  std::optional<kb_it::OwnerLoop<OwnerBus, OwnedIo>> owner{};
  explicit OwnerRig(const co::Config& cfg) : io(domain, can, t.timing, cfg) {
    static_cast<void>(io.Create());
    owner.emplace(domain, io);
  }
  std::size_t Sent() {
    std::size_t n = 0U;
    can.With([&n](SimBus& b) { n = b.sent.size(); });
    return n;
  }
};

/// Waits up to `ms` for `done()` to be true.
template <typename F>
bool WaitFor(uint32_t ms, F done) {
  bool ok = done();
  for (uint32_t i = 0U; (i < ms) && (!ok); ++i) {
    kb::rtos::SleepFor(kb::Milliseconds{1U});
    ok = done();
  }
  return ok;
}

KB_TEST("canopen_client: the owner starts Link then Configure; the NMT start reaches every node") {
  co::Config cfg = TwoNodes();
  cfg.start_nodes = true;
  OwnerRig r{cfg};
  KB_REQUIRE(r.io.EnsureStarted(kWait).IsOk());
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Ready);
  const kb::handlers::StartupReport report = r.io.Startup().Value();
  KB_CHECK(report.complete);
  KB_CHECK_EQ(report.stages, 2U);
  KB_CHECK_EQ(report.stage, 1U);
  KB_CHECK_EQ(report.kinds, kb::handlers::stage::kConfigure);
  uint8_t nmt5 = 0U;
  uint8_t nmt6 = 0U;
  r.can.With([&](SimBus& b) {
    nmt5 = NodeAt(b, 5U).nmt;
    nmt6 = NodeAt(b, 6U).nmt;
  });
  KB_CHECK_EQ(nmt5, 5U); // operational
  KB_CHECK_EQ(nmt6, 5U);
  KB_CHECK(r.io.EnsureStarted(kWait).IsOk()); // idempotent
  KB_CHECK_EQ(r.io.Health().Value().starts, 1U);
}

KB_TEST("canopen_client: calls before a start send nothing and return NotReady") {
  OwnerRig r{TwoNodes()};
  KB_CHECK(
      co::SdoRead(r.io, {.index = 0x1000U, .node = 5U, .sub = 0U, .size = 4U}, kWait).Error() ==
      kb::Status::NotReady);
  KB_CHECK(co::DriveDisable(r.io, 5U, kWait).Error() == kb::Status::NotReady);
  KB_CHECK(r.io.Latest().Error() == kb::Status::NotReady);
  KB_CHECK_EQ(r.Sent(), 0U);
}

KB_TEST("canopen_client: a controller that does not start fails Link with BusError; a retry starts "
        "again") {
  OwnerRig r{TwoNodes()};
  r.can.With([](SimBus& b) { b.init_ok = false; });
  const kb::Result<void> first = r.io.EnsureStarted(kWait);
  KB_CHECK(first.Error() == kb::Status::BusError);
  KB_CHECK_EQ(first.Detail(), kb::handlers::DetailOf(canopen_client::Error::Bus));
  const kb::handlers::StartupReport rep = r.io.Startup().Value();
  KB_CHECK(!rep.complete);
  KB_CHECK_EQ(rep.stage, 0U);
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Faulted);
  r.can.With([](SimBus& b) { b.init_ok = true; });
  KB_CHECK(r.io.EnsureStarted(kWait).IsOk());
  KB_CHECK_EQ(r.io.Startup().Value().runs, 2U);
}

KB_TEST("canopen_client: a node the consumer cannot watch fails Configure with InvalidArgument") {
  co::Config cfg = TwoNodes();
  cfg.nodes[1].node = 0U; // not a node identifier
  OwnerRig r{cfg};
  const kb::Result<void> first = r.io.EnsureStarted(kWait);
  KB_CHECK(first.Error() == kb::Status::InvalidArgument);
  KB_CHECK_EQ(first.Detail(), kb::handlers::DetailOf(canopen_client::Error::InvalidArgument));
  KB_CHECK_EQ(r.io.Startup().Value().stage, 1U);
}

KB_TEST("canopen_client: SDO reads and writes reach the node's object dictionary") {
  OwnerRig r{TwoNodes()};
  KB_REQUIRE(r.io.EnsureStarted(kWait).IsOk());
  const kb::Result<uint32_t> type =
      co::SdoRead(r.io, {.index = 0x1000U, .node = 5U, .sub = 0U, .size = 4U}, kWait);
  KB_REQUIRE(type.IsOk());
  KB_CHECK_EQ(type.Value(), 0x00020192U);
  KB_CHECK(co::SdoWrite(r.io, {.index = 0x2001U, .node = 6U, .sub = 0U, .size = 2U}, 0x1234U, kWait)
               .IsOk());
  KB_CHECK_EQ(
      co::SdoRead(r.io, {.index = 0x2001U, .node = 6U, .sub = 0U, .size = 2U}, kWait).ValueOr(0U),
      0x1234U);
  KB_CHECK(co::SdoWrite(r.io, {.index = 0x2002U, .node = 6U, .sub = 0U, .size = 1U}, 0x77U, kWait)
               .IsOk());
  KB_CHECK_EQ(
      co::SdoRead(r.io, {.index = 0x2002U, .node = 6U, .sub = 0U, .size = 1U}, kWait).ValueOr(0U),
      0x77U);
  const std::size_t sent = r.Sent();
  KB_CHECK(
      co::SdoRead(r.io, {.index = 0x1000U, .node = 5U, .sub = 0U, .size = 3U}, kWait).Error() ==
      kb::Status::InvalidArgument);
  KB_CHECK(co::SdoWrite(r.io, {.index = 0x2002U, .node = 5U, .sub = 0U, .size = 1U}, 0x100U, kWait)
               .Error() == kb::Status::InvalidArgument);
  KB_CHECK_EQ(r.Sent(), sent);
}

KB_TEST("canopen_client: NMT, node guarding, CiA 402 drive, SYNC and RPDO through the app API") {
  OwnerRig r{TwoNodes()};
  KB_REQUIRE(r.io.EnsureStarted(kWait).IsOk());
  KB_CHECK(co::SendNmt(r.io, canopen_client::NmtCommand::Start, 5U, kWait).IsOk());
  const kb::Result<canopen_client::NmtState> state = co::GuardNode(r.io, 5U, kWait);
  KB_REQUIRE(state.IsOk());
  KB_CHECK(state.Value() == canopen_client::NmtState::Operational);
  KB_CHECK(co::DriveEnable(r.io, 5U, kWait).IsOk());
  KB_CHECK(co::ReadDriveState(r.io, 5U, kWait).ValueOr(canopen_client::DriveState::Unknown) ==
           canopen_client::DriveState::OperationEnabled);
  co::Command mode{};
  mode.op = co::Op::SetDriveMode;
  mode.node = 5U;
  mode.mode = canopen_client::OperationMode::ProfileVelocity;
  KB_CHECK(r.io.Request(mode, kWait).IsOk());
  KB_CHECK(co::DriveQuickStop(r.io, 5U, kWait).IsOk());
  KB_CHECK(co::DriveDisable(r.io, 5U, kWait).IsOk());
  KB_CHECK(co::ReadDriveState(r.io, 5U, kWait).ValueOr(canopen_client::DriveState::Unknown) ==
           canopen_client::DriveState::SwitchOnDisabled);
  co::Command fault{};
  fault.op = co::Op::DriveResetFault;
  fault.node = 5U;
  KB_CHECK(r.io.Request(fault, kWait).IsOk());
  co::Command sync{};
  sync.op = co::Op::SendSync;
  KB_CHECK(r.io.Request(sync, kWait).IsOk());
  co::Command rpdo{};
  rpdo.op = co::Op::SendRpdo;
  rpdo.cob_id = 0x205U;
  rpdo.data[0] = 0xAAU;
  rpdo.data[1] = 0x55U;
  rpdo.len = 2U;
  KB_CHECK(r.io.Request(rpdo, kWait).IsOk());
  uint16_t last_id = 0U;
  r.can.With([&last_id](SimBus& b) {
    last_id = b.sent.empty() ? uint16_t{0U} : b.sent[b.sent.size() - 1U].id;
  });
  KB_CHECK_EQ(last_id, 0x205U);
  co::Command clear{};
  clear.op = co::Op::ClearBootUp;
  clear.node = 6U;
  KB_CHECK(r.io.Request(clear, kWait).IsOk());
}

KB_TEST("canopen_client: the snapshot carries heartbeats, the EMCY taken and the subscribed TPDO") {
  OwnerRig r{TwoNodes()};
  KB_REQUIRE(r.io.EnsureStarted(kWait).IsOk());
  KB_CHECK(co::SdoWrite(r.io, {.index = 0x1017U, .node = 5U, .sub = 0U, .size = 2U}, 10U, kWait)
               .IsOk()); // heartbeat every 10 ms
  KB_CHECK(WaitFor(500U, [&r] {
    const auto s = r.io.Latest();
    return s.IsOk() && s.Value().value.nodes[0].alive && (s.Value().value.nodes[0].count >= 2U);
  }));
  r.can.With([](SimBus& b) {
    canopen_client::CanFrame emcy{}; // node 6: error code 0x8130, error register 0x11
    emcy.id = 0x086U;
    emcy.dlc = 8U;
    emcy.data[0] = 0x30U;
    emcy.data[1] = 0x81U;
    emcy.data[2] = 0x11U;
    b.Inject(emcy, b.now_us);
    canopen_client::CanFrame pdo{};
    pdo.id = 0x185U;
    pdo.dlc = 2U;
    pdo.data[0] = 0x34U;
    pdo.data[1] = 0x12U;
    b.Inject(pdo, b.now_us);
  });
  KB_CHECK(WaitFor(500U, [&r] {
    const auto s = r.io.Latest();
    return s.IsOk() && (s.Value().value.emcy_count == 1U) &&
           (s.Value().value.tpdos[0].sequence == 1U);
  }));
  const co::Snapshot s = r.io.Latest().Value().value;
  KB_CHECK_EQ(s.node_count, 2U);
  KB_CHECK(!s.nodes[1].alive); // node 6 sends no heartbeat
  KB_CHECK_EQ(s.last_emcy.node, 6U);
  KB_CHECK_EQ(s.last_emcy.code, 0x8130U);
  KB_CHECK_EQ(s.tpdos[0].data[0], 0x34U);
  KB_CHECK(s.stats.frames_received >= 3U);
}

KB_TEST("canopen_client: the error map and the fatal set") {
  static_assert(co::ToStatus(canopen_client::Error::Ok) == kb::Status::Ok);
  static_assert(co::ToStatus(canopen_client::Error::SdoAbort) == kb::Status::Fault);
  static_assert(co::ToStatus(canopen_client::Error::Timeout) == kb::Status::Timeout);
  static_assert(co::ToStatus(canopen_client::Error::BufferTooSmall) == kb::Status::Overflow);
  static_assert(co::ToStatus(canopen_client::Error::NodeState) == kb::Status::NotReady);
  KB_CHECK(co::IsFatalError(canopen_client::Error::NotInitialized));
  KB_CHECK(!co::IsFatalError(canopen_client::Error::Bus));
}

KB_TEST("canopen_client: an SDO abort and lost replies degrade, the threshold faults, Recover() "
        "restores Ready") {
  co::Config cfg = TwoNodes();
  cfg.handler.sample_period =
      kb::Milliseconds{60000U}; // no periodic sample resets the failure count
  OwnerRig r{cfg};
  KB_REQUIRE(r.io.EnsureStarted(kWait).IsOk());
  KB_CHECK(WaitFor(200U, [&r] { return r.io.Latest().IsOk(); })); // the one sample after the start
  r.can.With([](SimBus& b) { NodeAt(b, 5U).abort_next_with = 0x06020000U; });
  const kb::Result<uint32_t> aborted =
      co::SdoRead(r.io, {.index = 0x2000U, .node = 5U, .sub = 0U, .size = 4U}, kWait);
  KB_CHECK(aborted.Error() == kb::Status::Fault);
  KB_CHECK_EQ(aborted.Detail(), kb::handlers::DetailOf(canopen_client::Error::SdoAbort));
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Degraded);
  r.can.With([](SimBus& b) { NodeAt(b, 5U).responds = false; });
  const kb::Result<uint32_t> lost =
      co::SdoRead(r.io, {.index = 0x2000U, .node = 5U, .sub = 0U, .size = 4U}, kWait);
  KB_CHECK(lost.Error() == kb::Status::Timeout);
  KB_CHECK_EQ(lost.Detail(), kb::handlers::DetailOf(canopen_client::Error::Timeout));
  KB_CHECK(!co::SdoRead(r.io, {.index = 0x2000U, .node = 5U, .sub = 0U, .size = 4U}, kWait).IsOk());
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Faulted);
  const std::size_t sent = r.Sent();
  KB_CHECK(
      co::SdoRead(r.io, {.index = 0x2000U, .node = 5U, .sub = 0U, .size = 4U}, kWait).Error() ==
      kb::Status::NotReady);
  KB_CHECK_EQ(r.Sent(), sent); // a faulted handler sends nothing
  r.can.With([](SimBus& b) { NodeAt(b, 5U).responds = true; });
  KB_CHECK(r.io.RequestRecovery(kWait).IsOk());
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Ready);
  KB_CHECK_EQ(r.io.Health().Value().recoveries, 1U);
  KB_CHECK(co::SdoRead(r.io, {.index = 0x2000U, .node = 5U, .sub = 0U, .size = 4U}, kWait).IsOk());
  r.can.With([](SimBus& b) { NodeAt(b, 5U).abort_next_with = 0x06090011U; });
  KB_CHECK(!co::SdoRead(r.io, {.index = 0x2000U, .node = 5U, .sub = 0U, .size = 4U}, kWait).IsOk());
  const auto snap = r.io.Sample(kWait); // the snapshot names the abort the node sent
  KB_REQUIRE(snap.IsOk());
  KB_CHECK_EQ(snap.Value().value.last_abort.code, 0x06090011U);
  KB_CHECK(snap.Value().value.last_abort.remote);
}

KB_TEST(
    "canopen_client: a receive error of the controller counts in health; Stop() ends the session") {
  OwnerRig r{TwoNodes()};
  KB_REQUIRE(r.io.EnsureStarted(kWait).IsOk());
  r.can.With([](SimBus& b) { b.receive_error = true; });
  KB_CHECK(WaitFor(500U, [&r] { return kb_it::StateOf(r.io) == HealthState::Faulted; }));
  const kb::handlers::HealthRecord h = r.io.Health().Value();
  KB_CHECK(h.last_status == kb::Status::BusError);
  KB_CHECK_EQ(h.last_detail, kb::handlers::DetailOf(canopen_client::Error::Bus));
  r.can.With([](SimBus& b) { b.receive_error = false; });
  KB_CHECK(r.io.RequestRecovery(kWait).IsOk());
  KB_CHECK(r.io.Liveness().Beats() > 0U);
  KB_CHECK(r.io.Stop(kWait).IsOk());
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Unknown);
}

} // namespace
