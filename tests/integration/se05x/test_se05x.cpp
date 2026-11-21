// se05x through kb::handlers::se05x: the start-up plan (open, platform SCP03, the Verify stage),
// the app API (status, random, unique id, attested reads, whole-object reads, sign and verify, an
// AESKey session per call), error mapping to the health record, recovery with and without the run
// line, and both thread models, over a kb::iface::I2cBus backed by the driver's own chip model
// chain (components/drivers/driver-se05x/tests/model: T1Target, ScpTarget, ChipModel) and the
// driver's ReferenceCrypto as the HAL's crypto port. Test keys only: every key is the model's test
// key.
//
// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The kb-core authors
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "binding_support.hpp"
#include "kb/handlers/bindings/se05x.hpp"
#include "kb_test.hpp"
#include "model/chip_bench.hpp"

namespace {

namespace hs = kb::handlers::se05x;
namespace sm = se05x::testmodel;
using kb::handlers::HealthState;

/// The driver's chip model chain: the T=1 stage, the SCP stage and the applet, with the model's
/// test keys.
struct Chip {
  sm::SimClock clock;
  sm::ScpTarget scp;
  sm::ChipModel chip;
  sm::T1Target t1;
  explicit Chip(const sm::ChipProfile& profile) : chip(scp, profile), t1(clock, &scp, Knobs()) {
    scp.SetPlatformKeys(sm::kPlatformEnc.data(), sm::kPlatformMac.data(), 16U, 0x0BU);
    scp.AddAesKey(sm::kAesCredential, sm::kAesKey.data());
  }
  static sm::T1Knobs Knobs() {
    sm::T1Knobs k{};
    k.process_us = 0U; // answers are ready at once: the host clock drives the model
    return k;
  }
};

/// kb::iface::I2cBus over the model's T=1 stage. The model's clock follows the host clock, so the
/// waits of the driver (real sleeps through HookTiming) are what the model sees. `absent` and
/// `stuck` are bus faults.
class ModelBus {
public:
  explicit ModelBus(Chip& chip) : chip_(chip), base_us_(kb::rtos::Now().Us()) {}
  bool absent{false};
  bool stuck{false};
  uint32_t writes{0U};

