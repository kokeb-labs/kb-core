/**
 * @file isf15acp4.hpp
 * @brief The isf15acp4 binding and handler: OLED display pushbutton (SSD1331-class controller over
 * write-only SPI, 15 V OLED supply enable, SPST switch).
 *
 * | Piece | What |
 * | --- | --- |
 * | Port | `Port<Dev>`: the driver's SPI hooks over a kb::iface::SpiDevice (SpiHooks) and its
 * control pins (GpioHooks): D/C# set before each transfer, RES# and the supply enable driven by the
 * driver, the switch read, delays and the millisecond clock | | Errors | `ToStatus()`: every
 * `isf15acp4::Error` to a kb::Status; the code in the health record's detail | | Start-up |
 * `Identify + Configure` = `driver.EnsureInitialized()` (sync: reset pulse, the whole
 * configuration, RAM cleared, supply off), then `DisplayOn()` when `Config::display_on` | | Thread
 * model | Guarded or owner (`kRequiresOwner = false`): nothing arrives unasked; the switch needs a
 * sample (`Poll()`) at least twice per debounce time, which the owner task gives with
 * `sample_period`, or a guarded caller with `Sample()` | | Health | `NotInitialized` faults at
 * once; other failures (a transfer or a pin that failed) count to the threshold | | App API |
 * snapshot `{last switch event, events, presses, switch down, output, supply, scrolling}`;
 * commands: display on, dim, off, clear, fill window, pixel, line, rectangle, filled rectangle,
 * contrast, display mode, activity, raw switch read |
 *
 * Pins (`Config::outputs`, `Config::inputs`, board values): `kPinDc` asserted is D/C# high (data);
 * `kPinReset` asserted holds the controller in reset (RES# is active low: `active_low = true`);
 * `kPinVcc` asserted switches the OLED supply on; `kPinSwitch` must report the physical level
 * (`active_low = false`), the driver applies `ButtonConfig::active_low` itself. Without a switch
 * input (or on the display-only variant) a sample reads the driver state only.
 *
 * @code
 * kb::handlers::Guarded<kb::handlers::isf15acp4::Handler<Dev>, decltype(spi1)> key{spi1, dev,
 * timing, cfg}; if (key.EnsureStarted(kb::Milliseconds{500U}).IsOk()) {
 *   static_cast<void>(kb::handlers::isf15acp4::Clear(key, isf15acp4::kBlue,
 * kb::Milliseconds{50U}));
 * }
 * @endcode
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 The kb-core authors
 */
#ifndef KB_HANDLERS_BINDINGS_ISF15ACP4_HPP
#define KB_HANDLERS_BINDINGS_ISF15ACP4_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

#include "kb/handlers/handlers.hpp"
#include <isf15acp4.hpp>

