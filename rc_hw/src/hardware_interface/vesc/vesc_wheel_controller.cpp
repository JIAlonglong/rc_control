/*********************************************************************
 * Copyright (c) 2022 SoftBank Corp.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 ********************************************************************/

#include "rc_hw/hardware_interface/vesc/vesc_wheel_controller.h"

namespace vesc_hw_interface
{
void VescWheelController::init(ros::NodeHandle nh, VescInterface* interface_ptr)
{
  if (interface_ptr == NULL)
  {
    ros::shutdown();
  }
  else
  {
    interface_ptr_ = interface_ptr;
  }

  nh.param<double>("motor/Kp", kp_, 0.005);
  nh.param<double>("motor/Ki", ki_, 0.005);
  nh.param<double>("motor/Kd", kd_, 0.0025);
  nh.param<double>("motor/i_clamp", i_clamp_, 0.2);
  nh.param<double>("motor/duty_limiter", duty_limiter_, 1.0);
  nh.param<bool>("motor/antiwindup", antiwindup_, true);
  nh.param<double>("motor/control_rate", control_rate_, 100.0);

  ROS_INFO("[Motor Gains] P: %f, I: %f, D: %f", kp_, ki_, kd_);
  ROS_INFO("[Motor Gains] I clamp: %f, Antiwindup: %s", i_clamp_, antiwindup_ ? "true" : "false");
  ROS_INFO("[Motor Control] control_rate: %f", control_rate_);

  // Smoothing differentiation when hall sensor resolution is insufficient
  bool smooth_diff;
  nh.param<bool>("motor/enable_smooth_diff", smooth_diff, true);
  if (smooth_diff)
  {
    double smooth_diff_max_sampling_time;
    int counter_td_vw_max_step;
    nh.param<double>("motor/smooth_diff/max_sample_sec", smooth_diff_max_sampling_time, 1.0);
    nh.param<int>("motor/smooth_diff/max_smooth_step", counter_td_vw_max_step, 10);
    vesc_step_difference_.enableSmooth(control_rate_, smooth_diff_max_sampling_time, counter_td_vw_max_step);
    ROS_INFO("[Motor Control] Smooth differentiation enabled, max_sample_sec: %f, max_smooth_step: %d",
             smooth_diff_max_sampling_time, counter_td_vw_max_step);
  }

  sensor_initialize_ = true;
  pid_initialize_ = true;
  position_sens_ = 0.0;
  target_velocity_ = 0.0;
  position_steps_ = 0.0;
  prev_steps_ = 0.0;
  velocity_sens_ = 0.0;
  effort_sens_ = 0.0;

  control_timer_ = nh.createTimer(ros::Duration(1.0 / control_rate_), &VescWheelController::controlTimerCallback, this);
}

void VescWheelController::control()
{
  if (pid_initialize_)
  {
    if (!sensor_initialize_)
    {
      // Start PID control only when sensor initialization is complete
      pid_initialize_ = false;
      vesc_step_difference_.resetStepDifference(position_steps_);
    }
    target_steps_ = position_steps_;
    error_ = 0.0;
    error_dt_ = 0.0;
    error_integ_ = 0.0;
    vesc_step_difference_.getStepDifference(position_steps_);
    interface_ptr_->setDutyCycle(0.0);
    return;
  }

  // convert rad/s to steps
  target_steps_ += target_velocity_ * (num_rotor_poles_ * num_hall_sensors_) / (2 * M_PI) / control_rate_ / gear_ratio_;

  // overflow check
  if ((target_steps_ - position_steps_) > std::numeric_limits<int>::max())
  {
    target_steps_ +=
        static_cast<double>(std::numeric_limits<int>::min()) - static_cast<double>(std::numeric_limits<int>::max());
  }
  else if ((target_steps_ - position_steps_) < std::numeric_limits<int>::min())
  {
    target_steps_ +=
        static_cast<double>(std::numeric_limits<int>::max()) - static_cast<double>(std::numeric_limits<int>::min());
  }

  // PID control
  double step_diff = vesc_step_difference_.getStepDifference(position_steps_);
  double current_vel = step_diff * 2.0 * M_PI / (num_rotor_poles_ * num_hall_sensors_) * control_rate_ * gear_ratio_;
  error_dt_ = target_velocity_ - current_vel;
  error_ = (target_steps_ - position_steps_) * 2.0 * M_PI / (num_rotor_poles_ * num_hall_sensors_);
  error_integ_ += (error_ / control_rate_);
  error_integ_ = std::clamp(error_integ_, -i_clamp_ / ki_, i_clamp_ / ki_);

  const double u_p = kp_ * error_;
  const double u_d = kd_ * error_dt_;
  const double u_i = ki_ * error_integ_;
  double u = u_p + u_d + u_i;

  // limit integral
  if (antiwindup_)
  {
    if (u > duty_limiter_ && error_integ_ > 0)
    {
      error_integ_ = std::max(0.0, (duty_limiter_ - u_p - u_d) / ki_);
    }
    else if (u < -duty_limiter_ && error_integ_ < 0)
    {
      error_integ_ = std::min(0.0, (-duty_limiter_ - u_p - u_d) / ki_);
    }
  }

  // limit duty value
  u = std::clamp(u, -duty_limiter_, duty_limiter_);

  interface_ptr_->setDutyCycle(u);
}

void VescWheelController::setTargetVelocity(const double velocity)
{
  target_velocity_ = velocity;
}

void VescWheelController::setGearRatio(const double gear_ratio)
{
  gear_ratio_ = gear_ratio;
  ROS_INFO("[VescWheelController]Gear ratio is set to %f", gear_ratio_);
}

void VescWheelController::setTorqueConst(const double torque_const)
{
  torque_const_ = torque_const;
  ROS_INFO("[VescWheelController]Torque constant is set to %f", torque_const_);
}

void VescWheelController::setRotorPoles(const int rotor_poles)
{
  num_rotor_poles_ = static_cast<double>(rotor_poles);
  num_rotor_pole_pairs_ = num_rotor_poles_ / 2;
  ROS_INFO("[VescWheelController]The number of rotor poles is set to %d", rotor_poles);
}

void VescWheelController::setHallSensors(const int hall_sensors)
{
  num_hall_sensors_ = static_cast<double>(hall_sensors);
  ROS_INFO("[VescWheelController]The number of hall sensors is set to %d", hall_sensors);
}

double VescWheelController::getPositionSens()
{
  return position_sens_;
}

double VescWheelController::getVelocitySens()
{
  return velocity_sens_;
}

double VescWheelController::getEffortSens()
{
  return effort_sens_;
}

void VescWheelController::controlTimerCallback(const ros::TimerEvent& e)
{
  control();
  interface_ptr_->requestState();
  pid_initialize_ = std::fabs(target_velocity_) < 0.0001;  // disable PID control when command is 0
}

void VescWheelController::updateSensor(const std::shared_ptr<const VescPacket>& packet)
{
  if (packet->getName() == "Values")
  {
    std::shared_ptr<VescPacketValues const> values = std::dynamic_pointer_cast<VescPacketValues const>(packet);
    const double current = values->getMotorCurrent();
    const double velocity_rpm = values->getVelocityERPM() / static_cast<double>(num_rotor_pole_pairs_) * gear_ratio_;
    const int32_t steps = static_cast<int32_t>(values->getPosition());
    if (sensor_initialize_)
    {
      prev_steps_ = steps;
      sensor_initialize_ = false;
    }
    position_steps_ += static_cast<double>(steps - prev_steps_);
    prev_steps_ = steps;

    position_sens_ =
        (position_steps_ * 2.0 * M_PI) / (num_rotor_poles_ * num_hall_sensors_) * gear_ratio_;  // convert steps to rad
    velocity_sens_ = velocity_rpm * 2 * M_PI / 60;                                              // convert rpm to rad/s
    effort_sens_ = current * torque_const_ / gear_ratio_;                                       // unit: Nm or N
  }
}
}  // namespace vesc_hw_interface