  [[nodiscard]] kb::Status EnsureReady() noexcept {
    return kb::Status::Ok;
  }
  [[nodiscard]] kb::Status Write(uint8_t address, std::span<const uint8_t> tx) noexcept {
    ++writes;
    sync();
    return outcome(fault() ? se05x::BusStatus::Nack : chip_.t1.Write(address, tx));
  }
  [[nodiscard]] kb::Status Read(uint8_t address, std::span<uint8_t> rx) noexcept {
    sync();
    const se05x::BusRead r =
        fault() ? se05x::BusRead{se05x::BusStatus::Nack, 0U} : chip_.t1.Read(address, rx);
    return outcome(((r.status == se05x::BusStatus::Ok) && (r.count != rx.size()))
                       ? se05x::BusStatus::BusError
                       : r.status);
  }
  [[nodiscard]] kb::Status WriteRead(uint8_t address, std::span<const uint8_t> tx,
                                     std::span<uint8_t> rx) noexcept {
    static_cast<void>(address);
    static_cast<void>(tx);
    static_cast<void>(rx);
    last_ = kb::iface::I2cFailure::Bus;
    return kb::Status::Unsupported; // the chip never needs a repeated START
  }
  [[nodiscard]] kb::iface::I2cFailure LastFailure() const noexcept {
    return last_;
  }

private:
  [[nodiscard]] bool fault() noexcept {
    return absent || stuck;
  }
  void sync() noexcept {
    const uint64_t host = kb::rtos::Now().Us() - base_us_;
    if (host > chip_.clock.now_us) {
      chip_.clock.now_us = host;
    }
  }
  [[nodiscard]] kb::Status outcome(se05x::BusStatus s) noexcept {
    kb::Status status = kb::Status::Ok;
    last_ = kb::iface::I2cFailure::None;
    if (stuck) {
      s = se05x::BusStatus::BusStuck;
    }
    switch (s) {
    case se05x::BusStatus::Ok:
      break;
    case se05x::BusStatus::Nack:
      last_ = kb::iface::I2cFailure::AddressNack;
      status = kb::Status::NoDevice;
      break;
    case se05x::BusStatus::Timeout:
      last_ = kb::iface::I2cFailure::Timeout;
      status = kb::Status::Timeout;
      break;
    case se05x::BusStatus::BusError:
      last_ = kb::iface::I2cFailure::Bus;
      status = kb::Status::BusError;
      break;
    case se05x::BusStatus::BusStuck:
      last_ = kb::iface::I2cFailure::BusStuck;
      status = kb::Status::BusError;
      break;
    }
    return status;
  }
  Chip& chip_;
  uint64_t base_us_;
  kb::iface::I2cFailure last_{kb::iface::I2cFailure::None};
};
static_assert(kb::iface::I2cBus<ModelBus>);

/// The board's run line (ENA) wired to the model: a rising edge power-cycles the chip and frees the
/// bus.
class RunLine {
public:
  RunLine(Chip& chip, ModelBus& bus) : chip_(chip), bus_(bus) {}
  uint32_t cycles{0U};
  [[nodiscard]] kb::Status Set(kb::iface::Level level) noexcept {
    if ((level == kb::iface::Level::High) && (level_ == kb::iface::Level::Low)) {
      ++cycles;
      bus_.stuck = false;
      chip_.t1.PowerCycleChip();
    }
    level_ = level;
    return kb::Status::Ok;
  }
  [[nodiscard]] kb::Status Toggle() noexcept {
    return Set((level_ == kb::iface::Level::High) ? kb::iface::Level::Low : kb::iface::Level::High);
  }

private:
  Chip& chip_;
  ModelBus& bus_;
  kb::iface::Level level_{kb::iface::Level::High};
};
static_assert(kb::iface::GpioOutput<RunLine>);

using Handler = hs::Handler<ModelBus, se05x::ReferenceCrypto>;
using ResetHandler = hs::Handler<ModelBus, se05x::ReferenceCrypto, true>;
using GuardedBus = kb::handlers::BusDomain<ModelBus, kb::handlers::ThreadModel::Guarded>;
using OwnerBus = kb::handlers::BusDomain<ModelBus, kb::handlers::ThreadModel::Owner>;
using GuardedIo = kb::handlers::Guarded<Handler, GuardedBus>;
using GuardedResetIo = kb::handlers::Guarded<ResetHandler, GuardedBus>;
using OwnedIo = kb::handlers::OwnerAccess<Handler, OwnerBus>;

constexpr kb::Milliseconds kWait{5000U};
constexpr se05x::ObjectId kAttestKey{sm::ChipModel::kAttestKey};
constexpr se05x::ObjectId kCertificate{0x00004001U};

hs::Config Quick() {
  hs::Config c{};
  c.chip.boot_wait_ms = 0U;
  c.chip.atr_poll_budget_ms = 50U;
  c.handler.health.fault_after = 2U;
  c.handler.recovery.backoff = kb::Milliseconds{1U};
  return c;
}

/// A guarded handler on its own bus over its own chip model, created. With `platform_keys` the
/// crypto port holds the model's platform SCP03 test keys and the configuration carries their
/// handles.
template <typename Io = GuardedIo>
struct GuardedRig {
  kb_it::Timing t{};
  Chip chip;
  ModelBus bus{chip};
  RunLine line{chip, bus};
  se05x::ReferenceCrypto crypto{&sm::CountingEntropy};
  GuardedBus domain{bus};
  Io io;
  explicit GuardedRig(const hs::Config& cfg, const sm::ChipProfile& profile = sm::kSe050E,
                      bool platform_keys = false)
      : chip(profile),
        io(domain, bus, crypto, t.timing, Prepare(cfg, line, crypto, platform_keys)) {
    static_cast<void>(domain.Create());
    static_cast<void>(io.Create());
  }
  static hs::Config Prepare(hs::Config cfg, RunLine& run, se05x::ReferenceCrypto& keys,
                            bool platform_keys) {
    cfg.reset.pin.pin = kb::iface::GpioOutputRef{run};
    if (platform_keys) {
      static_cast<void>(keys.ImportKey(se05x::KeyKind::Aes128, se05x::ByteSpan{sm::kPlatformEnc},
                                       cfg.platform.keys.enc));
      static_cast<void>(keys.ImportKey(se05x::KeyKind::Aes128, se05x::ByteSpan{sm::kPlatformMac},
                                       cfg.platform.keys.mac));
    }
    return cfg;
  }
};

/// The model's attestation key with its trusted public key.
hs::Attester ModelAttester(const sm::ChipModel& chip) {
  hs::Attester a{};
  a.key = kAttestKey;
  a.trusted_public = chip.AttestPublic();
  return a;
}

KB_TEST("se05x: guarded start opens the chip in one stage and skips the disabled Verify stage") {
  GuardedRig<> r{Quick()};
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Unknown);
  KB_REQUIRE(r.io.EnsureStarted(kWait).IsOk());
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Ready);
  const kb::handlers::StartupReport rep = r.io.Startup().Value();
  KB_CHECK(rep.complete);
  KB_CHECK_EQ(rep.stages, 2U);
  KB_CHECK_EQ(rep.stage, 0U); // the last stage that ran: Verify is optional and disabled
  KB_CHECK_EQ(rep.kinds, kb::handlers::stage::kLink | kb::handlers::stage::kIdentify |
                             kb::handlers::stage::kSecure);
  const kb::Result<hs::DeviceStatus> st = hs::ReadStatus(r.io, kWait);
  KB_REQUIRE(st.IsOk());
  KB_CHECK(st.Value().state == se05x::DeviceState::Ready);
  KB_CHECK(st.Value().family == se05x::Family::Se050E);
  KB_CHECK_EQ(st.Value().applet.major, 7U);
  KB_CHECK(!st.Value().platform_scp_active);
  KB_CHECK(st.Value().apdu_count >= 4U);
  const uint32_t selects = r.chip.chip.selects;
  KB_CHECK(r.io.EnsureStarted(kWait).IsOk()); // idempotent: nothing sent
  KB_CHECK_EQ(r.chip.chip.selects, selects);
  KB_CHECK_EQ(r.io.Health().Value().starts, 1U);
}

