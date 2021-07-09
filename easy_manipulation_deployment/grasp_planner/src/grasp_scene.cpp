// Copyright 2020 Advanced Remanufacturing and Technology Centre
// Copyright 2020 ROS-Industrial Consortium Asia Pacific Team
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

// Main PCL files
#include "grasp_planner/grasp_scene.hpp"

/***************************************************************************//**
 * Function that calls the grasp execution service after all grasp plans are
 * generated.
 * @param grasp_task Grasp Task to send
 ******************************************************************************/
template<typename T>
void grasp_planner::GraspScene<T>::sendToExecution(
  const emd_msgs::msg::GraspTask & grasp_task)
{
  if (grasp_task.grasp_targets.size() > 0) {
    RCLCPP_INFO(LOGGER, "Sending Grasp Request to grasp execution module");
    auto req = std::make_shared<emd_msgs::srv::GraspRequest::Request>();
    req->grasp_targets = grasp_task.grasp_targets;
    if (!this->result_future.valid()) {
      RCLCPP_INFO(LOGGER, "Client Not started");
      this->result_future = output_client->async_send_request(req);
    } else if (this->result_future.wait_for(std::chrono::nanoseconds(0)) ==
      std::future_status::timeout)
    {
      RCLCPP_INFO(LOGGER, "Grasp Execution still Ongoing");
    } else {
      auto result = this->result_future.get();
      RCLCPP_INFO(
        LOGGER, "Grasp Execution completed! STATUS: %s!!",
        (result->success) ? "SUCCESS" : "FAILURE");
      this->result_future = output_client->async_send_request(req);
    }
  } else {
    RCLCPP_ERROR(LOGGER, "No grasp tasks generated, Skipping request to grasp execution...");
  }
  this->grasp_objects.clear();
}

/****************************************************************************************//**
 * Method to generate Grasp Tasks for manipulation
 * @param msg Input message
 *******************************************************************************************/
template<typename T>
emd_msgs::msg::GraspTask grasp_planner::GraspScene<T>::generateGraspTask()
{
  emd_msgs::msg::GraspTask grasp_task;
  grasp_task.task_id = MathFunctions::generate_task_id();

  if (this->grasp_objects.size() == 0) {return grasp_task;}

  for (auto object : this->grasp_objects) {
    loadEndEffectors();
    std::chrono::steady_clock::time_point begin = std::chrono::steady_clock::now();
    for (auto gripper : this->end_effectors) {
      std::chrono::steady_clock::time_point grasp_begin = std::chrono::steady_clock::now();

      emd_msgs::msg::GraspMethod grasp_method;
      grasp_method.ee_id = gripper->getID();
      grasp_method.grasp_ranks.insert(
        grasp_method.grasp_ranks.begin(), std::numeric_limits<float>::min());

      gripper->planGrasps(
        object, &grasp_method, world_collision_object,
        node->get_parameter("camera_parameters.camera_frame").as_string());
      grasp_method.grasp_ranks.pop_back();

      if (grasp_method.grasp_ranks.size() > 0) {
        object->grasp_target.grasp_methods.push_back(grasp_method);
      } else {
        RCLCPP_ERROR(
          LOGGER, "For Object " + object->grasp_target.target_type +
          ", no grasp methods can be found with end effector " + gripper->getID());
        // continue;
      }

      std::chrono::steady_clock::time_point grasp_end = std::chrono::steady_clock::now();

      RCLCPP_INFO(
        LOGGER, "Grasp planning time for " + grasp_method.ee_id + " " +
        std::to_string(
          std::chrono::duration_cast<std::chrono::milliseconds>(grasp_end - grasp_begin).count()) +
        " [ms] ");

      if (node->get_parameter("visualization_params.point_cloud_visualization").as_bool()) {
        gripper->visualizeGrasps(viewer, object);
        std::cout << "Point Cloud Viewer Visualization" << std::endl;
      }
    }

    if (object->grasp_target.grasp_methods.size() > 0) {
      grasp_task.grasp_targets.push_back(object->grasp_target);
    } else {
      RCLCPP_ERROR(
        LOGGER, "For Object " + object->grasp_target.target_type +
        ", no grasp methods can be found with any given " +
        " end effectors provided. ");
      continue;
    }
    std::chrono::steady_clock::time_point end = std::chrono::steady_clock::now();
    RCLCPP_INFO(
      LOGGER, "Grasp planning time for object " + object->object_name + " " +
      std::to_string(
        std::chrono::duration_cast<std::chrono::milliseconds>(end - begin).count()) +
      " [ms] ");
  }

  objectPoseRectification(grasp_task);

  return grasp_task;
}

