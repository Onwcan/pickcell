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

  Cell cell(link, Cell::Config{});
  cell.step(1'000'000);
  EXPECT_EQ(cell.phase(), CellPhase::kHeldBySafety);
}

TEST(CellSafety, MovesWhileThePermitIsFresh) {
  ScriptedLink link;
  link.view.valid = true;
  link.view.torque_permitted = true;
  link.view.observed_monotonic_ns = 1'000'000;

  Cell cell(link, Cell::Config{});
  cell.step(1'000'000);
  EXPECT_NE(cell.phase(), CellPhase::kHeldBySafety);
}

// The regression test for the defect this repository found in itself: the
// safety runtime was stopped under docker compose and the cell carried on
// completing cycles, because the last permit it received never expired.
TEST(CellSafety, StopsBelievingAPermitThatHasGoneStale) {
  ScriptedLink link;
  link.view.valid = true;
  link.view.torque_permitted = true;
  link.view.observed_monotonic_ns = 1'000'000;

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

TEST(CellSafety, ResumesWhereTheHoldInterrupted) {
  ScriptedLink link;
  link.view.valid = true;
  link.view.torque_permitted = true;
  link.view.observed_monotonic_ns = 0;

  Cell cell(link, Cell::Config{});
  for (int i = 0; i < 50; ++i) {
    link.view.observed_monotonic_ns = static_cast<std::uint64_t>(i) * 1'000'000;
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
  cell.step(51'000'000);

  // A cell that restarted its cycle here would drop the part it was holding.
  EXPECT_EQ(cell.phase(), working);
}

}  // namespace
}  // namespace pickcell