KB_TEST("se05x: calls before a start send nothing and return NotReady") {
  GuardedRig<> r{Quick()};
  std::array<uint8_t, 8> out{};
  KB_CHECK(hs::GetRandom(r.io, out, kWait).Error() == kb::Status::NotReady);
  KB_CHECK(hs::ReadStatus(r.io, kWait).Error() == kb::Status::NotReady);
  KB_CHECK(r.io.Sample(kWait).Error() ==
           kb::Status::Unsupported); // no periodic read: no I/O behind the app
  KB_CHECK_EQ(r.bus.writes, 0U);
}

KB_TEST("se05x: no IoT applet fails the start (Fault, Counterfeit), an absent chip too (NoDevice); "
        "retries work") {
  GuardedRig<> r{Quick()};
  r.chip.chip.knobs.no_applet = true;
  const kb::Result<void> first = r.io.EnsureStarted(kWait);
  KB_CHECK(first.Error() == kb::Status::Fault);
  KB_CHECK_EQ(first.Detail(), kb::handlers::DetailOf(se05x::Error::Counterfeit));
  const kb::handlers::HealthRecord h = r.io.Health().Value();
  KB_CHECK(h.state == HealthState::Faulted);
  KB_CHECK_EQ(h.start_failures, 1U);
  KB_CHECK(!r.io.Startup().Value().complete);
  KB_CHECK_EQ(r.io.Startup().Value().stage, 0U);
  r.chip.chip.knobs.no_applet = false;
  r.bus.absent = true;
  const kb::Result<void> second = r.io.EnsureStarted(kWait);
  KB_CHECK(second.Error() == kb::Status::NoDevice);
  KB_CHECK_EQ(second.Detail(), kb::handlers::DetailOf(se05x::Error::Nack));
  r.bus.absent = false;
  KB_CHECK(r.io.EnsureStarted(kWait).IsOk()); // the plan runs again from the first stage
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Ready);
  KB_CHECK_EQ(r.io.Startup().Value().runs, 3U);
}

