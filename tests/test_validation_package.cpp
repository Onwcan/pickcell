// SPDX-License-Identifier: Apache-2.0
//
// The frozen external-validation package.
//
// Two things are worth testing here and the rest is not. First, that the
// committed files still match what the code produces -- a pre-registration that
// silently regenerates is not frozen, and a file that was hand-edited is not a
// prediction. Second, that the frozen state is in the regime the package claims
// it is in, because the whole point of choosing 2.0 m/s was the segment
// structure it produces.

#include <gtest/gtest.h>

#include <cmath>
#include <fstream>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>
#include <string>
#include <vector>

#include "motionkit/core/motion_state.hpp"
#include "motionkit/core/trajectory.hpp"

#include "cell/safety_distance.hpp"
#include "cell/validation_package.hpp"

namespace pickcell::validation {
namespace {

std::string readCommitted(const std::string& name) {
  const std::string path =
      std::string(PICKCELL_SOURCE_DIR) + "/docs/external-validation/" + name;
  std::ifstream in(path, std::ios::binary);
  EXPECT_TRUE(in.good()) << "could not open " << path;
  std::ostringstream buffer;
  buffer << in.rdbuf();
  return buffer.str();
}

// ---------------------------------------------------------------------------
// Version lifecycle
// ---------------------------------------------------------------------------
//
// Two different checks, and the distinction is the whole design.
//
// The ACTIVE version is compared against the generator, so drift is caught.
// ARCHIVED versions are compared against a recorded digest and never against
// the generator -- because after an intended change to the calculation, an old
// prediction is still what was published and must not be rewritten to agree
// with new behaviour. A single test that compared every version against current
// behaviour would make "add v2" impossible: v1 would fail forever.

TEST(ValidationPackage, TheActiveVersionStillMatchesWhatTheCodeProduces) {
  const char* const guidance =
      "\n  Do NOT edit the committed file to make this pass. If the"
      " calculation changed on purpose, archive this version and raise"
      " kActiveVersion.";
  EXPECT_EQ(readCommitted("scenario-v1.json"), scenarioJson())
      << "the committed scenario no longer matches the generator" << guidance;
  EXPECT_EQ(readCommitted("prediction-v1.json"), predictionJson())
      << "the committed prediction no longer matches the generator" << guidance;
  EXPECT_EQ(readCommitted("braking-reference-trace-v1.csv"), referenceTraceCsv())
      << "the committed reference trace no longer matches the generator" << guidance;
}

// Every generated file must carry the version the code believes is active. A
// file labelled v1 produced by a generator that has moved on to v2 would be a
// pre-registration pointing at the wrong scenario.
TEST(ValidationPackage, GeneratedFilesCarryTheActiveVersion) {
  const std::string marker = "\"version\": " + std::to_string(kActiveVersion);
  EXPECT_NE(scenarioJson().find(marker), std::string::npos);
  EXPECT_NE(predictionJson().find(marker), std::string::npos);
  EXPECT_NE(referenceTraceCsv().find("version," + std::to_string(kActiveVersion)),
            std::string::npos);
}

// Integrity of archived artifacts is a SHA-256 manifest checked outside this
// binary -- docs/external-validation/MANIFEST.sha256, verified in CI with
// sha256sum. It lives there rather than here so that no cryptographic
// dependency enters the C++ runtime for the sake of a file check, and so that
// an external party can verify the package with one standard command.

// The phase fractions are frozen as literals so the experiment does not depend
// on anyone else's approximation of phi. This checks the literals still are what
// the recorded generator produces, so the two cannot silently diverge -- a
// mistyped digit would otherwise sit in a frozen file describing a sequence it
// does not belong to.
TEST(ValidationPackage, PhaseFractionsMatchTheirGenerator) {
  const double phi = (1.0 + std::sqrt(5.0)) / 2.0;
  const std::size_t count =
      sizeof(kHazardPhaseFractions) / sizeof(kHazardPhaseFractions[0]);
  ASSERT_EQ(count, 5u);
  for (std::size_t k = 1; k <= count; ++k) {
    const double exact = static_cast<double>(k) / phi;
    const double fractional = exact - std::floor(exact);
    EXPECT_NEAR(kHazardPhaseFractions[k - 1], fractional, 1e-15)
        << "phase fraction " << (k - 1) << " is not frac(" << k << " / phi)";
  }
  // Inside the open interval, so no trial coincides with a poll boundary.
  for (std::size_t i = 0; i < count; ++i) {
    EXPECT_GT(kHazardPhaseFractions[i], 0.0);
    EXPECT_LT(kHazardPhaseFractions[i], 1.0);
  }
}

// The emitted decimals must parse back to the doubles the implementation used.
// A frozen value an external party cannot recover exactly is not frozen.
TEST(ValidationPackage, EmittedPhaseFractionsRoundTrip) {
  const std::string scenario = scenarioJson();
  const std::size_t count =
      sizeof(kHazardPhaseFractions) / sizeof(kHazardPhaseFractions[0]);
  for (std::size_t i = 0; i < count; ++i) {
    std::ostringstream expected;
    expected.imbue(std::locale::classic());
    expected << std::defaultfloat
             << std::setprecision(std::numeric_limits<double>::max_digits10)
             << kHazardPhaseFractions[i];
    const std::string text = expected.str();
    EXPECT_NE(scenario.find(text), std::string::npos)
        << "phase fraction " << i << " is not emitted as " << text;
    EXPECT_EQ(std::stod(text), kHazardPhaseFractions[i]) << "does not round trip";
  }
}

// The reason 2.0 m/s was frozen rather than the cell's 0.5 m/s transfer speed.
// Below max_acceleration^2 / max_jerk the stop never reaches the acceleration
// limit and its middle segment collapses to zero duration, which is a harder
// profile for an independent simulator to reproduce by accident and an easier
// one to reproduce wrongly.
TEST(ValidationPackage, TheFrozenStateReachesTheAccelerationLimit) {
  const std::vector<BrakingPhase> phases = brakingPhases();
  ASSERT_EQ(phases.size(), 3u);
  for (const BrakingPhase& phase : phases) {
    EXPECT_GT(phase.duration_s, 0.0) << "a segment the package describes is degenerate";
  }
  EXPECT_DOUBLE_EQ(phases[0].jerk_mps3, -frozenLimits().max_jerk);
  EXPECT_DOUBLE_EQ(phases[1].jerk_mps3, 0.0);
  EXPECT_DOUBLE_EQ(phases[2].jerk_mps3, frozenLimits().max_jerk);
  // The middle segment is the acceleration plateau, so it must actually hold
  // the limit.
  EXPECT_DOUBLE_EQ(phases[1].entry_acceleration_mps2, -frozenLimits().max_acceleration);
}

TEST(ValidationPackage, ASlowerStateWouldNotHaveGivenThatStructure) {
  // Not a test of the package -- a test of the claim the package makes about
  // why this state was chosen. The cell's own 0.5 m/s transfer speed is below
  // the threshold and produces a two-segment stop.
  const double threshold = frozenLimits().max_acceleration *
                           frozenLimits().max_acceleration / frozenLimits().max_jerk;
  EXPECT_LT(0.5, threshold);
  EXPECT_GT(kInitialVelocityMps, threshold);

  const auto slow =
      motionkit::StopProfile::plan(motionkit::MotionState{0.0, 0.5, 0.0}, frozenLimits());
  ASSERT_TRUE(slow.hasValue());
  EXPECT_LT(slow.value.peakAcceleration(), frozenLimits().max_acceleration)
      << "0.5 m/s was expected not to reach the acceleration limit";
}

// The package must not drift away from the numbers the repository publishes
// elsewhere; they come from the same code path and are meant to stay identical.
TEST(ValidationPackage, PredictionAgreesWithThePublicEvidence) {
  const double reference_s = static_cast<double>(kReferenceReactionNs) / 1e9;
  const auto distance =
      stoppingDistance(kInitialVelocityMps, kReferenceReactionNs, frozenLimits());
  ASSERT_TRUE(distance.hasValue());
  EXPECT_NEAR(distance.value.braking_distance_m, 0.450, 1e-9);
  EXPECT_NEAR(distance.value.reaction_travel_m, 0.382, 1e-9);
  EXPECT_NEAR(distance.value.total_m, 0.832, 1e-9);

  // The parameterised prediction must agree with the same calculation the rest
  // of the repository uses. Two routes to one number.
  EXPECT_NEAR(predictedTotalM(reference_s), distance.value.total_m, 1e-12);
  EXPECT_NEAR(brakingDistanceM(), distance.value.braking_distance_m, 1e-12);
}

// The prediction is a function, so its shape is worth pinning: linear in the
// reaction time, with a constant braking term that does not move.
TEST(ValidationPackage, TotalIsLinearInReactionTimeWithAConstantBrakingTerm) {
  EXPECT_NEAR(predictedTotalM(0.0), brakingDistanceM(), 1e-12);
  const double slope = (predictedTotalM(0.2) - predictedTotalM(0.1)) / 0.1;
  EXPECT_NEAR(slope, kInitialVelocityMps, 1e-9);
  for (const double reaction : {0.0, 0.05, 0.191, 0.4}) {
    EXPECT_NEAR(predictedTotalM(reaction),
                kInitialVelocityMps * reaction + brakingDistanceM(), 1e-12);
  }
}

// The hazard plane is only useful if the verdict changes somewhere inside the
// polling ladder. If it did not, the collision column would be a constant.
TEST(ValidationPackage, TheCollisionVerdictVariesAcrossThePollingLadder) {
  const double crossing = collisionCrossingS();
  EXPECT_GT(crossing, 0.0);
  EXPECT_NEAR(predictedTotalM(crossing), kHazardPlaneM, 1e-12);

  // Below the crossing the tool stops short; above it, not.
  EXPECT_LT(predictedTotalM(crossing - 0.05), kHazardPlaneM);
  EXPECT_GT(predictedTotalM(crossing + 0.05), kHazardPlaneM);

  // And the crossing sits inside the span of reaction times this ladder can
  // plausibly produce -- bounded below by nothing and above by the longest
  // polling interval, which is the worst case a poll can produce.
  const std::size_t count = sizeof(kPollIntervalsNs) / sizeof(kPollIntervalsNs[0]);
  const double longest_s = static_cast<double>(kPollIntervalsNs[count - 1]) / 1e9;
  EXPECT_LT(crossing, longest_s) << "no run could ever reach the plane";
}

TEST(ValidationPackage, ResidualTravelShrinksWithTheStopThreshold) {
  // The quantity a simulator's "stopped" threshold omits. Monotonic, and small
  // enough at any sensible threshold that it cannot excuse a large deviation.
  double previous = 1.0;
  for (const double threshold : {1e-2, 1e-3, 1e-4}) {
    const double residual = residualTravelBelow(threshold);
    EXPECT_GT(residual, 0.0);
    EXPECT_LT(residual, previous);
    previous = residual;
  }
  EXPECT_LT(residualTravelBelow(1e-2), 1e-3) << "a 10 mm/s threshold omits under a mm";
}

}  // namespace
}  // namespace pickcell::validation
