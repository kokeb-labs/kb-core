/**
 * @file canopen_client.hpp
 * @brief The canopen_client binding and handler: a CANopen client (NMT master, SDO client,
 * heartbeat and EMCY consumer, SYNC producer, PDO and CiA 402 helpers) on one CAN bus.
 *
 * | Piece | What |
 * | --- | --- |
 * | Port | `Port<Can>`: the driver's full client port over a kb::iface::CanBus, written here (no
 * CAN hook base): start, send, receive with a timeout (extended frames dropped), the 32-bit
 * microsecond clock, transfer counters | | Errors | `ToStatus()`: every `canopen_client::Error` to
 * a kb::Status; the code in the health record's detail | | Start-up | `Link` =
 * `driver.EnsureInitialized()` (sync: configuration checked, controller started); `Configure`
 * (sync, optional) = the watched nodes, the TPDO subscriptions, the SYNC period, and an NMT start
 * when the configuration asks | | Thread model | Owner only (`kRequiresOwner = true`): heartbeats,
 * EMCY and TPDOs arrive unasked and must be drained continuously | | Health | `NotInitialized`
 * faults at once; every other failure (a send the controller refused, a receive error, an SDO
 * timeout or abort) counts to the threshold | | App API | snapshot `{watched nodes, TPDO samples,
 * last EMCY and EMCY count, receive counters, last SDO abort}`; commands: NMT, SDO read and write,
 * node guarding, CiA 402 enable, disable, quick stop, fault reset, state, mode, SYNC, RPDO |
 *
 * Each `Service()` of the owner task drains up to `Config::frames_per_service` received frames
 * (`Poll(0)`), produces the SYNC when one is due, takes the queued EMCY messages and publishes the
 * snapshot. A command runs on the owner task and blocks it for as long as the driver waits (an SDO
 * reply, a drive state change), each wait bounded by the driver configuration.
 *
 * @code
 * kb::handlers::BusDomain<Can, kb::handlers::ThreadModel::Owner> can0{can};
 * kb::handlers::OwnerAccess<kb::handlers::canopen_client::Handler<Can>, decltype(can0)> co{can0,
 * can, timing, cfg};
 * // owner task: can0.Service(now, co) every step
 * const kb::Result<uint32_t> type = kb::handlers::canopen_client::SdoRead(co, 5U, 0x1000U, 0U, 4U,
 * kb::Milliseconds{300U});
 * @endcode
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 The kb-core authors
 */
#ifndef KB_HANDLERS_BINDINGS_CANOPEN_CLIENT_HPP
#define KB_HANDLERS_BINDINGS_CANOPEN_CLIENT_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include "kb/handlers/handlers.hpp"
#include <canopen_client.hpp>