KB_TEST("se05x: platform SCP03 required: no key handles is NotReady (WrongState); with handles it "
        "opens") {
  hs::Config cfg = Quick();
  cfg.chip.scp_policy = se05x::ScpPolicy::Require;
  GuardedRig<> none{cfg};
  const kb::Result<void> refused = none.io.EnsureStarted(kWait);
  KB_CHECK(refused.Error() == kb::Status::NotReady);
  KB_CHECK_EQ(refused.Detail(), kb::handlers::DetailOf(se05x::Error::WrongState));
  KB_CHECK(!none.chip.scp.platform_open);

  GuardedRig<> r{cfg, sm::kSe050E, true};
  KB_REQUIRE(r.io.EnsureStarted(kWait).IsOk());
  KB_CHECK(r.chip.scp.platform_open);
  const hs::DeviceStatus st = hs::ReadStatus(r.io, kWait).Value();
  KB_CHECK(st.platform_scp_active);
  KB_CHECK(st.answers_authenticated);
  KB_CHECK(st.traffic_confidential);
  const uint32_t ctr = r.chip.scp.platform_ctr;
  std::array<uint8_t, 32> rnd{};
  KB_CHECK(hs::GetRandom(r.io, rnd, kWait).IsOk());
  KB_CHECK(r.chip.scp.platform_ctr > ctr); // the command travelled inside platform SCP03
}

KB_TEST("se05x: the Verify stage refuses a part the product does not expect and accepts the "
        "expected one") {
  hs::Config cfg = Quick();
  cfg.expect.enabled = true;
  cfg.expect.family = se05x::Family::Se052F;
  GuardedRig<> wrong{cfg};
  const kb::Result<void> refused = wrong.io.EnsureStarted(kWait);
  KB_CHECK(refused.Error() == kb::Status::Fault);
  KB_CHECK_EQ(refused.Detail(), kb::handlers::DetailOf(se05x::Error::DeviceIdMismatch));
  const kb::handlers::StartupReport rep = wrong.io.Startup().Value();
  KB_CHECK_EQ(rep.stage, 1U);
  KB_CHECK_EQ(rep.kinds, kb::handlers::stage::kVerify);
  KB_CHECK(wrong.chip.t1.session_ends >= 1U); // the abort closed the link session

  cfg.expect.family = se05x::Family::Se050E;
  cfg.expect.min_applet = se05x::AppletVersion{7U, 2U, 0U};
  cfg.expect.applet_locked = hs::Expect::No;
  cfg.expect.platform_scp_required = hs::Expect::No;
  GuardedRig<> right{cfg};
  KB_REQUIRE(right.io.EnsureStarted(kWait).IsOk());
  KB_CHECK_EQ(right.io.Startup().Value().stage, 1U);
  cfg.expect.authenticated_answers = true; // a plain bus has no R-MAC
  GuardedRig<> plain{cfg};
  KB_CHECK_EQ(plain.io.EnsureStarted(kWait).Detail(),
              kb::handlers::DetailOf(se05x::Error::WrongState));
}

KB_TEST("se05x: random bytes, the unique id, a verified attested read and the alive check") {
  GuardedRig<> r{Quick()};
  KB_REQUIRE(r.io.EnsureStarted(kWait).IsOk());
  std::array<uint8_t, 32> a{};
  std::array<uint8_t, 32> b{};
  KB_REQUIRE(hs::GetRandom(r.io, a, kWait).IsOk());
  KB_REQUIRE(hs::GetRandom(r.io, b, kWait).IsOk());
  KB_CHECK(a != b);
  std::array<uint8_t, 300> too_many{};
  KB_CHECK(hs::GetRandom(r.io, too_many, kWait).Error() ==
           kb::Status::InvalidArgument); // above kMaxData
  const kb::Result<std::array<uint8_t, hs::kUniqueIdLength>> uid = hs::ReadUniqueId(r.io, kWait);
  KB_REQUIRE(uid.IsOk());
  KB_CHECK_EQ(uid.Value()[0], 0xC0U);
  // The attestation key's own public key, read with an attestation by that key.
  std::array<uint8_t, hs::kPointLength> key{};
  const kb::Result<hs::Attestation> proof =
      hs::ReadAttested(r.io, kAttestKey, ModelAttester(r.chip.chip), key, kWait);
  KB_REQUIRE(proof.IsOk());
  KB_CHECK_EQ(proof.Value().length, hs::kPointLength);
  KB_CHECK(key == r.chip.chip.AttestPublic());
  KB_CHECK(proof.Value().unique_id == uid.Value()); // the chip that signed is the chip read plainly
  std::array<uint8_t, 16> small{};
  KB_CHECK(hs::ReadAttested(r.io, kAttestKey, ModelAttester(r.chip.chip), small, kWait).Error() ==
           kb::Status::Overflow);
  hs::Attester forged = ModelAttester(r.chip.chip);
  se05x::KeyRef other{}; // a valid point, not the attestation key
  KB_REQUIRE(r.crypto.GenerateP256(other, std::span<uint8_t, hs::kPointLength>{
                                              forged.trusted_public}) == se05x::CryptoStatus::Ok);
  const kb::Result<hs::Attestation> bad = hs::ReadAttested(r.io, kAttestKey, forged, key, kWait);
  KB_CHECK(bad.Error() == kb::Status::Fault);
  KB_CHECK_EQ(bad.Detail(), kb::handlers::DetailOf(se05x::Error::AttestationInvalid));
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Degraded);
  const kb::Result<se05x::AppletVersion> alive = hs::CheckAlive(r.io, kWait);
  KB_REQUIRE(alive.IsOk());
  KB_CHECK_EQ(alive.Value().minor, 2U);
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Ready);
  hs::Command exists{};
  exists.op = hs::Op::ObjectExists;
  exists.id = kAttestKey;
  const kb::Result<hs::Reply> e = r.io.Request(exists, kWait);
  KB_REQUIRE(e.IsOk());
  KB_CHECK(e.Value().flag);
}

