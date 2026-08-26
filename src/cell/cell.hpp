// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "cell/safety_link.hpp"
#include "motionkit/core/frame_graph.hpp"
#include "motionkit/core/se3.hpp"
#include "robot/v1/motion.pb.h"

namespace pickcell {

/// Where the cell is in its cycle.
///
/// `kHeldBySafety` is a state of the cell, not of the safety runtime. The
/// runtime says whether torque is permitted; deciding to stop, and staying
/// stopped until told otherwise, is the cell's own job.
enum class CellPhase : std::uint8_t {
  kIdle,
  kApproachBin,
  kDescendToPart,
  kGrasp,
  kLift,
  kTransferToFixture,
  kPlace,
  kRetreat,
  kHeldBySafety,
};

const char* toString(CellPhase phase) noexcept;

/// A pick-and-place cell: a six-axis arm, a bin of parts and a fixture.
///
/// Assembled from the other repositories rather than reimplementing them.
/// motionkit owns the frames and the pose arithmetic, robot-contracts owns the
/// wire format of the status this publishes, and the safety verdict arrives
/// through a SafetyLink whose implementation the cell deliberately does not
/// know about.
class Cell {
 public:
  struct Config {
    /// How far above a part the tool waits before descending.
    double approach_height_m = 0.15;
    /// Cartesian speed of the tool during a transfer.
    double transfer_speed_mps = 0.5;
    /// Control period. Everything here is integrated against this, so a cell
    /// stepped at a different rate moves at the same speed.
    std::uint64_t period_ns = 1'000'000;

    /// How old a safety permit may be before the cell stops believing it.
    ///
    /// Without this the cell runs forever on the last permit it received. That
    /// is not hypothetical: stopping the safety runtime under docker compose
    /// left the cell completing pick-and-place cycles while every poll failed,
    /// because a cached "torque permitted" never expired.
    ///
    /// A permit is a statement about a moment, not a standing grant. 250 ms is
    /// comfortably longer than the default 100 ms poll -- long enough not to
    /// trip on one dropped request, short enough that a dead runtime stops the
    /// machine rather than being outlived by its own last answer.
    std::uint64_t max_safety_age_ns = 250'000'000;
  };

  Cell(SafetyLink& safety, Config config);

  /// One control cycle.
  void step(std::uint64_t now_ns);

  CellPhase phase() const noexcept { return phase_; }
  const motionkit::FrameGraph& frames() const noexcept { return frames_; }

  /// Current tool pose in the world frame.
  motionkit::SE3 toolPose() const;

  /// Status in the wire format, ready to publish.
  robot::v1::MotionStatus status() const;

  /// Completed pick-and-place cycles.
  std::uint64_t cycles_completed() const noexcept { return cycles_completed_; }

  /// How long the most recent safety stop took, measured from the instant the
  /// safety source stamped its decision to the instant this cell stopped
  /// commanding motion.
  ///
  /// This is the number the whole repository is built to produce. It spans the
  /// link, the poll interval, the control period and the cell's own reaction --
  /// which is to say it measures the *integration*, not any one component.
  std::uint64_t last_stop_reaction_ns() const noexcept { return last_stop_reaction_ns_; }
  bool has_stop_measurement() const noexcept { return has_stop_measurement_; }

  /// Cycles held because the newest safety information was too old to act on.
  std::uint64_t cycles_on_stale_safety() const noexcept { return cycles_on_stale_safety_; }

  /// Cycles in which the safety link reported nothing usable. A cell that has
  /// never heard from the safety runtime must not move, so these are cycles in
  /// which it did not.
  std::uint64_t cycles_without_safety() const noexcept { return cycles_without_safety_; }

 private:
  void buildFrames();
  void advance(std::uint64_t now_ns);
  motionkit::SE3 targetForPhase(CellPhase phase) const;

  SafetyLink& safety_;
  Config config_;

  motionkit::FrameGraph frames_;
  motionkit::FrameId world_{};
  motionkit::FrameId base_{};
  motionkit::FrameId flange_{};
  motionkit::FrameId tcp_{};
  motionkit::FrameId bin_{};
  motionkit::FrameId fixture_{};

  CellPhase phase_ = CellPhase::kIdle;
  CellPhase phase_before_hold_ = CellPhase::kIdle;
  double progress_ = 0.0;  ///< 0..1 through the current segment
  motionkit::SE3 segment_start_{};

  std::uint64_t cycles_completed_ = 0;
  std::uint64_t cycles_without_safety_ = 0;
  std::uint64_t cycles_on_stale_safety_ = 0;
  std::uint64_t last_stop_reaction_ns_ = 0;
  bool has_stop_measurement_ = false;
  bool was_permitted_ = false;
  std::uint64_t last_sequence_ = 0;
  std::string command_id_;
};

}  // namespace pickcell
