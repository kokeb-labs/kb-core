/**
 * @file ws2812_rmt.hpp
 * @brief The ws2812_rmt binding and handler: WS2812, SK6812 and compatible single-wire addressable
 * LEDs, driven by a timed pulse peripheral (the ESP32 RMT transmitter).
 *
 * | Piece | What |
 * | --- | --- |
 * | Port | `Port<Hw>`: a pass-through to `Hw`, the driver's own port type that the MCU port
 * supplies (kb has no contract for a pulse peripheral, `BusKind::Pio`); the optional `GpioSet`
 * (strip supply) and `Log` exist when `Hw` has them | | Errors | `ToStatus()`: every
 * `ws2812_rmt::Error` to a kb::Status; the code in the health record's detail | | Start-up | `Link`
 * = `EnsureInitialized()` (sync: validate, open the channel, resolve the waveform at the channel's
 * tick rate); `Configure` = power the strip (with the pin) and send a black frame (sync, optional)
 * | | Thread model | Guarded by preference (`kRequiresOwner = false`): a strip is updated
 * sporadically from an application task and its channel is its own bus; an animation loop can own
 * it on an owner bus instead | | Health | `NotInitialized` faults at once; a failed frame (`Bus`),
 * `NotPowered` and refusals count to the threshold | | App API | no snapshot (an actuator);
 * commands: set a pixel, fill, a range, clear, brightness, show, power, read a pixel back |
 *
 * The driver is MCU-gated to ESP32 in kb's build switch (cmake/kb_mcu_gates.cmake); this header
 * still compiles anywhere, so the host tests drive it over the driver's own model of the channel.
 *
 * @code
 * using Strip = kb::handlers::ws2812_rmt::Handler<board::RmtPort, 60U>;     // board::RmtPort: the
 * driver's port kb::handlers::BusDomain<board::RmtPort, kb::handlers::ThreadModel::Guarded>
 * rmt0{rmt_port}; kb::handlers::Guarded<Strip, decltype(rmt0)> leds{rmt0, rmt_port, cfg}; if
 * (leds.EnsureStarted(kb::Milliseconds{20U}).IsOk()) {
 *   static_cast<void>(kb::handlers::ws2812_rmt::FillStrip(leds,
 * ws2812_rmt::Colour::FromPacked(0x002000U), kb::Milliseconds{20U}));
 * }
 * @endcode
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 The kb-core authors
 */
#ifndef KB_HANDLERS_BINDINGS_WS2812_RMT_HPP
#define KB_HANDLERS_BINDINGS_WS2812_RMT_HPP

#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include "kb/handlers/handlers.hpp"
#include <ws2812_rmt.hpp>

namespace kb::handlers::ws2812_rmt {

/** @brief The one map of the driver's errors to kb::Status (every enumerator, no default). */
[[nodiscard]] constexpr kb::Status ToStatus(::ws2812_rmt::Error error) noexcept {
  kb::Status s = kb::Status::Fault;
  switch (error) {
  case ::ws2812_rmt::Error::Ok:
    s = kb::Status::Ok;
    break;
  case ::ws2812_rmt::Error::NotInitialized:
  case ::ws2812_rmt::Error::NotPowered:
    s = kb::Status::NotReady;
    break;
  case ::ws2812_rmt::Error::InvalidArgument:
    s = kb::Status::InvalidArgument;
    break;
  case ::ws2812_rmt::Error::Bus:
    s = kb::Status::BusError;
    break;
  case ::ws2812_rmt::Error::Timeout:
    s = kb::Status::Timeout;
    break;
  case ::ws2812_rmt::Error::DeviceIdMismatch:
  case ::ws2812_rmt::Error::DeviceFault:
    s = kb::Status::Fault;
    break;
  case ::ws2812_rmt::Error::Crc:
    s = kb::Status::CrcMismatch;
    break;
  case ::ws2812_rmt::Error::Busy:
    s = kb::Status::Busy;
    break;
  case ::ws2812_rmt::Error::Unsupported:
  case ::ws2812_rmt::Error::TimingUnreachable:
    s = kb::Status::Unsupported;
    break;
  }
  return s;
}

/** @brief True for a driver error after which the driver must start again (fault at once). */
[[nodiscard]] constexpr bool IsFatalError(::ws2812_rmt::Error error) noexcept {
  return error == ::ws2812_rmt::Error::NotInitialized;
}

/** @brief The required hooks of the driver's port, on the MCU port's own type. */
template <typename Hw>
concept PulsePort = requires(Hw& hw, uint32_t& hz, std::span<const uint8_t> wire,
                             const ::ws2812_rmt::Waveform& wave, uint32_t value) {
  { hw.EnsureInitialized() } noexcept -> std::same_as<bool>;
  { hw.ResolutionHz(hz) } noexcept -> std::same_as<bool>;
  { hw.Transmit(wire, wave, value) } noexcept -> std::same_as<bool>;
  { hw.DelayUs(value) } noexcept -> std::same_as<void>;
};

/** @brief The MCU port drives the strip supply pin. */
template <typename Hw>
concept PowerPinPort = requires(Hw& hw) {
  {
    hw.GpioSet(::ws2812_rmt::CtrlPin::Power, ::ws2812_rmt::GpioSignal::ACTIVE)
  } noexcept -> std::same_as<bool>;
};

/** @brief The MCU port takes log lines. */
template <typename Hw>
concept LogPort = requires(Hw& hw, const char* text) {
  { hw.Log(::ws2812_rmt::LogLevel::Info, text) } noexcept -> std::same_as<void>;
};

/**
 * @brief The driver's port as a pass-through to the MCU port's channel.
 * @tparam Hw The MCU port's type with the driver's hooks (the ESP32 RMT channel on target).
 */
template <PulsePort Hw>
class Port : public ::ws2812_rmt::GpioInterface<Port<Hw>> {
public:
  /** @brief Over `hw`; no I/O. */
  explicit Port(Hw& hw) noexcept : hw_(hw) {}