KB_TEST(
    "se05x: sign and verify by key id, in the default session and in an AESKey session per call") {
  GuardedRig<> r{Quick()};
  KB_REQUIRE(r.io.EnsureStarted(kWait).IsOk());
  std::array<uint8_t, 32> digest{};
  for (std::size_t i = 0U; i < digest.size(); ++i) {
    digest[i] = static_cast<uint8_t>(i * 7U);
  }
  const hs::SignWith with{kAttestKey, se05x::EcSignAlgo::Sha256};
  std::array<uint8_t, hs::kMaxSignature> sig{};
  const kb::Result<std::size_t> n = hs::Sign(r.io, with, digest, sig, kWait);
  KB_REQUIRE(n.IsOk());
  KB_CHECK(n.Value() >= 8U);
  const std::span<const uint8_t> der = std::span<const uint8_t>{sig}.first(n.Value());
  const kb::Result<bool> good = hs::Verify(r.io, with, digest, der, kWait);
  KB_REQUIRE(good.IsOk());
  KB_CHECK(good.Value());
  digest[0] = static_cast<uint8_t>(digest[0] ^ 0x01U);
  const kb::Result<bool> wrong = hs::Verify(r.io, with, digest, der, kWait);
  KB_REQUIRE(wrong.IsOk()); // an invalid signature is an answer, not a failure
  KB_CHECK(!wrong.Value());

  se05x::KeyRef aes{};
  KB_REQUIRE(r.crypto.ImportKey(se05x::KeyKind::Aes128, se05x::ByteSpan{sm::kAesKey}, aes) ==
             se05x::CryptoStatus::Ok);
  const hs::Session session = hs::AesKeySession(se05x::ObjectId{sm::kAesCredential}, aes);
  const uint32_t before = hs::ReadStatus(r.io, kWait).Value().apdu_count;
  KB_CHECK(hs::Sign(r.io, with, digest, sig, kWait).IsOk());
  const uint32_t plain = hs::ReadStatus(r.io, kWait).Value().apdu_count - before;
  for (std::size_t i = 0U; i < 2U; ++i) { // the same key handle serves every call
    const uint32_t at = hs::ReadStatus(r.io, kWait).Value().apdu_count;
    KB_CHECK(hs::Sign(r.io, with, digest, sig, kWait, session).IsOk());
    // CreateSession, the SCP03 handshake and CloseSession around the one signature.
    KB_CHECK(hs::ReadStatus(r.io, kWait).Value().apdu_count - at >= plain + 3U);
  }
  KB_CHECK(hs::ReadStatus(r.io, kWait).Value().state ==
           se05x::DeviceState::Ready); // closed after each call
  se05x::KeyRef other{};
  const std::array<uint8_t, 16> not_the_key{};
  KB_REQUIRE(r.crypto.ImportKey(se05x::KeyKind::Aes128, se05x::ByteSpan{not_the_key}, other) ==
             se05x::CryptoStatus::Ok);
  const kb::Result<std::size_t> refused =
      hs::Sign(r.io, with, digest, sig, kWait,
               hs::AesKeySession(se05x::ObjectId{sm::kAesCredential}, other));
  KB_CHECK(!refused.IsOk());
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Degraded); // the chip stays usable: not fatal
  KB_CHECK(hs::Sign(r.io, with, digest, sig, kWait).IsOk());
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Ready);
}

