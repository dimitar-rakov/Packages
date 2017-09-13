#include "obstacle_detector/obstacle_detector.h"
#include <pcl/filters/extract_indices.h>
#include <pcl/filters/passthrough.h>
#include <pcl/filters/approximate_voxel_grid.h>
#include <pcl/filters/voxel_grid.h>



ObstacleDetector::ObstacleDetector() { }


bool ObstacleDetector::init(ros::NodeHandle &nh){

  nh_ = nh;
  new_move_= false;
  max_nn_ = 40;
  points_status_ = -1;
  safety_ton_points_= ros::Time::now();

  // Get base_name from parameter server
  if (!nh_.getParam("base_name", base_name_)){
    nh_.param("base_name", base_name_, std::string ("kinect_fusion"));
    ROS_WARN("Parameter base_name was not found. Default name is used: %s ", base_name_.c_str());
  }

  if (!nh_.getParam("sim_obstacle_enb", sim_obstacles_enb_)){
    nh_.param("sim_obstacle_enb", sim_obstacles_enb_, false);
    ROS_WARN("Parameter sim_obstacle_enb was not found. Default value is used: false");
  }

  // Get point topic from parameter server
  if (!nh_.getParam("points_topic", points_topic_)){
    nh_.param("points_topic", points_topic_, std::string ("/kinect_fusion/points"));
    ROS_WARN("Parameter points_topic was not found. Default topic's name is used: %s ", points_topic_.c_str());
  }

  // Get fixed frame name from parameter server
  if (!nh_.getParam("fixed_frame", fixed_frame_)){
    nh_.param("fixed_frame", fixed_frame_, std::string ("world"));
    ROS_WARN("Parameter points_topic was not found. Default topic's name is used: %s ", fixed_frame_.c_str());
  }

  if (!nh_.getParam("octree_resolution", obs_octree_resolution_)){
    nh_.param("octree_resolution", obs_octree_resolution_, 0.2);
    ROS_WARN("Parameter octree_resolution was not found. Default value is used: %lf", obs_octree_resolution_);
  }


  // Get tf_names topics from parameter server (ToDo with auto if std +11 is future used)
  if (!nh_.getParam("tf_names", tf_names_)){
    std::vector<std::string> tf_names;
    tf_names.push_back(std::string ("lwr_base_link"));
    tf_names.push_back(std::string ("lwr_a1_link"));
    tf_names.push_back(std::string ("lwr_a2_link"));
    tf_names.push_back(std::string ("lwr_e1_link"));
    tf_names.push_back(std::string ("lwr_a3_link"));
    tf_names.push_back(std::string ("lwr_a4_link"));
    tf_names.push_back(std::string ("lwr_a5_link"));
    tf_names.push_back(std::string ("lwr_a6_link"));
    nh_.param("tf_names", tf_names_, tf_names);
    ROS_WARN("Parameter tf_names was not found.");
    for (size_t i = 0; i < tf_names_.size(); i++){
      ROS_WARN("Default topic's name is used: %s ", tf_names_[i].c_str());
    }
  }


  XmlRpc::XmlRpcValue obj;
  std::string ns_filter_group = std::string ("filter_env_obj");

  //Get all filter_env_obj from yaml
  if ( !nh_.getParam(nh_.getNamespace()+"/" + ns_filter_group, obj ) || obj.getType() != XmlRpc::XmlRpcValue::TypeStruct)
    ROS_WARN("%s is not properly set in yaml file",ns_filter_group.c_str());
  getFilterObjectsParameters(obj, filter_env_objects_, ns_filter_group);

  //Get all filter_robot_obj from yaml
  ns_filter_group = std::string ("filter_robot_obj");
  if ( !nh_.getParam(nh_.getNamespace()+"/" + ns_filter_group, obj ) || obj.getType() != XmlRpc::XmlRpcValue::TypeStruct)
    ROS_WARN("%s is not properly set in yaml file", ns_filter_group.c_str());
  getFilterObjectsParameters(obj, filter_robot_objects_, ns_filter_group);

  //Initialization
  radius_sphere_robot_body_= 0.14;

  // one default obstacle sphere. Used for simulation only
  visualization_msgs::Marker marker;
  marker.header.frame_id = "world";
  marker.header.stamp = ros::Time();
  marker.ns = "obstacles";
  marker.id = 3000;
  marker.type = visualization_msgs::Marker::SPHERE;
  marker.action = visualization_msgs::Marker::ADD;
  marker.scale.x =  0.40;
  marker.scale.y =  0.40;
  marker.scale.z =  0.40;
  marker.pose.position.x= 0.0;
  marker.pose.position.y= 0.0;
  marker.pose.position.z= 10.0;
  marker.pose.orientation.x = 0.0;
  marker.pose.orientation.y = 0.0;
  marker.pose.orientation.z = 0.0;
  marker.pose.orientation.w = 1.0;
  marker.color.a = 0.75; // Don't forget to set the alpha!
  marker.color.r = 1.0;
  marker.color.g = 0.0;
  marker.color.b = 0.0;
  marker.lifetime = ros::Duration(1);
  obstacle_objects_.markers.push_back(marker);
  cb_ros_cloud_ptr_.reset(new sensor_msgs::PointCloud2());
  filtered_ros_cloud_ptr_.reset(new sensor_msgs::PointCloud2());
  in_cloud_ptr_.reset(new pcl::PointCloud<pcl::PointXYZ>());
  cb_cloud_ptr_.reset(new pcl::PointCloud<pcl::PointXYZ>());
  filtered_cloud_ptr_.reset(new pcl::PointCloud<pcl::PointXYZ>());


  //Initializing of publisher and subscribers
  if (USE_PCL_CB)
    sub_point_cloud_ = nh_.subscribe<pcl::PointCloud<pcl::PointXYZ> > (points_topic_, 1, boost::bind(&ObstacleDetector::pclPointcloudCB, this, _1, &cb_cloud_ptr_, &safety_ton_points_, &points_status_));
  else
    sub_point_cloud_ = nh_.subscribe<sensor_msgs::PointCloud2> (points_topic_, 1, boost::bind(&ObstacleDetector::rosPointcloudCB, this, _1, &cb_ros_cloud_ptr_, &safety_ton_points_, &points_status_));

  srv_set_obstacle_trajectory_ = nh_.advertiseService(nh_.getNamespace()+"/set_obstacle_trajectory", &ObstacleDetector::setObstacleTrajectory, this);
  pub_output_cloud_ = nh_.advertise<sensor_msgs::PointCloud2> (nh_.getNamespace()+"/output_cloud", 1);
  pub_markers_obstacle_objects_ = nh_.advertise<visualization_msgs::MarkerArray>(nh_.getNamespace()+"/obstacle_objects", 1 );
  pub_markers_obstacle_objects_test_ = nh_.advertise<visualization_msgs::MarkerArray>(nh_.getNamespace()+"/obstacle_objects_test", 1 );

  TFs_.resize(tf_names_.size()-1);
  TFs_fixed_.resize(tf_names_.size());
  pub_markers_filter_env_objects_ = nh_.advertise<visualization_msgs::MarkerArray>(nh_.getNamespace()+"/env_objects", 1 );
  pub_markers_filter_robot_objects_ = nh_.advertise<visualization_msgs::MarkerArray>(nh_.getNamespace()+"/robot_objects", 1 );
  pub_markers_filter_robot_objects_fixed_ = nh_.advertise<visualization_msgs::MarkerArray>(nh_.getNamespace()+"/robot_objects_fixed", 1 );

  ROS_INFO ("ObstacleDetector with a name %s is initialized", base_name_.c_str());
  return true;
}


