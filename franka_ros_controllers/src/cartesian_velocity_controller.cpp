// Copyright (c) 2017 Franka Emika GmbH
// Use of this source code is governed by the Apache-2.0 license, see LICENSE
#include <franka_ros_controllers/cartesian_velocity_controller.h>

#include <array>
#include <cmath>
#include <memory>
#include <string>

#include <controller_interface/controller_base.h>
#include <hardware_interface/hardware_interface.h>
#include <hardware_interface/joint_command_interface.h>
#include <pluginlib/class_list_macros.h>
#include <ros/ros.h>

namespace franka_ros_controllers {

bool CartesianVelocityController::init(hardware_interface::RobotHW* robot_hardware,
                                              ros::NodeHandle& node_handle) {
  if (!node_handle.getParam("arm_id", arm_id)) {
    ROS_ERROR("CartesianVelocityController: Could not get parameter arm_id");
    return false;
  }

  velocity_cartesian_interface_ =
      robot_hardware->get<franka_hw::FrankaVelocityCartesianInterface>();
  if (velocity_cartesian_interface_ == nullptr) {
    ROS_ERROR(
        "CartesianVelocityController: Could not get Cartesian velocity interface from "
        "hardware");
    return false;
  }
  try {
    velocity_cartesian_handle_ = std::make_unique<franka_hw::FrankaCartesianVelocityHandle>(
        velocity_cartesian_interface_->getHandle(arm_id + "_robot"));
  } catch (const hardware_interface::HardwareInterfaceException& e) {
    ROS_ERROR_STREAM(
        "CartesianVelocityController: Exception getting Cartesian handle: " << e.what());
    return false;
  }

  this->state_interface = robot_hardware->get<franka_hw::FrankaStateInterface>();
  if (state_interface == nullptr) {
    ROS_ERROR("CartesianVelocityController: Could not get state interface from hardware");
    return false;
  }

  this->current_velocity.linear.x = 0;
  this->current_velocity.linear.y = 0;
  this->current_velocity.linear.z = 0;
  
  this->current_velocity.angular.x = 0;
  this->current_velocity.angular.y = 0;
  this->current_velocity.angular.z = 0;
  
  this->was_in_contact = false;
  this->time_since_last_target_velocity = ros::Time::now();
    
  this->target_velocity_subscriber =
    node_handle.subscribe("/franka_control/target_velocity", 1, &CartesianVelocityController::target_velocity_callback, this);
  
  this->current_velocity_publisher = node_handle.advertise<geometry_msgs::Twist>("/franka_control/current_target_velocity", 1);
  return true;
}

void CartesianVelocityController::target_velocity_callback(const geometry_msgs::Twist::ConstPtr& msg) {
  this->target_velocity = *msg;
  
  if (this->target_velocity.linear.x != this->target_velocity.linear.x ||
      this->target_velocity.linear.y != this->target_velocity.linear.y ||
      this->target_velocity.linear.z != this->target_velocity.linear.z ||
      this->target_velocity.angular.x != this->target_velocity.angular.x ||
      this->target_velocity.angular.y != this->target_velocity.angular.y ||
      this->target_velocity.angular.z != this->target_velocity.angular.z) {
    
    ROS_ERROR("Can't have NaN in the target velocity");
    exit(-1);
  }
  
  this->time_since_last_target_velocity = ros::Time::now();
}

void CartesianVelocityController::starting(const ros::Time& /* time */) {
  elapsed_time_ = ros::Duration(0.0);
  
  this->current_velocity.linear.x = 0;
  this->current_velocity.linear.y = 0;
  this->current_velocity.linear.z = 0;
  
  this->current_velocity.angular.x = 0;
  this->current_velocity.angular.y = 0;
  this->current_velocity.angular.z = 0;
}

  
void CartesianVelocityController::update(const ros::Time&,
                                                const ros::Duration& period) {
  elapsed_time_ += period;

  double v_max = 0.15;
  double angle = M_PI / 4.0;

  auto state_handle = this->state_interface->getHandle(arm_id + "_robot");
  
  bool is_in_contact_x = state_handle.getRobotState().cartesian_contact[0] > 0;
  bool is_in_contact_y = state_handle.getRobotState().cartesian_contact[1] > 0;
  bool is_in_contact_z = state_handle.getRobotState().cartesian_contact[2] > 0;
  
  bool is_in_contact = is_in_contact_x || is_in_contact_y || is_in_contact_z;

  ros::Time now = ros::Time::now();
  if ((now - this->time_since_last_target_velocity).toSec() > 2) {
    target_velocity = geometry_msgs::Twist(); // zero velocity
  }
    
  geometry_msgs::Twist new_target_velocity = target_velocity;
  
  if (!was_in_contact && is_in_contact) {
    ee_twist_cause_of_contact = state_handle.getRobotState().O_dP_EE_d;
  }
  
  if (is_in_contact_x && (new_target_velocity.linear.x * ee_twist_cause_of_contact[0]) > 0) {
    new_target_velocity.linear.x = 0.0;  
  }
  
  if (is_in_contact_y && (new_target_velocity.linear.y * ee_twist_cause_of_contact[1]) > 0) {
    new_target_velocity.linear.y = 0.0;  
  }

  if (is_in_contact_z && (new_target_velocity.linear.z * ee_twist_cause_of_contact[2]) > 0) {
    new_target_velocity.linear.z = 0.0;  
  }
  
  // Velocity needs to ramp up very slowly and smoothly, otherwise the arm locks
  // due to discontinuities in joint commands
  double alpha = 0.995;
  
  current_velocity.linear.x = alpha*current_velocity.linear.x +
    (1-alpha) * v_max/2.0 * new_target_velocity.linear.x;
  
  current_velocity.linear.y = alpha*current_velocity.linear.y +
    (1-alpha) * v_max/2.0 * new_target_velocity.linear.y;
  
  current_velocity.linear.z = alpha*current_velocity.linear.z +
    (1-alpha) * v_max/2.0 * new_target_velocity.linear.z;
  
  double v_x = std::cos(angle) * current_velocity.linear.x; 
  double v_y = std::cos(angle) * current_velocity.linear.y; 
  double v_z = std::cos(angle) * current_velocity.linear.z; 
  
  bool is_in_collision = state_handle.getRobotState().cartesian_collision[0] > 0 ||
                         state_handle.getRobotState().cartesian_collision[1] > 0 ||
                         state_handle.getRobotState().cartesian_collision[2] > 0;
  
  if (is_in_collision) {
    v_x = 0;
    v_y = 0;
    v_z = 0;
  }

  
  std::array<double, 6> command = {{v_x, v_y, v_z, 0.0, 0.0, 0.0}};
  velocity_cartesian_handle_->setCommand(command);

  geometry_msgs::Twist vel_msg;
  vel_msg.linear.x = v_x;
  vel_msg.angular.y = v_y;
  vel_msg.angular.z = v_z;

  was_in_contact = is_in_contact;
  
  current_velocity_publisher.publish(vel_msg);
}
  
  
void CartesianVelocityController::stopping(const ros::Time& /*time*/) {
  // WARNING: DO NOT SEND ZERO VELOCITIES HERE AS IN CASE OF ABORTING DURING MOTION
  // A JUMP TO ZERO WILL BE COMMANDED PUTTING HIGH LOADS ON THE ROBOT. LET THE DEFAULT
  // BUILT-IN STOPPING BEHAVIOR SLOW DOWN THE ROBOT.

  // Velocity needs to ramp up very slowly and smoothly, otherwise the arm locks
  // due to discontinuities in joint commands
  
  double alpha = 0.995;
  
  current_velocity.linear.x = alpha*current_velocity.linear.x;
  current_velocity.linear.y = alpha*current_velocity.linear.y;
  current_velocity.linear.z = alpha*current_velocity.linear.z;
  
  double v_x = current_velocity.linear.x; 
  double v_y = current_velocity.linear.y; 
  double v_z = current_velocity.linear.z; 
    
  std::array<double, 6> command = {{v_x, v_y, v_z, 0.0, 0.0, 0.0}};
  velocity_cartesian_handle_->setCommand(command);

  geometry_msgs::Twist vel_msg;
  vel_msg.linear.x = v_x;
  vel_msg.angular.y = v_y;
  vel_msg.angular.z = v_z;
  current_velocity_publisher.publish(vel_msg);
}

}  // namespace franka_example_controllers

PLUGINLIB_EXPORT_CLASS(franka_ros_controllers::CartesianVelocityController,
                       controller_interface::ControllerBase)