namespace kb::handlers::isf15acp4 {

/** @brief Output index of D/C# (asserted: data). */
inline constexpr std::size_t kPinDc = 0U;
/** @brief Output index of RES# (asserted: the controller held in reset). */
inline constexpr std::size_t kPinReset = 1U;
/** @brief Output index of the OLED supply enable (asserted: supply on). */
inline constexpr std::size_t kPinVcc = 2U;
/** @brief Input index of the switch (physical level). */
inline constexpr std::size_t kPinSwitch = 0U;
/** @brief Control outputs of the port. */
inline constexpr std::size_t kOutputs = 3U;
/** @brief Control inputs of the port. */
inline constexpr std::size_t kInputs = 1U;

/** @brief The one map of the driver's errors to kb::Status (every enumerator, no default). */
[[nodiscard]] constexpr kb::Status ToStatus(::isf15acp4::Error error) noexcept {
  kb::Status s = kb::Status::Fault;
  switch (error) {
  case ::isf15acp4::Error::Ok:
    s = kb::Status::Ok;
    break;
  case ::isf15acp4::Error::NotInitialized:
  case ::isf15acp4::Error::SupplyOff:
    s = kb::Status::NotReady;
    break;
  case ::isf15acp4::Error::InvalidArgument:
  case ::isf15acp4::Error::ScrollNotSet:
    s = kb::Status::InvalidArgument;
    break;
  case ::isf15acp4::Error::Bus:
    s = kb::Status::BusError;
    break;
  case ::isf15acp4::Error::Timeout:
    s = kb::Status::Timeout;
    break;
  case ::isf15acp4::Error::Crc:
    s = kb::Status::CrcMismatch;
    break;
  case ::isf15acp4::Error::Busy:
  case ::isf15acp4::Error::ScrollActive:
  case ::isf15acp4::Error::Locked:
    s = kb::Status::Busy;
    break;
  case ::isf15acp4::Error::Unsupported:
    s = kb::Status::Unsupported;
    break;
  case ::isf15acp4::Error::DeviceIdMismatch:
  case ::isf15acp4::Error::DeviceFault:
    s = kb::Status::Fault;
    break;
  }
  return s;
}

/** @brief True for a driver error after which the controller state is unknown (fault at once). */
[[nodiscard]] constexpr bool IsFatalError(::isf15acp4::Error error) noexcept {
  return error == ::isf15acp4::Error::NotInitialized;
}

/**
 * @brief The driver's SPI port and control pins over a kb::iface::SpiDevice and HAL pins.
 * @tparam Dev The device (a SpiDevice or SpiDeviceRef; the chip select is the device's).
 */
template <kb::iface::SpiDevice Dev>
class Port : public ::isf15acp4::SpiInterface<Port<Dev>>,
             public SpiHooks<Port<Dev>, Dev>,
             public GpioHooks<Port<Dev>, kOutputs, kInputs> {
  using Spi = SpiHooks<Port<Dev>, Dev>;
  using Pins = GpioHooks<Port<Dev>, kOutputs, kInputs>;

public:
  /** @brief Over `dev`, `timing` and the HAL's pins; no I/O. */
  Port(Dev& dev, const HookTiming& timing, const std::array<OutputPin, kOutputs>& outputs,
       const std::array<InputPin, kInputs>& inputs) noexcept
      : Spi(dev, timing), Pins(outputs, inputs) {}

  /** @brief Hook: bring the SPI device up. */
  bool EnsureInitialized() noexcept {
    return this->BusReady();
  }
  /** @brief Hook: D/C# for `kind`, then one write-only transfer. */
  bool Write(::isf15acp4::DataKind kind, const uint8_t* data, std::size_t len) noexcept {
    const bool dc = this->DriveOutput(kPinDc, kind == ::isf15acp4::DataKind::Data);
    return dc ? this->BusTransfer(data, nullptr, len) : false;
  }
  /** @brief Hook: RES# or the supply enable. */
  bool GpioSet(::isf15acp4::CtrlPin pin, ::isf15acp4::GpioSignal signal) noexcept {
    const std::size_t index = (pin == ::isf15acp4::CtrlPin::Reset) ? kPinReset : kPinVcc;
    return this->DriveOutput(index, signal == ::isf15acp4::GpioSignal::ACTIVE);
  }
  /**
   * @brief Hook: wait at least `us` microseconds (the hook timing: a spin below 1 ms, a sleep
   * from 1 ms).
   */
  void DelayUs(uint32_t us) noexcept {
    this->Timing().DelayUs(us);
  }
  /** @brief Optional hook: the switch pin's physical level. */
  bool ReadSwitch(bool& high) noexcept {
    return this->ReadInput(kPinSwitch, high);
  }
  /** @brief Optional hook: the millisecond clock (wraps). */
  uint32_t NowMs() noexcept {
    return this->Timing().NowMs32();
  }
  /** @brief The SPI transfer counters. */
  [[nodiscard]] const HookCounters& SpiCounters() const noexcept {
    return Spi::Counters();
  }
  /** @brief The pin counters. */
  [[nodiscard]] const HookCounters& PinCounters() const noexcept {
    return Pins::Counters();
  }
};

/** @brief The driver type a handler over `Dev` owns. */
template <kb::iface::SpiDevice Dev>
using Driver = ::isf15acp4::ISF15ACP4<Port<Dev>>;

/** @brief The handler's configuration: board values from the HAL. */
struct Config {
  ::isf15acp4::Config chip{};                ///< Module, controller table, switch handling.
  HandlerConfig handler{};                   ///< Health, recovery, polling, sample period.
  kb::Milliseconds start_deadline{500U};     ///< Bound of the start-up stage.
  std::array<OutputPin, kOutputs> outputs{}; ///< D/C#, RES#, supply enable (indices kPin*).
  std::array<InputPin, kInputs> inputs{};    ///< The switch (physical level), optional.
  bool display_on{true};                     ///< Turn the panel on at the end of the start-up.
};

/** @brief What a sample publishes. */
struct Snapshot {
  ::isf15acp4::ButtonUpdate last_event{};               ///< The newest switch event (not None).
  uint32_t events{0U};                                  ///< Switch events seen so far.
  uint32_t presses{0U};                                 ///< Presses counted by the debouncer.
  ::isf15acp4::Output output{::isf15acp4::Output::Off}; ///< What the panel shows.
  bool button_down{false};                              ///< The debounced level: switch down.
  bool supply_on{false};                                ///< The OLED supply enable is on.
  bool scrolling{false};                                ///< Scrolling is on.
  bool has_switch{false};                               ///< The switch is wired and sampled.
};

/** @brief The commands of the handler. */
enum class Op : uint8_t {
  DisplayOn = 0,       ///< Supply on, display on at normal brightness.
  DisplayDim = 1,      ///< Supply on, display on in dim mode.
  DisplayOff = 2,      ///< Display off, supply off.
  Clear = 3,           ///< The visible area to `color`.
  FillWindow = 4,      ///< The window at `x0, y0` of `x1` by `y1` pixels to `color`.
  SetPixel = 5,        ///< The pixel at `x0, y0` to `color`.
  DrawLine = 6,        ///< A line from `x0, y0` to `x1, y1` in `color`.
  DrawRectangle = 7,   ///< A rectangle outline `x0, y0` to `x1, y1` in `color`.
  FillRectangle = 8,   ///< A filled rectangle: outline `color`, inside `fill`.
  SetContrast = 9,     ///< The contrasts of colours A, B and C to `a`, `b`, `c`.
  SetDisplayMode = 10, ///< What the pixels show: `mode`.
  NotifyActivity = 11, ///< Restart the idle timers.
  ReadSwitch = 12,     ///< The switch level now (polarity applied).
};

/** @brief One command. */
struct Command {
  ::isf15acp4::Color color{}; ///< The colour, or the outline.
  ::isf15acp4::Color fill{};  ///< For FillRectangle.
  uint8_t x0{0U};             ///< Column (or window column).
  uint8_t y0{0U};             ///< Row (or window row).
  uint8_t x1{0U};             ///< End column (FillWindow: width).
  uint8_t y1{0U};             ///< End row (FillWindow: height).
  uint8_t a{0U};              ///< SetContrast: colour A.
  uint8_t b{0U};              ///< SetContrast: colour B.
  uint8_t c{0U};              ///< SetContrast: colour C.
  Op op{Op::NotifyActivity};  ///< What to do.
  ::isf15acp4::DisplayMode mode{::isf15acp4::DisplayMode::Normal}; ///< For SetDisplayMode.
};

/** @brief The reply of a command. */
struct Reply {
  bool closed{false}; ///< ReadSwitch: true while the switch is closed.
};

/**
 * @brief The isf15acp4 handler.
 * @tparam Dev The SPI device (a SpiDevice or SpiDeviceRef).
 */
template <kb::iface::SpiDevice Dev>
class Handler : public HandlerCore<Handler<Dev>, Driver<Dev>, Snapshot, Command, Reply> {
  using Core = HandlerCore<Handler<Dev>, Driver<Dev>, Snapshot, Command, Reply>;

public:
  /// The start-up plan: one sync stage, the driver's EnsureInitialized() (ADR-0010 generic plan).
  static constexpr std::array<Stage, 1> kStartupPlan{
      {{static_cast<uint16_t>(stage::kIdentify | stage::kConfigure), StageMode::Sync, false}}};
  static constexpr bool kRequiresOwner = false; ///< Nothing arrives unasked.
  static constexpr BusKind kBus = BusKind::Spi; ///< On an SPI device.

