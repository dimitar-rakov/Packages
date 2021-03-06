#include "lwr_hw/lwr_hw_fril.h"

namespace lwr_hw
{

  LWRHWFRIL::LWRHWFRIL() :
      LWRHW(),
      file_set_(false),
      result_status_ (0)
  {}
  LWRHWFRIL::~LWRHWFRIL() {}

  // Init, read, and write, with FRI hooks
  bool LWRHWFRIL::init(ros::NodeHandle &nh)
  {
    nh_ = nh;
    if( !(file_set_) )
    {
      std::cout << "Did you forget to set the init file? You must do that before init(). Exiting...\n";
      return false;
    }

    // construct a low-level lwr
    device_.reset( new FastResearchInterface( init_file_.c_str() ) );

    result_status_ =	device_->StartRobot( FRI_CONTROL_POSITION );
    if (result_status_ != 0)
    {
      std::cout << "An error occurred during starting up the robot...\n\n";
      return false;
    }

    return true;
  }

  void LWRHWFRIL::read(ros::Time time, ros::Duration period)
  {
    float msrJntPos[n_joints_];
    float msrJntTrq[n_joints_];

    device_->GetMeasuredJointPositions( msrJntPos );
    device_->GetMeasuredJointTorques( msrJntTrq );

    for (int j = 0; j < n_joints_; j++)
    {
      joint_msr_position_prev_[j] = joint_msr_position_[j];
      joint_msr_velocity_prev_[j] = joint_msr_velocity_[j];
      joint_msr_position_[j] = static_cast <double>(msrJntPos[j]);
      joint_position_kdl_(j) = joint_msr_position_[j];
      joint_msr_effort_[j] = static_cast <double>(msrJntTrq[j]);
      joint_msr_position_[j] = (joint_msr_position_[j]-joint_msr_position_prev_[j])/period.toSec();
      joint_msr_velocity_[j] = filters::exponentialSmoothing(joint_msr_velocity_[j], joint_msr_velocity_prev_[j], 0.9);
    }
    return;
  }

  void LWRHWFRIL::write(ros::Time time, ros::Duration period)
  {
    enforceLimits(period);

    // ensure the robot is powered and it is in control mode, almost like the isMachineOk() of Standford
    if ( device_->IsMachineOK() )
    {
      device_->WaitForKRCTick();

      switch (getControlStrategy())
      {
        case JOINT_POSITION:
          // Ensure the robot is in this mode
          if( (device_->GetCurrentControlScheme() == FRI_CONTROL_POSITION) )
          {
             float newJntPosition[n_joints_];
             for (size_t j = 0; j < n_joints_; j++)
               newJntPosition[j] = static_cast <float>(joint_cmd_position_[j]);
             device_->SetCommandedJointPositions(newJntPosition);
          }
          break;

        case CARTESIAN_IMPEDANCE:
          break;

         case JOINT_IMPEDANCE:

          // Ensure the robot is in this mode
          if( (device_->GetCurrentControlScheme() == FRI_CONTROL_JNT_IMP) )
          {
           float newJntPosition[n_joints_];
           float newJntStiff[n_joints_];
           float newJntDamp[n_joints_];
           float newJntAddTorque[n_joints_];

           // WHEN THE URDF MODEL IS PRECISE
           // 1. compute the gracity term
           // f_dyn_solver_->JntToGravity(joint_position_kdl_, gravity_effort_);

           // 2. read gravity term from FRI and add it with opposite sign and add the URDF gravity term
           // newJntAddTorque = gravity_effort_  - device_->getF_DYN??

            for(int j=0; j < n_joints_; j++)
            {
              newJntPosition[j] = static_cast<float> (joint_cmd_position_[j]);
              newJntAddTorque[j] = static_cast <float>(joint_cmd_effort_[j]);
              newJntStiff[j] = static_cast <float>(joint_cmd_stiffness_[j]);
              newJntDamp[j] = static_cast <float>(joint_cmd_damping_[j]);
            }
            device_->SetCommandedJointStiffness(newJntStiff);
            device_->SetCommandedJointPositions(newJntPosition);
            device_->SetCommandedJointDamping(newJntDamp);
            device_->SetCommandedJointTorques(newJntAddTorque);
          }
          break;
       }
    }
    return;
  }

  void LWRHWFRIL::doSwitch(const std::list<hardware_interface::ControllerInfo> &start_list, const std::list<hardware_interface::ControllerInfo> &stop_list)
  {

    if (device_->StopRobot() != 0)
    {
      std::cout << "An error occurred during stopping the robot, couldn't switch mode...\n\n";
      return;
    }

    // at this point, we now that there is only one controller that ones to command joints
    ControlStrategy desired_strategy = JOINT_POSITION; // default

    desired_strategy = getNewControlStrategy(start_list,stop_list,desired_strategy);

    // only allow joint position and joint impedance control strategies, otherwise set the default (JOINT_POSITION) strategy
    if(desired_strategy != JOINT_POSITION && desired_strategy != JOINT_IMPEDANCE)
      desired_strategy = JOINT_POSITION;

    for (size_t j = 0; j < n_joints_; ++j)
    {
      ///semantic Zero
      joint_cmd_position_[j] = joint_msr_position_[j];
      joint_cmd_effort_[j] = 0.0;

      ///call setCommand once so that the JointLimitsInterface receive the correct value on their getCommand()!
      try{  position_interface_.getHandle(joint_names_[j]).setCommand(joint_cmd_position_[j]);  }
      catch(const hardware_interface::HardwareInterfaceException&){}
      try{  effort_interface_.getHandle(joint_names_[j]).setCommand(joint_cmd_effort_[j]);  }
      catch(const hardware_interface::HardwareInterfaceException&){}

      ///reset joint_limit_interfaces
      pj_sat_interface_.reset();
      pj_limits_interface_.reset();
    }

    if(desired_strategy == getControlStrategy())
      std::cout << "The ControlStrategy didn't changed, it is already: " << getControlStrategy() <<"\n" ;
    else
    {
      switch( desired_strategy )
      {
      case JOINT_POSITION:
        if (device_->StartRobot( FRI_CONTROL_POSITION) != 0)
        {
          std::cout << "An error occurred during starting the robot, couldn't switch to JOINT_POSITION...\n" ;
          return;
        }
        break;
      case JOINT_IMPEDANCE:
        if (device_->StartRobot( FRI_CONTROL_POSITION) != 0)
        {
          std::cout << "An error occurred during starting the robot, couldn't switch to JOINT_IMPEDANCE...\n";
          return;
        }
        break;
      }

      // if sucess during the switch in FRI, set the ROS strategy
      setControlStrategy(desired_strategy);
      std::cout << "The ControlStrategy changed to: " << getControlStrategy() <<"\n" ;
    }
  }

  void LWRHWFRIL::stop(){return;}

  void LWRHWFRIL::set_mode(){return;}

  void LWRHWFRIL::setInitFile(std::string init_file){init_file_ = init_file; file_set_ = true;}

}