namespace kb::handlers::canopen_client {

/** @brief The most nodes a handler watches (heartbeat consumer entries it publishes). */
inline constexpr std::size_t kMaxNodes = 8U;
/** @brief The most TPDO identifiers a handler subscribes and publishes. */
inline constexpr std::size_t kMaxTpdos = 4U;

/** @brief The one map of the driver's errors to kb::Status (every enumerator, no default). */
[[nodiscard]] constexpr kb::Status ToStatus(::canopen_client::Error error) noexcept {
  kb::Status s = kb::Status::Fault;
  switch (error) {
  case ::canopen_client::Error::Ok:
    s = kb::Status::Ok;
    break;
  case ::canopen_client::Error::NotInitialized:
  case ::canopen_client::Error::NodeState:
    s = kb::Status::NotReady;
    break;
  case ::canopen_client::Error::InvalidArgument:
    s = kb::Status::InvalidArgument;
    break;
  case ::canopen_client::Error::Bus:
    s = kb::Status::BusError;
    break;
  case ::canopen_client::Error::Timeout:
    s = kb::Status::Timeout;
    break;
  case ::canopen_client::Error::Crc:
    s = kb::Status::CrcMismatch;
    break;
  case ::canopen_client::Error::Busy:
    s = kb::Status::Busy;
    break;
  case ::canopen_client::Error::Unsupported:
    s = kb::Status::Unsupported;
    break;
  case ::canopen_client::Error::BufferTooSmall:
    s = kb::Status::Overflow;
    break;
  case ::canopen_client::Error::DeviceIdMismatch:
  case ::canopen_client::Error::DeviceFault:
  case ::canopen_client::Error::SdoAbort:
  case ::canopen_client::Error::Protocol:
  case ::canopen_client::Error::InvalidFrame:
  case ::canopen_client::Error::DriveFault:
    s = kb::Status::Fault;
    break;
  }
  return s;
}

/** @brief True for a driver error after which the client has lost its state (fault at once). */
[[nodiscard]] constexpr bool IsFatalError(::canopen_client::Error error) noexcept {
  return error == ::canopen_client::Error::NotInitialized;
}

/** @brief The driver's frame as a kb frame (classic, 11-bit). */
[[nodiscard]] constexpr kb::iface::CanFrame ToKbFrame(
    const ::canopen_client::CanFrame& in) noexcept {
  kb::iface::CanFrame out{};
  out.id = static_cast<uint32_t>(in.id);
  out.extended = false;
  out.remote = in.rtr;
  out.dlc = in.dlc;
  out.data = in.data;
  return out;
}

/**
 * @brief The driver's full client port over a kb::iface::CanBus.
 * @tparam Can The controller (a CanBus or CanBusRef).
 */
template <kb::iface::CanBus Can>
class Port : public ::canopen_client::CanInterface<Port<Can>> {
public:
  /** @brief Over `can` with `timing`; a send waits at most `send_timeout` for a slot. No I/O. */
  Port(Can& can, const HookTiming& timing, kb::Milliseconds send_timeout) noexcept
      : can_(can), timing_(timing), send_timeout_(send_timeout) {}
  ~Port() = default;
  Port(const Port&) = delete;
  Port& operator=(const Port&) = delete;
  Port(Port&&) = delete;
  Port& operator=(Port&&) = delete;

