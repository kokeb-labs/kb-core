// Copying a *Ref copies the reference: the copy refers to the original object, never to the *Ref it
// was copied from. A *Ref copied from a by-value parameter therefore outlives the parameter (run
// under ASan in CI). SPDX-License-Identifier: MIT Copyright (c) 2026 The kb-core authors
#include <type_traits>
#include <utility>

#include "kb/interfaces/host/host.hpp"
#include "kb/interfaces/refs.hpp"
#include "kb_test.hpp"

namespace h = kb::iface::host;
namespace i = kb::iface;

namespace {

// Copy from a non-const lvalue, move, and copy out of a by-value parameter; each keeps the original
// context.
template <typename Ref, typename Impl>
bool KeepsContext(Impl& impl) {
  Ref original{impl};
  // Direct-initialisation from a non-const lvalue Ref must pick the copy constructor, not the
  // explicit constructor template (which would make the copy refer to `original` itself).
  Ref copied{original};
  Ref copy_init = original;
  Ref assigned{impl};
  assigned = original;
  Ref moved{std::move(copied)};
  const auto through_param = [](Ref by_value) { return by_value; };
  const Ref returned = through_param(original);
  const void* const want = static_cast<const void*>(&impl);
  return (original.Context() == want) && (moved.Context() == want) &&
         (copy_init.Context() == want) && (assigned.Context() == want) &&
         (returned.Context() == want);
}

// A *Ref built inside a function from a parameter and returned: with a wrapping copy it would
// dangle.
struct Holder {
  explicit Holder(i::BusyWaitRef ref) noexcept : spin{ref} {}
  i::BusyWaitRef spin;
};
Holder Forward(i::BusyWaitRef ref) {
  return Holder{ref};
}

} // namespace

static_assert(std::is_trivially_copyable_v<i::I2cBusRef> &&
              std::is_trivially_copyable_v<i::BusyWaitRef>);
static_assert(!std::is_constructible_v<i::I2cBusRef, h::FakeSpiDevice&>,
              "a Ref needs its own contract");

KB_TEST("refs: copy, move and assignment keep the referenced object, for every *Ref") {
  h::FakeI2cBus i2c{};
  h::FakeSpiDevice spi{};
  h::FakeUart uart{};
  h::FakeGpioPin pin{};
  h::FakeAdc adc{};
  h::FakePwm pwm{};
  h::FakeCanBus can{};
  h::FakeClock clock{};
  h::FakeBusyWait wait{};
  h::FakeLogSink log{};
  KB_CHECK((KeepsContext<i::I2cBusRef>(i2c)));
  KB_CHECK((KeepsContext<i::SpiDeviceRef>(spi)));
  KB_CHECK((KeepsContext<i::UartPortRef>(uart)));
  KB_CHECK((KeepsContext<i::GpioOutputRef>(pin)));
  KB_CHECK((KeepsContext<i::GpioInputRef>(pin)));
  KB_CHECK((KeepsContext<i::AdcChannelRef>(adc)));
  KB_CHECK((KeepsContext<i::PwmOutputRef>(pwm)));
  KB_CHECK((KeepsContext<i::CanBusRef>(can)));
  KB_CHECK((KeepsContext<i::BusyWaitRef>(wait)));
  KB_CHECK((KeepsContext<i::LogSinkRef>(log)));
  const i::MonotonicClockRef c{clock};
  const i::MonotonicClockRef c2 = c;
  KB_CHECK(c2.Context() == static_cast<const void*>(&clock));
}

KB_TEST("refs: a *Ref copied out of a by-value parameter still reaches the object") {
  h::FakeBusyWait wait{};
  Holder held = Forward(i::BusyWaitRef{wait});
  held.spin.WaitUs(kb::Microseconds{7U});
  held.spin.WaitUs(kb::Microseconds{3U});
  KB_CHECK(wait.TotalUs() == 10U);
  KB_CHECK(held.spin.Context() == static_cast<const void*>(&wait));
}