/***************************************************************************//**
 * Method that loads all available end effector based on the parameter files
 ******************************************************************************/
template<typename T>
void grasp_planner::GraspScene<T>::loadEndEffectors()
{
  this->end_effectors.clear();
  std::vector<std::string> end_effector_array = node->get_parameter(
    "end_effectors.end_effector_names").as_string_array();
  for (std::string end_effector : end_effector_array) {
    std::string end_effector_type =
      node->get_parameter("end_effectors." + end_effector + ".type").as_string();
    RCLCPP_INFO(LOGGER, "Loading " + end_effector_type + " gripper " + end_effector);
    if (end_effector_type.compare("finger") == 0) {
      FingerGripper gripper(
        end_effector,
        node->get_parameter("end_effectors." + end_effector + ".num_fingers_side_1").as_int(),
        node->get_parameter("end_effectors." + end_effector + ".num_fingers_side_2").as_int(),
        static_cast<float>(node->get_parameter(
          "end_effectors." + end_effector +
          ".distance_between_fingers_1").as_double()),
        static_cast<float>(node->get_parameter(
          "end_effectors." + end_effector +
          ".distance_between_fingers_2").as_double()),
        static_cast<float>(node->get_parameter(
          "end_effectors." + end_effector +
          ".finger_thickness").as_double()),
        static_cast<float>(node->get_parameter(
          "end_effectors." + end_effector +
          ".gripper_stroke").as_double()),
        static_cast<float>(node->get_parameter(
          "end_effectors." + end_effector +
          ".grasp_planning_params.voxel_size").as_double()),
        static_cast<float>(node->get_parameter(
          "end_effectors." + end_effector +
          ".grasp_planning_params.grasp_rank_weight_1").as_double()),
        static_cast<float>(node->get_parameter(
          "end_effectors." + end_effector +
          ".grasp_planning_params.grasp_rank_weight_2").as_double()),
        static_cast<float>(node->get_parameter(
          "end_effectors." + end_effector +
          ".grasp_planning_params.grasp_plane_dist_limit").as_double()),
        static_cast<float>(node->get_parameter(
          "point_cloud_params.cloud_normal_radius").as_double()),
        static_cast<float>(node->get_parameter(
          "end_effectors." + end_effector +
          ".grasp_planning_params.world_x_angle_threshold").as_double()),
        static_cast<float>(node->get_parameter(
          "end_effectors." + end_effector +
          ".grasp_planning_params.world_y_angle_threshold").as_double()),
        static_cast<float>(node->get_parameter(
          "end_effectors." + end_effector +
          ".grasp_planning_params.world_z_angle_threshold").as_double()),
        node->get_parameter(
          "end_effectors." + end_effector +
          ".gripper_coordinate_system.grasp_stroke_direction").as_string(),
        node->get_parameter(
          "end_effectors." + end_effector +
          ".gripper_coordinate_system.grasp_stroke_normal_direction").as_string(),
        node->get_parameter(
          "end_effectors." + end_effector +
          ".gripper_coordinate_system.grasp_approach_direction").as_string()
      );
      std::shared_ptr<EndEffector> gripper_ptr = std::make_shared<FingerGripper>(gripper);
      this->end_effectors.push_back(gripper_ptr);
    } else if (end_effector_type.compare("suction") == 0) {
      SuctionGripper gripper(
        end_effector,
        node->get_parameter("end_effectors." + end_effector + ".num_cups_length").as_int(),
        node->get_parameter("end_effectors." + end_effector + ".num_cups_breadth").as_int(),
        static_cast<float>(node->get_parameter(
          "end_effectors." + end_effector + ".dist_between_cups_length").as_double()),
        static_cast<float>(node->get_parameter(
          "end_effectors." + end_effector + ".dist_between_cups_breadth").as_double()),
        static_cast<float>(node->get_parameter(
          "end_effectors." + end_effector + ".cup_radius").as_double()),
        static_cast<float>(node->get_parameter(
          "end_effectors." + end_effector + ".cup_height").as_double()),
        node->get_parameter(
          "end_effectors." + end_effector +
          ".grasp_planning_params.num_sample_along_axis").as_int(),
        static_cast<float>(node->get_parameter(
          "end_effectors." + end_effector +
          ".grasp_planning_params.search_resolution").as_double()),
        node->get_parameter(
          "end_effectors." + end_effector +
          ".grasp_planning_params.search_angle_resolution").as_int(),
        static_cast<float>(node->get_parameter(
          "point_cloud_params.cloud_normal_radius").as_double()),
        static_cast<float>(node->get_parameter(
          "end_effectors." + end_effector +
          ".grasp_planning_params.weights.curvature").as_double()),
        static_cast<float>(node->get_parameter(
          "end_effectors." + end_effector +
          ".grasp_planning_params.weights.grasp_distance_to_center").as_double()),
        static_cast<float>(node->get_parameter(
          "end_effectors." + end_effector +
          ".grasp_planning_params.weights.number_contact_points").as_double()),
        node->get_parameter(
          "end_effectors." + end_effector +
          ".gripper_coordinate_system.length_direction").as_string(),
        node->get_parameter(
          "end_effectors." + end_effector +
          ".gripper_coordinate_system.breadth_direction").as_string(),
        node->get_parameter(
          "end_effectors." + end_effector +
          ".gripper_coordinate_system.grasp_approach_direction").as_string());

      std::shared_ptr<EndEffector> gripper_ptr = std::make_shared<SuctionGripper>(gripper);

      this->end_effectors.push_back(gripper_ptr);
    }
  }
  RCLCPP_INFO(LOGGER, "All End Effectors Loaded");
}


