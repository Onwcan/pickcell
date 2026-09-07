// SPDX-License-Identifier: Apache-2.0
//
// pickcell-safety-distance -- what the measured reaction time costs in metres.
//
// The reaction figures this repository produces are milliseconds, and
// milliseconds are not what a cell is laid out in. This converts them, using
// motionkit's jerk-limited stop for the part the cell cannot do instantly:
// however quickly the cell learns, the arm still has to decelerate.
//
// Two tables. The first says how far the tool travels at a given speed with a
// given link; the second inverts it -- given the clearance a cell actually has,
// how fast may it run.

#include <cstdint>
#include <cstdio>
#include <string>

#include "motionkit/core/trajectory.hpp"

#include "cell/safety_distance.hpp"

namespace {

using pickcell::kCellAxisLimits;

/// The links this cell can be built with, and the worst-case reaction each one
/// produces.
///
/// Worst case rather than median, deliberately. A guard is positioned for the
/// slowest reaction the system can have, not the usual one -- and for a poll
/// the worst case is not a measurement at all, it is the poll interval, because
/// an event at a uniformly random moment inside a fixed window is discovered at
/// worst a full window later.
struct Link {
  const char* name;
  std::uint64_t worst_reaction_ns;
  const char* basis;
};

constexpr Link kLinks[] = {
    {"shared memory", 1'100'000, "one control period, measured max 1.07 ms"},
    {"HTTP poll 10 ms", 10'700'000, "measured max 10.7 ms"},
    {"HTTP poll 100 ms", 96'100'000, "measured max 96.1 ms"},
    {"HTTP poll 200 ms", 191'000'000, "measured max 191.0 ms"},
};

double millimetres(double metres) { return metres * 1000.0; }

}  // namespace

int main() {
  std::printf(
      "How far the tool travels after the safety runtime decides\n"
      "=========================================================\n\n"
      "Axis limits: %.1f m/s, %.0f m/s^2, %.0f m/s^3.\n\n"
      "Reaction travel is the distance covered before the cell has reacted --\n"
      "linear in speed and in the reaction time. Braking is what the arm covers\n"
      "decelerating under those limits, which motionkit computes and which grows\n"
      "faster than linearly. A guard is positioned from the sum.\n\n",
      kCellAxisLimits.max_velocity, kCellAxisLimits.max_acceleration,
      kCellAxisLimits.max_jerk);

  for (const double speed : {0.25, 0.5, 1.0, 2.0}) {
    std::printf("At %.2f m/s\n", speed);
    std::printf("  %-18s %12s %12s %12s\n", "link", "reaction", "braking", "total");
    std::printf("  %-18s %12s %12s %12s\n", "", "mm", "mm", "mm");
    for (const Link& link : kLinks) {
      const auto distance =
          pickcell::stoppingDistance(speed, link.worst_reaction_ns, kCellAxisLimits);
      if (!distance) {
        std::printf("  %-18s  %s\n", link.name, toString(distance.error).data());
        continue;
      }
      std::printf("  %-18s %12.1f %12.1f %12.1f\n", link.name,
                  millimetres(distance.value.reaction_travel_m),
                  millimetres(distance.value.braking_distance_m),
                  millimetres(distance.value.total_m));
    }
    // The braking column is identical down every row, which is the point of
    // printing it: the link cannot change how the arm decelerates, only how
    // long it takes to be told to.
    const auto reference = pickcell::stoppingDistance(speed, 0, kCellAxisLimits);
    if (reference) {
      std::printf("  braking alone takes %.0f ms and is the same for every link\n\n",
                  reference.value.braking_seconds * 1000.0);
    }
  }

  std::printf(
      "The same question from the other side\n"
      "-------------------------------------\n"
      "Given the clearance a cell actually has, how fast may it run?\n\n");
  std::printf("  %-18s", "clearance");
  for (const Link& link : kLinks) {
    std::printf(" %16s", link.name);
  }
  std::printf("\n  %-18s", "");
  for (std::size_t i = 0; i < sizeof(kLinks) / sizeof(kLinks[0]); ++i) {
    std::printf(" %16s", "m/s");
  }
  std::printf("\n");

  for (const double clearance : {0.05, 0.1, 0.2, 0.5, 1.0}) {
    std::printf("  %15.0f mm", millimetres(clearance));
    for (const Link& link : kLinks) {
      const auto speed =
          pickcell::permittedSpeed(clearance, link.worst_reaction_ns, kCellAxisLimits);
      std::printf(" %16.2f", speed ? speed.value : 0.0);
    }
    std::printf("\n");
  }

  // Computed rather than written down, so the observation cannot outlive the
  // numbers it is about.
  const auto ratioAt = [](double speed) -> double {
    const auto fast =
        pickcell::stoppingDistance(speed, kLinks[0].worst_reaction_ns, kCellAxisLimits);
    const auto slow =
        pickcell::stoppingDistance(speed, kLinks[3].worst_reaction_ns, kCellAxisLimits);
    return (fast && slow) ? slow.value.total_m / fast.value.total_m : 0.0;
  };
  std::printf(
      "The penalty runs the other way to the instinct\n"
      "---------------------------------------------\n"
      "Swapping shared memory for a 200 ms poll multiplies the guard distance\n"
      "by %.1fx at 0.25 m/s and by only %.1fx at 2.00 m/s.\n\n"
      "The absolute cost is worse on a fast cell -- %.0f mm against %.0f mm -- and\n"
      "the relative cost is worse on a slow one, because braking distance grows\n"
      "faster than linearly with speed while reaction travel does not. A slow\n"
      "machine has almost no braking distance to hide a slow link behind. \"We\n"
      "run slowly here, so latency does not matter\" has it backwards.\n\n",
      ratioAt(0.25), ratioAt(2.0),
      millimetres(
          pickcell::stoppingDistance(2.0, kLinks[3].worst_reaction_ns, kCellAxisLimits)
              .value.total_m -
          pickcell::stoppingDistance(2.0, kLinks[0].worst_reaction_ns, kCellAxisLimits)
              .value.total_m),
      millimetres(
          pickcell::stoppingDistance(0.25, kLinks[3].worst_reaction_ns, kCellAxisLimits)
              .value.total_m -
          pickcell::stoppingDistance(0.25, kLinks[0].worst_reaction_ns, kCellAxisLimits)
              .value.total_m));

  std::printf(
      "\nWhat this says\n"
      "--------------\n"
      "The poll interval is not only a latency. It is floor space, and it is\n"
      "cycle time. Every millisecond of reaction costs the tool's speed in\n"
      "millimetres of clearance, and a cell that cannot have the clearance pays\n"
      "for it by running slower instead.\n\n"
      "The braking term is the floor. No link, however fast, gets below it --\n"
      "which is the honest limit on what this repository's measurement can buy.\n"
      "What the measurement decides is how much is added on top.\n\n"
      "Reaction figures are worst cases, from evidence/reaction-vs-poll-interval.txt.\n"
      "A guard is positioned for the slowest reaction the system can have, and\n"
      "for a poll that is not a measurement at all: it is the poll interval.\n");
  return 0;
}