bool ObstacleDetector::setObstacleTrajectory(obstacle_detector::SetObstacleTrajectory::Request &req, obstacle_detector::SetObstacleTrajectory::Response &res){
  obst_start_position_ = (tf::Vector3(req.start_position.x, req.start_position.y, req.start_position.z));
  obst_end_position_ = (tf::Vector3(req.end_position.x, req.end_position.y, req.end_position.z));
  obs_start_time_= ros::Time::now().toSec();
  obst_goal_time_ =ros::Time::now().toSec()+req.time;
  obst_curr_distance_ = 0.0;
  res.set =true;
  new_move_ = true;
  return true;
}

bool ObstacleDetector::getTFs(){

  tf::StampedTransform tf;
  for (size_t i = 0 ; i< TFs_.size(); i++){
    try{
      lr_.lookupTransform(tf_names_[i], tf_names_[i+1], ros::Time(0), tf);
      TFs_[i].setOrigin(tf.getOrigin());
      TFs_[i].setRotation(tf.getRotation());
      //ROS_INFO("TFs_[%ld]: x: %lf, y: %lf, z: %lf",i, TFs_[i].getOrigin().getX(), TFs_[i].getOrigin().getY(), TFs_[i].getOrigin().getZ());
    }
    catch (tf::TransformException ex){
      ROS_ERROR("%s",ex.what());
      ros::Duration(1.0).sleep();
      return false;
      break;
    }

  }

  for (size_t i = 0 ; i< TFs_fixed_.size(); i++){
    try{
      lr_.lookupTransform(fixed_frame_, tf_names_[i], ros::Time(0), tf);
      TFs_fixed_[i].setOrigin(tf.getOrigin());
      TFs_fixed_[i].setRotation(tf.getRotation());
      //ROS_INFO("TFs_fixed_[%ld]: x: %lf, y: %lf, z: %lf",i, TFs_fixed_[i].getOrigin().getX(), TFs_fixed_[i].getOrigin().getY(), TFs_fixed_[i].getOrigin().getZ());
    }
    catch (tf::TransformException ex){
      ROS_ERROR("%s",ex.what());
      ros::Duration(1.0).sleep();
      return false;
      break;
    }

  }
  return true;
}

