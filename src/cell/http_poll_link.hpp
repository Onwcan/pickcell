// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>

#include "cell/safety_link.hpp"
#include "safeedge/ipc/seqlock_slot.hpp"

namespace pickcell {

/// Safety state obtained by polling an HTTP endpoint.
///
/// This is how these things are usually wired, and it is worth being precise
/// about why: HTTP works across machines, survives one end restarting, is
/// trivially inspectable with curl, and needs no shared filesystem. Those are
/// real advantages and none of them are wrong.
///
/// What it costs is the thing this repository measures. The cell cannot learn
/// about a safety event any sooner than the next poll, so the reaction time is
/// bounded below by the poll period and has nothing to do with how quickly the
/// safety runtime decided. A runtime reacting in one millisecond, polled every
/// hundred, yields a cell that reacts in up to a hundred.
///
/// The polling happens on its own thread and `poll()` returns the last value
/// received, so the control loop never blocks on a socket. That is not an
/// optimisation -- a control loop that blocks on a network read has a period
/// determined by someone else's scheduler.
class HttpPollSafetyLink final : public SafetyLink {
 public:
  /// How the endpoint reports safety.
  enum class Format {
    /// Body is `sequence torque_permitted asserted_ns`. Carries the instant the
    /// runtime decided, so an end-to-end reaction time is measurable.
    kStamped,
    /// Body is ignored; HTTP 200 means permitted, anything else means not.
    ///
    /// This is what safeedge's /readyz offers, and what most readiness probes
    /// offer. It works, and it makes the reaction time *unmeasurable*: with no
    /// decision timestamp the reader can only stamp its own observation, so the
    /// interval it computes is zero by construction. That is precisely why
    /// robot-contracts puts `monotonic_ns` on StampedPose -- a signal that
    /// cannot be timed cannot be held to a deadline.
    kReadinessProbe,
  };

  HttpPollSafetyLink(std::string host, std::uint16_t port, std::string path,
                     std::chrono::milliseconds period,
                     Format format = Format::kStamped);
  ~HttpPollSafetyLink() override;

  SafetyView poll() override;
  std::string_view name() const override { return name_; }

  std::uint64_t requests_attempted() const noexcept {
    return requests_attempted_.load(std::memory_order_relaxed);
  }
  std::uint64_t requests_failed() const noexcept {
    return requests_failed_.load(std::memory_order_relaxed);
  }

 private:
  void pollLoop();
  bool fetchOnce(SafetySignal& out);

  std::string host_;
  std::uint16_t port_;
  std::string path_;
  std::chrono::milliseconds period_;
  Format format_;
  std::string name_;

  // The handoff from the polling thread to the control loop uses the same
  // seqlock primitive safeedge uses across processes. A mutex here would put a
  // blocking operation back into the control loop through the back door.
  safeedge::ipc::SeqlockSlot<SafetySignal> latest_;
  std::atomic<bool> ever_received_{false};

  // When the last *successful* fetch landed. Reported as the observation time,
  // so a cached value visibly ages instead of looking freshly confirmed.
  std::atomic<std::uint64_t> last_success_ns_{0};

  std::atomic<std::uint64_t> requests_attempted_{0};
  std::atomic<std::uint64_t> requests_failed_{0};

  std::uint64_t readiness_sequence_ = 0;
  std::atomic<bool> running_{true};
  std::thread poller_;
  SafetyView last_good_{};
};

}  // namespace pickcell
