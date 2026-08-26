// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>

namespace pickcell {

using Clock = std::chrono::steady_clock;

inline std::uint64_t nowNanos() noexcept {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch())
          .count());
}

/// The safety signal as it is written by the runtime and read by the cell.
///
/// Trivially copyable and standard layout so it can live in a seqlock slot in
/// shared memory. `asserted_monotonic_ns` is stamped by the *writer* at the
/// moment it decided, which is what makes an end-to-end reaction time
/// measurable rather than merely a round-trip time.
struct SafetySignal {
  std::uint64_t sequence = 0;
  std::uint64_t asserted_monotonic_ns = 0;
  std::uint32_t torque_permitted = 1;
  std::uint32_t reserved = 0;
};

static_assert(sizeof(SafetySignal) == 24);

/// What the cell currently believes about safety, and when it learned it.
struct SafetyView {
  bool torque_permitted = false;

  /// False until the link has ever produced a reading. A cell that has never
  /// heard from the safety runtime must not move, so this defaults to false and
  /// `torque_permitted` defaults to false with it.
  bool valid = false;

  /// The writer's stamp on the state being reported.
  std::uint64_t asserted_monotonic_ns = 0;

  /// When this reader observed it. The difference between the two is the
  /// quantity this repository exists to measure.
  std::uint64_t observed_monotonic_ns = 0;

  std::uint64_t sequence = 0;
};

/// How the cell learns about safety state.
///
/// An interface with two implementations because the choice between them is the
/// finding, not an implementation detail. Both are correct. They differ by three
/// orders of magnitude in how quickly the cell stops, and the difference is a
/// property of the integration rather than of the safety runtime at either end.
class SafetyLink {
 public:
  virtual ~SafetyLink() = default;

  SafetyLink() = default;
  SafetyLink(const SafetyLink&) = delete;
  SafetyLink& operator=(const SafetyLink&) = delete;
  SafetyLink(SafetyLink&&) = delete;
  SafetyLink& operator=(SafetyLink&&) = delete;

  /// Non-blocking. Returns the freshest state this link can offer right now.
  ///
  /// Non-blocking because it is called from the cell's control loop. A link
  /// that blocked would make the loop's period depend on a network round trip,
  /// which is how a safety signal ends up slower than the thing it protects.
  virtual SafetyView poll() = 0;

  virtual std::string_view name() const = 0;
};

}  // namespace pickcell