void ObstacleDetector::update(const ros::Time& time, const ros::Duration& period){

  {
    boost::lock_guard<boost::mutex> guard(points_cb_mutex_);
    // Safety timers and mutex
    if ((ros::Time::now()- safety_ton_points_).toSec()< 2.0 && points_status_> -1){ points_status_= 1; }
    else if ((ros::Time::now()- safety_ton_points_).toSec()> 2.0 && points_status_> -1){ points_status_= 0; }
    if (points_status_ == 0) ROS_WARN("Points' topic is not longer available");
    else if (points_status_ == -1) ROS_WARN_THROTTLE(5, "Waiting for points' topic");
    ros::Time last_time = ros::Time::now();
    if (USE_PCL_CB)
      in_cloud_ptr_ =  cb_cloud_ptr_;
    else if (!USE_PCL_CB && cb_ros_cloud_ptr_->data.size() > 0)
      pcl::fromROSMsg(*cb_ros_cloud_ptr_, *in_cloud_ptr_);

  }

  ros::Time last_time = ros::Time::now();
  if(getTFs()){
    //buildRobotBodyFromSpheres();


    // Build a robot filter fixed objects (fixed coordinates)
    filter_robot_objects_fixed_.markers.clear();
    env_octree_resolution_ = obs_octree_resolution_;
    for(size_t i = 0; i< filter_robot_objects_.markers.size(); i++){
      int tf_idx =  -1;
      for(size_t j = 0; j< tf_names_.size(); j++){
        if (tf_names_[j] == filter_robot_objects_.markers[i].header.frame_id){
          tf_idx = j;
          tf::Vector3 in_pos;
          tf::pointMsgToTF(filter_robot_objects_.markers[i].pose.position, in_pos);
          tf::Vector3 out_pos = TFs_fixed_[j]* in_pos;
          visualization_msgs::Marker marker = filter_robot_objects_.markers[i];
          marker.header.frame_id = fixed_frame_;
          marker.pose.position.x = out_pos.x();
          marker.pose.position.y = out_pos.y();
          marker.pose.position.z = out_pos.z();
          marker.id += 1000;
          marker.header.stamp = ros::Time();
          filter_robot_objects_fixed_.markers.push_back(marker);
          env_octree_resolution_ = std::max(env_octree_resolution_, std::max(marker.scale.x, marker.scale.y ));
          env_octree_resolution_ = std::max(env_octree_resolution_, marker.scale.z );
        }
      }

      if (tf_idx == -1){
        ROS_ERROR ("filter_robot_objects%ld has not correct robot frame", i);
      }

    }

    // Create the filtering object OctreePointCloud
    last_time = ros::Time::now();

    // Proceeds enviroment filter objects
    last_time = ros::Time::now();
    for (size_t i=0 ; i < filter_env_objects_.markers.size(); i++){
      if (filter_env_objects_.markers[i].type == visualization_msgs::Marker::CUBE){
        if (i==0 &&  in_cloud_ptr_->points.size()>0)
          filterBoxOut(filter_env_objects_.markers[i], in_cloud_ptr_, filtered_cloud_ptr_);
        else if (i>0 && filtered_cloud_ptr_->points.size()>0)
          filterBoxOut(filter_env_objects_.markers[i], filtered_cloud_ptr_, filtered_cloud_ptr_);
      }
      else if (filter_env_objects_.markers[i].type == visualization_msgs::Marker::SPHERE){
        if (i==0 &&  in_cloud_ptr_->points.size()>0)
          filterSphereOut(filter_env_objects_.markers[i], in_cloud_ptr_, filtered_cloud_ptr_);
        else if (i>0 && filtered_cloud_ptr_->points.size()>0)
          filterSphereOut(filter_env_objects_.markers[i], filtered_cloud_ptr_, filtered_cloud_ptr_);
      }
    }
    ROS_WARN("Processing 1 takes %lf Size of out pointcloud %ld" ,(ros::Time::now() -last_time).toSec(), filtered_cloud_ptr_->points.size());


    // Proceeds robot filter objects
    last_time = ros::Time::now();
    for (size_t i = 0 ; i < filter_robot_objects_fixed_.markers.size(); i++){

      if (filter_robot_objects_fixed_.markers[i].type == visualization_msgs::Marker::CUBE){
        if (filter_env_objects_.markers.size()==0)
          filterBoxOut(filter_robot_objects_fixed_.markers[i], in_cloud_ptr_, filtered_cloud_ptr_);
        else
          filterBoxOut(filter_robot_objects_fixed_.markers[i], filtered_cloud_ptr_, filtered_cloud_ptr_);
      }

      else if (filter_robot_objects_fixed_.markers[i].type == visualization_msgs::Marker::SPHERE){
        if (filter_env_objects_.markers.size()==0)
          filterSphereOut(filter_robot_objects_fixed_.markers[i], in_cloud_ptr_,filtered_cloud_ptr_);
        else
          filterSphereOut(filter_robot_objects_fixed_.markers[i], filtered_cloud_ptr_,filtered_cloud_ptr_);
      }
    }
    ROS_WARN("Processing 2 takes %lf Size of out pointcloud %ld" ,(ros::Time::now() -last_time).toSec(), filtered_cloud_ptr_->points.size());


    // Proceeds robot obstacle objects
    //detectObstaclesAsBox();
    detectOctreeVoxels();

    if (new_move_)
      simMovingObstacle(period);

    //prepare for publishing
    pcl::toROSMsg(*filtered_cloud_ptr_, *filtered_ros_cloud_ptr_);
    publish(); // here as soon as all calculation are done, another option could be at fixed frame rate in a node;
  }

}