  /** @brief Hook: start the controller. */
  bool EnsureInitialized() noexcept {
    return recorder_.Record(*this, can_.EnsureReady());
  }
  /** @brief Hook: queue one frame. */
  bool Send(const ::canopen_client::CanFrame& frame) noexcept {
    return recorder_.Record(*this, can_.Send(ToKbFrame(frame), send_timeout_));
  }
  /** @brief Hook: one frame within `timeout_us` (0 polls); extended or oversized frames dropped. */
  ::canopen_client::RxStatus Receive(::canopen_client::CanFrame& frame,
                                     uint32_t timeout_us) noexcept {
    kb::iface::CanFrame in{};
    const kb::Status s =
        can_.Receive(in, kb::utils::ToMillisecondsCeil(kb::Microseconds{timeout_us}));
    ::canopen_client::RxStatus out = ::canopen_client::RxStatus::Timeout;
    if (s == kb::Status::Timeout) {
      out = ::canopen_client::RxStatus::Timeout;
    } else if (!recorder_.Record(*this, s)) {
      out = ::canopen_client::RxStatus::Error;
    } else if (in.extended || (in.id > ::canopen_client::kMaxCanId) ||
               (in.dlc > kb::iface::kCanMaxDlc)) {
      out = ::canopen_client::RxStatus::Timeout; // not a frame of this network: dropped
    } else {
      frame.id = static_cast<uint16_t>(in.id);
      frame.dlc = in.dlc;
      frame.rtr = in.remote;
      frame.data = in.data;
      out = ::canopen_client::RxStatus::Frame;
    }
    return out;
  }
  /** @brief Hook: the microsecond clock (wraps at 2^32). */
  uint32_t NowUs() noexcept {
    return timing_.NowUs32();
  }
  /** @brief The transfer counters. */
  [[nodiscard]] const HookCounters& Counters() const noexcept {
    return recorder_.Counters();
  }

private:
  Can& can_;
  HookTiming timing_;
  kb::Milliseconds send_timeout_;
  HookRecorder<Port<Can>> recorder_{};
};

/** @brief The driver type a handler over `Can` owns. */
template <kb::iface::CanBus Can>
using Driver = ::canopen_client::Client<Port<Can>>;

/** @brief One node the handler watches: its heartbeat timeout. */
struct NodeWatch {
  uint8_t node{1U};                       ///< 1 to 127.
  uint32_t heartbeat_timeout_us{500000U}; ///< How old a heartbeat may be for `alive`.
};

/** @brief One TPDO identifier the handler subscribes. */
struct TpdoWatch {
  uint16_t cob_id{0x181U}; ///< The identifier.
  uint8_t dlc{8U};         ///< The length every frame on it has.
};

/** @brief The handler's configuration: board values from the HAL. */
struct Config {
  ::canopen_client::Config chip{};          ///< SDO, guarding and drive timeouts.
  HandlerConfig handler{};                  ///< Health, recovery, polling, sample period.
  kb::Milliseconds link_deadline{20U};      ///< Bound of the Link stage.
  kb::Milliseconds configure_deadline{50U}; ///< Bound of the Configure stage.
  kb::Milliseconds send_timeout{2U};        ///< Longest wait for a transmit slot.
  std::array<NodeWatch, kMaxNodes> nodes{}; ///< The nodes watched.
  uint8_t node_count{0U};                   ///< Entries of `nodes` in use.
  std::array<TpdoWatch, kMaxTpdos> tpdos{}; ///< The TPDOs subscribed.
  uint8_t tpdo_count{0U};                   ///< Entries of `tpdos` in use.
  uint32_t sync_period_us{0U};              ///< SYNC producer period; 0 = none.
  uint8_t sync_counter_overflow{0U};        ///< SYNC counter overflow; 0 = no counter.
  bool start_nodes{false};                  ///< NMT Start to every node at start-up.
  uint8_t frames_per_service{16U};          ///< Received frames routed per sample, at most.
};

/** @brief What a sample publishes. */
struct Snapshot {
  std::array<::canopen_client::NodeStatus, kMaxNodes> nodes{}; ///< In `Config::nodes` order.
  std::array<::canopen_client::PdoSample, kMaxTpdos> tpdos{};  ///< In `Config::tpdos` order.
  ::canopen_client::EmcyEvent last_emcy{};                     ///< The newest EMCY taken.
  ::canopen_client::Stats stats{};                             ///< Receive path counters.
  ::canopen_client::SdoAbortInfo last_abort{};                 ///< The last SDO abort.
  uint32_t emcy_count{0U};                                     ///< EMCY messages taken so far.
  uint8_t node_count{0U};                                      ///< Entries of `nodes` filled.
  uint8_t tpdo_count{0U};                                      ///< Entries of `tpdos` filled.
};

/** @brief The commands of the handler. */
enum class Op : uint8_t {
  SendNmt = 0,         ///< NMT `nmt` to `node` (0 = every node).
  SdoRead = 1,         ///< Object `index:sub` of `node`, `size` 1, 2 or 4 bytes.
  SdoWrite = 2,        ///< `value` to object `index:sub` of `node`, `size` 1, 2 or 4 bytes.
  GuardNode = 3,       ///< One node-guarding exchange with `node`.
  ReadDriveState = 4,  ///< The CiA 402 state of `node`.
  DriveEnable = 5,     ///< Walk `node` to Operation enabled.
  DriveDisable = 6,    ///< Disable the voltage of `node` (the switch-off).
  DriveQuickStop = 7,  ///< Quick stop of `node`.
  DriveResetFault = 8, ///< Fault reset of `node`.
  SetDriveMode = 9,    ///< Mode of operation of `node` to `mode`.
  SendSync = 10,       ///< One SYNC frame.
  SendRpdo = 11,       ///< `len` bytes of `data` on `cob_id`.
  ClearBootUp = 12,    ///< Forget the boot-up seen from `node`.
};

/** @brief One command. */
struct Command {
  std::array<uint8_t, 8> data{}; ///< For SendRpdo.
  uint32_t value{0U};            ///< For SdoWrite.
  uint16_t index{0U};            ///< Object index for the SDO calls.
  uint16_t cob_id{0U};           ///< For SendRpdo.
  Op op{Op::SendSync};           ///< What to do.
  uint8_t node{0U};              ///< The node.
  uint8_t sub{0U};               ///< Object sub-index for the SDO calls.
  uint8_t size{4U};              ///< Object size for the SDO calls: 1, 2 or 4.
  uint8_t len{0U};               ///< For SendRpdo: bytes of `data`, 1 to 8.
  ::canopen_client::NmtCommand nmt{::canopen_client::NmtCommand::Start}; ///< For SendNmt.
  ::canopen_client::OperationMode mode{
      ::canopen_client::OperationMode::NoMode}; ///< For SetDriveMode.
};

/** @brief The reply of a command. */
struct Reply {
  uint32_t value{0U};                                                        ///< SdoRead.
  ::canopen_client::NmtState nmt{::canopen_client::NmtState::BootUp};        ///< GuardNode.
  ::canopen_client::DriveState drive{::canopen_client::DriveState::Unknown}; ///< ReadDriveState.
};

/**
 * @brief The canopen_client handler: one CAN bus, served by its owner task.
 * @tparam Can The controller (a CanBus or CanBusRef).
 */
template <kb::iface::CanBus Can>
class Handler : public HandlerCore<Handler<Can>, Driver<Can>, Snapshot, Command, Reply> {
  using Core = HandlerCore<Handler<Can>, Driver<Can>, Snapshot, Command, Reply>;

public:
  /// The start-up plan: the controller up, then the consumer tables and the NMT start (optional).
  static constexpr std::array<Stage, 2> kStartupPlan{
      {{stage::kLink, StageMode::Sync, false}, {stage::kConfigure, StageMode::Sync, true}}};
  static constexpr bool kRequiresOwner = true;  ///< Received frames must be drained continuously.
  static constexpr BusKind kBus = BusKind::Can; ///< On a CAN bus.