  /** @brief Stores references and the configuration; no I/O. */
  Handler(Dev& dev, const HookTiming& timing, const Config& config) noexcept
      : Core(config.handler), port_(dev, timing, config.outputs, config.inputs), config_(config) {}

  /// @name Binding hooks (called by HandlerCore)
  /// @{
  void EmplaceDriver(std::optional<Driver<Dev>>& slot) noexcept {
    last_event_ = ::isf15acp4::ButtonUpdate{};
    events_ = 0U;
    slot.emplace(port_, config_.chip);
  }
  [[nodiscard]] kb::Result<StartProgress> RunStage(const StageCall& call,
                                                   Driver<Dev>& driver) noexcept {
    static_cast<void>(call);
    kb::Result<void> r = LiftStatus(driver.EnsureInitialized(), &ToStatus);
    if (r.IsOk() && config_.display_on) {
      r = LiftStatus(driver.DisplayOn(), &ToStatus);
    }
    return r.IsOk() ? kb::Result<StartProgress>::Success(StartProgress::Done)
                    : Forward<StartProgress>(r);
  }
  [[nodiscard]] kb::Milliseconds StageDeadline(std::size_t index) const noexcept {
    static_cast<void>(index);
    return config_.start_deadline;
  }
  [[nodiscard]] kb::Result<void> StopDriver(Driver<Dev>& driver) noexcept {
    return LiftStatus(driver.Deinitialize(), &ToStatus);
  }
  [[nodiscard]] bool IsFatal(kb::Status status, uint32_t detail) const noexcept {
    static_cast<void>(status);
    return detail == DetailOf(::isf15acp4::Error::NotInitialized);
  }
  [[nodiscard]] kb::Result<Snapshot> Measure(Driver<Dev>& driver, kb::Instant now) noexcept {
    static_cast<void>(now);
    kb::Result<void> r = kb::Result<void>::Success();
    const bool sampled = hasSwitch();
    if (sampled) {
      const kb::Result<::isf15acp4::ButtonUpdate> u = Lift(driver.Poll(), &ToStatus);
      r = u.IsOk() ? r : Forward<void>(u);
      if (u.IsOk() && (u.Value().event != ::isf15acp4::ButtonEvent::None)) {
        last_event_ = u.Value();
        events_ = SaturatingIncrement(events_);
      }
    }
    Snapshot s{};
    s.last_event = last_event_;
    s.events = events_;
    s.presses = driver.GetButtonPressCount();
    s.output = driver.GetOutput();
    s.button_down = driver.IsButtonDown();
    s.supply_on = driver.IsSupplyEnabled();
    s.scrolling = driver.IsScrolling();
    s.has_switch = sampled;
    return r.IsOk() ? kb::Result<Snapshot>::Success(s) : Forward<Snapshot>(r);
  }
  [[nodiscard]] kb::Result<Reply> Run(Driver<Dev>& driver, const Command& c,
                                      kb::Instant now) noexcept {
    static_cast<void>(now);
    kb::Result<Reply> result = kb::Result<Reply>::Failure(kb::Status::InvalidArgument);
    switch (c.op) {
    case Op::DisplayOn:
      result = done(LiftStatus(driver.DisplayOn(), &ToStatus));
      break;
    case Op::DisplayDim:
      result = done(LiftStatus(driver.DisplayDim(), &ToStatus));
      break;
    case Op::DisplayOff:
      result = done(LiftStatus(driver.DisplayOff(), &ToStatus));
      break;
    case Op::Clear:
      result = done(LiftStatus(driver.Clear(c.color), &ToStatus));
      break;
    case Op::FillWindow:
      result = done(LiftStatus(driver.FillWindow(c.x0, c.y0, c.x1, c.y1, c.color), &ToStatus));
      break;
    case Op::SetPixel:
      result = done(LiftStatus(driver.SetPixel(c.x0, c.y0, c.color), &ToStatus));
      break;
    case Op::DrawLine:
      result = done(LiftStatus(driver.DrawLine(c.x0, c.y0, c.x1, c.y1, c.color), &ToStatus));
      break;
    case Op::DrawRectangle:
      result = done(LiftStatus(driver.DrawRectangle(c.x0, c.y0, c.x1, c.y1, c.color), &ToStatus));
      break;
    case Op::FillRectangle:
      result = done(
          LiftStatus(driver.FillRectangle(c.x0, c.y0, c.x1, c.y1, c.color, c.fill), &ToStatus));
      break;
    case Op::SetContrast:
      result = done(LiftStatus(driver.SetContrast(c.a, c.b, c.c), &ToStatus));
      break;
    case Op::SetDisplayMode:
      result = done(LiftStatus(driver.SetDisplayMode(c.mode), &ToStatus));
      break;
    case Op::NotifyActivity:
      result = done(LiftStatus(driver.NotifyActivity(), &ToStatus));
      break;
    case Op::ReadSwitch:
      result = switchReply(Lift(driver.ReadSwitchRaw(), &ToStatus));
      break;
    }
    return result;
  }
  /// @}

