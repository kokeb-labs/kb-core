// at25040b through kb::handlers::at25040b: start-up, the app API, error mapping to the health
// record, recovery, both thread models, and ranges held whole (Guarded::Hold, OwnerAccess::Held),
// over a kb::iface::SpiDevice backed by the driver's own chip model
// (components/drivers/driver-at25040b/tests/mock_at25040b_bus.hpp), whose write cycles follow the
// RTOS clock. SPDX-License-Identifier: MIT Copyright (c) 2026 The kb-core authors
#include <array>
#include <atomic>
#include <cstdint>
#include <span>
#include <thread>

#include "binding_support.hpp"
#include "kb/handlers/bindings/at25040b.hpp"
#include "kb/interfaces/host/fake_pins.hpp"
#include "kb_test.hpp"
#include "mock_at25040b_bus.hpp"

namespace {

using kb::handlers::HealthState;
namespace binding = kb::handlers::at25040b;

/// kb::iface::SpiDevice over the chip model; the model's time follows the RTOS clock.
class ModelSpi {
public:
  at25040b_test::MockBusState state{};
  bool ready{true};

  [[nodiscard]] kb::Status EnsureReady() noexcept {
    return ready ? kb::Status::Ok : kb::Status::NotReady;
  }
  [[nodiscard]] kb::Status Transfer(std::span<const uint8_t> tx, std::span<uint8_t> rx) noexcept {
    Elapse();
    const bool ok = state.DoTransfer(tx.data(), rx.empty() ? nullptr : rx.data(), tx.size());
    return ok ? kb::Status::Ok : kb::Status::BusError;
  }
  void Elapse() noexcept {
    const uint64_t now = kb::rtos::Now().Us();
    state.chip.Elapse(static_cast<uint32_t>(now - last_us_));
    last_us_ = now;
  }

private:
  uint64_t last_us_{kb::rtos::Now().Us()};
};
static_assert(kb::iface::SpiDevice<ModelSpi>);

/// The WP pin: a host fake (fault injection) whose level reaches the model.
class WpPin {
public:
  explicit WpPin(ModelSpi& spi) noexcept : spi_(spi) {}
  kb::iface::host::FakeGpioPin pin{};
  [[nodiscard]] kb::Status Set(kb::iface::Level level) noexcept {
    const kb::Status s = pin.Set(level);
    if (s == kb::Status::Ok) {
      spi_.state.chip.wp_high = level == kb::iface::Level::High;
    }
    return s;
  }
  [[nodiscard]] kb::Status Toggle() noexcept {
    return Set((pin.LevelNow() == kb::iface::Level::High) ? kb::iface::Level::Low
                                                          : kb::iface::Level::High);
  }

private:
  ModelSpi& spi_;
};
static_assert(kb::iface::GpioOutput<WpPin>);

using Handler = binding::Handler<ModelSpi>;
using GuardedBus = kb::handlers::BusDomain<ModelSpi, kb::handlers::ThreadModel::Guarded>;
using OwnerBus = kb::handlers::BusDomain<ModelSpi, kb::handlers::ThreadModel::Owner>;
using GuardedIo = kb::handlers::Guarded<Handler, GuardedBus>;
using OwnedIo = kb::handlers::OwnerAccess<Handler, OwnerBus>;

constexpr kb::Milliseconds kWait{500U};

static_assert(binding::ToStatus(at25040b::Error::NoDevice) == kb::Status::NoDevice);
static_assert(binding::ToStatus(at25040b::Error::BlockProtected) == kb::Status::InvalidArgument);
static_assert(binding::ToStatus(at25040b::Error::BusNoise) == kb::Status::BusError);

binding::Config Defaults() {
  binding::Config c{};
  c.handler.health.fault_after = 2U;
  c.handler.recovery.backoff = kb::Milliseconds{1U};
  return c;
}

/// A guarded handler on its own chip select, created.
struct GuardedRig {
  kb_it::Timing t{};
  ModelSpi spi{};
  GuardedBus domain{spi};
  GuardedIo io;
  explicit GuardedRig(const binding::Config& cfg) : io(domain, spi, t.timing, cfg) {
    static_cast<void>(domain.Create());
    static_cast<void>(io.Create());
  }
};

std::array<uint8_t, 40> Pattern() {
  std::array<uint8_t, 40> p{};
  for (std::size_t i = 0U; i < p.size(); ++i) {
    p[i] = static_cast<uint8_t>(0x30U + i);
  }
  return p;
}

KB_TEST("at25040b: guarded start runs the one sync stage and the handler is Ready") {
  GuardedRig r{Defaults()};
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Unknown);
  KB_REQUIRE(r.io.EnsureStarted(kWait).IsOk());
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Ready);
  const kb::handlers::StartupReport report = r.io.Startup().Value();
  KB_CHECK(report.complete);
  KB_CHECK_EQ(report.stages, 1U);
  KB_CHECK_EQ(report.kinds, kb::handlers::stage::kIdentify | kb::handlers::stage::kConfigure);
  const int transfers = r.spi.state.transfers;
  KB_CHECK(r.io.EnsureStarted(kWait).IsOk()); // idempotent, no I/O
  KB_CHECK_EQ(r.spi.state.transfers, transfers);
  KB_CHECK(r.io.Sample(kWait).Error() == kb::Status::Unsupported); // a memory has nothing to sample
}