/****************************************************************************************//**
 * Function that processes the Objects in a point cloud scene and outputs a vector
 * of GraspObjects
 *******************************************************************************************/
template<>
void grasp_planner::GraspScene<sensor_msgs::msg::PointCloud2>::extractObjectsDirect()
{
  RCLCPP_INFO(LOGGER, "Extracting Objects from point cloud");

  std::string camera_frame = node->get_parameter("camera_parameters.camera_frame").as_string();

  int min_cluster_size = node->get_parameter(
    "point_cloud_params.min_cluster_size").as_int();

  float cloud_normal_radius = static_cast<float>(node->get_parameter(
      "point_cloud_params.cloud_normal_radius").as_double());

  float cluster_tolerance = static_cast<float>(node->get_parameter(
      "point_cloud_params.cluster_tolerance").as_double());

  // pcl::search::KdTree<pcl::PointXYZRGB>::Ptr tree(new pcl::search::KdTree<pcl::PointXYZRGB>);
  // std::vector<pcl::PointIndices> clusterIndices;
  // pcl::EuclideanClusterExtraction<pcl::PointXYZRGB> ecExtractor;

  // tree->setInputCloud(cloud);
  // ecExtractor.setClusterTolerance(cluster_tolerance);
  // ecExtractor.setMinClusterSize(min_cluster_size);
  // ecExtractor.setSearchMethod(tree);
  // ecExtractor.setInputCloud(cloud);
  // ecExtractor.extract(clusterIndices);

  std::vector<pcl::PointIndices> clusterIndices = PCLFunctions::extractPointCloudClusters(
    this->cloud_plane_removed, cluster_tolerance, min_cluster_size);

  if (clusterIndices.empty()) {
    RCLCPP_ERROR(LOGGER, "No Objects can be extracted");
  } else {
    std::vector<pcl::PointIndices>::const_iterator it = clusterIndices.begin();
    for (it = clusterIndices.begin(); it != clusterIndices.end(); ++it) {
      pcl::PointCloud<pcl::PointXYZRGB>::Ptr objectCloud(new pcl::PointCloud<pcl::PointXYZRGB>());
      for (std::vector<int>::const_iterator pit = it->indices.begin();
        pit != it->indices.end(); ++pit)
      {
        objectCloud->points.push_back(this->cloud_plane_removed->points[*pit]);
      }

      objectCloud->width = objectCloud->points.size();
      objectCloud->height = 1;
      objectCloud->is_dense = true;

      // Get the centroid of the point cloud
      Eigen::Vector4f centroid;
      pcl::compute3DCentroid(*objectCloud, centroid);
      std::shared_ptr<GraspObject> object = std::make_shared<GraspObject>(
        camera_frame,
        objectCloud,
        centroid);
      PCLFunctions::computeCloudNormal(objectCloud, object->cloud_normal, cloud_normal_radius);
      object->get_object_bb();
      object->get_object_world_angles();
      object->grasp_target.target_shape = object->getObjectShape();
      object->grasp_target.target_pose = object->getObjectPose(camera_frame);
      this->grasp_objects.push_back(object);
    }
  }
  RCLCPP_INFO(
    LOGGER, "Extracted " + std::to_string(
      this->grasp_objects.size()) + " from point cloud");
}