  /** @brief Stores references and the configuration; no I/O. */
  Handler(Can& can, const HookTiming& timing, const Config& config) noexcept
      : Core(config.handler), port_(can, timing, config.send_timeout), config_(config) {}

  /// @name Binding hooks (called by HandlerCore)
  /// @{
  void EmplaceDriver(std::optional<Driver<Can>>& slot) noexcept {
    emcy_count_ = 0U;
    last_emcy_ = ::canopen_client::EmcyEvent{};
    slot.emplace(port_, config_.chip);
  }
  [[nodiscard]] bool StageEnabled(std::size_t index) const noexcept {
    const bool tables = (nodeCount() != 0U) || (tpdoCount() != 0U);
    return (index == 0U) || tables || config_.start_nodes || (config_.sync_period_us != 0U);
  }
  [[nodiscard]] kb::Result<StartProgress> RunStage(const StageCall& call,
                                                   Driver<Can>& driver) noexcept {
    const kb::Result<void> r =
        (call.index == 0U) ? LiftStatus(driver.EnsureInitialized(), &ToStatus) : configure(driver);
    return r.IsOk() ? kb::Result<StartProgress>::Success(StartProgress::Done)
                    : Forward<StartProgress>(r);
  }
  [[nodiscard]] kb::Milliseconds StageDeadline(std::size_t index) const noexcept {
    return (index == 0U) ? config_.link_deadline : config_.configure_deadline;
  }
  [[nodiscard]] kb::Result<void> StopDriver(Driver<Can>& driver) noexcept {
    return LiftStatus(driver.Deinitialize(), &ToStatus);
  }
  [[nodiscard]] bool IsFatal(kb::Status status, uint32_t detail) const noexcept {
    static_cast<void>(status);
    return detail == DetailOf(::canopen_client::Error::NotInitialized);
  }
  [[nodiscard]] kb::Result<Snapshot> Measure(Driver<Can>& driver, kb::Instant now) noexcept {
    static_cast<void>(now);
    kb::Result<void> r = drain(driver);
    if (r.IsOk() && (config_.sync_period_us != 0U)) {
      const kb::Result<bool> sync = Lift(driver.ServiceSync(), &ToStatus);
      r = sync.IsOk() ? r : Forward<void>(sync);
    }
    takeEmcy(driver);
    return r.IsOk() ? kb::Result<Snapshot>::Success(snapshot(driver)) : Forward<Snapshot>(r);
  }
  [[nodiscard]] kb::Result<Reply> Run(Driver<Can>& driver, const Command& c,
                                      kb::Instant now) noexcept {
    static_cast<void>(now);
    kb::Result<Reply> result = kb::Result<Reply>::Failure(kb::Status::InvalidArgument);
    switch (c.op) {
    case Op::SendNmt:
      result = done(LiftStatus(driver.SendNmt(c.nmt, c.node), &ToStatus));
      break;
    case Op::SdoRead:
      result = sdoRead(driver, c);
      break;
    case Op::SdoWrite:
      result = sdoWrite(driver, c);
      break;
    case Op::GuardNode:
      result = nmtReply(Lift(driver.GuardNode(c.node), &ToStatus));
      break;
    case Op::ReadDriveState:
      result = driveReply(Lift(driver.ReadDriveState(c.node), &ToStatus));
      break;
    case Op::DriveEnable:
      result = done(LiftStatus(driver.DriveEnable(c.node), &ToStatus));
      break;
    case Op::DriveDisable:
      result = done(LiftStatus(driver.DriveDisable(c.node), &ToStatus));
      break;
    case Op::DriveQuickStop:
      result = done(LiftStatus(driver.DriveQuickStop(c.node), &ToStatus));
      break;
    case Op::DriveResetFault:
      result = done(LiftStatus(driver.DriveResetFault(c.node), &ToStatus));
      break;
    case Op::SetDriveMode:
      result = done(LiftStatus(driver.SetDriveMode(c.node, c.mode), &ToStatus));
      break;
    case Op::SendSync:
      result = done(LiftStatus(driver.SendSync(), &ToStatus));
      break;
    case Op::SendRpdo:
      result = rpdo(driver, c);
      break;
    case Op::ClearBootUp:
      result = done(LiftStatus(driver.ClearBootUp(c.node), &ToStatus));
      break;
    }
    return result;
  }
  /// @}