void ObstacleDetector::publish(){
  if (!filter_env_objects_.markers.empty())
    pub_markers_filter_env_objects_.publish(filter_env_objects_);
  if (!filter_robot_objects_.markers.empty())
    pub_markers_filter_robot_objects_.publish(filter_robot_objects_);
  if (!filter_robot_objects_fixed_.markers.empty())
    pub_markers_filter_robot_objects_fixed_.publish(filter_robot_objects_fixed_);
  if (!obstacle_objects_.markers.empty())
    pub_markers_obstacle_objects_.publish(obstacle_objects_);
  if (!obstacle_objects_test_.markers.empty())
  pub_markers_obstacle_objects_test_.publish(obstacle_objects_test_);
  if (!filtered_ros_cloud_ptr_->data.empty())
    pub_output_cloud_.publish(*filtered_ros_cloud_ptr_);

}


void ObstacleDetector::pclPointcloudCB (const pcl::PointCloud<pcl::PointXYZ>::ConstPtr &cloud_msg, pcl::PointCloud<pcl::PointXYZ>::Ptr *dst_cloud_ptr, ros::Time *safety_ton, int *points_status ){
  ros::Time last_time = ros::Time::now();
  boost::lock_guard<boost::mutex> guard(points_cb_mutex_);
  *dst_cloud_ptr = boost::const_pointer_cast<pcl::PointCloud<pcl::PointXYZ> >(cloud_msg);
  *safety_ton = ros::Time::now();
  *points_status = 1;
  ROS_DEBUG("pclPointsCB takes %lf Size of out pointcloud %ld" , (ros::Time::now() -last_time).toSec(), (*dst_cloud_ptr)->points.size());
}

void ObstacleDetector::rosPointcloudCB(const sensor_msgs::PointCloud2ConstPtr& msg, sensor_msgs::PointCloud2Ptr *dst_cloud_ptr, ros::Time *safety_ton, int *points_status){
  ros::Time last_time = ros::Time::now();
  boost::lock_guard<boost::mutex> guard(points_cb_mutex_);
  *dst_cloud_ptr = boost::const_pointer_cast<sensor_msgs::PointCloud2>(msg);
  *safety_ton = ros::Time::now();
  *points_status = 1;
  ROS_DEBUG("rosPointsCB takes %lf Pointcloud height %d and width %d" , (ros::Time::now() -last_time).toSec(), (*dst_cloud_ptr)->height, (*dst_cloud_ptr)->width);
}

void ObstacleDetector::buildRobotBodyFromSpheres(){
  //Equation of line (x,y,z)=(x0,y0,z0)+t(a,b,c), where t(a,b,c) end point (t=1), used for direction
  tf::Vector3 parent(0, 0 ,0), child(0, 0 ,0), result(0, 0 ,0),result_fixed(0, 0 ,0);
  double distance;
  filter_robot_objects_.markers.clear();
  visualization_msgs::Marker marker;
  marker.id = 100;
  marker.ns = "robot_collision_body";
  marker.type = visualization_msgs::Marker::SPHERE;
  marker.action = visualization_msgs::Marker::ADD;
  marker.scale.x = 2*radius_sphere_robot_body_;
  marker.scale.y = 2*radius_sphere_robot_body_;
  marker.scale.z = 2*radius_sphere_robot_body_;
  marker.color.a = 0.3; // Don't forget to set the alpha!
  marker.color.r = 0.0;
  marker.color.g = 0.0;
  marker.color.b = 1.0;
  marker.lifetime = ros::Duration(1);

  for (size_t i=0; i<TFs_.size(); i++){
    child = TFs_[i].getOrigin();
    parent = tf::Vector3(0.0, 0.0, 0.0);
    distance = parent.distance(child);
    double curr_t =0.0;

    // set next spheres if distance is enough
    while (true){
      result= parent + (curr_t/distance) * (child-parent);
      // coordinate of the sphere wrt to the parent frame
      marker.header.frame_id = tf_names_[i];
      marker.pose.position.x = result.x();
      marker.pose.position.y = result.y();
      marker.pose.position.z = result.z();
      marker.id += 1;
      marker.header.stamp = ros::Time();
      filter_robot_objects_.markers.push_back(marker);

      if((distance - curr_t ) < 1.25*radius_sphere_robot_body_) // 1.25 to have nice overlapping
        break;
      else
        curr_t+= radius_sphere_robot_body_;
    }
  }
}