KB_TEST("se05x: a certificate-sized object is read whole in chunks; a small buffer is Overflow") {
  GuardedRig<> r{Quick()};
  std::array<uint8_t, 600> cert{};
  for (std::size_t i = 0U; i < cert.size(); ++i) {
    cert[i] = static_cast<uint8_t>((i * 13U) + 1U);
  }
  { // provisioning, before the handler runs: a device of its own over the same chip model
    se05x::ReferenceCrypto crypto{&sm::CountingEntropy};
    se05x::Ports<sm::T1Target, sm::SimClock, se05x::ReferenceCrypto> ports(r.chip.t1, r.chip.clock,
                                                                           crypto);
    se05x::Device<decltype(ports)> device(ports, se05x::Config{});
    KB_REQUIRE(device.Open().ok());
    const se05x::Result<se05x::Policy> policy =
        se05x::PolicyBuilder<se05x::AppletGeneration::V7, se05x::ObjectClass::BinaryFile,
                             se05x::Profile::Strict>{}
            .ForAllOthers()
            .Allow<se05x::Rule::Read>()
            .Allow<se05x::Rule::Delete>() // a file nobody can delete is irreversible
            .Build();
    KB_REQUIRE(policy.ok());
    const se05x::Result<void> wrote =
        device.WriteBinary(kCertificate, policy.value, 0U, se05x::ByteSpan{cert}, 600U);
    KB_REQUIRE(wrote.ok());
    static_cast<void>(device.Close());
  }
  KB_REQUIRE(r.io.EnsureStarted(kWait).IsOk());
  std::array<uint8_t, 700> out{};
  const kb::Result<std::size_t> got = hs::ReadObjectAll(r.io, kCertificate, out, kWait);
  KB_REQUIRE(got.IsOk());
  KB_CHECK_EQ(got.Value(), 600U);
  bool same = true;
  for (std::size_t i = 0U; i < cert.size(); ++i) {
    same = same && (out[i] == cert[i]);
  }
  KB_CHECK(same);
  std::array<uint8_t, 100> small{};
  const kb::Result<std::size_t> overflow = hs::ReadObjectAll(r.io, kCertificate, small, kWait);
  KB_CHECK(overflow.Error() == kb::Status::Overflow);
  KB_CHECK_EQ(overflow.Detail(), kb::handlers::DetailOf(se05x::Error::BufferTooSmall));
  std::array<uint8_t, 4> part{};
  KB_REQUIRE(hs::ReadObject(r.io, kCertificate, 256U, part, kWait).IsOk());
  KB_CHECK_EQ(part[0], cert[256]);
}

KB_TEST("se05x: chip errors degrade, the threshold faults, calls send nothing, Recover() restores "
        "Ready") {
  GuardedRig<> r{Quick()};
  KB_REQUIRE(r.io.EnsureStarted(kWait).IsOk());
  r.chip.chip.knobs.inject_sw = 0x6F00U; // "no precise diagnosis": DeviceFault
  r.chip.chip.knobs.inject_count = 1U;
  const kb::Result<se05x::AppletVersion> bad = hs::CheckAlive(r.io, kWait);
  KB_CHECK(bad.Error() == kb::Status::Fault);
  KB_CHECK_EQ(bad.Detail(), kb::handlers::DetailOf(se05x::Error::DeviceFault));
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Degraded);
  KB_CHECK_EQ(hs::ReadStatus(r.io, kWait).Value().last_sw,
              0x6F00U); // the status word, for diagnosis
  KB_CHECK(hs::CheckAlive(r.io, kWait).IsOk());
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Ready);
  r.chip.chip.knobs.inject_count = 2U;
  KB_CHECK(!hs::CheckAlive(r.io, kWait).IsOk());
  KB_CHECK(!hs::CheckAlive(r.io, kWait).IsOk());
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Faulted);
  const uint32_t writes = r.bus.writes;
  KB_CHECK(hs::CheckAlive(r.io, kWait).Error() == kb::Status::NotReady);
  KB_CHECK_EQ(r.bus.writes, writes); // calls never restart a faulted chip
  KB_CHECK(r.io.RequestRecovery(kWait).IsOk());
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Ready);
  KB_CHECK_EQ(r.io.Health().Value().recoveries, 1U);
  KB_CHECK(hs::CheckAlive(r.io, kWait).IsOk());
}