KB_TEST("at25040b: calls before a start send nothing and return NotReady") {
  GuardedRig r{Defaults()};
  std::array<uint8_t, 4> out{};
  KB_CHECK(binding::Read(r.io, 0U, out, kWait).Error() == kb::Status::NotReady);
  KB_CHECK(binding::ReadStatus(r.io, kWait).Error() == kb::Status::NotReady);
  KB_CHECK_EQ(r.spi.state.transfers, 0);
}

KB_TEST("at25040b: an absent chip fails the start with NoDevice and the driver's code; a retry "
        "starts again") {
  GuardedRig r{Defaults()};
  r.spi.state.chip.miso = at25040b_test::Miso::FloatHigh;
  const kb::Result<void> first = r.io.EnsureStarted(kWait);
  KB_CHECK(first.Error() == kb::Status::NoDevice);
  KB_CHECK_EQ(first.Detail(), kb::handlers::DetailOf(at25040b::Error::NoDevice));
  const kb::handlers::HealthRecord h = r.io.Health().Value();
  KB_CHECK(h.state == HealthState::Faulted);
  KB_CHECK_EQ(h.start_failures, 1U);
  KB_CHECK(!r.io.Startup().Value().complete);
  r.spi.state.chip.miso = at25040b_test::Miso::Chip;
  KB_CHECK(r.io.EnsureStarted(kWait).IsOk());
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Ready);
}

KB_TEST("at25040b: Write and Read of 40 bytes across pages and chunks reach the array") {
  GuardedRig r{Defaults()};
  KB_REQUIRE(r.io.EnsureStarted(kWait).IsOk());
  const std::array<uint8_t, 40> data = Pattern();
  KB_REQUIRE(binding::Write(r.io, 0x0103U, data, kWait).IsOk());
  r.spi.Elapse();
  for (std::size_t i = 0U; i < data.size(); ++i) {
    KB_CHECK_EQ(r.spi.state.chip.memory[0x103U + i], data[i]);
  }
  std::array<uint8_t, 40> back{};
  KB_REQUIRE(binding::Read(r.io, 0x0103U, back, kWait, at25040b::Verify::ReadBack).IsOk());
  KB_CHECK(back == data);
  std::array<uint8_t, 2> past{};
  const kb::Result<void> out = binding::Read(r.io, 0x01FFU, past, kWait);
  KB_CHECK(out.Error() == kb::Status::InvalidArgument); // past the end of the array
}