void ObstacleDetector::getFilterObjectsParameters(XmlRpc::XmlRpcValue &obj, visualization_msgs::MarkerArray &filter_objects, const std::string &ns ){

  for (XmlRpc::XmlRpcValue::ValueStruct::iterator map_it=obj.begin(); map_it!=obj.end(); ++map_it) {
    ROS_INFO_STREAM("Found obj: " << (std::string)(map_it->first));

    if ( map_it->second.getType() != XmlRpc::XmlRpcValue::TypeStruct) // map iterator
      ROS_WARN("[%s] is not properly set in yaml file", map_it->first.c_str());
    else{
      visualization_msgs::Marker marker;
      // Check fields
      if ( !map_it->second.hasMember(std::string("id")))
        ROS_WARN("[%s] is not proceed, since key [id] is not found in yaml file.", map_it->first.c_str());
      else if ( !map_it->second.hasMember(std::string("type")))
        ROS_WARN("[%s] is not proceed, since key [type] is not found in yaml file.", map_it->first.c_str());
      else if(!map_it->second.hasMember(std::string("text")))
        ROS_WARN("[%s] is not proceed, since key [text] is not found in yaml file", map_it->first.c_str());
      else if(!map_it->second.hasMember(std::string("frame_id")))
        ROS_WARN("[%s] is not proceed, since key [frame_id] is not found in yaml file", map_it->first.c_str());
      else if(!map_it->second.hasMember(std::string("position")))
        ROS_WARN("[%s] is not proceed, since key [position] is not found in yaml file", map_it->first.c_str());
      else if(!map_it->second.hasMember(std::string("rgba")))
        ROS_WARN("[%s] is not proceed, since key [rgba] is not found in yaml file", map_it->first.c_str());
      else if(!map_it->second.hasMember(std::string("scale")))
        ROS_WARN("[%s] is not proceed, since key [scale] is not found in yaml file", map_it->first.c_str());
      else{
        marker.id  = static_cast<int> (map_it->second[std::string("id")]);
        marker.type  = (map_it->second[std::string("type")] == std::string("CUBE"))? visualization_msgs::Marker::CUBE : marker.type;
        marker.type  = (map_it->second[std::string("type")] == std::string("SPHERE"))? visualization_msgs::Marker::SPHERE : marker.type;
        marker.text = static_cast<std::string> (map_it->second[std::string("text")]);
        marker.header.frame_id  = static_cast<std::string> (map_it->second[std::string("frame_id")]);
        marker.header.stamp = ros::Time();
        marker.ns = ns;
        marker.action = visualization_msgs::Marker::ADD;

        // Check data types filter_obj[i]
        if ( map_it->second[std::string("position")].getType() != XmlRpc::XmlRpcValue::TypeStruct)
          ROS_WARN("[%s] is not proceed, since key [position] is incorrect data type.", map_it->first.c_str());
        else if ( map_it->second[std::string("rgba")].getType() != XmlRpc::XmlRpcValue::TypeStruct)
          ROS_WARN("[%s] is not proceed, since key [rgba] is incorrect data type.", map_it->first.c_str());
        else if ( map_it->second[std::string("scale")].getType() != XmlRpc::XmlRpcValue::TypeStruct)
          ROS_WARN("[%s] is not proceed, since key [scale] is incorrect data type.", map_it->first.c_str());
        else {
          XmlRpc::XmlRpcValue pos = map_it->second[std::string("position")];
          XmlRpc::XmlRpcValue rgba = map_it->second[std::string("rgba")];
          XmlRpc::XmlRpcValue scale =map_it->second[std::string("scale")];
          if (!pos.hasMember(std::string("x")) || !pos.hasMember(std::string("y")) || !pos.hasMember(std::string("z")))
            ROS_WARN("[%s] is not proceed, since not all keys in [position] are found in yaml file.", map_it->first.c_str());
          else if(!rgba.hasMember(std::string("r")) || !rgba.hasMember(std::string("g")) || !rgba.hasMember(std::string("b")) || !rgba.hasMember(std::string("a")))
            ROS_WARN("[%s] is not proceed, since not all keys in [rgba] are found in yaml file.", map_it->first.c_str());
          else if(!scale.hasMember(std::string("x")) || !scale.hasMember(std::string("y")) || !scale.hasMember(std::string("z")))
            ROS_WARN("[%s] is not proceed, since not all keys in [scale] are found in yaml file.", map_it->first.c_str());
          else{
            marker.pose.position.x = static_cast<double> (pos[std::string("x")]);
            marker.pose.position.y = static_cast<double> (pos[std::string("y")]);
            marker.pose.position.z = static_cast<double> (pos[std::string("z")]);
            marker.pose.orientation.x = 0.0;
            marker.pose.orientation.y = 0.0;
            marker.pose.orientation.z = 0.0;
            marker.pose.orientation.w = 1.0;
            marker.color.r = static_cast<double> (rgba[std::string("r")]);
            marker.color.g = static_cast<double> (rgba[std::string("g")]);
            marker.color.b = static_cast<double> (rgba[std::string("b")]);
            marker.color.a = static_cast<double> (rgba[std::string("a")]);
            marker.scale.x = static_cast<double> (scale[std::string("x")]);
            marker.scale.y = static_cast<double> (scale[std::string("y")]);
            marker.scale.z = static_cast<double> (scale[std::string("z")]);
            marker.lifetime = ros::Duration(1);
            filter_objects.markers.push_back(marker);
          }
        }
      }
    }
  }

}

