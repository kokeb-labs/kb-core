// kb::interfaces on a PC: the host port's fake I2C bus with one register-file device, a register
// write, a write-then-read, and an injected fault that a caller sees as a Status. Documentation
// snippets are taken from this file (docs/parts/interfaces.md). SPDX-License-Identifier: MIT
// Copyright (c) 2026 The kb-core authors
#include <array>
#include <cstdint>
#include <cstdio>

#include "kb/interfaces/host/fake_i2c.hpp"

namespace {

// --8<-- [start:checks]
constexpr uint8_t kAddress = 0x48U;
constexpr uint8_t kRegister = 0x10U;

// Any type that satisfies kb::iface::I2cBus can be passed here: the fake on a PC, a port's bus on a
// target.
template <kb::iface::I2cBus Bus>
bool RunChecks(Bus& bus) {
  bool ok = bus.EnsureReady() == kb::Status::Ok;
  // Write register 0x10 (the first byte is the register, the rest are data), then read it back.
  const std::array<uint8_t, 3> tx{kRegister, 0xABU, 0xCDU};
  ok = ok && (bus.Write(kAddress, tx) == kb::Status::Ok);
  const std::array<uint8_t, 1> pointer{kRegister};
  std::array<uint8_t, 2> rx{};
  ok = ok && (bus.WriteRead(kAddress, pointer, rx) == kb::Status::Ok);
  ok = ok && (rx[0] == 0xABU) && (rx[1] == 0xCDU);
  // A device that is not on the bus is reported, not ignored.
  ok = ok && (bus.Read(0x50U, rx) == kb::Status::NoDevice);
  // An injected fault shows up as the Status the contract documents, and clears after its count.
  bus.FailNext(kb::iface::I2cFailure::Timeout, 1U);
  ok = ok && (bus.Read(kAddress, rx) == kb::Status::Timeout);
  ok = ok && (bus.Read(kAddress, rx) == kb::Status::Ok);
  return ok;
}
// --8<-- [end:checks]

} // namespace

int main() {
  kb::iface::host::FakeI2cBus bus{};
  const bool ok = bus.AddDevice(kAddress) && RunChecks(bus);
  static_cast<void>(std::puts(ok ? "kb::interfaces checks: pass" : "kb::interfaces checks: FAIL"));
  return ok ? 0 : 1;
}
