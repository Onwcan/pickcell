// SPDX-License-Identifier: Apache-2.0
//
// The shared-memory link against a writer that genuinely dies.
//
// A thread pretending to be a dead writer is not the same experiment. A thread
// that stops publishing leaves an intact process holding the mapping; a process
// that exits leaves the region behind with its last value and nothing at all on
// the other end. The second is the case the heartbeat exists for, so the test
// forks -- the same choice safeedge's own shared-memory tests make.

#include <gtest/gtest.h>

#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <thread>

#include "cell/cell.hpp"
#include "cell/shared_memory_link.hpp"

namespace pickcell {
namespace {

constexpr const char* kRegion = "/pickcell-dead-writer-test";
constexpr auto kHeartbeatPeriod = std::chrono::milliseconds(20);
constexpr std::uint64_t kMaxAgeNs = 250'000'000;

/// Runs a cell against the region for `duration`, reporting whether it was ever
/// held for staleness.
struct Observation {
  bool moved_at_all = false;
  bool held_for_staleness = false;
};

Observation observe(SharedMemorySafetyLink& link, std::chrono::milliseconds duration) {
  Cell::Config config;
  config.max_safety_age_ns = kMaxAgeNs;
  Cell cell(link, config);

  Observation result;
  const std::uint64_t before_stale = cell.cycles_on_stale_safety();
  const auto deadline = Clock::now() + duration;
  while (Clock::now() < deadline) {
    cell.step(nowNanos());
    if (cell.phase() != CellPhase::kHeldBySafety) {
      result.moved_at_all = true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  result.held_for_staleness = cell.cycles_on_stale_safety() > before_stale;
  return result;
}

TEST(DeadWriter, TheCellStopsWhenTheWritingProcessExits) {
  // Clean up anything a previous crashed run left behind. POSIX shared memory
  // outlives its creator, and a leftover region would hand this test a stale
  // verdict from a process that is already gone -- which is, ironically, the
  // exact failure under test.
  safeedge::ipc::SharedMemoryRegion::unlinkName(kRegion);

  const pid_t writer = ::fork();
  ASSERT_NE(writer, -1) << "fork failed";

  if (writer == 0) {
    // Child: be the safety runtime for a second, then exit without cleaning up
    // -- exactly as a crashing process would.
    auto link = SharedMemorySafetyLink::create(kRegion);
    if (!link.valid()) {
      ::_exit(1);
    }
    SafetySignal permitted;
    permitted.sequence = 1;
    permitted.torque_permitted = 1;
    permitted.asserted_monotonic_ns = nowNanos();
    link.publish(permitted);

    const auto until = Clock::now() + std::chrono::milliseconds(1000);
    while (Clock::now() < until) {
      link.heartbeat();
      std::this_thread::sleep_for(kHeartbeatPeriod);
    }
    ::_exit(0);
  }

  // Parent: wait for the child to create the region, then read it.
  std::this_thread::sleep_for(std::chrono::milliseconds(150));
  auto reader = SharedMemorySafetyLink::openExisting(kRegion);
  ASSERT_TRUE(reader.valid()) << reader.error();

  // While the writer is alive and heartbeating, the cell runs.
  const Observation alive = observe(reader, std::chrono::milliseconds(500));
  EXPECT_TRUE(alive.moved_at_all) << "the cell should move while the writer is alive";
  EXPECT_FALSE(alive.held_for_staleness) << "a heartbeating writer must not look stale";

  int status = 0;
  ASSERT_EQ(::waitpid(writer, &status, 0), writer);
  ASSERT_TRUE(WIFEXITED(status));
  ASSERT_EQ(WEXITSTATUS(status), 0) << "the writer child failed";

  // The writer is now gone. The region survives it, and every read still
  // succeeds and still says torque is permitted -- which is precisely why the
  // verdict alone cannot be trusted.
  {
    const SafetyView view = reader.poll();
    EXPECT_TRUE(view.valid) << "the read still succeeds; that is the problem";
    EXPECT_TRUE(view.torque_permitted) << "the dead writer's last word stands";
  }

  // Past the staleness budget, the cell must stop believing it.
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  const Observation dead = observe(reader, std::chrono::milliseconds(200));
  EXPECT_TRUE(dead.held_for_staleness)
      << "the cell kept running against a writer that no longer exists";
  EXPECT_FALSE(dead.moved_at_all) << "the cell must not move at all once the permit expired";

  safeedge::ipc::SharedMemoryRegion::unlinkName(kRegion);
}

}  // namespace
}  // namespace pickcell