KB_TEST("se05x: a stuck bus without the reset line is fatal at once; recovery succeeds once the "
        "bus is free") {
  GuardedRig<> r{Quick()};
  KB_REQUIRE(r.io.EnsureStarted(kWait).IsOk());
  r.bus.stuck = true;
  const kb::Result<se05x::AppletVersion> stuck = hs::CheckAlive(r.io, kWait);
  KB_CHECK(!stuck.IsOk());
  KB_CHECK(stuck.Error() == kb::Status::BusError);
  KB_CHECK_EQ(stuck.Detail(), kb::handlers::DetailOf(se05x::Error::BusStuck));
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Faulted); // one failure: the driver's state is lost
  KB_CHECK(!r.io.RequestRecovery(kWait).IsOk());          // still stuck: the plan fails again
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Faulted);
  r.bus.stuck = false; // the board power-cycled the chip
  r.chip.t1.PowerCycleChip();
  kb::rtos::SleepFor(kb::Milliseconds{5U}); // past the backoff
  KB_CHECK(r.io.RequestRecovery(kWait).IsOk());
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Ready);
  KB_CHECK(hs::CheckAlive(r.io, kWait).IsOk());
}

KB_TEST("se05x: with the reset line, the driver power-cycles a stuck bus through the binding; "
        "recovery restores Ready") {
  GuardedRig<GuardedResetIo> r{Quick()};
  KB_REQUIRE(r.io.EnsureStarted(kWait).IsOk());
  r.bus.stuck = true;
  const kb::Result<se05x::AppletVersion> first = hs::CheckAlive(r.io, kWait);
  // The driver's ladder power-cycles through the run line, then needs the applet selected again.
  KB_CHECK(first.Error() == kb::Status::NotReady);
  KB_CHECK_EQ(first.Detail(), kb::handlers::DetailOf(se05x::Error::NeedsReselect));
  KB_CHECK_EQ(r.line.cycles, 1U);
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Faulted);
  KB_CHECK(r.io.RequestRecovery(kWait).IsOk());
  KB_CHECK(r.line.cycles >= 1U); // the run line went low and high again
  KB_CHECK(!r.bus.stuck);
  KB_CHECK(kb_it::StateOf(r.io) == HealthState::Ready);
  KB_CHECK(hs::CheckAlive(r.io, kWait).IsOk());
}

KB_TEST("se05x: owner model: the owner task starts the chip and serves requests") {
  kb_it::Timing t{};
  Chip chip{sm::kSe050E};
  ModelBus bus{chip};
  se05x::ReferenceCrypto crypto{&sm::CountingEntropy};
  OwnerBus domain{bus};
  OwnedIo io{domain, bus, crypto, t.timing, Quick()};
  KB_REQUIRE(io.Create().IsOk());
  const kb_it::OwnerLoop<OwnerBus, OwnedIo> owner{domain, io};
  KB_REQUIRE(io.EnsureStarted(kWait).IsOk());
  std::array<uint8_t, 16> rnd{};
  KB_CHECK(hs::GetRandom(io, rnd, kWait).IsOk());
  const hs::SignWith with{kAttestKey, se05x::EcSignAlgo::Sha256};
  const std::array<uint8_t, 32> digest{0x01U};
  std::array<uint8_t, hs::kMaxSignature> sig{};
  const kb::Result<std::size_t> n = hs::Sign(io, with, digest, sig, kWait);
  KB_REQUIRE(n.IsOk());
  const kb::Result<bool> ok =
      hs::Verify(io, with, digest, std::span<const uint8_t>{sig}.first(n.Value()), kWait);
  KB_CHECK(ok.IsOk() && ok.Value());
  KB_CHECK(io.Liveness().Beats() > 0U);
  KB_CHECK(io.Stop(kWait).IsOk());
  KB_CHECK(kb_it::StateOf(io) == HealthState::Unknown);
  KB_CHECK(hs::ReadStatus(io, kWait).Error() == kb::Status::NotReady);
}

} // namespace
