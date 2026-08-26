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
  if (auto* s = slot(); s != nullptr) {
    s->store(signal);
  }
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
  view.observed_monotonic_ns = nowNanos();
  view.sequence = signal.sequence;
  last_good_ = view;
  return view;
}

}  // namespace pickcell