/****************************************************************************************//**
 * Function that processes the Objects detected by EPD and outputs a vector
 * of GraspObjects
 * @param objects EPD detected objects
 *******************************************************************************************/
template<typename T>
void grasp_planner::GraspScene<T>::extractObjectsEPD(
  const std::vector<epd_msgs::msg::LocalizedObject> & objects)
{
  RCLCPP_INFO(LOGGER, "Processing Objects detected by EPD...");

  std::string camera_frame = node->get_parameter(
    "camera_parameters.camera_frame").as_string();

  float cloud_normal_radius = static_cast<float>(node->get_parameter(
      "point_cloud_params.cloud_normal_radius").as_double());

  for (auto raw_object : objects) {
    pcl::PointCloud<pcl::PointXYZRGB>::Ptr objectCloud(new pcl::PointCloud<pcl::PointXYZRGB>());
    pcl::PCLPointCloud2 * pcl_pc2(new pcl::PCLPointCloud2);
    PCLFunctions::SensorMsgtoPCLPointCloud2((raw_object.segmented_pcl), *pcl_pc2);
    pcl::fromPCLPointCloud2(*pcl_pc2, *(objectCloud));
    PCLFunctions::removeStatisticalOutlier(objectCloud, 0.5);

    objectCloud->width = objectCloud->points.size();
    objectCloud->height = 1;
    objectCloud->is_dense = true;

    // Get the centroid of the point cloud
    Eigen::Vector4f centroid;
    centroid(0) = raw_object.centroid.x;
    centroid(1) = raw_object.centroid.y;
    centroid(2) = raw_object.centroid.z;

    std::shared_ptr<GraspObject> object = std::make_shared<GraspObject>(
      raw_object.name,
      camera_frame, objectCloud,
      centroid);
    PCLFunctions::computeCloudNormal(objectCloud, object->cloud_normal, cloud_normal_radius);
    object->get_object_bb();
    object->get_object_world_angles();
    object->grasp_target.target_shape = object->getObjectShape();
    object->grasp_target.target_pose = object->getObjectPose(camera_frame);
    this->grasp_objects.push_back(object);
  }
  RCLCPP_INFO(LOGGER, "EPD detected " + std::to_string(this->grasp_objects.size()) + " objects.");
}

/****************************************************************************************//**
 * Method to Extract Grasp Objects
 * @param msg Input message
 *******************************************************************************************/
template<>
void grasp_planner::GraspScene<sensor_msgs::msg::PointCloud2>::extractObjects(
  const sensor_msgs::msg::PointCloud2::ConstSharedPtr & msg)
{
  extractObjectsDirect();
}

