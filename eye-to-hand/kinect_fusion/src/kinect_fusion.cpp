#include "kinect_fusion/kinect_fusion.h"
#include "boost/bind/bind.hpp"
#include "pcl_ros/transforms.h"
#include "opencv2/calib3d/calib3d.hpp"
#include <pcl/filters/filter.h>

namespace kinect_fusion
{

KinectFusion::KinectFusion(){ }

bool KinectFusion::init(ros::NodeHandle &nh)
{
  // Ros node handle for the class
  nh_ = nh;
  aruco_marker_id_= 66; // not used

  // Get base_name from parameter server
  if (!nh_.getParam("base_name", base_name_)){
    nh_.param("base_name", base_name_, std::string ("kinect_fusion"));
    ROS_WARN("Parameter base_name was not found. Default name is used: %s ", base_name_.c_str());
  }

  // Get raw image topics from parameter server
  if (!nh_.getParam("raw_images_topics", raw_images_topics_)){
    nh_.param("raw_images_topics", raw_images_topics_, {"/kinect2_k1/qhd/image_color", "/kinect2_k2/qhd/image_color"});
    ROS_WARN("Parameter raw_images_topics was not found.");
    for (const std::string &topic : raw_images_topics_)
      ROS_WARN("Default topic's name is used: %s ", topic.c_str());
  }

  // Get camera info topics from parameter server
  if (!nh_.getParam("cameras_info_topics", cam_info_topics_)){
    nh_.param("cameras_info_topics", cam_info_topics_, {"/kinect2_k1/hd/camera_info", "/kinect2_k2/hd/camera_info"});
    ROS_WARN("Parameter cameras_info_topics was not found.");
    for (const std::string &topic : cam_info_topics_)
      ROS_WARN("Default topic's name is used: %s ", topic.c_str());
  }

  if (raw_images_topics_.size() !=cam_info_topics_.size())
    ROS_WARN("Sizes of raw_images_topics and cameras_info_topics are different. Aruco detection cannot be used");


  // Get point topics from parameter server
  if (!nh_.getParam("points_topics", point_topics_)){
    nh_.param("points_topics", point_topics_, {"/kinect2_k1/sd/points", "/kinect2_k2/sd/points"});
    ROS_WARN("Parameter points_topics was not found.");
    for (const std::string &topic : point_topics_)
      ROS_WARN("Default topic's name is used: %s ", topic.c_str());
  }

  for (const std::string &topic : point_topics_)
    ROS_INFO("Following topic's name is used: %s ", topic.c_str());

  if (point_topics_.size () > 6){
    ROS_ERROR ("More than 6 topics are found!");
    return false;
  }

  if (!nh_.getParam("using_aruco", using_aruco_)){
    nh_.param("using_aruco", using_aruco_, false);
    ROS_WARN("Parameter using_aruco was not found. Default value is used: false");
  }

  if (!nh_.getParam("aruco_marker_size", aruco_marker_size_)){
    nh_.param("aruco_marker_size", aruco_marker_size_, static_cast<float>(0.265));
    ROS_WARN("Parameter aruco_marker_size was not found. Default value is used: %f", aruco_marker_size_);
  }

  TFs_w_c.resize(point_topics_.size (), tf::Transform::getIdentity());
  TFs_a_c_.resize(cam_info_topics_.size(), tf::Transform::getIdentity());

  images_tran_.resize(raw_images_topics_.size(), image_transport::ImageTransport(nh_));
  subs_image_raw_.resize(raw_images_topics_.size());
  cb_images_ptr_.resize(raw_images_topics_.size());
  in_images_ptr_.resize(raw_images_topics_.size());
  cb_image_status_.resize(raw_images_topics_.size(), -1);
  in_image_status_.resize(raw_images_topics_.size(), -1);
  safety_tons_images_.resize(raw_images_topics_.size(), ros::Time::now());

  subs_cam_info_.resize(cam_info_topics_.size());
  cb_cam_info_ptr_.resize(cam_info_topics_.size());
  in_cam_info_ptr_.resize(cam_info_topics_.size());
  cb_cam_info_status_.resize(cam_info_topics_.size(), -1);
  in_cam_info_status_.resize(cam_info_topics_.size(), -1);
  safety_tons_cam_info_.resize(cam_info_topics_.size(), ros::Time::now());

  subs_points_.resize(point_topics_.size());
  cb_clouds_ptr_.resize(point_topics_.size());
  in_clouds_ptr_.resize(point_topics_.size());
  transf_clouds_ptr_.resize(point_topics_.size());
  cb_points_status_= -1;
  in_points_status_= -1;
  safety_tons_points_ = ros::Time::now();
  msg_filters_.resize(point_topics_.size());
  fused_cloud_ptr_.reset(new sensor_msgs::PointCloud2());


  // Initialize  images subscribers and images containers pointers
  for (size_t i = 0; i <  raw_images_topics_.size(); i++){
    cb_images_ptr_[i].reset (new cv_bridge::CvImage());
    in_images_ptr_[i].reset (new cv_bridge::CvImage());
    if (using_aruco_)
      subs_image_raw_[i] = images_tran_[i].subscribe(raw_images_topics_[i], 1, boost::bind(&KinectFusion::imageCB, this, _1, cb_images_ptr_[i], safety_tons_images_[i], cb_image_status_[i]));
  }

  // Initialize  camera info subsribers and cam_info containers pointers
  for (size_t i = 0; i <  cam_info_topics_.size(); i++){
    cb_cam_info_ptr_[i].reset (new sensor_msgs::CameraInfo());
    in_cam_info_ptr_[i].reset (new sensor_msgs::CameraInfo());
    if (using_aruco_)
      subs_cam_info_[i] = nh_.subscribe<sensor_msgs::CameraInfo>(cam_info_topics_[i], 1, boost::bind(&KinectFusion::cameraInfoCB, this, _1, cb_cam_info_ptr_[i], safety_tons_cam_info_[i], cb_cam_info_status_[i]));
  }

  // Initialize filters for points topics and cointeiners for transformed pointers
  for (size_t i = 0; i < point_topics_.size (); i++){
    msg_filters_[i].reset (new message_filters::Subscriber<PC2> ());
    msg_filters_[i]->subscribe (nh_, point_topics_[i], QUEUE_SIZE);
    transf_clouds_ptr_[i].reset(new sensor_msgs::PointCloud2());
    in_clouds_ptr_[i].reset(new sensor_msgs::PointCloud2());
    cb_clouds_ptr_[i].reset(new sensor_msgs::PointCloud2());
  }


  ts_a6_ptr_.reset (new message_filters::Synchronizer<sync_policy6> (QUEUE_SIZE));
  // Use the first filter as default, when the number of topics is less than 6
  switch (point_topics_.size()) {
  case 2:
    ts_a6_ptr_->connectInput (*msg_filters_[0], *msg_filters_[1], *msg_filters_[0], *msg_filters_[0], *msg_filters_[0], *msg_filters_[0]);
    break;
  case 3:
    ts_a6_ptr_->connectInput (*msg_filters_[0], *msg_filters_[1], *msg_filters_[2], *msg_filters_[0], *msg_filters_[0], *msg_filters_[0]);
    break;
  case 4:
    ts_a6_ptr_->connectInput (*msg_filters_[0], *msg_filters_[1], *msg_filters_[2], *msg_filters_[3], *msg_filters_[0], *msg_filters_[0]);
    break;
  case 5:
    ts_a6_ptr_->connectInput (*msg_filters_[0], *msg_filters_[1], *msg_filters_[2], *msg_filters_[3], *msg_filters_[4], *msg_filters_[0]);
    break;
  case 6:
    ts_a6_ptr_->connectInput (*msg_filters_[0], *msg_filters_[1], *msg_filters_[2], *msg_filters_[3], *msg_filters_[4], *msg_filters_[5]);
    break;
  default:
    ROS_ERROR ("Inappropriate size of the toipics!");
    break;
  }

  ts_a6_ptr_->registerCallback (boost::bind (&KinectFusion::syncPointcloudsCB, this, _1, _2, _3, _4, _5, _6));
  pub_fussed_points = nh_.advertise<sensor_msgs::PointCloud2> (nh_.getNamespace()+"/points", 1);

  ROS_INFO ("KinectFusion with name %s is initialized", base_name_.c_str());
  return true;

}

void KinectFusion::update(const ros::Time& time, const ros::Duration& period)
{

  if (using_aruco_){
    // Safety timers and mutexes images
    std::lock_guard<std::mutex> guard(image_cb_mutex_);
    for (size_t i = 0; i< cb_image_status_.size(); i++){
      in_images_ptr_[i] = cb_images_ptr_[i];
      in_image_status_[i]= cb_image_status_[i];
      if ((ros::Time::now() - safety_tons_images_[i]).toSec()< 2.0 && in_image_status_[i]> -1){ in_image_status_[i]; }
      else if ((ros::Time::now() - safety_tons_images_[i]).toSec()> 2.0 && in_image_status_[i]> -1){ in_image_status_[i]= 0; }
      if ( in_image_status_[i] == 0) ROS_WARN("Camera's topic[%ld] is not longer available", i);
      else if (in_image_status_[i] == -1) ROS_WARN_THROTTLE(5, "Waiting for image's topic");
    }
  }

  if (using_aruco_){
    // Safety timers and mutexes camera info
    std::lock_guard<std::mutex> guard(cam_info_cb_mutex_);
    for (size_t i = 0; i< in_cam_info_status_.size(); i++){
      in_cam_info_ptr_[i] = cb_cam_info_ptr_[i];
      in_cam_info_status_[i]= cb_cam_info_status_[i];
      if ((ros::Time::now()- safety_tons_cam_info_[i]).toSec()< 2.0 && in_cam_info_status_[i]> -1){ in_cam_info_status_[i]; }
      else if ((ros::Time::now()- safety_tons_cam_info_[i]).toSec()> 2.0 && in_cam_info_status_[i]> -1){ in_cam_info_status_[i]= 0; }
      if ( in_cam_info_status_[i] == 0) ROS_WARN("Camera's info topic[%ld] is not longer available", i);
      else if (in_cam_info_status_[i] == -1) ROS_WARN_THROTTLE(5, "Waiting for camera info's topic");
    }
  }

  {
    // Safety timer and mutex synchronizer
    std::lock_guard<std::mutex> guard(sync_cb_mutex_);
    in_points_status_= cb_points_status_;
    in_clouds_ptr_ = std::vector<sensor_msgs::PointCloud2ConstPtr>(cb_clouds_ptr_);
    if ((ros::Time::now()- safety_tons_points_).toSec()< 2.0 && cb_points_status_> -1){ cb_points_status_= 1; }
    else if ((ros::Time::now()- safety_tons_points_).toSec()> 2.0 && cb_points_status_> -1){ cb_points_status_= 0; }
    if (cb_points_status_ == 0 ) ROS_WARN("Synchronizing is not longer available" );
    else if (cb_points_status_ == -1) ROS_WARN_THROTTLE(5, "Waiting for synchronizing");
  }

  // Detect aruco marker
  for (size_t i=0; i < in_images_ptr_.size(); i++){
    if(!in_images_ptr_[i]->image.empty() && !in_cam_info_ptr_[i]->D.empty()){
      markerDetect(in_images_ptr_[i]->image, in_cam_info_ptr_[i], TFs_a_c_[i], aruco_marker_id_, aruco_marker_size_, "Sensor " + std::to_string(i) );
    }
  }
}

void KinectFusion::imageCB(const sensor_msgs::ImageConstPtr& msg, cv_bridge::CvImagePtr &dst_image_ptr, ros::Time &safety_ton, int &image_status){
  std::lock_guard<std::mutex> guard(image_cb_mutex_);
  safety_ton = ros::Time::now();
  try{
    // transform ROS image into OpenCV image
    dst_image_ptr = cv_bridge::toCvCopy(msg, sensor_msgs::image_encodings::BGR8);
    image_status = 1;
  }
  catch (cv_bridge::Exception& e){
    // throw an error msg. if conversion fails
    ROS_ERROR("cv_bridge exception: %s", e.what());
    image_status = 2;
    return;
  }
}

void KinectFusion::cameraInfoCB(const sensor_msgs::CameraInfoConstPtr& msg,
                                sensor_msgs::CameraInfoConstPtr &dst_cam_info_ptr,
                                ros::Time &dst_safety_ton, int &dst_status )
{
  std::lock_guard<std::mutex> guard(cam_info_cb_mutex_);
  dst_cam_info_ptr = msg;
  dst_safety_ton = ros::Time::now();
  dst_status = 1;
}

void KinectFusion::markerDetect(const cv:: Mat& srs_image, const sensor_msgs::CameraInfoConstPtr &cam_info_ptr,
                                tf::Transform &dstTF, int marker_id, float marker_size, std::string windows_name )
{

  tf::Transform Trgb_a, Tw_ir, Trgb_ir, Tw_a;

  tf::StampedTransform transform;
  // aruco with respect to word
  try{
    lr_.lookupTransform("world", "aruco", ros::Time(0), transform);
    Tw_a.setOrigin(transform.getOrigin());
    Tw_a.setRotation(transform.getRotation());
  }
  catch (tf::TransformException ex){
    ROS_ERROR("%s",ex.what());
    ros::Duration(1.0).sleep();
  }

  try{
    if (windows_name == "Sensor 0")
      lr_.lookupTransform("kinect2_k1_rgb_optical_frame", "kinect2_k1_ir_optical_frame", ros::Time(0), transform);
    else if (windows_name == "Sensor 1")
      lr_.lookupTransform("kinect2_k2_rgb_optical_frame", "kinect2_k2_ir_optical_frame", ros::Time(0), transform);
    else
      ROS_WARN ("Transfor between rgb_optical_frame and ir_optical_frame was not found ");
    Trgb_ir.setOrigin(transform.getOrigin());
    Trgb_ir.setRotation(transform.getRotation());
  }
  catch (tf::TransformException ex){
    ROS_ERROR("%s",ex.what());
    ros::Duration(1.0).sleep();
  }

  cv::Mat img;
  srs_image.copyTo(img);

  // detect all markers in an
  cv::Ptr<cv::aruco::Dictionary> dictionary_ptr = cv::aruco::getPredefinedDictionary(cv::aruco::DICT_5X5_250);
  std::vector<int> ids;
  std::vector<std::vector<cv::Point2f> > corners;
  cv::aruco::detectMarkers(img, dictionary_ptr, corners, ids);

  // initialize camera matrix and distortion coefficients
  cv::Mat cameraMatrix = cv::Mat::eye(3, 3, CV_64F);
  cameraMatrix.at<double>(0,0) = cam_info_ptr ->K[0];
  cameraMatrix.at<double>(1,1) = cam_info_ptr ->K[4];
  cameraMatrix.at<double>(0,2) = cam_info_ptr ->K[2];
  cameraMatrix.at<double>(1,2) = cam_info_ptr ->K[5];

  cv::Mat distCoeffs = cv::Mat::zeros(1, 5, CV_64F);
  distCoeffs.at<double>(0,0) = cam_info_ptr ->D[0];
  distCoeffs.at<double>(0,1) = cam_info_ptr ->D[1];
  distCoeffs.at<double>(0,2) = cam_info_ptr ->D[2];
  distCoeffs.at<double>(0,3) = cam_info_ptr ->D[3];
  distCoeffs.at<double>(0,4) = cam_info_ptr ->D[4];

  std::vector< cv::Vec3d > rvecs, tvecs;
  cv::aruco::estimatePoseSingleMarkers(corners, marker_size, cameraMatrix, distCoeffs, rvecs, tvecs);

  // draw axis for each marker
  if (ids.size() > 0)
    cv::aruco::drawDetectedMarkers(img, corners, ids, cv::Scalar(0, 255, 0));

  for(size_t i=0; i<ids.size(); ++i){

    cv::Mat R;
    cv::Rodrigues(rvecs[i], R);
    // aruco marker wrt camera
    Trgb_a.setOrigin(tf::Vector3(tvecs[i][0], tvecs[i][1], tvecs[i][2]));
    Trgb_a.setBasis( tf::Matrix3x3(R.at<double>(0,0), R.at<double>(0,1), R.at<double>(0,2),
                                   R.at<double>(1,0), R.at<double>(1,1), R.at<double>(1,2),
                                   R.at<double>(2,0), R.at<double>(2,1), R.at<double>(2,2)));

    ROS_INFO("%s. Marker with id: %d. Trgb_a: Position [xyz]: [%lf, %lf, %lf]. Orientation[xyzw]: [%lf, %f, %lf, %lf]",
             windows_name.c_str(),ids[i], Trgb_a.getOrigin().getX(), Trgb_a.getOrigin().getY(), Trgb_a.getOrigin().getZ(),
             Trgb_a.getRotation().getX(), Trgb_a.getRotation().getY(), Trgb_a.getRotation().getZ(), Trgb_a.getRotation().getW());

    dstTF = Trgb_a.inverse();
    ROS_INFO("%s. Marker with id: %d. Ta_rgb Position [xyz]: [%lf, %lf, %lf]. Orientation[xyzw]: [%lf, %f, %lf, %lf]",
             windows_name.c_str(),ids[i], dstTF.getOrigin().getX(), dstTF.getOrigin().getY(), dstTF.getOrigin().getZ(),
             dstTF.getRotation().getX(), dstTF.getRotation().getY(), dstTF.getRotation().getZ(), dstTF.getRotation().getW());

    Tw_ir = Tw_a *dstTF * Trgb_ir;
    ROS_INFO("%s. Marker with id: %d. Tw_ir Position [xyz]: [%lf, %lf, %lf]. Orientation[xyzw]: [%lf, %f, %lf, %lf]",
             windows_name.c_str(),ids[i], Tw_ir.getOrigin().getX(), Tw_ir.getOrigin().getY(), Tw_ir.getOrigin().getZ(),
             Tw_ir.getRotation().getX(), Tw_ir.getRotation().getY(), Tw_ir.getRotation().getZ(), Tw_ir.getRotation().getW());

      cv::aruco::drawAxis(img, cameraMatrix, distCoeffs, rvecs[i], tvecs[i], 2);
  }

  cv::imshow(windows_name,img );
 cv::waitKey(1);
}

void KinectFusion::syncPointcloudsCB( const sensor_msgs::PointCloud2ConstPtr &msg1, const sensor_msgs::PointCloud2ConstPtr &msg2,
                                      const sensor_msgs::PointCloud2ConstPtr &msg3, const sensor_msgs::PointCloud2ConstPtr &msg4,
                                      const sensor_msgs::PointCloud2ConstPtr &msg5, const sensor_msgs::PointCloud2ConstPtr &msg6)
{

  ROS_DEBUG("Time between two callbacks takes %lf" , (ros::Time::now() -safety_tons_points_).toSec());
  {
    std::lock_guard<std::mutex> guard(sync_cb_mutex_);
    cb_clouds_ptr_[0] = msg1;
    cb_clouds_ptr_[1] = msg2;

    if (point_topics_.size() > 2)
      cb_clouds_ptr_[2] = msg3;
    if (point_topics_.size() > 3)
      cb_clouds_ptr_[3] = msg4;
    if (point_topics_.size() > 4)
      cb_clouds_ptr_[4] = msg5;
    if (point_topics_.size() > 5)
      cb_clouds_ptr_[5] = msg6;
    cb_points_status_ = 1;
    safety_tons_points_ = ros::Time::now();
  }

  /// \todo  {Research for event based callback (check std::future)}
  // All work done in callback.
  pointcloudsFusion(cb_clouds_ptr_);
}

void KinectFusion::pointcloudsFusion(const std::vector<sensor_msgs::PointCloud2ConstPtr> &in_clouds_ptrs){

  ros::Time tic_all = ros::Time::now();
  ros::Time tic = ros::Time::now();
  for (size_t i = 0 ; i < point_topics_.size(); ++i){
    tic = ros::Time::now();
    pcl_ros::transformPointCloud ("world", *in_clouds_ptrs[i], *transf_clouds_ptr_[i], lr_);
    transf_clouds_ptr_[i]->header.stamp = ros::Time::now();
    ROS_DEBUG("Transform %lu takes %lf" , i ,(ros::Time::now() -tic).toSec());
  }

  tic = ros::Time::now();
  pcl::concatenatePointCloud (*transf_clouds_ptr_[0], *transf_clouds_ptr_[1], *fused_cloud_ptr_);
  ROS_DEBUG("Concatenate 1 takes %lf" , (ros::Time::now() -tic).toSec());

  /// \todo { Test all cases for more than 2 kinects}
  if (point_topics_.size() > 2){
    for (size_t i = 2 ; i < point_topics_.size(); ++i){
      tic = ros::Time::now();
      pcl_ros::transformPointCloud ("world", *in_clouds_ptrs[i], *transf_clouds_ptr_[i], lr_);
      ROS_DEBUG("Transform %lu takes %lf" , i ,(ros::Time::now() -tic).toSec());
      tic = ros::Time::now();
      pcl::concatenatePointCloud (*transf_clouds_ptr_[i], *fused_cloud_ptr_, *fused_cloud_ptr_);
      ROS_DEBUG("Concatenate %lu takes %lf" , i, (ros::Time::now() -tic).toSec());
      fused_cloud_ptr_->header.stamp = ros::Time::now();
    }
  }

  pub_fussed_points.publish(fused_cloud_ptr_);
  ROS_INFO("clouds Fusion takes %lf Pointcloud height %d and width %d" , (ros::Time::now() -tic_all).toSec(), fused_cloud_ptr_->height, fused_cloud_ptr_->width);
}
} // end of namespace kinect_fusion
