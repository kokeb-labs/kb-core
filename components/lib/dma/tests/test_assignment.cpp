// kb::dma board declarations: stream assignments, interrupt limits, bus limits, stream shapes and
// their compile-time checks (assignment.hpp).
// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The kb-core authors
#include <array>
#include <cstddef>
#include <cstdint>

#include "kb/dma/assignment.hpp"
#include "kb/dma/dma.hpp"
#include "kb_test.hpp"

namespace d = kb::dma;

namespace {

struct Rtos {
  static constexpr d::InterruptLimits kLimits{.max_syscall_priority = 5U, .priority_bits = 4U};
};
struct SpiRx {
  static constexpr d::StreamAssignment kAssignment{.engine = 1U,
                                                   .stream = 0U,
                                                   .request = 37U,
                                                   .priority = d::ChannelPriority::High,
                                                   .irq_priority = 5U,
                                                   .notify = d::Notify::Task,
                                                   .irq = d::IrqUse::All,
                                                   .mode = d::ChannelMode::Normal};
};
struct SpiTx {
  static constexpr d::StreamAssignment kAssignment{.engine = 1U,
                                                   .stream = 1U,
                                                   .request = 38U,
                                                   .priority = d::ChannelPriority::High,
                                                   .irq_priority = 5U,
                                                   .notify = d::Notify::Task,
                                                   .irq = d::IrqUse::ErrorsOnly,
                                                   .mode = d::ChannelMode::Normal};
};
struct AdcScan {
  static constexpr d::StreamAssignment kAssignment{.engine = 1U,
                                                   .stream = 2U,
                                                   .request = 9U,
                                                   .priority = d::ChannelPriority::High,
                                                   .irq_priority = 4U,
                                                   .notify = d::Notify::None,
                                                   .irq = d::IrqUse::All,
                                                   .mode = d::ChannelMode::Circular};
  static constexpr d::StreamShape kShape{.frames = 2U, .element_bytes = 2U, .elements = 6U};
};
struct Spi1Bus {
  static constexpr d::BusLimits kBus{.dma = true,
                                     .max_frame_dma = 1028U,
                                     .max_frame_no_dma = 16U,
                                     .rx_length_multiple = 1U,
                                     .align_bytes = 4U};
};
struct Spi2Bus {
  static constexpr d::BusLimits kBus{.dma = false,
                                     .max_frame_dma = 0U,
                                     .max_frame_no_dma = 64U,
                                     .rx_length_multiple = 4U,
                                     .align_bytes = 4U};
};

struct Scan {
  std::array<uint16_t, 6> ranks;
};

// The checks a port or a binding writes at namespace scope compile for a consistent board.
constexpr bool kChecks = [] {
  d::RequireInterruptTier<SpiRx, Rtos>();
  d::RequireInterruptTier<AdcScan, Rtos>();
  d::RequireSamePriority<SpiRx, SpiTx>();
  d::RequireBusCarries<Spi1Bus, 1028U>();
  d::RequireBusCarries<Spi2Bus, 64U>();
  d::RequireShape<AdcScan, Scan, 2U>();
  return true;
}();
static_assert(kChecks);

} // namespace

KB_TEST("assignment: a task-notifying stream must not be more urgent than the RTOS limit") {
  const d::InterruptLimits limits{.max_syscall_priority = 5U, .priority_bits = 4U};
  KB_CHECK(d::InterruptTierAllowed(5U, d::Notify::Task, limits));
  KB_CHECK(d::InterruptTierAllowed(15U, d::Notify::Task, limits));
  KB_CHECK(!d::InterruptTierAllowed(4U, d::Notify::Task, limits));
  KB_CHECK(d::InterruptTierAllowed(4U, d::Notify::None, limits));
  KB_CHECK(d::InterruptTierAllowed(0U, d::Notify::None, limits));
}

KB_TEST("assignment: a priority outside the implemented bits is refused") {
  KB_CHECK(!d::InterruptTierAllowed(
      16U, d::Notify::None, d::InterruptLimits{.max_syscall_priority = 5U, .priority_bits = 4U}));
  KB_CHECK(d::InterruptTierAllowed(
      255U, d::Notify::Task, d::InterruptLimits{.max_syscall_priority = 1U, .priority_bits = 8U}));
  KB_CHECK(d::InterruptTierAllowed(
      127U, d::Notify::Task, d::InterruptLimits{.max_syscall_priority = 1U, .priority_bits = 9U}));
  KB_CHECK(!d::InterruptTierAllowed(
      2U, d::Notify::None, d::InterruptLimits{.max_syscall_priority = 0U, .priority_bits = 1U}));
}

KB_TEST("assignment: a bus carries the DMA limit with DMA and the interrupt limit without") {
  KB_CHECK_EQ(Spi1Bus::kBus.MaxFrame(), std::size_t{1028U});
  KB_CHECK_EQ(Spi2Bus::kBus.MaxFrame(), std::size_t{64U});
  KB_CHECK_EQ(AdcScan::kShape.FrameBytes(), sizeof(Scan));
}

KB_TEST("assignment: defaults describe no stream") {
  const d::StreamAssignment none{};
  KB_CHECK_EQ(none.engine, uint8_t{0U});
  KB_CHECK(none.notify == d::Notify::Task);
  KB_CHECK(none.mode == d::ChannelMode::Normal);
  KB_CHECK(none.irq == d::IrqUse::All);
  KB_CHECK(none.priority == d::ChannelPriority::Medium);
  const d::BusLimits bus{};
  KB_CHECK_EQ(bus.MaxFrame(), std::size_t{0U});
  const d::StreamShape shape{};
  KB_CHECK_EQ(shape.FrameBytes(), std::size_t{1U});
}