  /** @brief The port's transfer counters. */
  [[nodiscard]] const HookCounters& PortCounters() const noexcept {
    return port_.Counters();
  }

private:
  [[nodiscard]] std::size_t nodeCount() const noexcept {
    const std::size_t n = static_cast<std::size_t>(config_.node_count);
    return (n > kMaxNodes) ? kMaxNodes : n;
  }
  [[nodiscard]] std::size_t tpdoCount() const noexcept {
    const std::size_t n = static_cast<std::size_t>(config_.tpdo_count);
    return (n > kMaxTpdos) ? kMaxTpdos : n;
  }

  // The Configure stage: the consumer tables (no traffic), the SYNC period, the NMT start.
  [[nodiscard]] kb::Result<void> configure(Driver<Can>& driver) noexcept {
    kb::Result<void> r = kb::Result<void>::Success();
    for (std::size_t i = 0U; i < nodeCount(); ++i) {
      if (r.IsOk()) {
        const NodeWatch& w = config_.nodes[i];
        r = LiftStatus(driver.MonitorNode(w.node, w.heartbeat_timeout_us), &ToStatus);
      }
    }
    for (std::size_t i = 0U; i < tpdoCount(); ++i) {
      if (r.IsOk()) {
        r = LiftStatus(driver.SubscribeTpdo(config_.tpdos[i].cob_id, config_.tpdos[i].dlc),
                       &ToStatus);
      }
    }
    if (r.IsOk() && (config_.sync_period_us != 0U)) {
      r = LiftStatus(driver.ConfigureSync(config_.sync_period_us, config_.sync_counter_overflow),
                     &ToStatus);
    }
    if (r.IsOk() && config_.start_nodes) {
      r = LiftStatus(
          driver.SendNmt(::canopen_client::NmtCommand::Start, ::canopen_client::kBroadcastNode),
          &ToStatus);
    }
    return r;
  }