KB_TEST("at25040b: block protection is set, read back and refuses a protected write") {
  GuardedRig r{Defaults()};
  KB_REQUIRE(r.io.EnsureStarted(kWait).IsOk());
  KB_REQUIRE(binding::SetBlockProtection(r.io, at25040b::BlockProtect::UpperHalf, kWait).IsOk());
  const auto bp = binding::GetBlockProtection(r.io, kWait);
  KB_REQUIRE(bp.IsOk());
  KB_CHECK(bp.Value() == at25040b::BlockProtect::UpperHalf);
  const auto status = binding::ReadStatus(r.io, kWait);
  KB_REQUIRE(status.IsOk());
  KB_CHECK(status.Value().block_protect == at25040b::BlockProtect::UpperHalf);
  KB_CHECK(!status.Value().write_enabled);
  const std::array<uint8_t, 1> one{0x5AU};
  const kb::Result<void> refused = binding::Write(r.io, 0x0180U, one, kWait);
  KB_CHECK(refused.Error() == kb::Status::InvalidArgument);
  KB_CHECK_EQ(refused.Detail(), kb::handlers::DetailOf(at25040b::Error::BlockProtected));
  KB_CHECK(binding::Write(r.io, 0x0010U, one, kWait).IsOk()); // the lower half stays writable
}

KB_TEST("at25040b: the WP pin is parked at start and released only around a write; a pin failure "
        "is BusError") {
  kb_it::Timing t{};
  ModelSpi spi{};
  WpPin wp{spi};
  GuardedBus domain{spi};
  binding::Config cfg = Defaults();
  cfg.write_protect.pin = kb::iface::GpioOutputRef{wp};
  GuardedIo io{domain, spi, t.timing, cfg};
  KB_REQUIRE(domain.Create().IsOk());
  KB_REQUIRE(io.Create().IsOk());
  wp.pin.FailNext(kb::Status::BusError);
  const kb::Result<void> bad = io.EnsureStarted(kWait);
  KB_CHECK(bad.Error() == kb::Status::BusError);
  KB_CHECK_EQ(bad.Detail(), kb::handlers::DetailOf(at25040b::Error::Pin));
  KB_REQUIRE(io.EnsureStarted(kWait).IsOk());
  KB_CHECK(wp.pin.LevelNow() == kb::iface::Level::Low); // parked: writes blocked
  const uint32_t writes = wp.pin.Writes();
  const std::array<uint8_t, 3> data{1U, 2U, 3U};
  KB_CHECK(binding::Write(io, 0x0020U, data, kWait).IsOk());
  KB_CHECK(wp.pin.Writes() >= (writes + 2U)); // released, then parked again
  KB_CHECK(wp.pin.LevelNow() == kb::iface::Level::Low);
  spi.Elapse();
  KB_CHECK_EQ(spi.state.chip.memory[0x22U], 3U);
}

KB_TEST("at25040b: bus failures degrade, the threshold faults, Recover() restores Ready") {
  GuardedRig r{Defaults()};
  KB_REQUIRE(r.io.EnsureStarted(kWait).IsOk());
  r.spi.state.fail_transfer = r.spi.state.transfers;
  const auto bad = binding::ReadStatus(r.io, kWait);
  KB_CHECK(bad.Error() == kb::Status::BusError);
  KB_CHECK_EQ(bad.Detail(), kb::handlers::DetailOf(at25040b::Error::Bus));
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Degraded);
  KB_CHECK(binding::ReadStatus(r.io, kWait).IsOk());
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Ready);
  r.spi.state.fail_transfer = r.spi.state.transfers;
  KB_CHECK(!binding::ReadStatus(r.io, kWait).IsOk());
  r.spi.state.fail_transfer = r.spi.state.transfers;
  KB_CHECK(!binding::ReadStatus(r.io, kWait).IsOk());
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Faulted);
  const int before = r.spi.state.transfers;
  KB_CHECK(binding::ReadStatus(r.io, kWait).Error() == kb::Status::NotReady);
  KB_CHECK_EQ(r.spi.state.transfers, before);
  KB_CHECK(r.io.RequestRecovery(kWait).IsOk());
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Ready);
  KB_CHECK_EQ(r.io.Health().Value().recoveries, 1U);
}

KB_TEST("at25040b: NotInitialized is the fatal error; refusals are not") {
  kb_it::Timing t{};
  ModelSpi spi{};
  const Handler h{spi, t.timing, Defaults()};
  KB_CHECK(
      h.IsFatal(kb::Status::NotReady, kb::handlers::DetailOf(at25040b::Error::NotInitialized)));
  KB_CHECK(!h.IsFatal(kb::Status::Fault, kb::handlers::DetailOf(at25040b::Error::WriteProtected)));
}

