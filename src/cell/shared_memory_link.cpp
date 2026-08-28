// SPDX-License-Identifier: Apache-2.0
#include "cell/shared_memory_link.hpp"

#include <new>

namespace pickcell {
namespace {

// The region holds one seqlock slot and nothing else. Sized from the slot so a
// change to SafetySignal cannot silently produce a region too small for it.
constexpr std::size_t kRegionBytes = sizeof(SharedMemorySafetyLink::Slot);

}  // namespace

SharedMemorySafetyLink::SharedMemorySafetyLink(safeedge::ipc::SharedMemoryRegion region,
                                               std::string error)
    : region_(std::move(region)), error_(std::move(error)) {}

SharedMemorySafetyLink SharedMemorySafetyLink::create(const char* name) {
  // Remove a region left behind by a process that died without cleaning up.
  // POSIX shared memory outlives its creator, so a crashed run would otherwise
  // hand the next one a region of stale safety state -- which would read as a
  // valid signal from a runtime that is no longer there.
  safeedge::ipc::SharedMemoryRegion::unlinkName(name);

  auto region = safeedge::ipc::SharedMemoryRegion::create(name, kRegionBytes);
  if (!region.valid()) {
    return SharedMemorySafetyLink(std::move(region), "could not create shared memory");
  }

  // Placement-new the slot so its atomics are constructed rather than assumed.
  // A freshly mapped region is zeroed, and a zeroed seqlock happens to be a
  // valid empty one -- but relying on that is relying on an implementation
  // detail of both mmap and the slot.
  auto* slot = new (region.data()) Slot();
  slot->store(SafetySignal{});
  return SharedMemorySafetyLink(std::move(region), std::string{});
}

SharedMemorySafetyLink SharedMemorySafetyLink::openExisting(const char* name) {
  auto region = safeedge::ipc::SharedMemoryRegion::openExisting(name, kRegionBytes);
  if (!region.valid()) {
    return SharedMemorySafetyLink(std::move(region),
                                  "safety region not available; is the runtime up?");
  }
  return SharedMemorySafetyLink(std::move(region), std::string{});
}

SharedMemorySafetyLink::Slot* SharedMemorySafetyLink::slot() noexcept {
  if (!region_.valid()) {
    return nullptr;
  }
  return static_cast<Slot*>(region_.data());
}

void SharedMemorySafetyLink::publish(const SafetySignal& signal) {
  auto* s = slot();
  if (s == nullptr) {
    return;
  }
  // The heartbeat is stamped here rather than trusted from the caller. A caller
  // who forgot would leave the field frozen at whatever it was, which reads as a
  // writer that died at that instant -- or, worse, if it were never set at all,
  // as one that died at the epoch. Neither failure would be noticed until the
  // day the writer actually did die.
  SafetySignal stamped = signal;
  stamped.published_monotonic_ns = nowNanos();
  s->store(stamped);
}

void SharedMemorySafetyLink::heartbeat() {
  auto* s = slot();
  if (s == nullptr) {
    return;
  }
  // Republish the current verdict unchanged, with a fresh stamp. This is what a
  // writer does when nothing has happened -- and it is the only way a reader can
  // tell "nothing has happened" from "nobody is there".
  SafetySignal current;
  if (!s->tryLoad(current)) {
    return;
  }
  publish(current);
}

SafetyView SharedMemorySafetyLink::poll() {
  auto* s = slot();
  if (s == nullptr) {
    // No region means no safety information, which is not the same as a report
    // of safety. torque_permitted stays false.
    return SafetyView{};
  }

  SafetySignal signal;
  if (!s->tryLoad(signal)) {
    return last_good_;
  }

  SafetyView view;
  view.valid = true;
  view.torque_permitted = signal.torque_permitted != 0;
  view.asserted_monotonic_ns = signal.asserted_monotonic_ns;

  // When this reader found out -- used for the reaction time.
  view.observed_monotonic_ns = nowNanos();

  // When the writer was last demonstrably alive -- used for staleness. A seqlock
  // read succeeds whether the writer is alive or dead, since the slot keeps its
  // last value indefinitely, so this is the only field that can tell the
  // difference. It stops advancing the moment the heartbeat does.
  view.published_monotonic_ns = signal.published_monotonic_ns;
  view.sequence = signal.sequence;
  last_good_ = view;
  return view;
}

}  // namespace pickcell