  // Routes what arrived: at most frames_per_service frames, until none is waiting.
  [[nodiscard]] kb::Result<void> drain(Driver<Can>& driver) noexcept {
    kb::Result<void> r = kb::Result<void>::Success();
    bool more = true;
    for (std::size_t i = 0U; i < static_cast<std::size_t>(config_.frames_per_service); ++i) {
      if (more) {
        const kb::Result<::canopen_client::FrameClass> f = Lift(driver.Poll(0U), &ToStatus);
        r = f.IsOk() ? r : Forward<void>(f);
        more = f.IsOk() && (f.Value() != ::canopen_client::FrameClass::None);
      }
    }
    return r;
  }

  void takeEmcy(Driver<Can>& driver) noexcept {
    ::canopen_client::EmcyEvent e{};
    for (std::size_t i = 0U; i < ::canopen_client::kEmcyQueueCapacity; ++i) {
      if (driver.PopEmcy(e)) {
        last_emcy_ = e;
        emcy_count_ = SaturatingIncrement(emcy_count_);
      }
    }
  }

  [[nodiscard]] Snapshot snapshot(Driver<Can>& driver) const noexcept {
    Snapshot s{};
    for (std::size_t i = 0U; i < nodeCount(); ++i) {
      s.nodes[i] = driver.GetNodeStatus(config_.nodes[i].node).value;
    }
    for (std::size_t i = 0U; i < tpdoCount(); ++i) {
      s.tpdos[i] = driver.ReadTpdo(config_.tpdos[i].cob_id).value;
    }
    s.node_count = static_cast<uint8_t>(nodeCount());
    s.tpdo_count = static_cast<uint8_t>(tpdoCount());
    s.last_emcy = last_emcy_;
    s.emcy_count = emcy_count_;
    s.stats = driver.GetStats();
    s.last_abort = driver.GetLastAbort();
    return s;
  }

  [[nodiscard]] static kb::Result<Reply> sdoRead(Driver<Can>& driver, const Command& c) noexcept {
    kb::Result<uint32_t> v = kb::Result<uint32_t>::Failure(kb::Status::InvalidArgument);
    if (c.size == 1U) {
      const kb::Result<uint8_t> r = Lift(driver.SdoReadU8(c.node, c.index, c.sub), &ToStatus);
      v = r.IsOk() ? kb::Result<uint32_t>::Success(static_cast<uint32_t>(r.Value()))
                   : Forward<uint32_t>(r);
    } else if (c.size == 2U) {
      const kb::Result<uint16_t> r = Lift(driver.SdoReadU16(c.node, c.index, c.sub), &ToStatus);
      v = r.IsOk() ? kb::Result<uint32_t>::Success(static_cast<uint32_t>(r.Value()))
                   : Forward<uint32_t>(r);
    } else if (c.size == 4U) {
      v = Lift(driver.SdoReadU32(c.node, c.index, c.sub), &ToStatus);
    } else {
      // Another size is not an expedited object of this API: InvalidArgument, nothing sent.
    }
    kb::Result<Reply> result = Forward<Reply>(v);
    if (v.IsOk()) {
      Reply reply{};
      reply.value = v.Value();
      result = kb::Result<Reply>::Success(reply);
    }
    return result;
  }

