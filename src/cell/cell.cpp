// SPDX-License-Identifier: Apache-2.0
#include "cell/cell.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace pickcell {
namespace {

using motionkit::SE3;
using motionkit::SO3;
using motionkit::Vec3;

constexpr motionkit::Scalar kPi = std::numbers::pi_v<motionkit::Scalar>;

/// Straight-line interpolation between two poses.
///
/// Linear in position, and shortest-arc in orientation via the SO(3) log/exp
/// pair -- interpolating quaternion components directly would not stay on the
/// manifold, and interpolating Euler angles would take a different path through
/// space depending on which convention was picked.
SE3 interpolate(const SE3& from, const SE3& to, double t) {
  const double clamped = std::clamp(t, 0.0, 1.0);
  const Vec3 position = from.translation() + (to.translation() - from.translation()) * clamped;
  const SO3 relative = from.rotation().inverse() * to.rotation();
  const SO3 rotation =
      from.rotation() * SO3::fromRotationVector(relative.rotationVector() * clamped);
  return SE3(rotation, position);
}

}  // namespace

const char* toString(CellPhase phase) noexcept {
  switch (phase) {
    case CellPhase::kIdle:
      return "idle";
    case CellPhase::kApproachBin:
      return "approach_bin";
    case CellPhase::kDescendToPart:
      return "descend_to_part";
    case CellPhase::kGrasp:
      return "grasp";
    case CellPhase::kLift:
      return "lift";
    case CellPhase::kTransferToFixture:
      return "transfer_to_fixture";
    case CellPhase::kPlace:
      return "place";
    case CellPhase::kRetreat:
      return "retreat";
    case CellPhase::kHeldBySafety:
      return "held_by_safety";
  }
  return "unknown";
}

Cell::Cell(SafetyLink& safety, Config config) : safety_(safety), config_(config) {
  buildFrames();
  segment_start_ = toolPose();
  command_id_ = "cycle-0";
}

void Cell::buildFrames() {
  // The cell layout, as a frame tree. Reading `a_T_b` as "the pose of b
  // expressed in a" throughout, matching motionkit's convention.
  frames_.reserve(8);
  world_ = frames_.declareRoot("world").value;

  // The arm stands on the floor, rotated a quarter turn from the world axes
  // because that is how it was bolted down. Exactly the kind of detail that a
  // frame graph exists to stop anyone having to remember.
  base_ = frames_
              .declareFrame("base", world_,
                            SE3(SO3::fromRPY(0.0, 0.0, kPi / 2),
                                Vec3{0.0, 0.0, 0.35}))
              .value;

  flange_ = frames_
                .declareFrame("flange", base_,
                              SE3(SO3::fromRPY(0.0, -kPi / 2, 0.0),
                                  Vec3{0.45, 0.0, 0.60}))
                .value;

  // Tool centre point: 125 mm along the flange's z axis.
  tcp_ = frames_
             .declareFrame("tcp", flange_, SE3::fromTranslation(Vec3{0.0, 0.0, 0.125}))
             .value;

  // Bin and fixture are furniture: fixed in the world, not on the arm.
  bin_ = frames_
             .declareFrame("bin", world_,
                           SE3(SO3::fromRPY(0.0, 0.0, 0.0), Vec3{0.55, -0.30, 0.10}))
             .value;

  fixture_ = frames_
                 .declareFrame("fixture", world_,
                               SE3(SO3::fromRPY(0.0, 0.0, 0.4), Vec3{0.55, 0.30, 0.12}))
                 .value;
}

motionkit::SE3 Cell::toolPose() const {
  const auto pose = frames_.lookup(world_, tcp_);
  return pose ? pose.value : SE3{};
}

motionkit::SE3 Cell::targetForPhase(CellPhase phase) const {
  const auto bin = frames_.lookup(world_, bin_);
  const auto fixture = frames_.lookup(world_, fixture_);
  if (!bin || !fixture) {
    return toolPose();
  }

  // The tool points down at whatever it is working over.
  const SO3 tool_down = SO3::fromRPY(kPi, 0.0, 0.0);
  const Vec3 above{0.0, 0.0, config_.approach_height_m};

  switch (phase) {
    case CellPhase::kApproachBin:
      return SE3(tool_down, bin.value.translation() + above);
    case CellPhase::kDescendToPart:
    case CellPhase::kGrasp:
      return SE3(tool_down, bin.value.translation());
    case CellPhase::kLift:
      return SE3(tool_down, bin.value.translation() + above);
    case CellPhase::kTransferToFixture:
      return SE3(tool_down, fixture.value.translation() + above);
    case CellPhase::kPlace:
      return SE3(tool_down, fixture.value.translation());
    case CellPhase::kRetreat:
      return SE3(tool_down, fixture.value.translation() + above);
    case CellPhase::kIdle:
    case CellPhase::kHeldBySafety:
      break;
  }
  return toolPose();
}

void Cell::advance(std::uint64_t /*now_ns*/) {
  static constexpr CellPhase kOrder[] = {
      CellPhase::kApproachBin, CellPhase::kDescendToPart, CellPhase::kGrasp,
      CellPhase::kLift,        CellPhase::kTransferToFixture, CellPhase::kPlace,
      CellPhase::kRetreat,
  };

  if (phase_ == CellPhase::kIdle) {
    phase_ = CellPhase::kApproachBin;
    progress_ = 0.0;
    segment_start_ = toolPose();
    return;
  }

  const auto* it = std::find(std::begin(kOrder), std::end(kOrder), phase_);
  if (it == std::end(kOrder)) {
    return;
  }
  const auto* next = it + 1;
  if (next == std::end(kOrder)) {
    ++cycles_completed_;
    command_id_ = "cycle-" + std::to_string(cycles_completed_);
    phase_ = CellPhase::kApproachBin;
  } else {
    phase_ = *next;
  }
  progress_ = 0.0;
  segment_start_ = toolPose();
}

