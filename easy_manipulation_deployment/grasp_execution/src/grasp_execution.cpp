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

#include <memory>
#include <string>
#include <utility>

#include "grasp_execution/grasp_execution.hpp"

namespace grasp_execution
{

GraspExecutionInterface::GraspExecutionInterface(
  const rclcpp::Node::SharedPtr & node,
  const std::string & grasp_task_topic,
  size_t planning_concurrency,
  size_t execution_concurrency
)
: node_(node),
  tf_buffer_(std::make_shared<tf2_ros::Buffer>(node_->get_clock())),
  tf_listener_(*tf_buffer_),
  planning_scheduler(planning_concurrency),
  execution_scheduler(execution_concurrency)
{
  grasp_task_sub_ = node_->create_subscription<emd_msgs::msg::GraspTask>(
    grasp_task_topic, 10,
    [ = ](emd_msgs::msg::GraspTask::UniquePtr msg) {
      order_schedule(std::move(msg));
    }
  );
}

GraspExecutionInterface::~GraspExecutionInterface()
{
}

void GraspExecutionInterface::prompt_job_start(
  const std::string & job_id,
  const std::string & message,
  char separator,
  std::ostream & out)
{
  int length = 80;
  out << "\033[96m";
  if (!job_id.empty()) {
    out << "[" << job_id << "] ";
    length -= 3;
  }
  out << message;
  for (int i = 0;
    i < length - static_cast<int>(job_id.size()) - static_cast<int>(message.size()); i++)
  {
    out << separator;
  }
  out << "\033[0m" << std::endl;
}

void GraspExecutionInterface::prompt_job_end(
  bool end_result,
  char separator,
  std::ostream & out)
{
  if (end_result) {
    out << "\033[92m";
  } else {
    out << "\033[91m";
  }

  for (int i = 0; i < 80; i++) {
    out << separator;
  }

  out << "\033[0m" << std::endl;
}


std::string GraspExecutionInterface::gen_target_object_id(
  const emd_msgs::msg::GraspTask::SharedPtr & msg,
  size_t index) const
{
  using shape_ns = shape_msgs::msg::SolidPrimitive;
  std::string shape_type;
  switch (msg->grasp_targets[index].target_shape.type) {
    case shape_ns::BOX:
      shape_type = "box";
      break;
    case shape_ns::SPHERE:
      shape_type = "sphere";
      break;
    case shape_ns::CYLINDER:
      shape_type = "cylinder";
      break;
    case shape_ns::CONE:
      shape_type = "cone";
      break;
    default:
      shape_type = "unknown";
      break;
  }
  std::stringstream ss;
  ss << "#" << shape_type << "-" << msg->task_id << "-" << index;
  return ss.str();
}

}  // namespace grasp_execution