void ObstacleDetector::filterBoxOut(const visualization_msgs::Marker &filter_object, const pcl::PointCloud<pcl::PointXYZ>::Ptr &ptr_in_cloud, pcl::PointCloud<pcl::PointXYZ>::Ptr &ptr_filtered_cloud){

  //  // Passtrough filters to build box inliers / outliers (faster than cropbox and conditional remove)
  //  pcl::PassThrough<pcl::PointXYZ> pass;
  //  pcl::ExtractIndices<pcl::PointXYZ> extract;
  //  pass.setInputCloud (ptr_in_cloud);
  //  extract.setInputCloud (ptr_in_cloud);
  //  pass.setFilterFieldName ("x");
  //  double min =  filter_object.pose.position.x - 0.5 * filter_object.scale.x;
  //  double max =  filter_object.pose.position.x + 0.5 * filter_object.scale.x;
  //  pass.setFilterLimits (min, max);
  //  pcl::PointIndices::Ptr indices_x (new pcl::PointIndices);
  //  pass.filter (indices_x->indices);

  //  min =  filter_object.pose.position.y - 0.5 * filter_object.scale.y;
  //  max =  filter_object.pose.position.y + 0.5 * filter_object.scale.y;
  //  pass.setIndices(indices_x);
  //  pass.setFilterFieldName ("y");
  //  pass.setFilterLimits (min, max);
  //  pcl::PointIndices::Ptr indices_y (new pcl::PointIndices);
  //  pass.filter (indices_y->indices);

  //  min =  filter_object.pose.position.z - 0.5 * filter_object.scale.z;
  //  max =  filter_object.pose.position.z + 0.5 * filter_object.scale.z;
  //  pass.setIndices(indices_y);
  //  pass.setFilterFieldName ("z");
  //  pass.setFilterLimits (min, max);
  //  pcl::PointIndices::Ptr indices_z (new pcl::PointIndices);
  //  pass.filter (indices_z->indices);

  //  if (filter_object.text == std::string("KEEP")){
  //    extract.setIndices (indices_z);
  //    extract.setNegative (false);
  //    extract.filter (*ptr_filtered_cloud);
  //  }
  //  else if (filter_object.text == std::string("REMOVE")){
  //    extract.setIndices (indices_z);
  //    extract.setNegative (true);
  //    extract.filter (*ptr_filtered_cloud);
  //  }


  // Remove the inliers, extract the rest
  pcl::ExtractIndices<pcl::PointXYZ> extract;
  pcl::PointIndices::Ptr indices (new pcl::PointIndices);
  pcl::octree::OctreePointCloudSearch<pcl::PointXYZ>::Ptr octree_ptr(new pcl::octree::OctreePointCloudSearch<pcl::PointXYZ>(env_octree_resolution_));
  double min_x = filter_object.pose.position.x - 0.5 * filter_object.scale.x;
  double max_x = filter_object.pose.position.x + 0.5 * filter_object.scale.x;
  double min_y = filter_object.pose.position.y - 0.5 * filter_object.scale.y;
  double max_y = filter_object.pose.position.y + 0.5 * filter_object.scale.y;
  double min_z = filter_object.pose.position.z - 0.5 * filter_object.scale.z;
  double max_z = filter_object.pose.position.z + 0.5 * filter_object.scale.z;
  Eigen::Vector3f min_pt = Eigen::Vector3f(min_x, min_y, min_z);
  Eigen::Vector3f max_pt = Eigen::Vector3f(max_x, max_y, max_z);

  octree_ptr->setInputCloud (ptr_in_cloud);
  octree_ptr->addPointsFromInputCloud();
  octree_ptr->boxSearch 	(min_pt,	max_pt,	indices->indices ) ;
  extract.setInputCloud (ptr_in_cloud);
  extract.setIndices (indices);
  extract.setNegative (filter_object.text == std::string("REMOVE"));
  extract.filter (*ptr_filtered_cloud);

}


void ObstacleDetector::filterSphereOut(const visualization_msgs::Marker &filter_objects, const pcl::PointCloud<pcl::PointXYZ>::Ptr &ptr_in_cloud, pcl::PointCloud<pcl::PointXYZ>::Ptr &ptr_filtered_cloud){

  //  /// check if point is inside of any of safety spheres
  //  float xc = static_cast<float>(filter_objects.pose.position.x);
  //  float yc = static_cast<float>(filter_objects.pose.position.y);
  //  float zc = static_cast<float>(filter_objects.pose.position.z);
  //  float inv_a = static_cast<float>(1/(0.5 * filter_objects.scale.x));
  //  float inv_b = static_cast<float>(1/(0.5 * filter_objects.scale.y));
  //  float inv_c = static_cast<float>(1/(0.5 * filter_objects.scale.z));
  //  pcl::PointIndices::Ptr inliers (new pcl::PointIndices);

  //  for (size_t j = 0; j < filtered_cloud_ptr_->size(); j++){
  //    float   x = filtered_cloud_ptr_->points[j].x;
  //    float   y = filtered_cloud_ptr_->points[j].y;
  //    float   z = filtered_cloud_ptr_->points[j].z;

  //    // Sphere or elipsoid inliers
  //    if(pow( inv_a * (x - xc), 2) + pow(inv_b * (y - yc), 2) + pow(inv_c * (z - zc), 2)  <= 1)
  //      inliers->indices.push_back(j);
  //  }
  //  // Remove the planar inliers, extract the rest
  //  pcl::ExtractIndices<pcl::PointXYZ> extract;
  //  extract.setInputCloud (ptr_in_cloud);
  //  extract.setIndices (inliers);
  //  extract.setNegative (filter_objects.text == std::string("REMOVE"));
  //  extract.filter (*ptr_filtered_cloud);

  pcl::octree::OctreePointCloudSearch<pcl::PointXYZ>::Ptr octree_ptr(new pcl::octree::OctreePointCloudSearch<pcl::PointXYZ>(env_octree_resolution_));
  pcl::PointIndices::Ptr inliers (new pcl::PointIndices);
  pcl::ExtractIndices<pcl::PointXYZ> extract;
  pcl::PointXYZ query_point;
  std::vector< float > k_sqr_distances;
  query_point.x= static_cast<float>(filter_objects.pose.position.x);
  query_point.y = static_cast<float>(filter_objects.pose.position.y);
  query_point.z = static_cast<float>(filter_objects.pose.position.z);

  octree_ptr->setInputCloud (ptr_in_cloud);
  octree_ptr->addPointsFromInputCloud();
  octree_ptr->radiusSearch(query_point, filter_objects.scale.x, inliers->indices, k_sqr_distances);
  extract.setInputCloud (ptr_in_cloud);
  extract.setIndices (inliers);
  extract.setNegative (filter_objects.text == std::string("REMOVE"));
  extract.filter (*ptr_filtered_cloud);

}