void Cell::step(std::uint64_t now_ns) {
  const SafetyView safety = safety_.poll();

  if (!safety.valid) {
    // No safety information is not a report of safety. The cell does not move.
    ++cycles_without_safety_;
    if (phase_ != CellPhase::kHeldBySafety) {
      phase_before_hold_ = phase_;
      phase_ = CellPhase::kHeldBySafety;
    }
    was_permitted_ = false;
    return;
  }

  // A permit expires. The link reports when its information was obtained; if
  // that was too long ago, the cell has no current statement about safety and
  // must behave as though it had none at all.
  // Against published_monotonic_ns, not observed: the question is how long ago
  // the writer was last known to be alive, which for a shared-memory link is not
  // the same as how long ago this reader looked.
  const bool stale = now_ns > safety.published_monotonic_ns &&
                     (now_ns - safety.published_monotonic_ns) > config_.max_safety_age_ns;
  if (stale) {
    ++cycles_on_stale_safety_;
    if (phase_ != CellPhase::kHeldBySafety) {
      phase_before_hold_ = phase_;
      phase_ = CellPhase::kHeldBySafety;
    }
    was_permitted_ = false;
    return;
  }

  if (!safety.torque_permitted) {
    // The transition from permitted to not is where the reaction time is
    // measured. Only on the edge, and only once per assertion -- measuring on
    // every cycle while held would report the age of the signal rather than how
    // long the cell took to react to it.
    if (was_permitted_ && safety.sequence != last_sequence_) {
      last_stop_reaction_ns_ =
          safety.observed_monotonic_ns > safety.asserted_monotonic_ns
              ? safety.observed_monotonic_ns - safety.asserted_monotonic_ns
              : 0;
      has_stop_measurement_ = true;
      last_sequence_ = safety.sequence;
    }
    if (phase_ != CellPhase::kHeldBySafety) {
      phase_before_hold_ = phase_;
      phase_ = CellPhase::kHeldBySafety;
    }
    was_permitted_ = false;
    return;
  }

  // Torque is permitted again. Resume where the hold interrupted, rather than
  // restarting the cycle -- a cell that restarts on every safety event will
  // drop the part it was holding.
  if (phase_ == CellPhase::kHeldBySafety) {
    phase_ = phase_before_hold_;
    segment_start_ = toolPose();
  }
  was_permitted_ = true;
  last_sequence_ = safety.sequence;

  if (phase_ == CellPhase::kIdle) {
    advance(now_ns);
    return;
  }

  const SE3 target = targetForPhase(phase_);
  const double distance = (target.translation() - segment_start_.translation()).norm();
  const double period_s = static_cast<double>(config_.period_ns) * 1e-9;

  if (distance < 1e-9) {
    // A zero-length segment -- a grasp, or a move to where we already are.
    progress_ = 1.0;
  } else {
    progress_ += config_.transfer_speed_mps * period_s / distance;
  }

  const SE3 commanded = interpolate(segment_start_, target, progress_);

  // Commanding the tool means moving the frame the tool hangs off. The frame
  // graph then reports every other relationship in the cell for free.
  const auto world_T_flange_now = frames_.lookup(world_, flange_);
  const auto flange_T_tcp = frames_.transformToParent(tcp_);
  if (world_T_flange_now && flange_T_tcp) {
    const SE3 desired_flange = commanded * flange_T_tcp.value.inverse();
    const auto base_T_world = frames_.lookup(base_, world_);
    if (base_T_world) {
      frames_.setTransform(flange_, base_T_world.value * desired_flange);
    }
  }

  if (progress_ >= 1.0) {
    advance(now_ns);
  }
}

robot::v1::MotionStatus Cell::status() const {
  robot::v1::MotionStatus status;

  const SE3 tool = toolPose();
  auto* stamped = status.mutable_tool_pose();
  stamped->set_monotonic_ns(nowNanos());
  auto* pose = stamped->mutable_pose();
  pose->set_frame_id("world");
  pose->mutable_position()->set_x_m(tool.translation().x);
  pose->mutable_position()->set_y_m(tool.translation().y);
  pose->mutable_position()->set_z_m(tool.translation().z);

  const auto& rotation = tool.rotation();
  pose->mutable_orientation()->set_w(rotation.w());
  pose->mutable_orientation()->set_x(rotation.x());
  pose->mutable_orientation()->set_y(rotation.y());
  pose->mutable_orientation()->set_z(rotation.z());

  switch (phase_) {
    case CellPhase::kHeldBySafety:
      status.set_phase(robot::v1::MOTION_PHASE_HELD_BY_SAFETY);
      break;
    case CellPhase::kIdle:
      status.set_phase(robot::v1::MOTION_PHASE_IDLE);
      break;
    default:
      status.set_phase(robot::v1::MOTION_PHASE_CRUISING);
      break;
  }

  status.set_command_id(command_id_);
  status.set_detail(toString(phase_));

  // The safety half of the message. The cell reports what it was told, and
  // reports UNSPECIFIED when it was told nothing -- never a safe-looking value.
  auto* safety = status.mutable_safety();
  safety->set_active_function(phase_ == CellPhase::kHeldBySafety
                                  ? robot::v1::SAFETY_FUNCTION_SS1_T
                                  : robot::v1::SAFETY_FUNCTION_UNSPECIFIED);
  safety->set_since_monotonic_ns(stamped->monotonic_ns());

  return status;
}

}  // namespace pickcell