KB_TEST("at25040b: owner model: the owner task starts and serves requests") {
  kb_it::Timing t{};
  ModelSpi spi{};
  OwnerBus domain{spi};
  OwnedIo io{domain, spi, t.timing, Defaults()};
  KB_REQUIRE(io.Create().IsOk());
  const kb_it::OwnerLoop<OwnerBus, OwnedIo> owner{domain, io};
  KB_REQUIRE(io.EnsureStarted(kWait).IsOk());
  const std::array<uint8_t, 20> data{9U, 8U, 7U, 6U, 5U, 4U, 3U, 2U, 1U, 0U,
                                     9U, 8U, 7U, 6U, 5U, 4U, 3U, 2U, 1U, 0U};
  KB_REQUIRE(binding::Write(io, 0x0040U, data, kWait).IsOk());
  std::array<uint8_t, 20> back{};
  KB_REQUIRE(binding::Read(io, 0x0040U, back, kWait).IsOk());
  KB_CHECK(back == data);
  KB_CHECK(io.Liveness().Beats() > 0U);
  KB_CHECK(io.Stop(kWait).IsOk());
  KB_CHECK(kb_it::StateOf(io) == HealthState::Unknown);
}

// --- Held ranges (ADR-0015) ---------------------------------------------------------------------

/// 40 bytes of `value`: three chunks, five pages.
std::array<uint8_t, 40> Filled(uint8_t value) {
  std::array<uint8_t, 40> p{};
  p.fill(value);
  return p;
}

/// True when every byte of `bytes` is the same.
bool Uniform(const std::array<uint8_t, 40>& bytes) {
  bool same = true;
  for (const uint8_t b : bytes) {
    same = same && (b == bytes[0]);
  }
  return same;
}

KB_TEST("at25040b: under Guarded::Hold a record of three writes lands whole although another "
        "task writes the same range in the middle of it") {
  GuardedRig r{Defaults()};
  KB_REQUIRE(r.io.EnsureStarted(kWait).IsOk());
  std::atomic<bool> first_written{false};
  std::atomic<bool> other_done{false};
  std::atomic<uint64_t> other_done_us{0U};
  // The other task writes all 40 bytes as 0x22 as soon as the record's first part is out.
  std::thread other{[&r, &first_written, &other_done, &other_done_us]() {
    while (!first_written.load()) {
      kb::rtos::SleepFor(kb::Milliseconds{1U});
    }
    const bool ok = binding::Write(r.io, 0x0080U, Filled(0x22U), kb::Milliseconds{2000U}).IsOk();
    other_done_us.store(kb::rtos::Now().Us());
    other_done.store(ok);
  }};
  uint64_t released_us = 0U;
  bool held_ok = false;
  {
    kb::handlers::GuardedHeld<GuardedIo> held = r.io.Hold(kWait);
    KB_REQUIRE(held.OwnsLock());
    const std::array<uint8_t, 40> record = Filled(0x11U);
    const std::span<const uint8_t> bytes{record};
    held_ok = binding::Write(held, 0x0080U, bytes.first(16U), kWait).IsOk();
    first_written.store(true);
    kb::rtos::SleepFor(kb::Milliseconds{30U}); // the other task is waiting for the lock now
    held_ok = held_ok && binding::Write(held, 0x0090U, bytes.subspan(16U, 16U), kWait).IsOk() &&
              binding::Write(held, 0x00A0U, bytes.subspan(32U), kWait).IsOk();
    std::array<uint8_t, 40> back{};
    held_ok = held_ok && binding::Read(held, 0x0080U, back, kWait).IsOk() && (back == record);
    released_us = kb::rtos::Now().Us();
  }
  other.join();
  KB_CHECK(held_ok);
  KB_CHECK(other_done.load());
  KB_CHECK(other_done_us.load() >= released_us); // it ran only after the hold
  std::array<uint8_t, 40> last{};
  KB_REQUIRE(binding::Read(r.io, 0x0080U, last, kWait).IsOk());
  KB_CHECK(last == Filled(0x22U)); // whole: the other write came after the whole record
  KB_CHECK_EQ(r.io.LockTimeouts(), 0U);
}

