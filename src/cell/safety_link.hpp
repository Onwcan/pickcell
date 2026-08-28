// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>

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

  /// When the writer *decided*. Changes only when the verdict changes.
  std::uint64_t asserted_monotonic_ns = 0;

  /// When the writer last *published*, whether or not anything changed.
  ///
  /// The heartbeat, and the reason it is a separate field from the one above.
  /// A seqlock read succeeds whether the writer is alive or dead: the slot
  /// retains its last value forever, and a reader that only looks at the
  /// contents cannot tell "the machine is still safe" from "the process that
  /// was telling me so exited an hour ago". The decision timestamp does not
  /// help, because it is *supposed* to stay still while the verdict holds.
  ///
  /// So the writer stamps this on every publish, and a reader can subtract it
  /// from its own clock to get the age of the information directly. Same
  /// machine is guaranteed here -- shared memory does not cross one -- so
  /// CLOCK_MONOTONIC has a common epoch and the subtraction is meaningful.
  ///
  /// Set by SharedMemorySafetyLink::publish() rather than by its caller. A
  /// caller who forgot would reintroduce exactly the defect this closes, and
  /// nothing would fail until the writer died.
  std::uint64_t published_monotonic_ns = 0;

  std::uint32_t torque_permitted = 1;
  std::uint32_t reserved = 0;
};

static_assert(sizeof(SafetySignal) == 32);
static_assert(std::is_trivially_copyable_v<SafetySignal>,
              "must be memcpy-able to live in a seqlock slot");

/// What the cell currently believes about safety, and when it learned it.
struct SafetyView {
  bool torque_permitted = false;

  /// False until the link has ever produced a reading. A cell that has never
  /// heard from the safety runtime must not move, so this defaults to false and
  /// `torque_permitted` defaults to false with it.
  bool valid = false;

  /// When the writer decided. The difference between this and
  /// `observed_monotonic_ns` is the quantity this repository exists to measure.
  std::uint64_t asserted_monotonic_ns = 0;

  /// When this reader learned the value. Used for the reaction time.
  std::uint64_t observed_monotonic_ns = 0;

  /// The most recent instant at which this link can *prove* the information was
  /// current. Used for staleness, and deliberately not the same field as
  /// `observed_monotonic_ns`.
  ///
  /// Collapsing the two was a real mistake here, and an instructive one. The
  /// shared-memory writer stamps its publish at the same instant it decides, so
  /// a reader reporting the writer's stamp as its own observation measured
  /// decision-to-decision and produced a reaction time of 0.1 microseconds --
  /// a number that looks like a triumph and means the measurement is broken.
  ///
  /// They answer different questions:
  ///   * observed  -- when did *I* find out? (reaction)
  ///   * published -- when was the writer last demonstrably alive? (staleness)
  ///
  /// For the HTTP link both are the moment of a successful fetch: the server
  /// answering is simultaneously the reader learning and the proof of life. For
  /// the shared-memory link they differ, because a read succeeds whether or not
  /// anybody is still writing.
  std::uint64_t published_monotonic_ns = 0;

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
