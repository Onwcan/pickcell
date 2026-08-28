// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <string>

#include "cell/safety_link.hpp"
#include "safeedge/ipc/seqlock_slot.hpp"
#include "safeedge/ipc/shared_memory.hpp"

namespace pickcell {

/// Safety state read straight out of a shared-memory seqlock.
///
/// A read is a couple of loads and a sequence check -- no syscall, no lock, no
/// waiting on a writer. That is what makes it callable from a control loop
/// without the loop's period becoming a function of somebody else's scheduling.
///
/// The cost is that both ends must be on the same machine. That is the actual
/// trade, and it is the one worth arguing about: the HTTP link works across
/// machines and is three orders of magnitude slower to react.
class SharedMemorySafetyLink final : public SafetyLink {
 public:
  using Slot = safeedge::ipc::SeqlockSlot<SafetySignal>;

  /// Opens an existing region published by the safety runtime.
  static SharedMemorySafetyLink openExisting(const char* name);

  /// Creates the region. Used by the safety source and by the tests.
  static SharedMemorySafetyLink create(const char* name);

  SafetyView poll() override;
  std::string_view name() const override { return "shared-memory seqlock"; }

  bool valid() const noexcept { return region_.valid(); }
  const std::string& error() const noexcept { return error_; }

  /// Writer side. Only the process that created the region should call this.
  ///
  /// Stamps `published_monotonic_ns` itself; the caller must not.
  void publish(const SafetySignal& signal);

  /// Writer side. Republishes the current verdict with a fresh stamp.
  ///
  /// Must be called on a period shorter than the reader's staleness budget, even
  /// when nothing has changed. A verdict that stops being restated is
  /// indistinguishable, to any reader, from a writer that stopped existing --
  /// and the reader is obliged to assume the worse of the two.
  void heartbeat();

 private:
  explicit SharedMemorySafetyLink(safeedge::ipc::SharedMemoryRegion region,
                                  std::string error);

  Slot* slot() noexcept;

  safeedge::ipc::SharedMemoryRegion region_;
  std::string error_;

  // Last good reading, returned when a torn read is detected. A seqlock reader
  // that fails its retries has learned nothing new; reporting a default would
  // be reporting "unsafe" on the basis of a transient contention, which would
  // stop the machine for no reason.
  SafetyView last_good_{};
};

}  // namespace pickcell