template<>
void grasp_planner::GraspScene<epd_msgs::msg::EPDObjectLocalization>::extractObjects(
  const epd_msgs::msg::EPDObjectLocalization::ConstSharedPtr & msg)
{
  extractObjectsEPD(msg->objects);
}

template<>
void grasp_planner::GraspScene<epd_msgs::msg::EPDObjectTracking>::extractObjects(
  const epd_msgs::msg::EPDObjectTracking::ConstSharedPtr & msg)
{
  extractObjectsEPD(msg->objects);
}

/***************************************************************************//**
 * Function that converts a sensor_msg pointcloud2 message into an FCL compatible
 * collision object.
 * @param msg Pointcloud input
 ******************************************************************************/
template<>
void grasp_planner::GraspScene<sensor_msgs::msg::PointCloud2>::createWorldCollision(
  const sensor_msgs::msg::PointCloud2::ConstSharedPtr & msg)
{
  geometry_msgs::msg::TransformStamped sensorToWorldTf =
    this->buffer_->lookupTransform(
    "base_link", msg->header.frame_id,
    msg->header.stamp);
  octomap::point3d sensor_origin = octomap::pointTfToOctomap(sensorToWorldTf.transform.translation);
  this->world_collision_object = FCLFunctions::createCollisionObjectFromPointCloudRGB(
    this->org_cloud, sensor_origin,
    static_cast<float>(node->get_parameter("point_cloud_params.octomap_resolution").as_double()));
}

/***************************************************************************//**
 * Function that converts an EPD localization message into an FCL compatible
 * collision object
 * @param msg EPD input
 ******************************************************************************/
template<typename T>
void grasp_planner::GraspScene<T>::createWorldCollision(
  const typename T::ConstSharedPtr & msg)
{
  // auto ppx = camera_info.k.at(2);
  // auto fx = camera_info.k.at(0);
  // auto ppy = camera_info.k.at(5);
  // auto fy = camera_info.k.at(4);

  float ppx = 323.3077697753906;
  float fx = 610.3740844726562;
  float ppy = 235.43516540527344;
  float fy = 609.8685913085938;
  cv_bridge::CvImagePtr cv_ptr;
  cv_ptr = cv_bridge::toCvCopy(msg->depth_image, sensor_msgs::image_encodings::TYPE_16UC1);
  cv::Mat depth_img = cv_ptr->image;
  pcl::PointCloud<pcl::PointXYZRGB>::Ptr scene_cloud(new pcl::PointCloud<pcl::PointXYZRGB>());
  for (size_t i = 0; i < msg->depth_image.width; i++) {
    for (size_t j = 0; j < msg->depth_image.height; j++) {
      pcl::PointXYZRGB temp_point;
      auto depth = depth_img.at<ushort>(j, i) * 0.001;    // NOLINT
      temp_point.x = (i - ppx) / fx * depth;
      temp_point.y = (j - ppy) / fy * depth;
      temp_point.z = depth;
      scene_cloud->points.push_back(temp_point);
    }
  }

  PCLFunctions::passthroughFilter(
    scene_cloud,
    static_cast<float>(node->get_parameter("point_cloud_params.passthrough_filter_limits_x").
    as_double_array()[1]),
    static_cast<float>(node->get_parameter("point_cloud_params.passthrough_filter_limits_x").
    as_double_array()[0]),
    static_cast<float>(node->get_parameter("point_cloud_params.passthrough_filter_limits_y").
    as_double_array()[1]),
    static_cast<float>(node->get_parameter("point_cloud_params.passthrough_filter_limits_y").
    as_double_array()[0]),
    static_cast<float>(node->get_parameter("point_cloud_params.passthrough_filter_limits_z").
    as_double_array()[1]),
    static_cast<float>(node->get_parameter("point_cloud_params.passthrough_filter_limits_z").
    as_double_array()[0]));
  PCLFunctions::removeStatisticalOutlier(scene_cloud, 1.0);

  geometry_msgs::msg::TransformStamped sensorToWorldTf =
    this->buffer_->lookupTransform(
    "base_link", msg->header.frame_id,
    msg->header.stamp);
  octomap::point3d sensor_origin = octomap::pointTfToOctomap(sensorToWorldTf.transform.translation);

  PCLFunctions::voxelizeCloud
  <pcl::PointCloud<pcl::PointXYZRGB>::Ptr, pcl::VoxelGrid<pcl::PointXYZRGB>>(
    scene_cloud,
    static_cast<float>(node->get_parameter(
      "point_cloud_params.fcl_voxel_size").as_double()),
    this->org_cloud);

  this->world_collision_object = FCLFunctions::createCollisionObjectFromPointCloudRGB(
    this->org_cloud, sensor_origin,
    static_cast<float>(node->get_parameter("point_cloud_params.octomap_resolution").as_double()));
}