void ObstacleDetector::simMovingObstacle(const ros::Duration &period){

  //Equation of line (x,y,z)=(x0,y0,z0)+t(a,b,c), where t(a,b,c) end point (t=1), used for direction
  double distance = obst_start_position_.distance(obst_end_position_);
  double t = distance/((obst_goal_time_-obs_start_time_)/period.toSec()); //update withim 10 ms same as nodes's ros::Rate loop_rate(100);

  if (obst_curr_distance_ <distance){
    tf::Vector3 v= obst_start_position_ + (obst_curr_distance_/distance) * (obst_end_position_-obst_start_position_);
    obstacle_objects_.markers[0].pose.position.x = v.x();
    obstacle_objects_.markers[0].pose.position.y = v.y();
    obstacle_objects_.markers[0].pose.position.z = v.z();
    obst_curr_distance_+= t;
  }
  else
    new_move_ = false;
}



void ObstacleDetector:: detectObstaclesAsBox(){

  if  (filtered_cloud_ptr_->size() > 0){
    ros::Time last_time = ros::Time::now();

    double lenght_min_point = 10.0;
    double lenght_max_point =-10.0;
    double width_min_point =  10.0;
    double width_max_point = -10.0;
    double height_min_point = 10.0;
    double height_max_point =-10.0;
    double   x, y, z;
    int max_nn =10;

    last_time = ros::Time::now();
    float res = 0.05;
    // Create the filtering object ToDo remove in future
    pcl::ApproximateVoxelGrid<pcl::PointXYZ> sor1;
    sor1.setInputCloud (filtered_cloud_ptr_);
    sor1.setLeafSize (0.01f, 0.01f, 0.01f);
    sor1.filter (*filtered_cloud_ptr_);
    ROS_WARN("Processing 3.0 takes %lf Size of out pointcloud %ld" ,(ros::Time::now() -last_time).toSec(), filtered_cloud_ptr_->points.size());

    last_time = ros::Time::now();
    std::vector<pcl::PointXYZ, Eigen::aligned_allocator<pcl::PointXYZ> > point_grid;
    pcl::octree::OctreePointCloudSearch<pcl::PointXYZ>::Ptr octree_ptr(new pcl::octree::OctreePointCloudSearch<pcl::PointXYZ>(res));
    octree_ptr->setInputCloud (filtered_cloud_ptr_);
    octree_ptr->addPointsFromInputCloud();
    octree_ptr->getOccupiedVoxelCenters (point_grid);
    ROS_WARN("Processing 3.1 takes %lf Size of out point_grid %ld" ,(ros::Time::now() -last_time).toSec(), point_grid.size());

    last_time = ros::Time::now();
    for (size_t i = 0; i < point_grid.size(); i++){
      // parametrize the marker with founded coeficients
      pcl::PointIndices::Ptr inliers (new pcl::PointIndices);
      std::vector< float > k_sqr_distances;
      // Check for noisy voxels
      octree_ptr->radiusSearch(point_grid[i], 0.5 * res, inliers->indices, k_sqr_distances, max_nn);
      if (inliers->indices.size() == max_nn){
        x = point_grid[i].x;
        y = point_grid[i].y;
        z = point_grid[i].z;
        lenght_min_point = (lenght_min_point > x)? x :  lenght_min_point;
        lenght_max_point = (lenght_max_point < x)? x :  lenght_max_point;
        width_min_point = (width_min_point > y)? y :  width_min_point;
        width_max_point = (width_max_point < y)? y :  width_max_point;
        height_min_point = (height_min_point > z)? z :  height_min_point;
        height_max_point = (height_max_point < z)? z :  height_max_point;
      }
    }
    ROS_WARN("Processing OctreePointCloud2 takes %lf" ,(ros::Time::now() -last_time).toSec());


    // parametrize the marker with founded coeficients
    visualization_msgs::Marker marker;
    marker.header.frame_id = "world";
    marker.header.stamp = ros::Time();
    marker.ns = "obstacles";
    marker.id = 1234;
    marker.type = visualization_msgs::Marker::CUBE;
    marker.action = visualization_msgs::Marker::ADD;
    marker.scale.x =  static_cast<double>(std::abs(lenght_max_point - lenght_min_point) + res);
    marker.scale.y =  static_cast<double>(std::abs(width_max_point - width_min_point)+ res);
    marker.scale.z =  static_cast<double>(std::abs(height_max_point - height_min_point)+ res);
    marker.pose.position.x= 0.5 * static_cast<double>((lenght_max_point + lenght_min_point));
    marker.pose.position.y= 0.5 * static_cast<double>((width_max_point + width_min_point));
    marker.pose.position.z= 0.5 * static_cast<double>((height_max_point + height_min_point));
    marker.pose.orientation.x = 0.0;
    marker.pose.orientation.y = 0.0;
    marker.pose.orientation.z = 0.0;
    marker.pose.orientation.w = 1.0;
    marker.color.a = 0.75;
    marker.color.r = 1.0;
    marker.color.g = 1.0;
    marker.color.b = 0.0;
    marker.text = std::string("KEEP");
    marker.lifetime = ros::Duration(0.5);
    obstacle_objects_.markers.push_back(marker);

  }
}

