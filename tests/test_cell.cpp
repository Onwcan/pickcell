// SPDX-License-Identifier: Apache-2.0
#include <gtest/gtest.h>

#include "cell/cell.hpp"

namespace pickcell {
namespace {

/// A link whose answer the test controls completely.
class ScriptedLink final : public SafetyLink {
 public:
  SafetyView poll() override { return view; }
  std::string_view name() const override { return "scripted"; }
  SafetyView view{};
};

TEST(CellSafety, HoldsWhenTheLinkHasNeverReported) {
  ScriptedLink link;  // valid == false
  Cell cell(link, Cell::Config{});
  cell.step(1'000'000);
  EXPECT_EQ(cell.phase(), CellPhase::kHeldBySafety);
  EXPECT_EQ(cell.cycles_without_safety(), 1u);
}

TEST(CellSafety, HoldsWhenTorqueIsNotPermitted) {
  ScriptedLink link;
  link.view.valid = true;
  link.view.torque_permitted = false;
  link.view.observed_monotonic_ns = 1'000'000;
  link.view.published_monotonic_ns = 1'000'000;

  Cell cell(link, Cell::Config{});
  cell.step(1'000'000);
  EXPECT_EQ(cell.phase(), CellPhase::kHeldBySafety);
}

TEST(CellSafety, MovesWhileThePermitIsFresh) {
  ScriptedLink link;
  link.view.valid = true;
  link.view.torque_permitted = true;
  link.view.observed_monotonic_ns = 1'000'000;
  link.view.published_monotonic_ns = 1'000'000;

  Cell cell(link, Cell::Config{});
  cell.step(1'000'000);
  EXPECT_NE(cell.phase(), CellPhase::kHeldBySafety);
}

// The regression test for the defect this repository found in itself: the
// safety runtime was stopped under docker compose and the cell carried on
// completing cycles, because the last permit it received never expired.
//
// Staleness is judged against published_monotonic_ns -- when the writer was
// last demonstrably alive -- not against when this reader happened to look. For
// a shared-memory link those are different numbers, and using the wrong one
// means a dead writer's verdict never ages.
TEST(CellSafety, StopsBelievingAPermitThatHasGoneStale) {
  ScriptedLink link;
  link.view.valid = true;
  link.view.torque_permitted = true;
  link.view.observed_monotonic_ns = 1'000'000;
  link.view.published_monotonic_ns = 1'000'000;

  Cell::Config config;
  config.max_safety_age_ns = 250'000'000;
  Cell cell(link, config);

  // Fresh: the cell moves.
  cell.step(1'000'000);
  ASSERT_NE(cell.phase(), CellPhase::kHeldBySafety);

  // The link keeps returning the same permit -- nobody has reissued it -- and
  // time passes. One nanosecond inside the budget is still good.
  cell.step(1'000'000 + config.max_safety_age_ns);
  EXPECT_NE(cell.phase(), CellPhase::kHeldBySafety);
  EXPECT_EQ(cell.cycles_on_stale_safety(), 0u);

  // One nanosecond past it is not.
  cell.step(1'000'000 + config.max_safety_age_ns + 1);
  EXPECT_EQ(cell.phase(), CellPhase::kHeldBySafety);
  EXPECT_EQ(cell.cycles_on_stale_safety(), 1u);
}

// A reader that keeps reading successfully from a writer that has died.
//
// This is the shared-memory failure the HTTP fix did not cover: a seqlock read
// succeeds whether or not anybody is still writing, so `valid` stays true, the
// verdict stays "permitted", and only the publish stamp betrays that nothing has
// restated it. The reader goes on observing at the current instant the whole
// time, which is why observed_monotonic_ns cannot be what staleness is measured
// against.
TEST(CellSafety, HoldsWhenTheWriterStopsHeartbeatingEvenThoughReadsSucceed) {
  ScriptedLink link;
  link.view.valid = true;  // the read keeps succeeding
  link.view.torque_permitted = true;
  link.view.published_monotonic_ns = 1'000'000;  // frozen: the writer is gone

  Cell::Config config;
  config.max_safety_age_ns = 250'000'000;
  Cell cell(link, config);

  // While the heartbeat is fresh, the cell moves.
  link.view.observed_monotonic_ns = 1'000'000;
  cell.step(1'000'000);
  ASSERT_NE(cell.phase(), CellPhase::kHeldBySafety);

  // Time passes. The reader is still reading successfully and still observing
  // "now" on every call -- but nobody has restated the verdict.
  for (std::uint64_t elapsed = 50'000'000; elapsed <= 240'000'000; elapsed += 50'000'000) {
    link.view.observed_monotonic_ns = 1'000'000 + elapsed;
    cell.step(1'000'000 + elapsed);
  }
  EXPECT_NE(cell.phase(), CellPhase::kHeldBySafety) << "still inside the budget";

  link.view.observed_monotonic_ns = 1'000'000 + 300'000'000;
  cell.step(1'000'000 + 300'000'000);
  EXPECT_EQ(cell.phase(), CellPhase::kHeldBySafety);
  EXPECT_GT(cell.cycles_on_stale_safety(), 0u);
}

// The heartbeat working: a writer that keeps restating an unchanged verdict
// keeps the cell running indefinitely.
TEST(CellSafety, AHeartbeatingWriterKeepsTheCellMoving) {
  ScriptedLink link;
  link.view.valid = true;
  link.view.torque_permitted = true;

  Cell::Config config;
  config.max_safety_age_ns = 250'000'000;
  Cell cell(link, config);

  // Ten seconds of nothing happening, restated every 20 ms.
  for (std::uint64_t t_ns = 0; t_ns <= 10'000'000'000ULL; t_ns += 20'000'000) {
    link.view.observed_monotonic_ns = t_ns;
    link.view.published_monotonic_ns = t_ns;  // the heartbeat
    cell.step(t_ns);
  }
  EXPECT_NE(cell.phase(), CellPhase::kHeldBySafety);
  EXPECT_EQ(cell.cycles_on_stale_safety(), 0u);
}

TEST(CellSafety, ResumesWhereTheHoldInterrupted) {
  ScriptedLink link;
  link.view.valid = true;
  link.view.torque_permitted = true;
  link.view.observed_monotonic_ns = 0;
  link.view.published_monotonic_ns = 0;

  Cell cell(link, Cell::Config{});
  for (int i = 0; i < 50; ++i) {
    link.view.observed_monotonic_ns = static_cast<std::uint64_t>(i) * 1'000'000;
    link.view.published_monotonic_ns = static_cast<std::uint64_t>(i) * 1'000'000;
    cell.step(static_cast<std::uint64_t>(i) * 1'000'000);
  }
  const CellPhase working = cell.phase();
  ASSERT_NE(working, CellPhase::kHeldBySafety);

  link.view.torque_permitted = false;
  link.view.sequence = 1;
  cell.step(50'000'000);
  ASSERT_EQ(cell.phase(), CellPhase::kHeldBySafety);

  link.view.torque_permitted = true;
  link.view.sequence = 2;
  link.view.observed_monotonic_ns = 51'000'000;
  link.view.published_monotonic_ns = 51'000'000;
  cell.step(51'000'000);

  // A cell that restarted its cycle here would drop the part it was holding.
  EXPECT_EQ(cell.phase(), working);
}

}  // namespace
}  // namespace pickcell