  [[nodiscard]] static kb::Result<Reply> sdoWrite(Driver<Can>& driver, const Command& c) noexcept {
    kb::Result<void> r = kb::Result<void>::Failure(kb::Status::InvalidArgument);
    constexpr uint32_t kU8 = 0xFFU;
    constexpr uint32_t kU16 = 0xFFFFU;
    if ((c.size == 1U) && (c.value <= kU8)) {
      r = LiftStatus(driver.SdoWriteU8(c.node, c.index, c.sub, static_cast<uint8_t>(c.value)),
                     &ToStatus);
    } else if ((c.size == 2U) && (c.value <= kU16)) {
      r = LiftStatus(driver.SdoWriteU16(c.node, c.index, c.sub, static_cast<uint16_t>(c.value)),
                     &ToStatus);
    } else if (c.size == 4U) {
      r = LiftStatus(driver.SdoWriteU32(c.node, c.index, c.sub, c.value), &ToStatus);
    } else {
      // A size the API does not offer, or a value that does not fit it: nothing sent.
    }
    return done(r);
  }

  [[nodiscard]] static kb::Result<Reply> rpdo(Driver<Can>& driver, const Command& c) noexcept {
    kb::Result<void> r = kb::Result<void>::Failure(kb::Status::InvalidArgument);
    const std::size_t len = static_cast<std::size_t>(c.len);
    if ((len >= 1U) && (len <= c.data.size())) {
      const std::span<const uint8_t> bytes = std::span<const uint8_t>{c.data}.first(len);
      r = LiftStatus(driver.SendRpdo(c.cob_id, bytes), &ToStatus);
    }
    return done(r);
  }

  [[nodiscard]] static kb::Result<Reply> done(const kb::Result<void>& r) noexcept {
    return r.IsOk() ? kb::Result<Reply>::Success(Reply{}) : Forward<Reply>(r);
  }
  [[nodiscard]] static kb::Result<Reply> nmtReply(
      const kb::Result<::canopen_client::NmtState>& r) noexcept {
    kb::Result<Reply> result = Forward<Reply>(r);
    if (r.IsOk()) {
      Reply reply{};
      reply.nmt = r.Value();
      result = kb::Result<Reply>::Success(reply);
    }
    return result;
  }
  [[nodiscard]] static kb::Result<Reply> driveReply(
      const kb::Result<::canopen_client::DriveState>& r) noexcept {
    kb::Result<Reply> result = Forward<Reply>(r);
    if (r.IsOk()) {
      Reply reply{};
      reply.drive = r.Value();
      result = kb::Result<Reply>::Success(reply);
    }
    return result;
  }

