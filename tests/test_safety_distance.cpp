// SPDX-License-Identifier: Apache-2.0
//
// Turning a measured reaction time into the distance a guard is positioned from.
//
// The interesting assertions here are the ones that check the two halves
// separately: reaction travel is linear in speed and in time and can be
// computed by hand, while the braking half comes from motionkit and is checked
// against an independent closed form where one exists.

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <limits>

#include "motionkit/core/trajectory.hpp"

#include "cell/safety_distance.hpp"

namespace pickcell {
namespace {

using motionkit::MotionLimits;
using motionkit::TrajectoryError;

constexpr std::uint64_t kMillisecond = 1'000'000;

StoppingDistance mustCompute(double speed, std::uint64_t reaction_ns,
                             const MotionLimits& limits = kCellAxisLimits) {
  const auto distance = stoppingDistance(speed, reaction_ns, limits);
  EXPECT_EQ(distance.error, TrajectoryError::None) << motionkit::toString(distance.error);
  return distance.value;
}

TEST(SafetyDistance, ReactionTravelIsSpeedTimesTime) {
  // The half that can be done in one's head, which is exactly why it is worth
  // asserting: if this drifts, the arithmetic joining the two halves is wrong.
  const StoppingDistance at_100ms = mustCompute(0.5, 100 * kMillisecond);
  EXPECT_NEAR(at_100ms.reaction_travel_m, 0.05, 1e-12);

  const StoppingDistance at_1ms = mustCompute(0.5, kMillisecond);
  EXPECT_NEAR(at_1ms.reaction_travel_m, 0.0005, 1e-12);

  // Doubling the speed doubles the reaction travel and does not double the
  // braking distance, which is the whole reason they are reported separately.
  const StoppingDistance faster = mustCompute(1.0, 100 * kMillisecond);
  EXPECT_NEAR(faster.reaction_travel_m, 2.0 * at_100ms.reaction_travel_m, 1e-12);
  EXPECT_GT(faster.braking_distance_m, 2.0 * at_100ms.braking_distance_m);
}

TEST(SafetyDistance, TheLinkCannotChangeHowTheArmDecelerates) {
  // Same speed, three reaction times. The braking term is a property of the
  // axis, so it must be identical -- and it is the floor no link can get below.
  const StoppingDistance shared = mustCompute(1.0, kMillisecond);
  const StoppingDistance polled = mustCompute(1.0, 100 * kMillisecond);
  const StoppingDistance instant = mustCompute(1.0, 0);

  EXPECT_DOUBLE_EQ(shared.braking_distance_m, polled.braking_distance_m);
  EXPECT_DOUBLE_EQ(shared.braking_distance_m, instant.braking_distance_m);
  EXPECT_EQ(instant.reaction_travel_m, 0.0);
  EXPECT_GT(instant.total_m, 0.0) << "an instant reaction still has to stop";
  EXPECT_LT(shared.total_m, polled.total_m);
}

TEST(SafetyDistance, TheTotalIsTheSumAndTheBrakingTimeIsReported) {
  const StoppingDistance distance = mustCompute(2.0, 50 * kMillisecond);
  EXPECT_NEAR(distance.total_m, distance.reaction_travel_m + distance.braking_distance_m,
              1e-15);
  // Braking time is not reaction time, and conflating them is how a stopping
  // distance ends up short. At 2 m/s under these limits the arm spends longer
  // decelerating than a 100 ms poll spends noticing.
  EXPECT_GT(distance.braking_seconds, 0.05);
}

// ---------------------------------------------------------------------------
// Inverting it
// ---------------------------------------------------------------------------

TEST(SafetyDistance, PermittedSpeedRoundTripsThroughTheForwardCalculation) {
  for (const std::uint64_t reaction :
       {std::uint64_t{0}, kMillisecond, 100 * kMillisecond}) {
    for (const double clearance : {0.02, 0.1, 0.3, 0.8}) {
      const auto speed = permittedSpeed(clearance, reaction, kCellAxisLimits);
      ASSERT_TRUE(speed.hasValue()) << motionkit::toString(speed.error);
      if (speed.value >= kCellAxisLimits.max_velocity) {
        continue;  // clamped by the axis, checked separately
      }
      const StoppingDistance at_limit = mustCompute(speed.value, reaction);
      EXPECT_NEAR(at_limit.total_m, clearance, clearance * 1e-6)
          << "clearance " << clearance << ", reaction " << reaction;
    }
  }
}

// The bisection is robust and opaque. This checks it against arithmetic that
// can be read, in the regime where that arithmetic exists.
TEST(SafetyDistance, BisectionAgreesWithTheClosedFormWhereOneExists) {
  const MotionLimits& limits = kCellAxisLimits;
  const double a = limits.max_acceleration;
  const double j = limits.max_jerk;
  const double reaction_s = 0.05;
  const std::uint64_t reaction_ns = 50 * kMillisecond;

  // Above v = a^2/j the jerk-limited stop reaches the acceleration plateau and
  // its distance is v*a/(2j) + v^2/(2a). Adding the reaction term gives
  //     v^2/(2a) + v*(t + a/(2j)) - D == 0,
  // solved here in the form that does not subtract two nearly equal numbers.
  const double plateau_speed = (a / j) * a;
  for (const double clearance : {0.6, 0.9, 1.4}) {
    const double b = 2.0 * a * (reaction_s + a / (2.0 * j));
    const double k = 2.0 * a * clearance;
    const double closed_form = 2.0 * k / (b + std::sqrt(b * b + 4.0 * k));
    if (closed_form < plateau_speed || closed_form > limits.max_velocity) {
      continue;  // outside the regime the closed form describes
    }
    const auto bisected = permittedSpeed(clearance, reaction_ns, limits);
    ASSERT_TRUE(bisected.hasValue());
    EXPECT_NEAR(bisected.value, closed_form, 1e-9) << "clearance " << clearance;
  }
}

TEST(SafetyDistance, NoRoomPermitsNoSpeed) {
  // The theme this cell is built around: the absence of clearance is not
  // permission to move. Zero is an answer, not an error.
  const auto flush = permittedSpeed(0.0, kMillisecond, kCellAxisLimits);
  ASSERT_TRUE(flush.hasValue());
  EXPECT_EQ(flush.value, 0.0);

  const auto negative = permittedSpeed(-0.5, kMillisecond, kCellAxisLimits);
  ASSERT_TRUE(negative.hasValue());
  EXPECT_EQ(negative.value, 0.0);
}

TEST(SafetyDistance, AmpleRoomIsCappedByTheAxisAndNotByThePhysics) {
  const auto roomy = permittedSpeed(500.0, kMillisecond, kCellAxisLimits);
  ASSERT_TRUE(roomy.hasValue());
  EXPECT_DOUBLE_EQ(roomy.value, kCellAxisLimits.max_velocity);
}

TEST(SafetyDistance, MoreReactionTimeNeverPermitsMoreSpeed) {
  double previous = std::numeric_limits<double>::infinity();
  for (const std::uint64_t reaction : {std::uint64_t{0}, kMillisecond, 10 * kMillisecond,
                                       100 * kMillisecond, 200 * kMillisecond}) {
    const auto speed = permittedSpeed(0.3, reaction, kCellAxisLimits);
    ASSERT_TRUE(speed.hasValue());
    EXPECT_LE(speed.value, previous) << "a slower link permitted more speed";
    previous = speed.value;
  }
}

TEST(SafetyDistance, BadInputsAreRefused) {
  const double nan = std::numeric_limits<double>::quiet_NaN();
  EXPECT_EQ(stoppingDistance(nan, kMillisecond, kCellAxisLimits).error,
            TrajectoryError::NonFiniteInput);
  EXPECT_EQ(stoppingDistance(-1.0, kMillisecond, kCellAxisLimits).error,
            TrajectoryError::NonFiniteInput);
  // An unconfigured axis must not yield a distance. motionkit refuses zeroed
  // limits, and that refusal has to survive being wrapped rather than being
  // turned into a plausible-looking number here.
  EXPECT_EQ(stoppingDistance(1.0, kMillisecond, MotionLimits{}).error,
            TrajectoryError::NonPositiveLimit);
  EXPECT_EQ(permittedSpeed(1.0, kMillisecond, MotionLimits{}).error,
            TrajectoryError::NonPositiveLimit);
}

}  // namespace
}  // namespace pickcell
