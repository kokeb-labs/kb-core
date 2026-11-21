// Fuzz target: RingBuffer, StaticVector and SpscRing follow a simple array model for any operation
// sequence. SPDX-License-Identifier: MIT Copyright (c) 2026 The kb-core authors
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>

#include "kb/utils/ring_buffer.hpp"
#include "kb/utils/static_vector.hpp"

namespace {

void Require(bool condition) {
  if (!condition) {
    std::abort();
  }
}

constexpr std::size_t kCap = 5U;

// The model: a plain array FIFO with explicit shifting.
struct Model {
  std::array<uint8_t, kCap> items{};
  std::size_t count = 0U;
  void Push(uint8_t v) {
    items[count] = v;
    ++count;
  }
  uint8_t Pop() {
    const uint8_t v = items[0];
    for (std::size_t i = 1U; i < count; ++i) {
      items[i - 1U] = items[i];
    }
    --count;
    return v;
  }
};

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, std::size_t size) {
  kb::utils::RingBuffer<uint8_t, kCap> ring{};
  kb::utils::StaticVector<uint8_t, kCap> vec{};
  kb::utils::SpscRing<uint8_t, 4> spsc{};
  Model model{};
  std::size_t spsc_count = 0U;
  for (std::size_t i = 0U; i < size; ++i) {
    const uint8_t op = data[i];
    uint8_t out = 0U;
    switch (op % 6U) {
    case 0U:
      Require(ring.Push(op) == (model.count < kCap));
      if (model.count < kCap) {
        model.Push(op);
      }
      break;
    case 1U:
      Require(ring.Pop(out) == (model.count > 0U));
      if (model.count > 0U) {
        Require(out == model.Pop());
      }
      break;
    case 2U:
      if (model.count == kCap) {
        static_cast<void>(model.Pop());
      }
      model.Push(op);
      static_cast<void>(ring.PushOverwrite(op));
      break;
    case 3U: {
      const std::size_t at = static_cast<std::size_t>(op / 6U) % (kCap + 2U);
      const bool fits = (vec.Size() < kCap) && (at <= vec.Size());
      Require(vec.Insert(at, op) == fits);
      const std::size_t idx = static_cast<std::size_t>(op) % (kCap + 1U);
      const bool present = idx < vec.Size();
      Require(vec.Erase(idx) == present);
      break;
    }
    case 4U:
      Require(spsc.TryPush(op) == (spsc_count < 4U));
      if (spsc_count < 4U) {
        ++spsc_count;
      }
      break;
    default:
      Require(spsc.TryPop(out) == (spsc_count > 0U));
      if (spsc_count > 0U) {
        --spsc_count;
      }
      break;
    }
    Require(ring.Size() == model.count);
    Require(spsc.Size() == spsc_count);
    Require(vec.Size() <= kCap);
    for (std::size_t k = 0U; k < model.count; ++k) {
      Require(ring.PeekAt(k, out) && (out == model.items[k]));
    }
  }
  return 0;
}