  Port<Can> port_;
  Config config_;
  ::canopen_client::EmcyEvent last_emcy_{};
  uint32_t emcy_count_{0U};
};

/// @name App API: the calls through OwnerAccess
/// @{

/** @brief NMT `command` to `node` (0 = every node). */
template <typename Access>
[[nodiscard]] kb::Result<void> SendNmt(Access& io, ::canopen_client::NmtCommand command,
                                       uint8_t node, kb::Milliseconds timeout) noexcept {
  Command c{};
  c.op = Op::SendNmt;
  c.nmt = command;
  c.node = node;
  const kb::Result<Reply> r = io.Request(c, timeout);
  return r.IsOk() ? kb::Result<void>::Success() : Forward<void>(r);
}

/** @brief One object of a node's dictionary, for the SDO calls. */
struct SdoObject {
  uint16_t index{0U}; ///< Object index.
  uint8_t node{0U};   ///< Node-ID, 1 to 127.
  uint8_t sub{0U};    ///< Sub-index.
  uint8_t size{4U};   ///< Bytes: 1, 2 or 4.
};

/** @brief Object `index:sub` of `node` (`size` 1, 2 or 4 bytes), by expedited SDO upload. */
template <typename Access>
[[nodiscard]] kb::Result<uint32_t> SdoRead(Access& io, const SdoObject& object,
                                           kb::Milliseconds timeout) noexcept {
  Command c{};
  c.op = Op::SdoRead;
  c.node = object.node;
  c.index = object.index;
  c.sub = object.sub;
  c.size = object.size;
  const kb::Result<Reply> r = io.Request(c, timeout);
  return r.IsOk() ? kb::Result<uint32_t>::Success(r.Value().value) : Forward<uint32_t>(r);
}

/** @brief `value` to `object` (1, 2 or 4 bytes), confirmed. */
template <typename Access>
[[nodiscard]] kb::Result<void> SdoWrite(Access& io, const SdoObject& object, uint32_t value,
                                        kb::Milliseconds timeout) noexcept {
  Command c{};
  c.op = Op::SdoWrite;
  c.node = object.node;
  c.index = object.index;
  c.sub = object.sub;
  c.size = object.size;
  c.value = value;
  const kb::Result<Reply> r = io.Request(c, timeout);
  return r.IsOk() ? kb::Result<void>::Success() : Forward<void>(r);
}

/** @brief One node-guarding exchange: the state `node` reports. */
template <typename Access>
[[nodiscard]] kb::Result<::canopen_client::NmtState> GuardNode(Access& io, uint8_t node,
                                                               kb::Milliseconds timeout) noexcept {
  Command c{};
  c.op = Op::GuardNode;
  c.node = node;
  const kb::Result<Reply> r = io.Request(c, timeout);
  return r.IsOk() ? kb::Result<::canopen_client::NmtState>::Success(r.Value().nmt)
                  : Forward<::canopen_client::NmtState>(r);
}

/** @brief The CiA 402 state of `node`. */
template <typename Access>
[[nodiscard]] kb::Result<::canopen_client::DriveState> ReadDriveState(
    Access& io, uint8_t node, kb::Milliseconds timeout) noexcept {
  Command c{};
  c.op = Op::ReadDriveState;
  c.node = node;
  const kb::Result<Reply> r = io.Request(c, timeout);
  return r.IsOk() ? kb::Result<::canopen_client::DriveState>::Success(r.Value().drive)
                  : Forward<::canopen_client::DriveState>(r);
}

/** @brief Walk the drive of `node` to Operation enabled. */
template <typename Access>
[[nodiscard]] kb::Result<void> DriveEnable(Access& io, uint8_t node,
                                           kb::Milliseconds timeout) noexcept {
  Command c{};
  c.op = Op::DriveEnable;
  c.node = node;
  const kb::Result<Reply> r = io.Request(c, timeout);
  return r.IsOk() ? kb::Result<void>::Success() : Forward<void>(r);
}

/** @brief Disable the voltage of the drive of `node` (the switch-off). */
template <typename Access>
[[nodiscard]] kb::Result<void> DriveDisable(Access& io, uint8_t node,
                                            kb::Milliseconds timeout) noexcept {
  Command c{};
  c.op = Op::DriveDisable;
  c.node = node;
  const kb::Result<Reply> r = io.Request(c, timeout);
  return r.IsOk() ? kb::Result<void>::Success() : Forward<void>(r);
}

/** @brief Quick stop of the drive of `node`. */
template <typename Access>
[[nodiscard]] kb::Result<void> DriveQuickStop(Access& io, uint8_t node,
                                              kb::Milliseconds timeout) noexcept {
  Command c{};
  c.op = Op::DriveQuickStop;
  c.node = node;
  const kb::Result<Reply> r = io.Request(c, timeout);
  return r.IsOk() ? kb::Result<void>::Success() : Forward<void>(r);
}

/// @}

} // namespace kb::handlers::canopen_client

#endif // KB_HANDLERS_BINDINGS_CANOPEN_CLIENT_HPP
