// SPDX-License-Identifier: Apache-2.0
#include "cell/safety_distance.hpp"

#include <cmath>
#include <cstdint>
#include <limits>

#include "motionkit/core/expected.hpp"
#include "motionkit/core/motion_state.hpp"
#include "motionkit/core/trajectory.hpp"

namespace pickcell {
namespace {

using motionkit::Expected;
using motionkit::MotionLimits;
using motionkit::MotionState;
using motionkit::StopProfile;
using motionkit::TrajectoryError;

double secondsOf(std::uint64_t nanoseconds) noexcept {
  return static_cast<double>(nanoseconds) / 1e9;
}

}  // namespace

Expected<StoppingDistance, TrajectoryError> stoppingDistance(
    double speed_mps, std::uint64_t reaction_ns, const MotionLimits& limits) {
  if (!std::isfinite(speed_mps) || speed_mps < 0.0) {
    return {StoppingDistance{}, TrajectoryError::NonFiniteInput};
  }
  if (const TrajectoryError error = limits.validate();
      error != TrajectoryError::None) {
    return {StoppingDistance{}, error};
  }

  // The axis is at constant speed when the hazard occurs, so acceleration is
  // zero. That is the assumption a guard calculation is normally allowed to
  // make, and it is worth naming: an axis that is still accelerating when the
  // stop is called for travels further, and motionkit's StopProfile reports how
  // much further if the caller has the real state to hand.
  const auto stop = StopProfile::plan(MotionState{0.0, speed_mps, 0.0}, limits);
  if (!stop) {
    return {StoppingDistance{}, stop.error};
  }

  StoppingDistance out;
  out.reaction_travel_m = speed_mps * secondsOf(reaction_ns);
  out.braking_distance_m = stop.value.stoppingDistance();
  out.braking_seconds = stop.value.duration();
  out.total_m = out.reaction_travel_m + out.braking_distance_m;
  return {out, TrajectoryError::None};
}

Expected<double, TrajectoryError> permittedSpeed(double available_m,
                                                 std::uint64_t reaction_ns,
                                                 const MotionLimits& limits) {
  if (!std::isfinite(available_m)) {
    return {0.0, TrajectoryError::NonFiniteInput};
  }
  if (const TrajectoryError error = limits.validate();
      error != TrajectoryError::None) {
    return {0.0, error};
  }
  if (available_m <= 0.0) {
    // No room to stop in permits no speed. A value, not an error: a guard
    // flush against the hazard is a real thing to describe.
    return {0.0, TrajectoryError::None};
  }

  const auto totalAt = [&](double speed) -> double {
    const auto distance = stoppingDistance(speed, reaction_ns, limits);
    return distance ? distance.value.total_m
                    : std::numeric_limits<double>::infinity();
  };

  // If the axis cannot exceed its own ceiling and that already fits, the room
  // is not the binding constraint and reporting more would be meaningless.
  if (totalAt(limits.max_velocity) <= available_m) {
    return {limits.max_velocity, TrajectoryError::None};
  }

  // Bisection on a strictly increasing function: reaction travel is linear in
  // speed and braking distance is monotonic in it, so the sum crosses
  // available_m exactly once and the interval always brackets the root.
  //
  // Sixty iterations halve a 2 m/s interval to well below the precision of a
  // double, so the loop is bounded by iteration count rather than by a
  // tolerance nobody chose. A fixed count also makes the cost constant, which
  // matters if this is ever called from anywhere with a deadline.
  double low = 0.0;
  double high = limits.max_velocity;
  for (int i = 0; i < 60; ++i) {
    const double middle = 0.5 * (low + high);
    if (totalAt(middle) > available_m) {
      high = middle;
    } else {
      low = middle;
    }
  }
  // The low end of the bracket, so the answer is a speed that fits rather than
  // one that is half an ulp past the guard.
  return {low, TrajectoryError::None};
}

}  // namespace pickcell