/***************************************************************************//**
 * Function that processes an input sensor_msgs pointcloud2 message.
 * Includes Conversion to PCL Pointcloud2 type, undergoing passthrough filtering,
 * Removing statistical outlier, downsampling and plane segmentation.
 * @param msg Pointcloud input
 ******************************************************************************/
template<typename T>
void grasp_planner::GraspScene<T>::processPointCloud(
  const sensor_msgs::msg::PointCloud2::ConstSharedPtr & msg)
{
  RCLCPP_INFO(LOGGER, "Processing Point Cloud... ");
  pcl::PCLPointCloud2 * pcl_pc2(new pcl::PCLPointCloud2);
  PCLFunctions::SensorMsgtoPCLPointCloud2(*msg, *pcl_pc2);
  pcl::fromPCLPointCloud2(*pcl_pc2, *(this->cloud));
  RCLCPP_INFO(LOGGER, "Applying Passthrough filters");
  PCLFunctions::passthroughFilter(
    this->cloud,
    static_cast<float>(node->get_parameter("point_cloud_params.passthrough_filter_limits_x").
    as_double_array()[1]),
    static_cast<float>(node->get_parameter("point_cloud_params.passthrough_filter_limits_x").
    as_double_array()[0]),
    static_cast<float>(node->get_parameter("point_cloud_params.passthrough_filter_limits_y").
    as_double_array()[1]),
    static_cast<float>(node->get_parameter("point_cloud_params.passthrough_filter_limits_y").
    as_double_array()[0]),
    static_cast<float>(node->get_parameter("point_cloud_params.passthrough_filter_limits_z").
    as_double_array()[1]),
    static_cast<float>(node->get_parameter("point_cloud_params.passthrough_filter_limits_z").
    as_double_array()[0]));
  RCLCPP_INFO(LOGGER, "Removing Statistical Outlier");
  PCLFunctions::removeStatisticalOutlier(this->cloud, 1.0);
  RCLCPP_INFO(LOGGER, "Downsampling Point Cloud");
  PCLFunctions::voxelizeCloud
  <pcl::PointCloud<pcl::PointXYZRGB>::Ptr, pcl::VoxelGrid<pcl::PointXYZRGB>>(
    this->cloud,
    static_cast<float>(node->get_parameter(
      "point_cloud_params.fcl_voxel_size").as_double()),
    this->org_cloud);
  RCLCPP_INFO(LOGGER, "Segmenting plane");
  PCLFunctions::planeSegmentation(
    this->cloud, this->cloud_plane_removed, this->cloud_table,
    node->get_parameter(
      "point_cloud_params.segmentation_max_iterations").as_int(),
    static_cast<float>(node->get_parameter(
      "point_cloud_params.segmentation_distance_threshold").as_double()));
  RCLCPP_INFO(LOGGER, "Point cloud successfully processed!");
}

/****************************************************************************************//**
 * General Callback function for Direct Point Cloud pipeline for tracking and localization
 * @param msg Input message
 *******************************************************************************************/
