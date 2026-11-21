// Tests of StaticVector, RingBuffer and SpscRing (static_vector.hpp, ring_buffer.hpp).
// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The kb-core authors
#include <cstddef>
#include <cstdint>

#include "kb/utils/ring_buffer.hpp"
#include "kb/utils/static_vector.hpp"
#include "kb_test.hpp"

using kb::utils::RingBuffer;
using kb::utils::SpscRing;
using kb::utils::StaticVector;

KB_TEST("static vector: push, pop, at, set and the full and empty edges") {
  StaticVector<uint16_t, 3> v{};
  uint16_t out = 0U;
  KB_CHECK(v.Empty() && !v.Full());
  KB_CHECK_EQ((StaticVector<uint16_t, 3>::Capacity()), 3U);
  KB_CHECK(!v.PopBack(out));
  KB_CHECK(v.PushBack(1U) && v.PushBack(2U) && v.PushBack(3U));
  KB_CHECK(v.Full());
  KB_CHECK(!v.PushBack(4U));
  KB_CHECK_EQ(v.Size(), 3U);
  KB_CHECK(v.At(2U, out) && (out == 3U));
  KB_CHECK(!v.At(3U, out));
  KB_CHECK(v.Set(0U, 9U) && v.At(0U, out) && (out == 9U));
  KB_CHECK(!v.Set(3U, 9U));
  KB_CHECK(v.PopBack(out) && (out == 3U));
  uint32_t seen = 0U;
  for (const uint16_t x : v) {
    KB_CHECK((x == 9U) || (x == 2U));
    ++seen;
  }
  KB_CHECK_EQ(seen, 2U);
  KB_CHECK_EQ(v.AsSpan().size(), 2U);
  v.AsMutableSpan()[1] = 5U;
  KB_CHECK(v.At(1U, out) && (out == 5U));
  v.Clear();
  KB_CHECK(v.Empty());
}

KB_TEST("static vector: insert, erase and resize keep order and bounds") {
  StaticVector<uint8_t, 4> v{};
  uint8_t out = 0U;
  KB_CHECK(v.Insert(0U, 2U) && v.Insert(0U, 1U) && v.Insert(2U, 4U) && v.Insert(2U, 3U));
  KB_CHECK(!v.Insert(0U, 0U));
  for (std::size_t i = 0U; i < 4U; ++i) {
    KB_CHECK(v.At(i, out) && (out == (i + 1U)));
  }
  KB_CHECK(v.Erase(1U));
  KB_CHECK(v.At(1U, out) && (out == 3U));
  KB_CHECK(!v.Erase(3U));
  KB_CHECK(!v.Insert(5U, 1U));
  KB_CHECK(v.Resize(1U) && (v.Size() == 1U));
  KB_CHECK(v.Resize(4U) && v.At(3U, out) && (out == 0U));
  KB_CHECK(!v.Resize(kb_test::Rt(5U)));
}

KB_TEST("ring buffer: FIFO order across the wrap, refuse when full") {
  RingBuffer<uint32_t, 3> r{};
  uint32_t out = 0U;
  KB_CHECK(r.Empty() && !r.Peek(out) && !r.Pop(out));
  KB_CHECK_EQ((RingBuffer<uint32_t, 3>::Capacity()), 3U);
  for (uint32_t round = 0U; round < 5U; ++round) {
    KB_CHECK(r.Push(round) && r.Push(round + 10U));
    KB_CHECK(r.Pop(out) && (out == round));
    KB_CHECK(r.Pop(out) && (out == (round + 10U)));
  }
  KB_CHECK(r.Push(1U) && r.Push(2U) && r.Push(3U));
  KB_CHECK(r.Full() && !r.Push(4U));
  KB_CHECK(r.PeekAt(2U, out) && (out == 3U));
  KB_CHECK(!r.PeekAt(3U, out));
  KB_CHECK(r.Peek(out) && (out == 1U));
  KB_CHECK_EQ(r.Size(), 3U);
}

KB_TEST("ring buffer: overwrite drops the oldest, discard and clear") {
  RingBuffer<uint8_t, 2> r{};
  uint8_t out = 0U;
  KB_CHECK(!r.PushOverwrite(1U));
  KB_CHECK(!r.PushOverwrite(2U));
  KB_CHECK(r.PushOverwrite(3U));
  KB_CHECK(r.Pop(out) && (out == 2U));
  KB_CHECK(r.Pop(out) && (out == 3U));
  KB_CHECK(r.Push(4U) && r.Push(5U));
  KB_CHECK_EQ(r.Discard(1U), 1U);
  KB_CHECK(r.Peek(out) && (out == 5U));
  KB_CHECK_EQ(r.Discard(9U), 1U);
  KB_CHECK(r.Push(6U));
  r.Clear();
  KB_CHECK(r.Empty());
  KB_CHECK(r.Push(7U) && r.Pop(out) && (out == 7U));
}

KB_TEST("spsc ring: wait-free push and pop with free-running indices") {
  SpscRing<uint16_t, 4> q{};
  uint16_t out = 0U;
  KB_CHECK(!q.TryPop(out));
  KB_CHECK_EQ((SpscRing<uint16_t, 4>::Capacity()), 4U);
  for (uint16_t round = 0U; round < 10U; ++round) {
    KB_CHECK(q.TryPush(round) && q.TryPush(static_cast<uint16_t>(round + 100U)));
    KB_CHECK_EQ(q.Size(), 2U);
    KB_CHECK(q.TryPop(out) && (out == round));
    KB_CHECK(q.TryPop(out) && (out == (round + 100U)));
  }
  KB_CHECK(q.TryPush(1U) && q.TryPush(2U) && q.TryPush(3U) && q.TryPush(4U));
  KB_CHECK(!q.TryPush(5U));
  KB_CHECK_EQ(q.Size(), 4U);
  KB_CHECK(kb::utils::IsPowerOfTwo(8U) && !kb::utils::IsPowerOfTwo(6U) &&
           !kb::utils::IsPowerOfTwo(0U));
}
