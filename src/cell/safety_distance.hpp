// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>

#include "motionkit/core/expected.hpp"
#include "motionkit/core/trajectory.hpp"

namespace pickcell {

/// How far the tool travels between a safety decision and coming to rest.
///
/// The reaction time this repository measures is not an answer on its own. It
/// is half of one. The other half is that an axis cannot stop instantly: the
/// cell "stops commanding motion" the moment it learns, and the arm then
/// decelerates under whatever acceleration and jerk it is capable of. What a
/// cell layout actually turns on is the sum, because that is the distance the
/// tool covers after the hazard is detected -- and therefore how far back a
/// guard, a light curtain or a fence has to sit.
///
/// This is ISO 13855's minimum-distance calculation in the form this cell can
/// compute for itself: a term for the time the system takes to react, and a
/// term for the machine's own stopping performance.
struct StoppingDistance {
  /// Distance covered while the cell has not yet reacted, at constant speed.
  /// Linear in both the speed and the reaction time.
  double reaction_travel_m{0.0};

  /// Distance covered decelerating to rest under the axis limits, from
  /// motionkit's jerk-limited stop. Grows faster than linearly with speed.
  double braking_distance_m{0.0};

  /// The number a guard is positioned from.
  double total_m{0.0};

  /// Time spent braking, which is not part of the reaction and is often
  /// mistaken for it.
  double braking_seconds{0.0};
};

/// Total travel from the instant the safety runtime decides, at `speed_mps`.
///
/// `reaction_ns` is the whole path from decision to the cell stopping its
/// command: the link, the poll interval, the control period and the cell's own
/// reaction. It is what `pickcell-reaction` and the end-to-end script measure,
/// and it belongs here rather than being assumed.
motionkit::Expected<StoppingDistance, motionkit::TrajectoryError> stoppingDistance(
    double speed_mps, std::uint64_t reaction_ns, const motionkit::MotionLimits& limits);

/// The highest speed whose total stopping distance still fits in `available_m`.
///
/// Never more than `limits.max_velocity`, and zero when there is no room --
/// which is the honest reading of no room rather than an error.
///
/// Solved by bisection. The total is strictly increasing in speed, so the root
/// is unique and the search cannot land on the wrong one; a closed form exists
/// only in the regime where the acceleration plateau is reached, and
/// `SafetyDistance.BisectionAgreesWithTheClosedFormWhereOneExists` checks the
/// two against each other there rather than trusting either alone.
motionkit::Expected<double, motionkit::TrajectoryError> permittedSpeed(
    double available_m, std::uint64_t reaction_ns, const motionkit::MotionLimits& limits);

/// Axis limits used for the cell's reports, so the numbers in the evidence and
/// the numbers in the tests come from one place.
inline constexpr motionkit::MotionLimits kCellAxisLimits{
    /*max_velocity=*/2.0, /*max_acceleration=*/8.0, /*max_jerk=*/40.0};

}  // namespace pickcell