template<>
void grasp_planner::GraspScene<sensor_msgs::msg::PointCloud2>::startPlanning(
  const sensor_msgs::msg::PointCloud2::ConstSharedPtr & msg)
{
  RCLCPP_INFO(LOGGER, "Perception input received!");
  processPointCloud(msg);
  createWorldCollision(msg);
  extractObjects(msg);
  // loadEndEffectors();
  emd_msgs::msg::GraspTask grasp_task = generateGraspTask();
  sendToExecution(grasp_task);
  RCLCPP_INFO(LOGGER, "Grasp Planning complete.");
}

/****************************************************************************************//**
 * General Callback function for EPD-EMD pipeline for tracking and localization
 * @param msg Input message
 *******************************************************************************************/
template<typename T>
void grasp_planner::GraspScene<T>::startPlanning(const typename T::ConstSharedPtr & msg)
{
  RCLCPP_INFO(LOGGER, "Perception input received!");
  createWorldCollision(msg);
  extractObjects(msg);
  // loadEndEffectors();
  emd_msgs::msg::GraspTask grasp_task = generateGraspTask();
  sendToExecution(grasp_task);
  RCLCPP_INFO(LOGGER, "Grasp Planning complete.");
  triggerEPDPipeline();
}

/******************************************************************************************//**
 * Method to set up all communication methods with perception system for Direct Input
 *********************************************************************************************/
template<>
void grasp_planner::GraspScene<sensor_msgs::msg::PointCloud2>::setup(std::string topic_name)
{
  this->output_client =
    this->node->create_client<emd_msgs::srv::GraspRequest>(
    this->node->get_parameter("grasp_output_service").as_string());

  RCLCPP_INFO(LOGGER, "Listening to: " + topic_name + "...");
  this->perception_sub = std::make_shared<
    message_filters::Subscriber<sensor_msgs::msg::PointCloud2>>(
    node, topic_name);

  this->tf_perception_sub =
    std::make_shared<tf2_ros::MessageFilter<sensor_msgs::msg::PointCloud2>>(
    *buffer_, "base_link", 5,
    node->get_node_logging_interface(),
    node->get_node_clock_interface(),
    std::chrono::seconds(1));

  this->tf_perception_sub->connectInput(*perception_sub);

  this->tf_perception_sub->registerCallback(
    &grasp_planner::GraspScene<sensor_msgs::msg::PointCloud2>::startPlanning, this);

  RCLCPP_INFO(LOGGER, "waiting....");
}

/******************************************************************************************//**
 * Method to set up all communication methods with perception system for EPD-EMD input
 *********************************************************************************************/
template<typename T>
void grasp_planner::GraspScene<T>::setup(std::string topic_name)
{
  this->output_client =
    this->node->create_client<emd_msgs::srv::GraspRequest>(
    this->node->get_parameter("grasp_output_service").as_string());

  this->epd_client =
    this->node->create_client<epd_msgs::srv::Perception>(
    this->node->get_parameter("easy_perception_deployment.epd_service").as_string());
  //this->node->get_parameter("epd_service").as_string());

  RCLCPP_INFO(LOGGER, "Listening to: " + topic_name + "...");
  this->perception_sub = std::make_shared<
    message_filters::Subscriber<T>>(
    node, topic_name);

  this->tf_perception_sub = std::make_shared<tf2_ros::MessageFilter<T>>(
    *buffer_, "base_link", 5,
    node->get_node_logging_interface(),
    node->get_node_clock_interface(),
    std::chrono::seconds(1));

  this->tf_perception_sub->connectInput(*perception_sub);

  this->tf_perception_sub->registerCallback(
    &grasp_planner::GraspScene<T>::startPlanning, this);

  //First trigger to start EPD
  triggerEPDPipeline();

  RCLCPP_INFO(LOGGER, "waiting....");
}