KB_TEST("at25040b: a hold that is not had sends nothing; a released hold refuses") {
  GuardedRig r{Defaults()};
  KB_REQUIRE(r.io.EnsureStarted(kWait).IsOk());
  const int before = r.spi.state.transfers;
  {
    const kb::handlers::BusHold<GuardedBus> other = r.domain.Hold(kb::Milliseconds{10U});
    KB_REQUIRE(other.OwnsLock());
    kb::handlers::GuardedHeld<GuardedIo> held = r.io.Hold(kb::Milliseconds{5U});
    KB_CHECK(!held.OwnsLock());
    KB_CHECK(binding::Write(held, 0x0000U, Filled(0x33U), kWait).Error() == kb::Status::Timeout);
  }
  kb::handlers::GuardedHeld<GuardedIo> held = r.io.Hold(kb::Milliseconds{5U});
  KB_REQUIRE(held.OwnsLock());
  held.Release();
  KB_CHECK(binding::ReadStatus(held, kWait).Error() == kb::Status::NotReady);
  KB_CHECK_EQ(r.spi.state.transfers, before);
}

/// An owner task that, every few passes, holds the bus between passes and writes then reads back a
/// range inline through OwnerAccess::Held.
class HeldOwner {
public:
  HeldOwner(OwnerBus& domain, OwnedIo& io) : domain_(domain), io_(io), thread_([this] { run(); }) {}
  HeldOwner(const HeldOwner&) = delete;
  HeldOwner& operator=(const HeldOwner&) = delete;
  ~HeldOwner() {
    stop_.store(true);
    thread_.join();
  }
  std::atomic<int> rounds{0};
  std::atomic<int> failures{0};
  std::atomic<int> refused_passes{0};

private:
  void run() {
    uint32_t pass = 0U;
    while (!stop_.load()) {
      static_cast<void>(domain_.Service(kb::rtos::Now(), io_));
      ++pass;
      if (((pass % 4U) == 0U) && (kb_it::StateOf(io_) == HealthState::Ready)) {
        heldRound(static_cast<uint8_t>(0x40U + (pass & 0x0FU)));
      }
      kb::rtos::SleepFor(kb::Milliseconds{1U});
    }
  }
  void heldRound(uint8_t value) {
    kb::handlers::BusHold<OwnerBus> bus = domain_.Hold(kb::Milliseconds{0U});
    kb::handlers::OwnerHeld<OwnedIo> held = io_.Held(bus);
    std::array<uint8_t, 40> back{};
    const bool ok = held.OwnsLock() && binding::Write(held, 0x0100U, Filled(value), kWait).IsOk() &&
                    binding::Read(held, 0x0100U, back, kWait).IsOk() && (back == Filled(value));
    if (!domain_.Service(kb::rtos::Now(), io_).IsOk()) {
      refused_passes.fetch_add(1); // a pass inside the hold is refused
    }
    (ok ? rounds : failures).fetch_add(1);
  }
  OwnerBus& domain_;
  OwnedIo& io_;
  std::atomic<bool> stop_{false};
  std::thread thread_;
};

KB_TEST("at25040b: owner model: the owner task writes and reads a range whole under its own hold") {
  kb_it::Timing t{};
  ModelSpi spi{};
  OwnerBus domain{spi};
  OwnedIo io{domain, spi, t.timing, Defaults()};
  KB_REQUIRE(io.Create().IsOk());
  {
    HeldOwner owner{domain, io};
    KB_REQUIRE(io.EnsureStarted(kWait).IsOk());
    const kb::utils::Deadline end = kb::utils::Deadline::After(kb::rtos::Now(), kWait);
    while ((owner.rounds.load() < 3) && (!end.Expired(kb::rtos::Now()))) {
      kb::rtos::SleepFor(kb::Milliseconds{1U});
    }
    KB_CHECK(owner.rounds.load() >= 3);
    KB_CHECK_EQ(owner.failures.load(), 0);
    KB_CHECK(owner.refused_passes.load() >= 3);
  }
  KB_CHECK(domain.Stats().holds >= 3U);
  std::array<uint8_t, 40> stored{};
  for (std::size_t i = 0U; i < stored.size(); ++i) {
    stored[i] = spi.state.chip.memory[0x100U + i];
  }
  KB_CHECK(Uniform(stored) && (stored[0] >= 0x40U));
}

} // namespace
