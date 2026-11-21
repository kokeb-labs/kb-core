/**
 * @file static_vector.hpp
 * @brief kb::utils::StaticVector<T, N>: a vector with a fixed capacity and inline storage. No heap.
 *
 * Every operation that can fail (push into a full vector, index past the end) reports it with a
 * `[[nodiscard]] bool` and changes nothing. Elements are value types (std::semiregular, trivially
 * copyable), held in a std::array, so the vector itself is trivially copyable and constexpr.
 *
 * @code
 * kb::utils::StaticVector<uint8_t, 4> v{};
 * const bool pushed = v.PushBack(7U);   // true
 * for (const uint8_t x : v) { ... }
 * @endcode
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 The kb-core authors
 */
#ifndef KB_UTILS_STATIC_VECTOR_HPP
#define KB_UTILS_STATIC_VECTOR_HPP

#include <array>
#include <concepts>
#include <cstddef>
#include <span>
#include <type_traits>

namespace kb::utils {

/** @brief What a fixed-capacity container holds: a trivially copyable value type with a default. */
template <typename T>
concept InlineElement = std::semiregular<T> && std::is_trivially_copyable_v<T>;

/**
 * @brief A sequence of at most N elements of T, stored inline.
 * @tparam T Element type (InlineElement).
 * @tparam N Capacity, at least 1.
 */
template <InlineElement T, std::size_t N>
  requires(N > 0U)
class StaticVector {
public:
  constexpr StaticVector() noexcept = default;

  /** @brief The number of elements. */
  [[nodiscard]] constexpr std::size_t Size() const noexcept {
    return size_;
  }
  /** @brief The capacity N. */
  [[nodiscard]] static constexpr std::size_t Capacity() noexcept {
    return N;
  }
  /** @brief True when there is no element. */
  [[nodiscard]] constexpr bool Empty() const noexcept {
    return size_ == 0U;
  }
  /** @brief True when Size() equals the capacity. */
  [[nodiscard]] constexpr bool Full() const noexcept {
    return size_ == N;
  }

  /** @brief Appends `value`; false (and no change) when full. */
  [[nodiscard]] constexpr bool PushBack(const T& value) noexcept {
    const bool room = size_ < N;
    if (room) {
      items_[size_] = value;
      ++size_;
    }
    return room;
  }
  /** @brief Removes the last element into `out`; false when empty. */
  [[nodiscard]] constexpr bool PopBack(T& out) noexcept {
    const bool any = size_ > 0U;
    if (any) {
      --size_;
      out = items_[size_];
      items_[size_] = T{};
    }
    return any;
  }
  /** @brief Inserts `value` before `index` (`index == Size()` appends); false when full or out of
   * range. */
  [[nodiscard]] constexpr bool Insert(std::size_t index, const T& value) noexcept {
    const bool ok = (size_ < N) && (index <= size_);
    if (ok) {
      for (std::size_t i = size_; i > index; --i) {
        items_[i] = items_[i - 1U];
      }
      items_[index] = value;
      ++size_;
    }
    return ok;
  }
  /** @brief Removes the element at `index`, keeping order; false when out of range. */
  [[nodiscard]] constexpr bool Erase(std::size_t index) noexcept {
    const bool ok = index < size_;
    if (ok) {
      for (std::size_t i = index + 1U; i < size_; ++i) {
        items_[i - 1U] = items_[i];
      }
      --size_;
      items_[size_] = T{};
    }
    return ok;
  }
  /** @brief Copies the element at `index` into `out`; false when out of range. */
  [[nodiscard]] constexpr bool At(std::size_t index, T& out) const noexcept {
    const bool ok = index < size_;
    if (ok) {
      out = items_[index];
    }
    return ok;
  }
  /** @brief Replaces the element at `index`; false when out of range. */
  [[nodiscard]] constexpr bool Set(std::size_t index, const T& value) noexcept {
    const bool ok = index < size_;
    if (ok) {
      items_[index] = value;
    }
    return ok;
  }
  /** @brief Grows with value-initialised elements or shrinks to `count`; false when `count > N`. */
  [[nodiscard]] constexpr bool Resize(std::size_t count) noexcept {
    const bool ok = count <= N;
    if (ok) {
      for (std::size_t i = count; i < size_; ++i) {
        items_[i] = T{};
      }
      size_ = count;
    }
    return ok;
  }
  /** @brief Removes every element. */
  constexpr void Clear() noexcept {
    for (std::size_t i = 0U; i < size_; ++i) {
      items_[i] = T{};
    }
    size_ = 0U;
  }

  /** @brief The elements as a span (valid until the next change of size). */
  [[nodiscard]] constexpr std::span<const T> AsSpan() const noexcept {
    return std::span<const T>{items_.data(), size_};
  }
  /** @brief The elements as a mutable span (valid until the next change of size). */
  [[nodiscard]] constexpr std::span<T> AsMutableSpan() noexcept {
    return std::span<T>{items_.data(), size_};
  }

  /** @brief Iteration (range-for) over the elements. */
  [[nodiscard]] constexpr const T* begin() const noexcept {
    return items_.data();
  }
  /** @brief End of iteration. */
  [[nodiscard]] constexpr const T* end() const noexcept {
    return AsSpan().data() + size_;
  }

private:
  std::array<T, N> items_{};
  std::size_t size_{0U};
};

} // namespace kb::utils

#endif // KB_UTILS_STATIC_VECTOR_HPP