  /** @brief Hook: open the channel, line idle low. */
  bool EnsureInitialized() noexcept {
    return hw_.EnsureInitialized();
  }
  /** @brief Hook: the tick rate the channel runs at. */
  bool ResolutionHz(uint32_t& hz) noexcept {
    return hw_.ResolutionHz(hz);
  }
  /** @brief Hook: one gapless stream, then the reset low. */
  bool Transmit(std::span<const uint8_t> wire, const ::ws2812_rmt::Waveform& wave,
                uint32_t timeout_ms) noexcept {
    return hw_.Transmit(wire, wave, timeout_ms);
  }
  /** @brief Hook: wait with the line low. */
  void DelayUs(uint32_t us) noexcept {
    hw_.DelayUs(us);
  }
  /** @brief Optional hook: the strip supply (present when the MCU port has it). */
  bool GpioSet(::ws2812_rmt::CtrlPin pin, ::ws2812_rmt::GpioSignal level) noexcept
    requires PowerPinPort<Hw>
  {
    return hw_.GpioSet(pin, level);
  }
  /** @brief Optional hook: one log line (present when the MCU port has it). */
  void Log(::ws2812_rmt::LogLevel level, const char* message) noexcept
    requires LogPort<Hw>
  {
    hw_.Log(level, message);
  }

private:
  Hw& hw_;
};

/** @brief The driver type a handler over `Hw` with `Pixels` pixels owns. */
template <PulsePort Hw, std::size_t Pixels>
using Driver = ::ws2812_rmt::Ws2812Rmt<Port<Hw>, Pixels>;

/** @brief The handler's configuration: board values from the HAL. */
struct Config {
  ::ws2812_rmt::Config chip{};           ///< Variant, order, pixel count, brightness, gamma, waits.
  HandlerConfig handler{};               ///< Health, recovery, polling.
  kb::Milliseconds link_deadline{20U};   ///< Bound of the channel stage.
  kb::Milliseconds blank_deadline{100U}; ///< Bound of the power and blank-frame stage.
  bool blank_on_start{true};             ///< Run the `Configure` stage (power, black frame).
};

/** @brief The commands of the handler. */
enum class Op : uint8_t {
  SetPixel = 0,      ///< Pixel `index` to `colour`.
  Fill = 1,          ///< Every pixel to `colour`.
  SetRange = 2,      ///< `count` pixels from `index` to `colour` (clipped to the strip).
  Clear = 3,         ///< Every pixel black.
  SetBrightness = 4, ///< Linear dimming to `brightness`.
  Show = 5,          ///< Send the frame.
  PowerOn = 6,       ///< Strip supply on (needs the pin), then the settle wait.
  PowerOff = 7,      ///< Strip supply off; frames are refused until PowerOn.
  GetPixel = 8,      ///< The colour stored for pixel `index`.
};

/** @brief One command; with `show` the frame is sent after the change. */
struct Command {
  ::ws2812_rmt::Colour colour{}; ///< For SetPixel, Fill, SetRange.
  uint32_t index{0U};            ///< For SetPixel, SetRange, GetPixel.
  uint32_t count{0U};            ///< For SetRange.
  Op op{Op::Show};               ///< What to do.
  uint8_t brightness{255U};      ///< For SetBrightness.
  bool show{false};              ///< Send the frame after the change.
};

/** @brief The reply of a command. */
struct Reply {
  ::ws2812_rmt::Colour colour{}; ///< GetPixel.
  uint32_t frame_us{0U};         ///< Time on the wire of one frame, reset included.
};

/**
 * @brief The ws2812_rmt handler.
 * @tparam Hw The MCU port's channel type with the driver's hooks.
 * @tparam Pixels The pixel capacity (the strip length when `Config::chip.pixel_count` is 0).
 */
template <PulsePort Hw, std::size_t Pixels>
class Handler
    : public HandlerCore<Handler<Hw, Pixels>, Driver<Hw, Pixels>, NoSnapshot, Command, Reply> {
  using Core = HandlerCore<Handler<Hw, Pixels>, Driver<Hw, Pixels>, NoSnapshot, Command, Reply>;
  using Drv = Driver<Hw, Pixels>;

public:
  /// The start-up plan: the channel, then power and a black frame (optional).
  static constexpr std::array<Stage, 2> kStartupPlan{
      {{stage::kLink, StageMode::Sync, false}, {stage::kConfigure, StageMode::Sync, true}}};
  static constexpr bool kRequiresOwner = false; ///< Served by occasional calls.
  static constexpr BusKind kBus = BusKind::Pio; ///< A pulse peripheral, no kb contract.

  /** @brief Stores the reference and the configuration; no I/O. */
  Handler(Hw& hw, const Config& config) noexcept
      : Core(config.handler), port_(hw), config_(config) {}

  /// @name Binding hooks (called by HandlerCore)
  /// @{
  void EmplaceDriver(std::optional<Drv>& slot) noexcept {
    slot.emplace(port_, config_.chip);
  }
  [[nodiscard]] kb::Result<StartProgress> RunStage(const StageCall& call, Drv& driver) noexcept {
    const kb::Result<void> r =
        (call.index == 0U) ? LiftStatus(driver.EnsureInitialized(), &ToStatus) : blank(driver);
    return r.IsOk() ? kb::Result<StartProgress>::Success(StartProgress::Done)
                    : Forward<StartProgress>(r);
  }
  [[nodiscard]] kb::Milliseconds StageDeadline(std::size_t index) const noexcept {
    return (index == 0U) ? config_.link_deadline : config_.blank_deadline;
  }
  [[nodiscard]] bool StageEnabled(std::size_t index) const noexcept {
    return (index != 1U) || config_.blank_on_start;
  }
  [[nodiscard]] kb::Result<void> StopDriver(Drv& driver) noexcept {
    kb::Result<void> r = kb::Result<void>::Success();
    if constexpr (PowerPinPort<Hw>) {
      r = LiftStatus(driver.PowerOff(), &ToStatus);
    }
    return r;
  }
  [[nodiscard]] bool IsFatal(kb::Status status, uint32_t detail) const noexcept {
    static_cast<void>(status);
    return detail == DetailOf(::ws2812_rmt::Error::NotInitialized);
  }
  [[nodiscard]] kb::Result<Reply> Run(Drv& driver, const Command& c, kb::Instant now) noexcept {
    static_cast<void>(now);
    Reply reply{};
    kb::Result<void> r = change(driver, c, reply);
    if (r.IsOk() && c.show && (c.op != Op::Show)) {
      r = LiftStatus(driver.Show(), &ToStatus);
    }
    const ::ws2812_rmt::Result<uint32_t> frame = driver.FrameTimeUs();
    reply.frame_us = frame.value;
    return r.IsOk() ? kb::Result<Reply>::Success(reply) : Forward<Reply>(r);
  }
  /// @}

private:
  // The Configure stage: the supply (when the port has the pin), then a black frame.
  [[nodiscard]] static kb::Result<void> blank(Drv& driver) noexcept {
    kb::Result<void> r = kb::Result<void>::Success();
    if constexpr (PowerPinPort<Hw>) {
      r = LiftStatus(driver.PowerOn(), &ToStatus);
    }
    driver.Clear();
    return r.IsOk() ? LiftStatus(driver.Show(), &ToStatus) : r;
  }
  [[nodiscard]] static kb::Result<void> change(Drv& driver, const Command& c,
                                               Reply& reply) noexcept {
    kb::Result<void> r = kb::Result<void>::Success();
    switch (c.op) {
    case Op::SetPixel:
      r = LiftStatus(driver.SetPixel(c.index, c.colour), &ToStatus);
      break;
    case Op::Fill:
      driver.Fill(c.colour);
      break;
    case Op::SetRange:
      r = range(driver, c);
      break;
    case Op::Clear:
      driver.Clear();
      break;
    case Op::SetBrightness:
      driver.SetBrightness(c.brightness);
      break;
    case Op::Show:
      r = LiftStatus(driver.Show(), &ToStatus);
      break;
    case Op::PowerOn:
      r = LiftStatus(driver.PowerOn(), &ToStatus);
      break;
    case Op::PowerOff:
      r = LiftStatus(driver.PowerOff(), &ToStatus);
      break;
    case Op::GetPixel:
      r = pixel(driver, c.index, reply);
      break;
    }
    return r;
  }
  [[nodiscard]] static kb::Result<void> range(Drv& driver, const Command& c) noexcept {
    const std::size_t length = driver.Length();
    const std::size_t first = static_cast<std::size_t>(c.index);
    const std::size_t room = (first < length) ? (length - first) : 0U;
    const std::size_t count =
        (static_cast<std::size_t>(c.count) < room) ? static_cast<std::size_t>(c.count) : room;
    kb::Result<void> r =
        (count == 0U) ? kb::Result<void>::Failure(kb::Status::InvalidArgument,
                                                  DetailOf(::ws2812_rmt::Error::InvalidArgument))
                      : kb::Result<void>::Success();
    for (std::size_t i = 0U; i < count; ++i) {
      if (r.IsOk()) {
        r = LiftStatus(driver.SetPixel(first + i, c.colour), &ToStatus);
      }
    }
    return r;
  }
  [[nodiscard]] static kb::Result<void> pixel(const Drv& driver, uint32_t index,
                                              Reply& reply) noexcept {
    const ::ws2812_rmt::Result<::ws2812_rmt::Colour> p =
        driver.GetPixel(static_cast<std::size_t>(index));
    reply.colour = p.value;
    return LiftStatus(p, &ToStatus);
  }

  Port<Hw> port_;
  Config config_;
};

/// @name App API: the same calls through OwnerAccess or Guarded
/// @{

/** @brief Runs `c` and drops the reply's values. */
template <typename Access>
[[nodiscard]] kb::Result<void> Send(Access& io, const Command& c,
                                    kb::Milliseconds timeout) noexcept {
  const kb::Result<Reply> r = io.Request(c, timeout);
  return r.IsOk() ? kb::Result<void>::Success() : Forward<void>(r);
}

/** @brief Pixel `index` to `colour`; with `show` the frame is sent too. */
template <typename Access>
[[nodiscard]] kb::Result<void> SetPixel(Access& io, uint32_t index,
                                        const ::ws2812_rmt::Colour& colour,
                                        kb::Milliseconds timeout, bool show = false) noexcept {
  Command c{};
  c.op = Op::SetPixel;
  c.index = index;
  c.colour = colour;
  c.show = show;
  return Send(io, c, timeout);
}

/** @brief Every pixel to `colour`; with `show` (default) the frame is sent too. */
template <typename Access>
[[nodiscard]] kb::Result<void> FillStrip(Access& io, const ::ws2812_rmt::Colour& colour,
                                         kb::Milliseconds timeout, bool show = true) noexcept {
  Command c{};
  c.op = Op::Fill;
  c.colour = colour;
  c.show = show;
  return Send(io, c, timeout);
}

/** @brief Every pixel black; with `show` (default) the frame is sent too. */
template <typename Access>
[[nodiscard]] kb::Result<void> ClearStrip(Access& io, kb::Milliseconds timeout,
                                          bool show = true) noexcept {
  Command c{};
  c.op = Op::Clear;
  c.show = show;
  return Send(io, c, timeout);
}

/** @brief Linear dimming of the next frames, 0 to 255. */
template <typename Access>
[[nodiscard]] kb::Result<void> SetStripBrightness(Access& io, uint8_t brightness,
                                                  kb::Milliseconds timeout) noexcept {
  Command c{};
  c.op = Op::SetBrightness;
  c.brightness = brightness;
  return Send(io, c, timeout);
}

/** @brief Sends the frame. */
template <typename Access>
[[nodiscard]] kb::Result<void> Show(Access& io, kb::Milliseconds timeout) noexcept {
  Command c{};
  c.op = Op::Show;
  return Send(io, c, timeout);
}

/** @brief The strip supply on or off (needs the MCU port's Power pin). */
template <typename Access>
[[nodiscard]] kb::Result<void> SetPower(Access& io, bool on, kb::Milliseconds timeout) noexcept {
  Command c{};
  c.op = on ? Op::PowerOn : Op::PowerOff;
  return Send(io, c, timeout);
}

/// @}

} // namespace kb::handlers::ws2812_rmt

#endif // KB_HANDLERS_BINDINGS_WS2812_RMT_HPP
