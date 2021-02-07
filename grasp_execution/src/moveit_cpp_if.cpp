// Copyright 2020 ROS Industrial Consortium Asia Pacific
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <chrono>
#include <limits>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "grasp_execution/moveit_cpp_if.hpp"

#include "rclcpp/rclcpp.hpp"

#include "moveit/moveit_cpp/moveit_cpp.h"
#include "moveit/moveit_cpp/planning_component.h"
#include "moveit/robot_state/cartesian_interpolator.h"
#include "moveit/macros/console_colors.h"
#include "moveit/utils/message_checks.h"
#include "moveit/trajectory_processing/iterative_time_parameterization.h"

#include "tf2_ros/buffer.h"
#include "tf2_geometry_msgs/tf2_geometry_msgs.h"
#include "tf2_ros/transform_listener.h"
#include "tf2/impl/utils.h"
#include "tf2_eigen/tf2_eigen.h"

#include "geometry_msgs/msg/pose_stamped.hpp"

namespace grasp_execution
{

static const rclcpp::Logger LOGGER = rclcpp::get_logger("grasp_execution");

MoveitCppGraspExecution::MoveitCppGraspExecution(
  const rclcpp::Node::SharedPtr & node,
  const std::string & grasp_poses_topic,
  size_t planning_concurrency,
  size_t execution_concurrency)
: GraspExecutionInterface(
    node, grasp_poses_topic, planning_concurrency, execution_concurrency),
  moveit_cpp_(std::make_shared<moveit::planning_interface::MoveItCpp>(node_))
{
  // let RViz display query PlanningScene
  moveit_cpp_->getPlanningSceneMonitor()->providePlanningSceneService();
  moveit_cpp_->getPlanningSceneMonitor()->setPlanningScenePublishingFrequency(100);

  // Set Robot Root link
  robot_frame_ = moveit_cpp_->getRobotModel()->getRootLinkName();
  RCLCPP_INFO(
    LOGGER,
    "Robot Root Link Name: %s", robot_frame_.c_str());
}

MoveitCppGraspExecution::~MoveitCppGraspExecution()
{
  // Exit everything in order
  for (auto & arm : arms_) {
    arm.second.planner.reset();
  }
  moveit_cpp_.reset();
}

bool MoveitCppGraspExecution::init(
  const std::string & planning_group, const std::string & _ee_link)
{
  prompt_job_start(
    LOGGER, "",
    "Initializing planning group: [" + planning_group + "].");
  // Check if planner is already registered
  if (arms_.find(planning_group) == arms_.end()) {
    // Print out basic planning group info for debugging
    moveit::core::RobotStatePtr state;
    moveit_cpp_->getCurrentState(state, 0);
    auto robot_model = state->getRobotModel();
    auto joint_model_group = robot_model->getJointModelGroup(planning_group);
    size_t dof = joint_model_group->getVariableCount();
    auto link_names = joint_model_group->getLinkModelNames();
    auto root_link = robot_model->getRootLinkName();

    if (dof < 1) {
      RCLCPP_ERROR(LOGGER, "Planning Group [%s] is empty", planning_group.c_str());
      prompt_job_end(LOGGER, false);
      return false;
    } else {
      RCLCPP_INFO(
        LOGGER,
        "Basic Info:\n"
        "Base frame: %s\n"
        "Joint Model Group: %s\n"
        "\tDoF: %u\n"
        "\tfirst_link: %s\n"
        "\tend_link:  %s\n"
        "\tChain: %s\n"
        "\tEnd Effector: %s\n",
        root_link.c_str(), planning_group.c_str(), dof,
        link_names.front().c_str(), link_names.back().c_str(),
        (joint_model_group->isChain() ? "Yes" : "No"),
        (joint_model_group->isEndEffector() ? "Yes" : "No"));

      // Initializing planner
      arms_.emplace(
        planning_group,
        JmgContext{planning_group, moveit_cpp_,
          (_ee_link.empty() ? link_names.back() : _ee_link)});

      if (_ee_link != link_names.back()) {
        RCLCPP_WARN(
          LOGGER,
          MOVEIT_CONSOLE_COLOR_YELLOW
          "Assuming [%s] is rigidly attached to end_link [%s]"
          MOVEIT_CONSOLE_COLOR_RESET, _ee_link.c_str(), link_names.back().c_str());
      }

      prompt_job_end(LOGGER, true);
      return true;
    }

  } else {
    RCLCPP_WARN(LOGGER, "Planning Group [%s] already ended", planning_group.c_str());
    prompt_job_end(LOGGER, false);
    return false;
  }
}

void MoveitCppGraspExecution::register_target_objects(
  const emd_msgs::msg::GraspTask::SharedPtr & msg)
{
  // Add all targets into the scene
  moveit_msgs::msg::CollisionObject temp_collision_object;
  temp_collision_object.operation = temp_collision_object.ADD;

  for (size_t i = 0; i < msg->grasp_targets.size(); i++) {
    const auto & target = msg->grasp_targets[i];
    const auto target_id =
      this->gen_target_object_id(msg, i);

    prompt_job_start(
      LOGGER, target_id,
      "Register object in the world");

    temp_collision_object.id = target_id;
    temp_collision_object.header.frame_id =
      target.target_pose.header.frame_id;

    temp_collision_object.primitives.push_back(target.target_shape);
    temp_collision_object.primitive_poses.push_back(target.target_pose.pose);

    // // Print out all object poses as debug information
    // std::ostringstream oss;
    // print_pose(target.target_pose, oss);
    // RCLCPP_INFO(node_->get_logger(), oss.str());

    auto tmp_pose = target.target_pose;
    to_frame(target.target_pose, tmp_pose, this->robot_frame_);
    // std::ostringstream oss2;
    // print_pose(tmp_pose, oss2);
    // RCLCPP_INFO(node_->get_logger(), oss2.str());

    bool result;
    // Add object to planning scene
    {    // Lock PlanningScene
      planning_scene_monitor::LockedPlanningSceneRW scene(moveit_cpp_->getPlanningSceneMonitor());
      result = scene->processCollisionObjectMsg(temp_collision_object);
    }    // Unlock PlanningScene

    prompt_job_end(LOGGER, !result);
  }
}

geometry_msgs::msg::Pose MoveitCppGraspExecution::get_object_pose(
  const std::string & object_id) const
{
  geometry_msgs::msg::Pose object_pose;
  {    // Lock PlanningScene
    planning_scene_monitor::LockedPlanningSceneRW scene(moveit_cpp_->getPlanningSceneMonitor());
    collision_detection::World::ObjectConstPtr object = scene->getWorld()->getObject(object_id);
    auto poses = (*object).shape_poses_[0];
    Eigen::Quaterniond q(poses.linear());
    object_pose.position.x = poses.translation().x();
    object_pose.position.y = poses.translation().y();
    object_pose.position.z = poses.translation().z();
    object_pose.orientation.x = q.x();
    object_pose.orientation.y = q.y();
    object_pose.orientation.z = q.z();
    object_pose.orientation.w = q.w();
  }    // Unlock PlanningScene

  return object_pose;
}

moveit::core::RobotStatePtr MoveitCppGraspExecution::get_curr_state() const
{
  // Get home state
  moveit::core::RobotStatePtr curr_state;
  moveit_cpp_->getCurrentState(curr_state, 0);
  return curr_state;
}

geometry_msgs::msg::PoseStamped MoveitCppGraspExecution::get_curr_pose(
  const std::string & link_name) const
{
  moveit::core::RobotStatePtr state;
  moveit_cpp_->getCurrentState(state, 0);
  const auto & transform = state->getGlobalLinkTransform(link_name);
  ASSERT_ISOMETRY(transform);  // unsanitized input, could contain a non-isometry

  Eigen::Quaterniond q(transform.linear());
  geometry_msgs::msg::PoseStamped output_pose;
  output_pose.header.frame_id = state->getRobotModel()->getRootLinkName();
  output_pose.header.stamp = node_->now();
  output_pose.pose.position.x = transform.translation().x();
  output_pose.pose.position.y = transform.translation().y();
  output_pose.pose.position.z = transform.translation().z();
  output_pose.pose.orientation.x = q.x();
  output_pose.pose.orientation.y = q.y();
  output_pose.pose.orientation.z = q.z();
  output_pose.pose.orientation.w = q.w();
  return output_pose;
}


bool MoveitCppGraspExecution::move_to(
  const std::string & planning_group,
  const geometry_msgs::msg::PoseStamped & pose,
  const std::string & link,
  bool execute)
{
  JmgContext & arm = arms_[planning_group];
  // Set the start state to the last point of the trajectory
  // if immediate execution is not needed
  if (!execute &&
    !arm.traj.empty())
  {
    arm.planner->setStartState(arm.traj.back()->getLastWayPoint());
  } else {
    arm.planner->setStartStateToCurrentState();
  }
  arm.planner->setGoal(pose, link);
  const auto plan_solution = arm.planner->plan();  // PlanningComponent::PlanSolution
  if (plan_solution) {
    // Execute immediately
    if (execute) {
      RCLCPP_INFO(LOGGER, "Sending the trajectory for execution");
      arm.planner->execute(true);  // blocked execution
    } else {
      arm.traj.push_back(plan_solution.trajectory);
    }

    return true;
  } else {
    return false;
  }
}

bool MoveitCppGraspExecution::move_to(
  const std::string & planning_group,
  const moveit::core::RobotState & state,
  bool execute)
{
  auto & arm = arms_[planning_group];
  // Set the start state to the last point of the trajectory
  // if immediate execution is not needed
  if (!execute &&
    !arm.traj.empty())
  {
    arm.planner->setStartState(arm.traj.back()->getLastWayPoint());
  } else {
    arm.planner->setStartStateToCurrentState();
  }
  arm.planner->setGoal(state);
  const auto plan_solution = arm.planner->plan();  // PlanningComponent::PlanSolution
  if (plan_solution) {
    // Execute immediately
    if (execute) {
      RCLCPP_INFO(LOGGER, "Sending the trajectory for execution");
      arm.planner->execute(true);  // blocked execution
    } else {
      arm.traj.push_back(plan_solution.trajectory);
    }
    return true;
  } else {
    return false;
  }
}

/// Referenced from Move Group capability MoveGroupCartesianPathService::computeService
bool MoveitCppGraspExecution::cartesian_to(
  const std::string & planning_group,
  const std::vector<geometry_msgs::msg::Pose> & _waypoints,
  const std::string & _link, double step, double jump_threshold,
  bool execute)
{
  const auto & arm = arms_[planning_group];
  double fraction = 0.0;
  robot_trajectory::RobotTrajectoryPtr rt;

  RCLCPP_INFO(LOGGER, "Received request to compute Cartesian path");
  // Start Cartesian Planning
  {    // Lock PlanningScene
    moveit_cpp_->getPlanningSceneMonitor()->updateFrameTransforms();

    // Get start state
    moveit::core::RobotState start_state(moveit_cpp_->getRobotModel());
    // Set the start state to the last point of the trajectory
    // if immediate execution is not needed
    if (!execute &&
      !arm.traj.empty())
    {
      start_state = arm.traj.back()->getLastWayPoint();
    } else {
      planning_scene_monitor::LockedPlanningSceneRO lscene(
        moveit_cpp_->getPlanningSceneMonitor());
      start_state = lscene->getCurrentState();
    }
    // Check if planning group valid
    if (const moveit::core::JointModelGroup * jmg =
      start_state.getJointModelGroup(planning_group))
    {
      std::string link_name = _link;
      if (link_name.empty() && !jmg->getLinkModelNames().empty()) {
        link_name = jmg->getLinkModelNames().back();
      }

      EigenSTL::vector_Isometry3d waypoints(_waypoints.size());

      for (size_t i = 0; i < _waypoints.size(); i++) {
        tf2::fromMsg(_waypoints[i], waypoints[i]);
      }

      // TODO(Briancbn): Properly deal with this
      // Create path_constraint
      moveit_msgs::msg::Constraints path_constraints;
      bool avoid_collisions = true;

      if (step < std::numeric_limits<double>::epsilon()) {
        RCLCPP_ERROR(
          LOGGER, "Maximum step to take between consecutive configrations along Cartesian path"
          "was not specified (this value needs to be > 0)");
        return false;
      } else {
        if (!waypoints.empty()) {
          moveit::core::GroupStateValidityCallbackFn constraint_fn;
          std::unique_ptr<planning_scene_monitor::LockedPlanningSceneRO> ls;
          std::unique_ptr<kinematic_constraints::KinematicConstraintSet> kset;
          if (avoid_collisions || !moveit::core::isEmpty(path_constraints)) {
            ls.reset(
              new planning_scene_monitor::LockedPlanningSceneRO(
                moveit_cpp_->
                getPlanningSceneMonitor()));
            kset.reset(new kinematic_constraints::KinematicConstraintSet((*ls)->getRobotModel()));
            kset->add(path_constraints, (*ls)->getTransforms());
            constraint_fn = [
              planning_scene =
              (avoid_collisions ? static_cast<const planning_scene::PlanningSceneConstPtr &>(*ls)
              .get() : nullptr),
              constraint_set = (kset->empty() ? nullptr : kset.get())
              ](moveit::core::RobotState * state,
                const moveit::core::JointModelGroup * group, const double * ik_solution) {
                state->setJointGroupPositions(group, ik_solution);
                state->update();
                return (!planning_scene ||
                       !planning_scene->isStateColliding(*state, group->getName())) &&
                       (!constraint_set || constraint_set->decide(*state).satisfied);
              };
          }

          bool global_frame = true;

          std::vector<moveit::core::RobotStatePtr> traj;
          fraction = moveit::core::CartesianInterpolator::computeCartesianPath(
            &start_state, jmg, traj, start_state.getLinkModel(link_name), waypoints, global_frame,
            moveit::core::MaxEEFStep(step), moveit::core::JumpThreshold(
              jump_threshold), constraint_fn);

          rt = std::make_shared<robot_trajectory::RobotTrajectory>(
            moveit_cpp_->getPlanningSceneMonitor()->getRobotModel(), planning_group);
          for (const moveit::core::RobotStatePtr & traj_state : traj) {
            rt->addSuffixWayPoint(traj_state, 0.0);
          }

          // time trajectory
          // \todo optionally compute timing to move the eef with constant speed
          trajectory_processing::IterativeParabolicTimeParameterization time_param;
          time_param.computeTimeStamps(*rt, 1.0);

          RCLCPP_INFO(
            LOGGER,
            "Computed Cartesian path with %u points (followed %lf%% of requested trajectory)",
            (unsigned int)traj.size(), fraction * 100.0);
        }
      }
    } else {
      return false;
    }
  }    // Lock PlanningScene

  // Execute cartesian path
  if (fraction > 0) {
    // Execute immediately
    if (execute) {
      RCLCPP_INFO(LOGGER, "Sending the trajectory for execution");
      moveit_cpp_->execute(planning_group, rt);
    } else {
      auto & arm = arms_[planning_group];
      arm.traj.push_back(rt);
    }
    return true;
  } else {
    return false;
  }
}

bool MoveitCppGraspExecution::move_until_before_collide(
  const std::string & planning_group,
  const geometry_msgs::msg::PoseStamped & pose,
  const std::string & link, double step_size, int max_attempts,
  char axis,
  bool execute)
{
  auto next_waypoint = [&axis, &step_size](tf2::Vector3 & _origin) {
      switch (axis) {
        case 'x':
          _origin.m_floats[0] += step_size;
          break;
        case 'y':
          _origin.m_floats[1] += step_size;
          break;
        case 'z':
          _origin.m_floats[2] += step_size;
          break;
        default:
          return false;
      }
      return true;
    };
  if (pose.header.frame_id != robot_frame_) {
    RCLCPP_ERROR(LOGGER, "Please use global frame for this function");
    return false;
  }

  std::vector<geometry_msgs::msg::Pose> waypoints;

  auto temp_target_pose = pose;

  tf2::Transform base_to_ee;
  tf2::fromMsg(pose.pose, base_to_ee);

  tf2::Transform ee_w_clearance;
  ee_w_clearance.setIdentity();
  if (!next_waypoint(ee_w_clearance.getOrigin())) {
    return false;
  }
  for (int i = 0; i < max_attempts; i++) {
    tf2::toMsg(base_to_ee * ee_w_clearance, temp_target_pose.pose);
    waypoints.push_back(temp_target_pose.pose);
    next_waypoint(ee_w_clearance.getOrigin());
  }

  return cartesian_to(planning_group, waypoints, link, std::abs(step_size / 3), 0, execute);
}

void MoveitCppGraspExecution::attach_object_to_ee(
  const moveit_msgs::msg::CollisionObject & object,
  const std::string & ee_link)
{
  /*
  moveit_msgs::AttachedCollisionObject
  -----------------------------------
  string link_name
  moveit_msgs/CollisionObject object
  string[] touch_links
  trajectory_msgs/JointTrajectory detach_posture
  float64 weight
  */
  geometry_msgs::msg::PoseStamped ee_pose;
  geometry_msgs::msg::PoseStamped object_pose;

  object_pose.header.frame_id = object.header.frame_id;
  object_pose.pose = object.pose;

  to_frame(object_pose, ee_pose, ee_link);

  moveit_msgs::msg::AttachedCollisionObject attached_object;
  attached_object.link_name = ee_link;

  attached_object.object = object;
  attached_object.object.pose = ee_pose.pose;
  attached_object.object.operation = attached_object.object.ADD;

  // Attach object to ee_link
  {    // Lock PlanningScene
    planning_scene_monitor::LockedPlanningSceneRW scene(moveit_cpp_->getPlanningSceneMonitor());
    scene->processAttachedCollisionObjectMsg(attached_object);
  }    // Unlock PlanningScene
}

void MoveitCppGraspExecution::attach_object_to_ee(
  const std::string & target_id,
  const std::string & ee_link)
{
  moveit_msgs::msg::AttachedCollisionObject attach_object;

  attach_object.object.id = target_id;
  attach_object.link_name = ee_link;
  attach_object.object.operation = moveit_msgs::msg::CollisionObject::ADD;

  // Attach object to ee_link
  {    // Lock PlanningScene
    planning_scene_monitor::LockedPlanningSceneRW scene(moveit_cpp_->getPlanningSceneMonitor());
    scene->processAttachedCollisionObjectMsg(attach_object);
  }    // Unlock PlanningScene
}

void MoveitCppGraspExecution::detach_object_from_ee(
  const moveit_msgs::msg::CollisionObject & object,
  const std::string & ee_link)
{
  /*
  moveit_msgs::AttachedCollisionObject
  -----------------------------------
  string link_name
  moveit_msgs/CollisionObject object
  string[] touch_links
  trajectory_msgs/JointTrajectory detach_posture
  float64 weight
  */

  moveit_msgs::msg::AttachedCollisionObject detach_object;

  detach_object.object.id = object.id;
  detach_object.object.operation = object.REMOVE;

  // Add object to planning scene
  {    // Lock PlanningScene
    planning_scene_monitor::LockedPlanningSceneRW scene(moveit_cpp_->getPlanningSceneMonitor());
    scene->processAttachedCollisionObjectMsg(detach_object);
  }    // Unlock PlanningScene
}


void MoveitCppGraspExecution::detach_object_from_ee(
  const std::string & target_id,
  const std::string & ee_link)
{
  moveit_msgs::msg::AttachedCollisionObject detach_object;

  detach_object.object.id = target_id;
  detach_object.link_name = ee_link;
  detach_object.object.operation = moveit_msgs::msg::CollisionObject::REMOVE;

  // Attach object to ee_link
  {    // Lock PlanningScene
    planning_scene_monitor::LockedPlanningSceneRW scene(moveit_cpp_->getPlanningSceneMonitor());
    scene->processAttachedCollisionObjectMsg(detach_object);
  }    // Unlock PlanningScene
}

void MoveitCppGraspExecution::remove_object(
  const std::string & target_id)
{
  moveit_msgs::msg::CollisionObject object;
  object.id = target_id;
  object.operation = moveit_msgs::msg::CollisionObject::REMOVE;

  // Add object to planning scene
  {    // Lock PlanningScene
    planning_scene_monitor::LockedPlanningSceneRW scene(moveit_cpp_->getPlanningSceneMonitor());
    scene->processCollisionObjectMsg(object);
  }    // Unlock PlanningScene
}

void MoveitCppGraspExecution::squash_trajectories(
  const std::string & planning_group,
  int start_idx, int end_idx,
  bool time_parameterization)
{
  auto & trajs = arms_[planning_group].traj;
  int _end_idx = (end_idx == -1) ? (trajs.size() - 1) : end_idx;
  size_t size = trajs.size();
  if (start_idx <= _end_idx) {
    while (trajs.size() > size - (_end_idx - start_idx)) {
      trajs[start_idx]->append(*trajs[start_idx + 1], 0);
      trajs.erase(trajs.begin() + start_idx + 1);
    }
    if (time_parameterization) {
      trajectory_processing::IterativeParabolicTimeParameterization time_param;
      time_param.computeTimeStamps(*trajs[start_idx], 1.0);
    }
  }
}

void MoveitCppGraspExecution::print_trajectory(
  const robot_trajectory::RobotTrajectoryPtr & traj,
  std::ostream & _out)
{
  _out << "Robot Trajectory:" << std::endl;
  _out << "Plannning Group: " << traj->getGroupName() << std::endl;
  _out << "Total waypoints: " << traj->getWayPointCount() << std::endl;

  _out << "t\t\tpos\t\tvel\t\taccel" << std::endl;
  int dof = traj->getGroup()->getVariableCount();
  for (size_t i = 0; i < traj->getWayPointCount(); i++) {
    _out << traj->getWayPointDurationFromStart(i) << "\t";
    _out << "|\t";
    for (int j = 0; j < dof; j++) {
      _out << traj->getWayPointPtr(i)->getVariablePosition(j) << "\t";
    }
    if (traj->getWayPointPtr(i)->hasVelocities()) {
      _out << "|\t";
      for (int j = 0; j < dof; j++) {
        _out << traj->getWayPointPtr(i)->getVariableVelocity(j) << "\t";
      }
    }
    if (traj->getWayPointPtr(i)->hasAccelerations()) {
      _out << "|\t";
      for (int j = 0; j < dof; j++) {
        _out << traj->getWayPointPtr(i)->getVariableAcceleration(j) << "\t";
      }
    }
    _out << std::endl;
  }
}

}  // namespace grasp_execution