void ObstacleDetector::detectOctreeVoxels(){
  if  (filtered_cloud_ptr_->size() > 0){
    ros::Time last_time = ros::Time::now();

    // Create the filtering object ToDo remove in future
    pcl::ApproximateVoxelGrid<pcl::PointXYZ> sor;
    sor.setInputCloud (filtered_cloud_ptr_);
    sor.setLeafSize (0.01f, 0.01f, 0.01f);
    sor.filter (*filtered_cloud_ptr_);
    ROS_WARN("Processing 3.0 (VoxelGrid)  takes %lf Size of out pointcloud %ld" ,(ros::Time::now() -last_time).toSec(), filtered_cloud_ptr_->points.size());

    std::vector<pcl::PointXYZ, Eigen::aligned_allocator<pcl::PointXYZ> > point_grid;
    pcl::octree::OctreePointCloudSearch<pcl::PointXYZ>::Ptr octree_ptr(new pcl::octree::OctreePointCloudSearch<pcl::PointXYZ>(obs_octree_resolution_));
    octree_ptr->setInputCloud (filtered_cloud_ptr_);
    octree_ptr->addPointsFromInputCloud();
    octree_ptr->getOccupiedVoxelCenters (point_grid);
    ROS_WARN("Processing 3.1 (VoxelCenters) takes %lf Size of out point_grid %ld" ,(ros::Time::now() -last_time).toSec(), point_grid.size());


//    last_time = ros::Time::now();
//    visualization_msgs::Marker marker1;
//    obstacle_objects_test_.markers.clear();
//    obstacle_objects_test_.markers.push_back(marker1);
//    marker1.header.frame_id = "world";
//    marker1.ns = "obstacles_test";
//    marker1.type = visualization_msgs::Marker::CUBE;
//    marker1.action = visualization_msgs::Marker::ADD;
//    marker1.pose.orientation.x = 0.0;
//    marker1.pose.orientation.y = 0.0;
//    marker1.pose.orientation.z = 0.0;
//    marker1.pose.orientation.w = 1.0;
//    marker1.color.a = 0.75;
//    marker1.color.r = 0.3;
//    marker1.color.g = 1.0;
//    marker1.color.b = 0.0;
//    marker1.text = std::string("KEEP");
//    marker1.lifetime = ros::Duration(2.0);


//    pcl::octree::OctreePointCloudSearch<pcl::PointXYZ>::Iterator tree_it;
//    pcl::octree::OctreePointCloudSearch<pcl::PointXYZ>::Iterator tree_it_end = octree_ptr->end();
//    Eigen::Vector3f min_pt, max_pt;
//    // iterate over tree
//    for (tree_it=octree_ptr->begin(); tree_it!=tree_it_end; ++tree_it){
//      octree_ptr->getVoxelBounds (tree_it, min_pt, max_pt);
//    }
//    ROS_WARN("Processing OctreePointCloud2 takes %lf Size of out point_grid %ld" ,(ros::Time::now() -last_time).toSec(), obstacle_objects_test_.markers.size());



    last_time = ros::Time::now();
    visualization_msgs::Marker marker =  obstacle_objects_.markers[0];
    obstacle_objects_.markers.clear();
    obstacle_objects_.markers.push_back(marker);
    marker.header.frame_id = "world";
    marker.ns = "obstacles";
    marker.type = visualization_msgs::Marker::SPHERE;
    marker.action = visualization_msgs::Marker::ADD;
    marker.pose.orientation.x = 0.0;
    marker.pose.orientation.y = 0.0;
    marker.pose.orientation.z = 0.0;
    marker.pose.orientation.w = 1.0;
    marker.color.a = 0.75;
    marker.color.r = 1.0;
    marker.color.g = 1.0;
    marker.color.b = 0.0;
    marker.text = std::string("KEEP");
    marker.lifetime = ros::Duration(1.0);

    for (size_t i = 0; i < point_grid.size(); i++){
      // parametrize the marker with founded coeficients
      pcl::PointIndices::Ptr inliers (new pcl::PointIndices);
      std::vector< float > k_sqr_distances;
      // Check for noisy voxels
      octree_ptr->radiusSearch(point_grid[i], 0.5 * obs_octree_resolution_, inliers->indices, k_sqr_distances, max_nn_);
      if (inliers->indices.size()== max_nn_){
        marker.header.stamp = ros::Time();
        marker.id = 3001+i;
        marker.scale.x = 1.44 * obs_octree_resolution_; //sqrt(2) * octree_resolution_
        marker.scale.y = 1.44 * obs_octree_resolution_;
        marker.scale.z = 1.44 * obs_octree_resolution_;
        marker.pose.position.x= point_grid[i].x;
        marker.pose.position.y= point_grid[i].y;
        marker.pose.position.z= point_grid[i].z;
        obstacle_objects_.markers.push_back(marker);
      }

    }
    ROS_WARN("Processing 3.3 (inliers() takes %lf Size of out point_grid %ld" ,(ros::Time::now() -last_time).toSec(), obstacle_objects_.markers.size());
  }

}