  /** @brief The port's SPI transfer counters. */
  [[nodiscard]] const HookCounters& PortCounters() const noexcept {
    return port_.SpiCounters();
  }

private:
  [[nodiscard]] bool hasSwitch() const noexcept {
    return port_.HasInput(kPinSwitch) && ::isf15acp4::HasSwitch(config_.chip.variant);
  }
  [[nodiscard]] static kb::Result<Reply> done(const kb::Result<void>& r) noexcept {
    return r.IsOk() ? kb::Result<Reply>::Success(Reply{}) : Forward<Reply>(r);
  }
  [[nodiscard]] static kb::Result<Reply> switchReply(const kb::Result<bool>& r) noexcept {
    kb::Result<Reply> result = Forward<Reply>(r);
    if (r.IsOk()) {
      Reply reply{};
      reply.closed = r.Value();
      result = kb::Result<Reply>::Success(reply);
    }
    return result;
  }

  Port<Dev> port_;
  Config config_;
  ::isf15acp4::ButtonUpdate last_event_{};
  uint32_t events_{0U};
};

/// @name App API: the same calls through OwnerAccess or Guarded
/// @{

/** @brief Supply on, display on at normal brightness. */
template <typename Access>
[[nodiscard]] kb::Result<void> DisplayOn(Access& io, kb::Milliseconds timeout) noexcept {
  Command c{};
  c.op = Op::DisplayOn;
  const kb::Result<Reply> r = io.Request(c, timeout);
  return r.IsOk() ? kb::Result<void>::Success() : Forward<void>(r);
}

/** @brief Display off, supply off. */
template <typename Access>
[[nodiscard]] kb::Result<void> DisplayOff(Access& io, kb::Milliseconds timeout) noexcept {
  Command c{};
  c.op = Op::DisplayOff;
  const kb::Result<Reply> r = io.Request(c, timeout);
  return r.IsOk() ? kb::Result<void>::Success() : Forward<void>(r);
}

/** @brief The visible area to `color`. */
template <typename Access>
[[nodiscard]] kb::Result<void> Clear(Access& io, ::isf15acp4::Color color,
                                     kb::Milliseconds timeout) noexcept {
  Command c{};
  c.op = Op::Clear;
  c.color = color;
  const kb::Result<Reply> r = io.Request(c, timeout);
  return r.IsOk() ? kb::Result<void>::Success() : Forward<void>(r);
}

/** @brief Two corners on the panel, for the drawing calls (pixels). */
struct Corners {
  uint8_t x0{0U}; ///< First column.
  uint8_t y0{0U}; ///< First row.
  uint8_t x1{0U}; ///< Second column (FillWindow: the width).
  uint8_t y1{0U}; ///< Second row (FillWindow: the height).
};

/** @brief The window at `window.x0, window.y0` of `window.x1` by `window.y1` pixels to `color`. */
template <typename Access>
[[nodiscard]] kb::Result<void> FillWindow(Access& io, const Corners& window,
                                          ::isf15acp4::Color color,
                                          kb::Milliseconds timeout) noexcept {
  Command c{};
  c.op = Op::FillWindow;
  c.x0 = window.x0;
  c.y0 = window.y0;
  c.x1 = window.x1;
  c.y1 = window.y1;
  c.color = color;
  const kb::Result<Reply> r = io.Request(c, timeout);
  return r.IsOk() ? kb::Result<void>::Success() : Forward<void>(r);
}

/** @brief A line between the two corners of `line` in `color` (the controller's engine). */
template <typename Access>
[[nodiscard]] kb::Result<void> DrawLine(Access& io, const Corners& line, ::isf15acp4::Color color,
                                        kb::Milliseconds timeout) noexcept {
  Command c{};
  c.op = Op::DrawLine;
  c.x0 = line.x0;
  c.y0 = line.y0;
  c.x1 = line.x1;
  c.y1 = line.y1;
  c.color = color;
  const kb::Result<Reply> r = io.Request(c, timeout);
  return r.IsOk() ? kb::Result<void>::Success() : Forward<void>(r);
}

/** @brief The contrasts of colours A, B and C. */
template <typename Access>
[[nodiscard]] kb::Result<void> SetContrast(Access& io, uint8_t a, uint8_t b, uint8_t c_level,
                                           kb::Milliseconds timeout) noexcept {
  Command c{};
  c.op = Op::SetContrast;
  c.a = a;
  c.b = b;
  c.c = c_level;
  const kb::Result<Reply> r = io.Request(c, timeout);
  return r.IsOk() ? kb::Result<void>::Success() : Forward<void>(r);
}

/// @}

} // namespace kb::handlers::isf15acp4

#endif // KB_HANDLERS_BINDINGS_ISF15ACP4_HPP