template<typename T>
void grasp_planner::GraspScene<T>::triggerEPDPipeline()
{
  RCLCPP_INFO(LOGGER, "Waiting for EPD Service");
  while (!epd_client->wait_for_service(std::chrono::seconds(1))) {
    if (!rclcpp::ok()) {
      RCLCPP_ERROR(node->get_logger(), "client interrupted while waiting for service to appear.");
      return;
    }
  }
  RCLCPP_INFO(LOGGER, "EPD Service found. Sending trigger to EPD pipeline");
  auto req = std::make_shared<epd_msgs::srv::Perception::Request>();
  req->ready = true;
  if (!this->epd_result_future.valid()) {
    RCLCPP_INFO(LOGGER, "Client Not started");
    this->epd_result_future = epd_client->async_send_request(req);
  } else if (this->epd_result_future.wait_for(std::chrono::nanoseconds(0)) ==
    std::future_status::timeout)
  {
    RCLCPP_INFO(LOGGER, "EPD Pipeline already Ongoing");
  } else {
    auto result = this->epd_result_future.get();
    RCLCPP_INFO(
      LOGGER, "EPD Pipeline triggering complete. STATUS: %s!!",
      (result->success) ? "SUCCESS" : "FAILURE");
    this->epd_result_future = epd_client->async_send_request(req);
  }
}

// LCOV_EXCL_START

/***************************************************************************//**
 * Function that prints Pose
 * @param _pose target Pose to print
 ******************************************************************************/
template<typename T>
void grasp_planner::GraspScene<T>::printPose(
  const geometry_msgs::msg::Pose & _pose)
{
  std::cout << "Position:" << std::endl;

  std::cout << "X: " << _pose.position.x << std::endl;
  std::cout << "Y: " << _pose.position.y << std::endl;
  std::cout << "Z: " << _pose.position.z << std::endl << std::endl;

  std::cout << "X: " << _pose.orientation.x << std::endl;
  std::cout << "Y: " << _pose.orientation.y << std::endl;
  std::cout << "Z: " << _pose.orientation.z << std::endl;
  std::cout << "W: " << _pose.orientation.w << std::endl << std::endl;

  std::cout << "Orientation:" << std::endl;
}

/***************************************************************************//**
 * Function that prints PoseStamped messages
 * @param _pose target PoseStamped pose to print
 ******************************************************************************/
template<typename T>
void grasp_planner::GraspScene<T>::printPose(const geometry_msgs::msg::PoseStamped & _pose)
{
  std::cout << "Frame ID: " << _pose.header.frame_id << std::endl;
  printPose(_pose.pose);
}

/****************************************************************************************//**
 * Not used
 *******************************************************************************************/
template<typename T>
void grasp_planner::GraspScene<T>::getCameraPosition()
{
  Eigen::Vector3f worldZVector(0, 0, 1);
  Eigen::Vector3f table_normal_vector(this->table_coeff->values[0],
    this->table_coeff->values[1],
    this->table_coeff->values[2]);

  float cos_world_table = std::abs(
    (table_normal_vector.dot(worldZVector)) /
    (table_normal_vector.norm() * worldZVector.norm()));

  std::cout << table_normal_vector(0) << " , " << table_normal_vector(1) << " , " <<
    table_normal_vector(2) << std::endl;

  // Compute initial points accordingly
  if (cos_world_table > 0.9) {
    std::cout << "Camera in top view\n";
  } else {
    std::cout << "Camera in side view\n";
  }
}

template<typename T>
void grasp_planner::GraspScene<T>::objectPoseRectification(
  emd_msgs::msg::GraspTask & grasp_task)
{
  for (auto & grasp_target : grasp_task.grasp_targets) {
    grasp_target.target_shape.dimensions[0] =
      std::abs(
      grasp_target.target_pose.pose.position.z -
      static_cast<float>(node->get_parameter("table_to_camera_height").as_double()));
    grasp_target.target_pose.pose.position.z += grasp_target.target_shape.dimensions[0] / 2;
  }
}

// LCOV_EXCL_STOP

template class grasp_planner::GraspScene<sensor_msgs::msg::PointCloud2>;
template class grasp_planner::GraspScene<epd_msgs::msg::EPDObjectLocalization>;
template class grasp_planner::GraspScene<epd_msgs::msg::EPDObjectTracking>;
