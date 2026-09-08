// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "motionkit/core/trajectory.hpp"

/// A frozen, pre-registered package for independent external validation.
///
/// It describes ONE bounded experiment precisely enough that a third party can
/// reproduce the physics-side consequence of a reaction delay without using this
/// repository's code, and it records what this repository predicts before that
/// party produces a result. See docs/external-validation/README.md.
///
/// Nothing here names a particular external party. The package is an artifact of
/// this repository and should outlive any one collaboration.
namespace pickcell::validation {

/// The version this repository currently generates and checks.
///
/// Earlier versions are archived rather than regenerated -- see kArchived below
/// and the lifecycle section of the documentation.
inline constexpr int kActiveVersion = 1;

/// Identifier carried in every generated file, so a returned result can be tied
/// to the scenario it was produced against.
inline constexpr const char* kScenarioId = "pickcell-stopping-distance-xcheck";

/// Revisions of the fetched dependencies, supplied by CMake from the same
/// variables that pin FetchContent.
///
/// A pickcell commit SHA alone does not identify the stopping law: the library
/// that computes it is fetched. Recording the revision here closes that gap, and
/// taking it from the build rather than from a hand-typed string keeps it from
/// drifting away from what was actually built.
#ifndef PICKCELL_MOTIONKIT_REV
#define PICKCELL_MOTIONKIT_REV "unpinned"
#endif
#ifndef PICKCELL_CONTRACTS_REV
#define PICKCELL_CONTRACTS_REV "unpinned"
#endif
#ifndef PICKCELL_SAFEEDGE_REV
#define PICKCELL_SAFEEDGE_REV "unpinned"
#endif

/// The frozen initial state.
///
/// 2.0 m/s rather than the cell's 0.5 m/s transfer speed, for a reason that is a
/// property of the braking law rather than a preference: below
/// max_acceleration^2 / max_jerk a stop never reaches the acceleration limit and
/// its middle segment collapses to zero duration. A state above that threshold
/// gives three non-degenerate segments, which is harder to reproduce by accident
/// and easier to reproduce wrongly, so a mismatch carries information. The
/// threshold is not asserted here; brakingPhases() recovers the structure from
/// the profile that is actually generated.
inline constexpr double kInitialVelocityMps = 2.0;

/// Zero, and a modelling choice rather than a claim about hazards.
///
/// It is the state the cell is genuinely in: Cell::step advances the tool at a
/// constant transfer speed along straight-line segments and never ramps.
/// motionkit::StopProfile accepts an arbitrary initial state and this package
/// deliberately does not exercise that; a non-zero initial acceleration is a
/// different experiment and would be a new version.
inline constexpr double kInitialAccelerationMps2 = 0.0;

/// Polling intervals for the one-factor experiment, in nanoseconds.
///
/// The independent variable, and the only thing that changes between runs. They
/// match the intervals in evidence/reaction-vs-poll-interval.txt so the external
/// runs and this repository's own measurements describe the same ladder.
///
/// The simulator reports the reaction time IT observes at each interval. This
/// package does not predict that reaction time and must not: deriving it from
/// the interval would mean asserting the very relationship the measurement is
/// there to establish.
inline constexpr std::uint64_t kPollIntervalsNs[] = {10'000'000, 25'000'000, 50'000'000,
                                                     100'000'000, 200'000'000};

/// Normalised phase offsets of the hazard assertion within the poll window.
///
/// The nuisance variable this controls is real. For a fixed polling interval the
/// reaction time depends on where the hazard lands relative to the polling
/// schedule, so a hazard always asserted at the same absolute instant would
/// phase-lock differently against different intervals and produce numbers that
/// are artifacts of the chosen origin. This repository's own benchmark
/// randomises the trip within the window for exactly that reason.
///
/// These are frozen rather than randomised, and not because seeded randomness
/// would be irreproducible -- it would be. The reasons are narrower: a fixed
/// list depends on no RNG implementation or seed-handling convention, every
/// trial is enumerable before anything runs, an outside party can reproduce and
/// audit the schedule by reading it, and the identical nuisance-variable
/// schedule applies across every polling interval.
///
/// The values are the first five terms of a golden-ratio Kronecker sequence,
/// frac(k / phi) for k = 1..5. Low discrepancy, so five points cover (0, 1)
/// evenly without clustering, and the generator is not a simple fraction of the
/// interval the way a set such as {0.1, 0.3, 0.5, 0.7, 0.9} would be. That
/// reduces the chance of landing on a timestep boundary; it does not eliminate
/// it, since any floating-point value is rational.
///
/// A second reason worth naming: the prediction is evaluated at each run's own
/// measured reaction time, so sweeping phase produces a SPREAD of reaction
/// times. Without it the ladder could return one nearly identical value per
/// interval and the linear prediction would be tested at five points instead of
/// across its range.
///
/// Frozen as literals rather than computed, so the experiment does not depend on
/// anyone else's approximation of phi.
/// ValidationPackage.PhaseFractionsMatchTheirGenerator checks them against frac(k / phi)
/// so the two cannot silently diverge.
inline constexpr double kHazardPhaseFractions[] = {0.618033988749895, 0.236067977499790,
                                                   0.854101966249685, 0.472135954999580,
                                                   0.090169943749475};

/// The cell's control period. Recorded because it is part of what a reaction
/// figure means, not because the arithmetic uses it.
inline constexpr std::uint64_t kControlPeriodNs = 1'000'000;

/// One reaction time this repository measured, for context.
///
/// The worst case observed at a 200 ms poll interval. It is kept as a reference
/// point and as provenance for the public evidence -- NOT as the experiment's
/// input. The prediction below is a function of whatever reaction time the
/// simulator itself observes.
inline constexpr std::uint64_t kReferenceReactionNs = 191'000'000;
inline constexpr std::uint64_t kReferenceReactionPollNs = 200'000'000;

/// Distance from the tool's start position to the hazard plane. One plane,
/// frozen, and **purely observational**.
///
/// The plane exerts zero force and produces zero collision response. That is not
/// a detail: the quantity being cross-checked is the FREE stopping distance
/// under the commanded braking law, and a rigid collider at this position would
/// apply a contact impulse that alters the very trajectory the comparison is
/// about. A simulator should realise it as a trigger, a sensor boundary with
/// collision response disabled, or by post-processing an unobstructed
/// trajectory -- whichever its own tooling offers.
///
/// 0.700 m is chosen so the crossing verdict actually varies across the polling
/// ladder rather than reading the same value in every row: with a 0.450 m
/// braking term the plane is reached once the reaction time exceeds 0.125 s,
/// which falls inside the range this ladder produces.
inline constexpr double kHazardPlaneM = 0.700;

/// Velocity thresholds for the residual table. Values, not a recommendation --
/// the simulator declares its own.
inline constexpr double kStopThresholds[] = {1e-2, 1e-3, 1e-4};

/// One constant-jerk segment of the braking profile, as generated.
///
/// Recovered from the running StopProfile rather than restated from the
/// construction, so that a changed regime reports what was produced instead of
/// what someone expected.
struct BrakingPhase {
  double duration_s{0.0};
  double jerk_mps3{0.0};
  double entry_velocity_mps{0.0};
  double entry_acceleration_mps2{0.0};
};

/// The limits the frozen scenario uses, shared with the rest of the cell so the
/// package cannot describe a machine this repository does not model.
motionkit::MotionLimits frozenLimits() noexcept;

/// The segments of the frozen stop, recovered by locating the instants at which
/// commanded jerk changes.
std::vector<BrakingPhase> brakingPhases();

/// Braking distance for the frozen state: the travel after braking begins. It
/// does not depend on the reaction time, because the state at brake onset is the
/// frozen state however long the signal took to arrive.
double brakingDistanceM();

/// The pre-registered prediction, as a function of the reaction time the
/// simulator itself observes.
///
///     total(T) = initial_velocity * T + braking_distance
double predictedTotalM(double reaction_s);

/// Reaction time at which the predicted stop exactly reaches the hazard plane.
double collisionCrossingS();

/// Distance still to travel at the moment speed first falls below
/// `threshold_mps`, taken from the profile rather than from a closed-form tail.
double residualTravelBelow(double threshold_mps);

std::string scenarioJson();
std::string predictionJson();
std::string referenceTraceCsv();

}  // namespace pickcell::validation
