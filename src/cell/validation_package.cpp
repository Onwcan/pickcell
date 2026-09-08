// SPDX-License-Identifier: Apache-2.0
#include "cell/validation_package.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>
#include <string>
#include <vector>

#include "motionkit/core/motion_state.hpp"
#include "motionkit/core/trajectory.hpp"

#include "cell/safety_distance.hpp"

namespace pickcell::validation {
namespace {

using motionkit::MotionSample;
using motionkit::MotionState;
using motionkit::StopProfile;

/// A stream whose output does not depend on the ambient locale.
///
/// These files are frozen and compared byte for byte. A machine with a
/// comma-decimal locale would otherwise generate different bytes from identical
/// code, and the drift check would fail for a reason that has nothing to do with
/// the physics.
std::ostringstream fixedStream(int precision) {
  std::ostringstream out;
  out.imbue(std::locale::classic());
  out << std::fixed << std::setprecision(precision);
  return out;
}

std::string number(double value, int precision = 9) {
  std::ostringstream out = fixedStream(precision);
  out << value;
  return out.str();
}

/// A decimal form that parses back to exactly the same double.
///
/// max_digits10 is the standard's answer to "how many significant digits does a
/// round trip need", which is the question here -- these values are the frozen
/// experiment definition, and a reader who parses them must obtain the doubles
/// this implementation actually used. A fixed count of decimal places would be
/// an arbitrary choice that happens to work for numbers of this magnitude.
std::string roundTripNumber(double value) {
  std::ostringstream out;
  out.imbue(std::locale::classic());
  out << std::defaultfloat << std::setprecision(std::numeric_limits<double>::max_digits10)
      << value;
  return out.str();
}

std::string seconds(std::uint64_t nanoseconds) {
  return number(static_cast<double>(nanoseconds) / 1e9, 9);
}

StopProfile frozenStop() {
  const auto stop = StopProfile::plan(
      MotionState{0.0, kInitialVelocityMps, kInitialAccelerationMps2}, frozenLimits());
  return stop ? stop.value : StopProfile{};
}

/// The instant at which the commanded jerk stops being `current`.
double findJerkChange(const StopProfile& stop, double lo, double hi, double current) {
  for (int i = 0; i < 100; ++i) {
    const double mid = 0.5 * (lo + hi);
    if (stop.sample(mid).jerk == current) {
      lo = mid;
    } else {
      hi = mid;
    }
  }
  return hi;
}

}  // namespace

motionkit::MotionLimits frozenLimits() noexcept { return kCellAxisLimits; }

std::vector<BrakingPhase> brakingPhases() {
  const StopProfile stop = frozenStop();
  std::vector<BrakingPhase> phases;
  if (stop.duration() <= 0.0) {
    return phases;
  }

  // Walk the profile, finding each instant the commanded jerk changes. A coarse
  // scan locates the change and a bisection pins it. Nothing here assumes how
  // many segments there are or what their jerks should be.
  constexpr int kScanSteps = 200000;
  const double step = stop.duration() / static_cast<double>(kScanSteps);

  double segment_start = 0.0;
  double current_jerk = stop.sample(0.0).jerk;
  MotionSample entry = stop.sample(0.0);

  for (int i = 1; i <= kScanSteps; ++i) {
    const double t = std::min(static_cast<double>(i) * step, stop.duration());
    const double jerk_here =
        t >= stop.duration() ? current_jerk + 1.0 : stop.sample(t).jerk;
    if (jerk_here == current_jerk) {
      continue;
    }
    const double boundary = t >= stop.duration()
                                ? stop.duration()
                                : findJerkChange(stop, t - step, t, current_jerk);
    phases.push_back(BrakingPhase{boundary - segment_start, current_jerk, entry.velocity,
                                  entry.acceleration});
    segment_start = boundary;
    if (boundary >= stop.duration()) {
      break;
    }
    entry = stop.sample(boundary);
    current_jerk = entry.jerk;
  }
  return phases;
}

double brakingDistanceM() { return frozenStop().stoppingDistance(); }

double predictedTotalM(double reaction_s) {
  // The braking distance does not depend on the reaction time: however long the
  // signal took, the state at brake onset is the frozen state.
  return kInitialVelocityMps * reaction_s + brakingDistanceM();
}

double collisionCrossingS() {
  return (kHazardPlaneM - brakingDistanceM()) / kInitialVelocityMps;
}

double residualTravelBelow(double threshold_mps) {
  const StopProfile stop = frozenStop();
  if (stop.duration() <= 0.0) {
    return 0.0;
  }
  // The first instant the speed is at or below the threshold, then the distance
  // from there to rest. Taken from the profile, because the tail of a
  // jerk-limited stop has acceleration going to zero with the velocity and a
  // constant-deceleration approximation of it would be wrong.
  double lo = 0.0;
  double hi = stop.duration();
  for (int i = 0; i < 100; ++i) {
    const double mid = 0.5 * (lo + hi);
    if (std::fabs(stop.sample(mid).velocity) > threshold_mps) {
      lo = mid;
    } else {
      hi = mid;
    }
  }
  return stop.restPosition() - stop.sample(hi).position;
}

std::string scenarioJson() {
  const StopProfile stop = frozenStop();
  const std::vector<BrakingPhase> phases = brakingPhases();
  std::ostringstream out = fixedStream(9);

  out << "{\n";
  out << "  \"schema\": \"pickcell.external-validation.scenario\",\n";
  out << "  \"scenario_id\": \"" << kScenarioId << "\",\n";
  out << "  \"version\": " << kActiveVersion << ",\n";
  out << "  \"frozen\": true,\n";
  out << "  \"provenance_legend\": {\n";
  out << "    \"MEASURED\": \"produced by this repository's own benchmarks. Context "
         "here, never an assumption this experiment relies on\",\n";
  out << "    \"CONFIGURED\": \"chosen for this scenario and held fixed\",\n";
  out << "    \"DERIVED\": \"computed from the entries above\",\n";
  out << "    \"REPORTED_BY_SIMULATOR\": \"an output of the external run, not "
         "predicted here\"\n";
  out << "  },\n";

  out << "  \"implementation_provenance\": {\n";
  out << "    \"provenance\": \"CONFIGURED\",\n";
  out << "    \"note\": \"a pickcell commit does not by itself identify the "
         "stopping law, because the library that computes it is fetched. These "
         "are the revisions this repository pins and builds against; they are "
         "supplied by the build system rather than typed here, so they cannot "
         "drift from what was actually compiled\",\n";
  out << "    \"motionkit\": \"" << PICKCELL_MOTIONKIT_REV << "\",\n";
  out << "    \"robot_contracts\": \"" << PICKCELL_CONTRACTS_REV << "\",\n";
  out << "    \"safeedge\": \"" << PICKCELL_SAFEEDGE_REV << "\"\n";
  out << "  },\n";

  out << "  \"experiment_design\": {\n";
  out << "    \"type\": \"one factor\",\n";
  out << "    \"independent_variable\": \"stop-signal polling interval\",\n";
  out << "    \"held_fixed\": \"the initial state, the limits and the commanded "
         "braking law, which are identical in every run\",\n";
  out << "    \"simulator_outputs\": [\"reaction time\", \"stopping distance\", "
         "\"clearance at stop\", \"collision state\"],\n";
  out << "    \"reaction_time_is_not_predicted_here\": \"the simulator reports the "
         "reaction time it observes at each interval. This package predicts the "
         "stopping distance CONDITIONAL on that reported value. Deriving reaction "
         "time from the polling interval would assert the relationship the "
         "measurement exists to establish\"\n";
  out << "  },\n";

  out << "  \"degrees_of_freedom\": 1,\n";
  out << "  \"motion_model\": {\n";
  out << "    \"provenance\": \"CONFIGURED\",\n";
  out << "    \"description\": \"a single translational degree of freedom along "
         "+x; straight line, constant direction, no gravity component along the "
         "travel axis, no joint-space coupling, no external contact before the "
         "hazard plane\",\n";
  out << "    \"initial_position_m\": 0.0,\n";
  out << "    \"initial_velocity_mps\": " << number(kInitialVelocityMps) << ",\n";
  out << "    \"initial_acceleration_mps2\": " << number(kInitialAccelerationMps2)
      << "\n";
  out << "  },\n";

  out << "  \"limits\": {\n";
  out << "    \"provenance\": \"CONFIGURED\",\n";
  out << "    \"max_velocity_mps\": " << number(frozenLimits().max_velocity) << ",\n";
  out << "    \"max_acceleration_mps2\": " << number(frozenLimits().max_acceleration)
      << ",\n";
  out << "    \"max_jerk_mps3\": " << number(frozenLimits().max_jerk) << "\n";
  out << "  },\n";

  out << "  \"trial_schedule\": {\n";
  out << "    \"provenance\": \"CONFIGURED\",\n";
  out << "    \"independent_variable\": \"polling_interval_s\",\n";
  out << "    \"controlled_nuisance_variable\": \"the phase of the hazard assertion "
         "within the poll window. For a fixed interval the reaction time depends on "
         "where the hazard lands relative to the polling schedule, so a hazard always "
         "asserted at the same absolute instant would phase-lock differently against "
         "different intervals and report artifacts of the chosen origin\",\n";
  out << "    \"hazard_assertion_time\": \"phase_fraction times polling_interval_s after "
         "a poll boundary, with the poller on a regular schedule\",\n";
  out << "    \"phase_fraction_generator\": \"frac(k / phi) for k = 1..5, phi = (1 + "
         "sqrt(5)) / 2. Both the generator and the concrete values are recorded so "
         "reproduction does not depend on anyone else approximating phi; the values "
         "below are authoritative and are emitted at round-trip-safe precision\",\n";
  out << "    \"phase_fraction_rationale\": \"a low-discrepancy sequence covers the "
         "window evenly from five points without clustering, and its generator is not a "
         "simple fraction of the interval the way a set such as 0.1, 0.3, 0.5, 0.7, 0.9 "
         "would be. That reduces the chance of landing on a timestep boundary; it does "
         "not eliminate it, since any floating-point value is rational\",\n";
  out << "    \"why_frozen_rather_than_random\": \"not because seeded randomness would "
         "be irreproducible, which it would not. A fixed list depends on no RNG "
         "implementation or seed-handling convention, every trial is enumerable before "
         "anything runs, an outside party can reproduce and audit the schedule by "
         "reading it, and the identical schedule applies across every polling "
         "interval\",\n";
  out << "    \"also_spreads_the_reaction_times\": \"the prediction is evaluated at each "
         "run own measured T, so sweeping phase produces a spread of T values. Without "
         "it the ladder could return one nearly identical value per interval and the "
         "linear prediction would be tested at five points rather than across its "
         "range\",\n";
  out << "    \"phase_fractions\": [";
  const std::size_t phase_count =
      sizeof(kHazardPhaseFractions) / sizeof(kHazardPhaseFractions[0]);
  for (std::size_t i = 0; i < phase_count; ++i) {
    out << roundTripNumber(kHazardPhaseFractions[i]) << (i + 1 < phase_count ? ", " : "");
  }
  out << "],\n";
  out << "    \"polling_intervals_s\": [";
  const std::size_t interval_count =
      sizeof(kPollIntervalsNs) / sizeof(kPollIntervalsNs[0]);
  for (std::size_t i = 0; i < interval_count; ++i) {
    out << seconds(kPollIntervalsNs[i]) << (i + 1 < interval_count ? ", " : "");
  }
  out << "],\n";
  out << "    \"trial_count\": " << (phase_count * interval_count) << ",\n";
  out << "    \"runs\": \"every polling interval crossed with every phase fraction, "
         "identically. Each trial reports its own T\",\n";
  out << "    \"phase_audit\": \"if the simulator can report the timestamp or tick of "
         "the poll boundary preceding each hazard assertion, the realised phase can be "
         "checked as observed_phase = (t_hazard_assertion - t_preceding_poll_boundary) / "
         "polling_interval against the frozen fraction. This is provenance and "
         "diagnostics; it is NOT an acceptance gate on the physics result\"\n";
  out << "  },\n";

  out << "  \"reference_reaction\": {\n";
  out << "    \"provenance\": \"MEASURED\",\n";
  out << "    \"note\": \"context and provenance, NOT the experiment's input. One "
         "point this repository measured, offered so the external numbers have "
         "something to sit beside\",\n";
  out << "    \"source\": \"evidence/reaction-vs-poll-interval.txt, HTTP poll "
         "link, worst case\",\n";
  out << "    \"poll_interval_s\": " << seconds(kReferenceReactionPollNs) << ",\n";
  out << "    \"reaction_s\": " << seconds(kReferenceReactionNs) << ",\n";
  out << "    \"control_period_s\": " << seconds(kControlPeriodNs) << "\n";
  out << "  },\n";

  out << "  \"commanded_braking_law\": {\n";
  out << "    \"provenance\": \"DERIVED\",\n";
  out << "    \"generator\": \"motionkit::StopProfile::plan\",\n";
  out << "    \"note\": \"this is the COMMAND, not a solution. The segments were "
         "recovered from the generated profile by locating the instants at which "
         "commanded jerk changes, not restated from a formula. An independent "
         "simulator should integrate this law with its own solver; its numerical "
         "answer is part of the result rather than something to match in "
         "advance\",\n";
  out << "    \"reaches_acceleration_limit\": "
      << (std::fabs(stop.peakAcceleration()) >= frozenLimits().max_acceleration - 1e-12
              ? "true"
              : "false")
      << ",\n";
  out << "    \"peak_acceleration_mps2\": " << number(stop.peakAcceleration()) << ",\n";
  out << "    \"duration_s\": " << number(stop.duration()) << ",\n";
  out << "    \"segment_count\": " << phases.size() << ",\n";
  out << "    \"segments\": [\n";
  for (std::size_t i = 0; i < phases.size(); ++i) {
    out << "      { \"index\": " << i
        << ", \"duration_s\": " << number(phases[i].duration_s)
        << ", \"jerk_mps3\": " << number(phases[i].jerk_mps3)
        << ", \"entry_velocity_mps\": " << number(phases[i].entry_velocity_mps)
        << ", \"entry_acceleration_mps2\": " << number(phases[i].entry_acceleration_mps2)
        << " }" << (i + 1 < phases.size() ? "," : "") << "\n";
  }
  out << "    ]\n";
  out << "  },\n";

  out << "  \"hazard_plane\": {\n";
  out << "    \"provenance\": \"CONFIGURED\",\n";
  out << "    \"distance_m\": " << number(kHazardPlaneM) << ",\n";
  out << "    \"description\": \"a plane at x = the distance above, normal to the "
         "direction of travel, measured from the initial tool position\",\n";
  out << "    \"interacts_with_the_body\": false,\n";
  out << "    \"zero_force_zero_collision_response\": \"REQUIRED. The plane must not be "
         "a physical collider. The quantity being cross-checked is the FREE stopping "
         "distance under the commanded braking law, and a rigid obstacle here would "
         "apply a contact impulse that alters the very trajectory the comparison is "
         "about. Realise it as a trigger volume, a sensor boundary with collision "
         "response disabled, or by post-processing an unobstructed trajectory -- "
         "whichever the simulator offers\",\n";
  out << "    \"crossing_is_observational\": \"crossing the plane is a derived verdict "
         "about a trajectory, not a physical event in it\",\n";
  out << "    \"analytical_crossing_reaction_s\": " << number(collisionCrossingS(), 9)
      << ",\n";
  out << "    \"chosen_so_the_verdict_varies\": \"the plane is reached once the reaction "
         "time exceeds that value, which falls inside the range this polling ladder "
         "produces. A plane the tool never reaches would yield a column of identical "
         "values rather than a check\",\n";
  out << "    \"role\": \"stopping distance is the primary physics result. Clearance and "
         "crossing are consequences of the simulated trajectory against this plane, not "
         "separate independent validations\"\n";
  out << "  },\n";

  out << "  \"semantics\": {\n";
  out << "    \"provenance\": \"CONFIGURED\",\n";
  out << "    \"reaction_time_T_definition\": \"T = t_profile_start - "
         "t_hazard_assertion. This is authoritative and is NOT the same as stop-signal "
         "observation latency\",\n";
  out << "    \"t_hazard_assertion\": \"the simulation timestamp at which the stop "
         "condition becomes true\",\n";
  out << "    \"t_profile_start\": \"the simulation timestamp of the FIRST INTEGRATION "
         "STEP that applies the frozen braking jerk, i.e. the step at which commanded "
         "jerk becomes -40 m/s^3. Where a simulator distinguishes command issuance, "
         "command activation, and the first integration step using the new value, it is "
         "the THIRD that defines this boundary\",\n";
  out << "    \"why_that_boundary\": \"the prediction splits the motion into "
         "constant-speed travel and integration of the braking profile. T must be "
         "exactly the duration of the first, or the reaction_travel term is not "
         "initial_velocity * T. A simulator can observe the stop signal on one tick and "
         "not apply the braking command until a later one; that interval belongs inside "
         "T\",\n";
  out << "    \"t_stop_signal_observed\": \"requested as well, but DIAGNOSTIC only. It "
         "is not necessarily the endpoint of T\",\n";
  out << "    \"decomposition\": \"where a simulator has both stages: "
         "signal_detection_latency = t_stop_signal_observed - t_hazard_assertion, "
         "control_application_latency = t_profile_start - t_stop_signal_observed, and T "
         "is their sum\",\n";
  out << "    \"braking_begins\": \"at t_profile_start. Velocity is the frozen initial "
         "velocity throughout [t_hazard_assertion, t_profile_start) and the commanded "
         "braking law is integrated from t_profile_start\",\n";
  out << "    \"stopped\": \"the commanded law reaches exactly zero velocity and "
         "zero acceleration at the end of its final segment. A simulator "
         "declaring a velocity threshold instead should report the threshold it "
         "used; stop_threshold_residual gives the distance that choice omits\",\n";
  out << "    \"stopping_distance\": \"position at rest minus position at the "
         "safety decision, along +x. It includes the travel during the reaction "
         "interval\",\n";
  out << "    \"braking_distance\": \"position at rest minus position at the "
         "start of braking. Independent of the reaction time\",\n";
  out << "    \"clearance_at_stop\": \"hazard plane distance minus stopping "
         "distance. Positive means the tool stopped short of the plane\",\n";
  out << "    \"collision\": \"stopping distance greater than the hazard plane "
         "distance, subject to the indeterminate band above\",\n";
  out << "    \"direction\": \"motion is along +x throughout; the axis does not "
         "reverse, and the commanded law ends at zero velocity and zero "
         "acceleration\"\n";
  out << "  },\n";

  out << "  \"out_of_scope\": [\n";
  out << "    \"the Linux, HTTP and shared-memory implementations this repository "
         "measures. A simulated polling model is not those implementations\",\n";
  out << "    \"any claim that the simulated reaction time validates this "
         "repository's measured reaction time. Both are reported; neither "
         "confirms the other\",\n";
  out << "    \"any claim of safety certification or functional-safety "
         "compliance\",\n";
  out << "    \"multi-axis or joint-space behaviour: this is one degree of "
         "freedom by construction\"\n";
  out << "  ]\n";
  out << "}\n";
  return out.str();
}

std::string predictionJson() {
  const StopProfile stop = frozenStop();
  std::ostringstream out = fixedStream(9);

  out << "{\n";
  out << "  \"schema\": \"pickcell.external-validation.prediction\",\n";
  out << "  \"scenario_id\": \"" << kScenarioId << "\",\n";
  out << "  \"version\": " << kActiveVersion << ",\n";
  out << "  \"preregistered\": true,\n";
  out << "  \"note\": \"produced by the same code path as the public evidence. "
         "Nothing here is hand-entered, and nothing here may be revised after an "
         "external result exists\",\n";

  out << "  \"prediction_is_a_function_of_reaction_time\": {\n";
  out << "    \"provenance\": \"DERIVED\",\n";
  out << "    \"why\": \"the simulator varies the polling interval and reports the "
         "reaction time it observes. Predicting a fixed distance would require "
         "predicting that reaction time, which this package deliberately does "
         "not do\",\n";
  out << "    \"formula\": \"total_stopping_distance_m = initial_velocity_mps * T + "
         "braking_distance_m\",\n";
  out << "    \"T_is\": \"t_profile_start - t_hazard_assertion, as defined in the "
         "scenario semantics. Not stop-signal observation latency: the formula is only "
         "exact if T is the duration of the constant-speed phase, which ends where "
         "profile integration begins\",\n";
  out << "    \"initial_velocity_mps\": " << number(kInitialVelocityMps) << ",\n";
  out << "    \"braking_distance_m\": " << number(brakingDistanceM()) << ",\n";
  out << "    \"braking_duration_s\": " << number(stop.duration()) << ",\n";
  out << "    \"braking_distance_independent_of_reaction_time\": \"the state at "
         "brake onset is the frozen state however long the signal took, so the "
         "braking term is constant across every run\",\n";
  out << "    \"clearance_at_stop_m\": \"hazard_plane_m - total_stopping_distance_m\",\n";
  out << "    \"hazard_crossing_classification\": \"stated symbolically, because the "
         "numeric distance tolerance delta_m is deliberately not instantiated until the "
         "simulator parameters are known. Given the frozen delta_m from gate 2: "
         "stop_position < hazard_plane_m - delta_m is CLEAR; stop_position > "
         "hazard_plane_m + delta_m is CROSSED; otherwise INDETERMINATE. The "
         "classification therefore inherits the declared numerical uncertainty rather "
         "than introducing a constant of its own\",\n";
  out << "    \"analytical_boundary_m\": \"the exact boundary is "
         "total_stopping_distance_m = hazard_plane_m, which for the analytical "
         "prediction occurs at the reaction time below\",\n";
  out << "    \"collision_crossing_reaction_s\": " << number(collisionCrossingS(), 9)
      << "\n";
  out << "  },\n";

  out << "  \"worked_example\": {\n";
  out << "    \"provenance\": \"MEASURED input, DERIVED outputs\",\n";
  out << "    \"note\": \"the formula evaluated at ONE reaction time this "
         "repository measured, so the arithmetic can be checked before any "
         "external run. It is not a prediction of what the simulator will "
         "observe\",\n";
  out << "    \"reaction_s\": " << seconds(kReferenceReactionNs) << ",\n";
  out << "    \"reaction_travel_m\": "
      << number(kInitialVelocityMps * static_cast<double>(kReferenceReactionNs) / 1e9)
      << ",\n";
  out << "    \"braking_distance_m\": " << number(brakingDistanceM()) << ",\n";
  out << "    \"total_stopping_distance_m\": "
      << number(predictedTotalM(static_cast<double>(kReferenceReactionNs) / 1e9))
      << ",\n";
  out << "    \"clearance_at_stop_m\": "
      << number(kHazardPlaneM -
                predictedTotalM(static_cast<double>(kReferenceReactionNs) / 1e9))
      << ",\n";
  out << "    \"hazard_crossing\": \"analytically the stop lies beyond the plane at this "
         "reaction time. A final CLEAR / CROSSED / INDETERMINATE classification is not "
         "stated here, because it depends on delta_m, which is instantiated only once "
         "the simulator parameters are known\"\n";
  out << "  },\n";

  out << "  \"stop_threshold_residual\": {\n";
  out << "    \"provenance\": \"DERIVED\",\n";
  out << "    \"description\": \"distance still to travel at the moment speed "
         "first falls below the threshold. Taken from the generated profile, not "
         "from a constant-deceleration tail approximation, because the tail of a "
         "jerk-limited stop has acceleration going to zero with the velocity\",\n";
  out << "    \"by_threshold_mps\": [\n";
  const std::size_t threshold_count =
      sizeof(kStopThresholds) / sizeof(kStopThresholds[0]);
  for (std::size_t i = 0; i < threshold_count; ++i) {
    out << "      { \"threshold_mps\": " << number(kStopThresholds[i], 6)
        << ", \"residual_travel_m\": "
        << number(residualTravelBelow(kStopThresholds[i]), 12) << " }"
        << (i + 1 < threshold_count ? "," : "") << "\n";
  }
  out << "    ]\n";
  out << "  },\n";

  out << "  \"agreement_rule\": {\n";
  out << "    \"fixed_before_any_external_result\": true,\n";
  out << "    \"gate_1_same_experiment\": {\n";
  out << "      \"what_it_checks\": \"that both sides ran the same experiment, "
         "NOT that they arrived at the same numbers. It is a semantic gate\",\n";
  out << "      \"requires_confirmation_of\": [\n";
  out << "        \"one translational degree of freedom, motion along +x, no "
         "reversal\",\n";
  out << "        \"initial velocity and initial acceleration as given in the "
         "scenario\",\n";
  out << "        \"the acceleration and jerk limits as given\",\n";
  out << "        \"the commanded segment durations and jerk values as given\",\n";
  out << "        \"no gravity component along the travel axis and no external "
         "contact before the hazard plane\",\n";
  out << "        \"velocity held constant during the reaction interval\"\n";
  out << "      ],\n";
  out << "      \"explicitly_not_required\": \"that the simulator's integrated "
         "velocity trace reproduce the reference trace. Numerical deviation in "
         "the integrated solution is part of what this cross-check is measuring; "
         "treating it as a precondition would discard the result being sought\",\n";
  out << "      \"outcome_if_failed\": \"a different experiment was run. The "
         "distance comparison is not meaningful and the mismatch is the finding\"\n";
  out << "    },\n";
  out << "    \"gate_2_distance\": {\n";
  out << "      \"quantity\": \"total_stopping_distance_m, per run, against the "
         "formula evaluated at that run's reported reaction time\",\n";
  out << "      \"tolerance_terms\": {\n";
  out << "        \"stop_threshold_residual_m\": \"read from the table above at "
         "the velocity threshold the simulator actually used\",\n";
  out << "        \"numerical_solution_error_m\": \"NOT ASSUMED HERE. A per-step "
         "position error proportional to velocity times timestep is a heuristic "
         "for a first-order explicit scheme and is not a bound for an arbitrary "
         "integrator, so this term is instantiated from the parameters below "
         "rather than guessed. It is not zero merely because gate 1 passed: "
         "agreeing on the experiment says nothing about the numerical error in "
         "solving it\",\n";
  out << "        \"reaction_time_resolution_m\": \"initial_velocity_mps times the "
         "resolution or quantisation of the reported reaction time, since the "
         "prediction is evaluated at that reported value\"\n";
  out << "      },\n";
  out << "      \"agreement\": \"absolute difference within the sum of the "
         "instantiated terms\",\n";
  out << "      \"meaningful_deviation\": \"absolute difference exceeding that "
         "sum. This is a discrepancy requiring investigation, not proof that "
         "either model is wrong: it could arise from modelling assumptions, "
         "numerical integration, experiment semantics, configuration or a defect "
         "on either side\",\n";
  out << "      \"forbidden\": \"instantiating or widening any term after the "
         "measured stopping distances are known\",\n";
  out << "      \"protocol\": {\n";
  out << "        \"why_two_phases\": \"asking for solver settings and measured "
         "distances in one exchange, then claiming the tolerance was fixed before the "
         "distances were seen, is a contradiction. The phases separate them so the "
         "ordering is real rather than asserted\",\n";
  out << "        \"phase_a_metadata_only\": [\n";
  out << "          \"simulator version and exact commit revision\",\n";
  out << "          \"physics timestep\",\n";
  out << "          \"substeps per step, if the integrator uses them\",\n";
  out << "          \"solver or integrator identification and any settings that bear on "
         "local and accumulated position error\",\n";
  out << "          \"the velocity threshold at which the run declares the axis stopped, "
         "and its semantics\",\n";
  out << "          \"the resolution at which reaction time is reported, and whether "
         "values are rounded, truncated, or taken directly from simulation ticks\",\n";
  out << "          \"the raw simulation timestamps or tick indices for "
         "t_hazard_assertion, t_stop_signal_observed and t_profile_start. The first and "
         "third define T; the second is diagnostic and lets T be decomposed into "
         "detection and control-application latency. Supplying the ticks lets T be "
         "recomputed from the simulation timeline rather than trusted as a rounded "
         "scalar\",\n";
  out << "          \"how poll boundaries are represented, so the frozen phase schedule "
         "can be expressed in terms the implementation actually has\"\n";
  out << "        ],\n";
  out << "        \"then\": \"delta_m is instantiated and frozen, and the frozen value "
         "is committed to this repository in its own commit before any measured distance "
         "is received. Git history then carries the ordering, which is the only part of "
         "this protocol that cannot be asserted after the fact\",\n";
  out << "        \"phase_b_measured_outputs\": [\n";
  out << "          \"per trial, keyed by polling interval and phase fraction: "
         "t_hazard_assertion, t_stop_signal_observed, t_profile_start, the resulting T, "
         "stopping distance, clearance, and hazard-plane crossing state\",\n";
  out << "          \"if practical, the timestamp or tick of the poll boundary preceding "
         "each hazard assertion, so the realised phase can be audited against the frozen "
         "fraction. Provenance, not an acceptance gate\",\n";
  out << "          \"optionally the simulated velocity trace\",\n";
  out << "          \"commands or configuration sufficient to reproduce the run\"\n";
  out << "        ],\n";
  out << "        \"if_already_run\": \"the measured outputs should be withheld until "
         "the tolerance freeze is acknowledged. Having the numbers in hand is not the "
         "problem; our seeing them before the criterion is fixed is\"\n";
  out << "      },\n";
  out << "      \"ordering\": \"the numeric tolerance is frozen AFTER phase A and BEFORE "
         "phase B. Fixing it earlier would mean guessing at an integrator; fixing it "
         "later would mean choosing a tolerance that accommodates the answer\"\n";
  out << "    },\n";
  out << "    \"diagnostic_trace_comparison\": {\n";
  out << "      \"status\": \"diagnostic, not a gate\",\n";
  out << "      \"purpose\": \"if the simulator can return its own velocity trace, "
         "comparing it against the reference trace localises WHERE a distance "
         "discrepancy arises rather than only that one exists\",\n";
  out << "      \"tolerance\": \"instantiated with the same parameters as gate 2 "
         "and applied per sample to velocity. A deviation here is information "
         "about the integrator, and on its own does not invalidate the run\"\n";
  out << "    }\n";
  out << "  }\n";
  out << "}\n";
  return out.str();
}

std::string referenceTraceCsv() {
  const StopProfile stop = frozenStop();
  std::ostringstream out = fixedStream(9);

  out << "# pickcell external-validation braking reference trace\n";
  out << "# scenario_id," << kScenarioId << "\n";
  out << "# version," << kActiveVersion << "\n";
  out << "#\n";
  out << "# t_s is measured from the START OF BRAKING, not from the safety\n";
  out << "# decision. Before braking begins the axis travels at constant\n";
  out << "# velocity for whatever reaction time the run reports.\n";
  out << "#\n";
  out << "# DIAGNOSTIC, not a gate and not a result to replay. The commanded\n";
  out << "# law is the segment list in the scenario file; this is one solution\n";
  out << "# of it. An independent simulator should integrate that law with its\n";
  out << "# own solver and report its own trace -- a difference between the two\n";
  out << "# is information about the integrators, not a failure.\n";
  out << "t_s,velocity_mps,acceleration_mps2,jerk_mps3\n";

  constexpr double kStep = 0.001;
  const int steps = static_cast<int>(std::lround(stop.duration() / kStep));
  for (int i = 0; i <= steps; ++i) {
    const double t = std::min(static_cast<double>(i) * kStep, stop.duration());
    const MotionSample s = stop.sample(t);
    out << number(t, 6) << "," << number(s.velocity) << "," << number(s.acceleration)
        << "," << number(s.jerk) << "\n";
  }
  if (std::fabs(static_cast<double>(steps) * kStep - stop.duration()) > 1e-12) {
    out << number(stop.duration(), 6) << "," << number(0.0) << "," << number(0.0) << ","
        << number(0.0) << "\n";
  }
  return out.str();
}

}  // namespace pickcell::validation
