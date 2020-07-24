#include <cmath>
#include <math.h>
#include <deque>
#include <mutex>
#include <thread>
#include <fstream>
#include <csignal>
#include <ros/ros.h>
#include <Eigen/Eigen>
#include <eigen_conversions/eigen_msg.h>
#include <condition_variable>

#include <opencv2/opencv.hpp>
#include <nav_msgs/Odometry.h>
#include <tf/transform_broadcaster.h>

#include <sophus/se3.hpp>
#include <sophus/so3.hpp>

#include <pcl/common/io.h>
#include <pcl/point_cloud.h>
#include <pcl/point_types.h>
#include <pcl/common/transforms.h>
#include <pcl/kdtree/kdtree_flann.h>
#include <pcl_conversions/pcl_conversions.h>

#include <sensor_msgs/Imu.h>
#include <sensor_msgs/PointCloud2.h>
#include <livox_loam_kp/KeyPointPose.h>
#include <geometry_msgs/Vector3.h>

//#include "imu_processor/data_process.h"

/// *************Preconfiguration
using Sophus::SE3d;
using Sophus::SO3d;

#define SKEW_SYM_MATRX(v)  0.0,-v[2],v[1],v[2],0.0,-v[0],-v[1],v[0],0.0
#define MAX_INI_COUNT (50)
#define FORWARD  (0)
#define BACKWARD (1)
#define SET_POS6D [](const double t, const Vec3 &a, const Vec3 &g, \
                  const Eigen::Vector3d &b_a, const Eigen::Vector3d &b_g, \
                  const Eigen::Vector3d &p, const Eigen::Matrix3d &R) \
  { Pose6D rot_kp; \
    rot_kp.offset_time = t; \
    rot_kp.acc = a; \
    rot_kp.gyr = g; \
    tf::vectorEigenToMsg(b_a, rot_kp.bias_acc); \
    tf::vectorEigenToMsg(b_g, rot_kp.bias_gyr); \
    tf::vectorEigenToMsg(p, rot_kp.pos); \
    for (int i = 0; i < 9; i++) {rot_kp.rot[i] = R(i/3, i%3);} \
    return rot_kp;}

inline double rad2deg(double radians) { return radians * 180.0 / M_PI; }
inline double deg2rad(double degrees) { return degrees * M_PI / 180.0; }

pcl::PointCloud<pcl::PointXYZINormal>::Ptr laserCloudtmp(
    new pcl::PointCloud<pcl::PointXYZINormal>());

typedef pcl::PointXYZINormal PointType;
typedef pcl::PointCloud<PointType> PointCloudXYZI;
typedef std::vector<PointType, Eigen::aligned_allocator<PointType> > PointsList;
typedef livox_loam_kp::Pose6D Pose6D;
typedef geometry_msgs::Vector3 Vec3;

struct MeasureGroup
{
  sensor_msgs::PointCloud2ConstPtr lidar;
  std::vector<sensor_msgs::Imu::ConstPtr> imu;
};

const bool time_list(PointType &x, PointType &y) {return (x.curvature < y.curvature);};

/// *************IMU Process and undistortion
class ImuProcess
{
 public:
  EIGEN_MAKE_ALIGNED_OPERATOR_NEW

  ImuProcess();
  ~ImuProcess();

  void Process(const MeasureGroup &meas);
  void Reset();

  Eigen::Matrix3d Exp(const Eigen::Vector3d &ang_vel, const double &dt);

  void IntegrateGyr(const std::vector<sensor_msgs::Imu::ConstPtr> &v_imu);

  void UndistortPcl(const MeasureGroup &meas, PointCloudXYZI &pcl_in_out, const bool orientation);

  ros::NodeHandle nh;

  void Integrate(const sensor_msgs::ImuConstPtr &imu);
  void Reset(double start_timestamp, const sensor_msgs::ImuConstPtr &lastimu);

  const Sophus::SO3d GetRot() const;

  double scale_gravity;

  Eigen::Vector3d zero_bias_acc;
  Eigen::Vector3d zero_bias_gyr;

  Eigen::Vector3d cov_acc;
  Eigen::Vector3d cov_gyr;

 private:
  /*** Whether is the first frame, init for first frame ***/
  bool b_first_frame_ = true;
  bool Need_init      = true;

  int init_iter_num = 1;
  Eigen::Vector3d mean_acc;
  Eigen::Vector3d mean_gyr;

  /*** Input pointcloud ***/
  PointCloudXYZI::Ptr cur_pcl_in_;
  /*** Undistorted pointcloud ***/
  PointCloudXYZI::Ptr cur_pcl_un_;

  /*** Transform form lidar to imu ***/
  Sophus::SE3d T_i_l;
  //// For timestamp usage
  sensor_msgs::PointCloud2ConstPtr last_lidar_;
  sensor_msgs::ImuConstPtr last_imu_;

  /*** For gyroscope integration ***/
  double start_timestamp_;
  /// Making sure the equal size: v_imu_ and v_rot_
  std::vector<sensor_msgs::ImuConstPtr> v_imu_;
  std::vector<Sophus::SO3d> v_rot_;
  std::vector<Eigen::Matrix3d> v_rot_pcl_;

  livox_loam_kp::KeyPointPose v_rot_kp_;
};


ImuProcess::ImuProcess()
    : b_first_frame_(true), Need_init(true), last_lidar_(nullptr), last_imu_(nullptr), start_timestamp_(-1)
{
  Eigen::Quaterniond q(0, 1, 0, 0);
  Eigen::Vector3d t(0, 0, 0);
  T_i_l = Sophus::SE3d(q, t);
  init_iter_num = 1;
  scale_gravity = 1.0;
  cov_acc       = Eigen::Vector3d(0.1, 0.1, 0.1);
  cov_gyr       = Eigen::Vector3d(0.1, 0.1, 0.1);
  mean_acc      = Eigen::Vector3d(0, 0, -1.0);
  mean_gyr      = Eigen::Vector3d(0, 0, 0);
}

ImuProcess::~ImuProcess() {std::cout<<"**********destroy imuprocess************"<<std::endl;}

void ImuProcess::Reset() 
{
  ROS_WARN("Reset ImuProcess");

  scale_gravity  = 1.0;

  zero_bias_acc  = Eigen::Vector3d(0, 0, 0);
  zero_bias_gyr  = zero_bias_acc;

  cov_acc        = Eigen::Vector3d(0.1, 0.1, 0.1);
  cov_gyr        = Eigen::Vector3d(0.1, 0.1, 0.1);
  mean_acc       = Eigen::Vector3d(0, 0, -1.0);
  mean_gyr       = Eigen::Vector3d(0, 0, 0);

  Need_init      = true;
  b_first_frame_ = true;
  init_iter_num  = 1;

  last_lidar_    = nullptr;
  last_imu_      = nullptr;

  //gyr_int_.Reset(-1, nullptr);
  start_timestamp_ = -1;
  v_rot_.clear();
  v_imu_.clear();
  v_rot_pcl_.clear();
  v_rot_kp_.pose6D.clear();

  cur_pcl_in_.reset(new PointCloudXYZI());
  cur_pcl_un_.reset(new PointCloudXYZI());
}

/* Eigen::Matrix3d Exp(const Eigen::Vector3d &r_axis, const double &r_ang)
{
    Eigen::Matrix3d K;
    Eigen::Matrix3d Eye3d = Eigen::Matrix3d::Identity();
    K << SKEW_SYM_MATRX(r_axis);
    return (Eye3d + std::sin(-r_ang) * K + (1.0 - std::cos(-r_ang)) * K * K);
} */

Eigen::Matrix3d ImuProcess::Exp(const Eigen::Vector3d &ang_vel, const double &dt)
{
  double ang_vel_norm = ang_vel.norm();
  Eigen::Matrix3d Eye3d = Eigen::Matrix3d::Identity();
  if (ang_vel_norm > 0.0000001)
  {
    Eigen::Vector3d r_axis = ang_vel / ang_vel_norm;
    Eigen::Matrix3d K;
    
    K << SKEW_SYM_MATRX(r_axis);

    double r_ang = ang_vel_norm * dt;

    Eigen::Matrix3d R = Eye3d + std::sin(r_ang) * K + (1.0 - std::cos(r_ang)) * K * K;

    /// Roderigous Tranformation
    return R;
  }
  else
  {
    return Eye3d;
  }
}

const Sophus::SO3d ImuProcess::GetRot() const 
{
  if (v_rot_.empty())
  {
    return SO3d();
  }
  else 
  {
    return v_rot_.back();
  }
}

void ImuProcess::IntegrateGyr(const std::vector<sensor_msgs::Imu::ConstPtr> &v_imu)
{
  /// Reset gyr integrator
  // gyr_int_.Reset(last_lidar_->header.stamp.toSec(), last_imu_);
  start_timestamp_=last_lidar_->header.stamp.toSec();

  /// And then integrate all the imu measurements
  for (const auto &imu : v_imu)
  {
    if (v_rot_.empty())
    {
      ROS_ASSERT(start_timestamp_ > 0);
      ROS_ASSERT(last_imu_ != nullptr);

      /// Identity rotation
      v_rot_.push_back(SO3d());

      /// Interpolate imu in
      sensor_msgs::ImuPtr imu_inter(new sensor_msgs::Imu());
      double dt1 = start_timestamp_ - last_imu_->header.stamp.toSec();
      double dt2 = imu->header.stamp.toSec() - start_timestamp_;
      /* ROS_ASSERT_MSG(dt1 <= 0 && dt2 <= 0, "%f - %f - %f",
                    last_imu_->header.stamp.toSec(), start_timestamp_,
                    imu->header.stamp.toSec()); */
      double w1 = dt2 / (dt1 + dt2 + 1e-9);
      double w2 = dt1 / (dt1 + dt2 + 1e-9);

      const auto &gyr1 = last_imu_->angular_velocity;
      const auto &acc1 = last_imu_->linear_acceleration;
      const auto &gyr2 = imu->angular_velocity;
      const auto &acc2 = imu->linear_acceleration;

      imu_inter->header.stamp.fromSec(start_timestamp_);
      imu_inter->angular_velocity.x = w1 * gyr1.x + w2 * gyr2.x;
      imu_inter->angular_velocity.y = w1 * gyr1.y + w2 * gyr2.y;
      imu_inter->angular_velocity.z = w1 * gyr1.z + w2 * gyr2.z;
      imu_inter->linear_acceleration.x = w1 * acc1.x + w2 * acc2.x;
      imu_inter->linear_acceleration.y = w1 * acc1.y + w2 * acc2.y;
      imu_inter->linear_acceleration.z = w1 * acc1.z + w2 * acc2.z;

      v_imu_.push_back(imu_inter);
    }

    const SO3d &rot_last    = v_rot_.back();
    const auto &imumsg_last = v_imu_.back();
    const double &time_last = imumsg_last->header.stamp.toSec();
    Eigen::Vector3d gyr_last(imumsg_last->angular_velocity.x,
                            imumsg_last->angular_velocity.y,
                            imumsg_last->angular_velocity.z);
    double time = imu->header.stamp.toSec();
    Eigen::Vector3d gyr(imu->angular_velocity.x, imu->angular_velocity.y,
                        imu->angular_velocity.z);
    assert(time >= 0);
    
    Eigen::Vector3d ang_vel_avr = 0.5 * (gyr + gyr_last);

    double dt_seg    = time - time_last;
    auto delta_angle = dt_seg * ang_vel_avr;
    auto delta_r = SO3d::exp(delta_angle);

    SO3d rot = rot_last * delta_r;

    v_imu_.push_back(imu);
    v_rot_.push_back(rot);
  }

  std::cout<< "size of imu stack:" <<v_imu.size() <<std::endl;

  ROS_INFO("integrate rotation angle [x, y, z]: [%.2f, %.2f, %.2f]",
           GetRot().angleX() * 180.0 / M_PI,
           GetRot().angleY() * 180.0 / M_PI,
           GetRot().angleZ() * 180.0 / M_PI);
}

void ImuProcess::UndistortPcl(const MeasureGroup &meas, PointCloudXYZI &pcl_in_out, const bool orientation)
{
  Eigen::Matrix3d Eye3d(Eigen::Matrix3d::Identity());
  Eigen::Vector3d Zero3d(0, 0, 0);

  const auto &v_imu       = meas.imu;
  const auto &v_imu_last  = meas.imu.back();
  const auto &v_imu_first = meas.imu.front();
  const double &imu_beg_time = v_imu_first->header.stamp.toSec();
  const double &imu_end_time = v_imu_last->header.stamp.toSec();
  const double &pcl_beg_time = meas.lidar->header.stamp.toSec();
  
  /// sort point clouds by offset time
  pcl::fromROSMsg(*(meas.lidar), pcl_in_out);
  std::sort(pcl_in_out.points.begin(), pcl_in_out.points.end(), time_list);

  /// initialize the rotation of first point clouds
  
  double max_offs_t = imu_end_time - imu_beg_time;
  Eigen::Matrix3d R_kp(Eye3d);
  auto set_pose6d = SET_POS6D;
  Pose6D rot_kp = set_pose6d(max_offs_t, v_imu_last->linear_acceleration, v_imu_last->angular_velocity, \
                      Zero3d, zero_bias_gyr, Zero3d, R_kp);

  v_rot_kp_.header = v_imu_first->header;
  v_rot_kp_.pose6D.clear();
  v_rot_kp_.pose6D.push_back(rot_kp);

  v_rot_pcl_.clear();
  v_rot_pcl_.push_back(Eye3d);

  // std::cout<< "******IMU key time: "
  if (orientation == FORWARD)
  {
    /// iterator definitions for imu and point clouds contatintor
    std::vector<sensor_msgs::Imu::ConstPtr>::const_iterator it_imu = v_imu.begin();
    PointCloudXYZI::iterator it_pcl = pcl_in_out.points.begin();
    
    for (; it_imu != (v_imu.end() - 1); it_imu++)
    {
      auto &imu_seg_head = *(it_imu);
      auto &imu_seg_tail = *(it_imu + 1);

      double imu_seg_tail_time = imu_seg_tail->header.stamp.toSec();
      double imu_seg_head_time = imu_seg_head->header.stamp.toSec();

      Eigen::Vector3d gyr_seg_end(imu_seg_tail->angular_velocity.x,
                                imu_seg_tail->angular_velocity.y,
                                imu_seg_tail->angular_velocity.z);

      Eigen::Vector3d gyr_seg_head(imu_seg_head->angular_velocity.x,
                                imu_seg_head->angular_velocity.y,
                                imu_seg_head->angular_velocity.z);
      
      Eigen::Vector3d ang_vel_avr = 0.5 * (gyr_seg_end + gyr_seg_head) - zero_bias_gyr;

      /// unstort the point cloud points in each IMU segment
      double pt_time_abs = it_pcl->curvature / double(1000) + pcl_beg_time;
      int i = 0;
      for(; (pt_time_abs <= imu_seg_tail_time) && (it_pcl != (pcl_in_out.points.end() - 1)); it_pcl ++)
      {
        /// find the base tor keypoint
        pt_time_abs = it_pcl->curvature / double(1000) + pcl_beg_time;
        double dt   = pt_time_abs - imu_seg_head_time;
        // int upper_kp_index = floor((imu_end_time - pt_time_abs) / imu_seg_time);

        /// Transform to the 'end' frame, using only the rotation
        /// Note: Compensation direction is INVERSE of Frame's moving direction
        /// So if we want to compensate a point at timestamp-i to the frame-e
        /// P_compensate = Exp(omega, dt) * R_last * Pi + t_ei

        Eigen::Vector3d tie(0, 0, 0);
        Eigen::Matrix3d Rie(Exp(ang_vel_avr, dt) * R_kp);
        Eigen::Vector3d v_pt_comp_e = Rie * (Eigen::Vector3d(it_pcl->x, it_pcl->y, it_pcl->z) - tie);

        /// save Undistorted points and their rotation
        it_pcl->x = v_pt_comp_e.x();
        it_pcl->y = v_pt_comp_e.y();
        it_pcl->z = v_pt_comp_e.z();

        v_rot_pcl_.push_back(Rie);

        // if ((i++) % 50 == 0)
        // {
        //   std::cout<<i<< ": current pcl time:"<< std::setprecision(8) << pt_time_abs << " and "<< pcl_beg_time <<std::endl;
        //   // std::cout<< "undistort rotation matrix:\n" << Rie <<std::endl;
        // }
      }

      R_kp = Exp(ang_vel_avr, imu_seg_tail_time - imu_seg_head_time) * R_kp;
      double offset_t = imu_seg_tail_time - imu_beg_time;
      set_pose6d(offset_t, imu_seg_tail->linear_acceleration, imu_seg_tail->angular_velocity, \
                      Zero3d, zero_bias_gyr, Zero3d, R_kp);
      v_rot_kp_.pose6D.push_back(rot_kp);
    }
  }
  else
  {
    /// iterator definitions for imu and point clouds contatintor
    std::vector<sensor_msgs::Imu::ConstPtr>::const_iterator it_imu = v_imu.end() - 1;
    PointCloudXYZI::iterator it_pcl = pcl_in_out.points.end() - 1;
    
    for (; it_imu != v_imu.begin(); it_imu--)
    {
      auto &imu_seg_head = *(it_imu - 1);
      auto &imu_seg_tail = *(it_imu);

      double imu_seg_tail_time = imu_seg_tail->header.stamp.toSec();
      double imu_seg_head_time = imu_seg_head->header.stamp.toSec();

      Eigen::Vector3d gyr_seg_end(imu_seg_tail->angular_velocity.x,
                                imu_seg_tail->angular_velocity.y,
                                imu_seg_tail->angular_velocity.z);

      Eigen::Vector3d gyr_seg_head(imu_seg_head->angular_velocity.x,
                                imu_seg_head->angular_velocity.y,
                                imu_seg_head->angular_velocity.z);
      
      Eigen::Vector3d ang_vel_avr = 0.5 * (gyr_seg_end + gyr_seg_head) - zero_bias_gyr;

      std::cout<<"head time: "<<imu_seg_head_time << " tail time: " << imu_seg_tail_time <<std::endl;

      /// unstort the point cloud points in each IMU segment
      double pt_time_abs = it_pcl->curvature / double(1000) + pcl_beg_time;
      int i = 0;
      for(; (pt_time_abs >= imu_seg_head_time) && (it_pcl != pcl_in_out.points.begin()); it_pcl --)
      {
        /// find the base tor keypoint
        pt_time_abs = it_pcl->curvature / double(1000) + pcl_beg_time;
        double dt   = imu_seg_tail_time - pt_time_abs;
        // int upper_kp_index = floor((imu_end_time - pt_time_abs) / imu_seg_time);

        /// Transform to the 'end' frame, using only the rotation
        /// Note: Compensation direction is INVERSE of Frame's moving direction
        /// So if we want to compensate a point at timestamp-i to the frame-e
        /// P_compensate = Exp(omega, dt) * R_last * Pi + t_ei

        Eigen::Vector3d tie(0, 0, 0);
        Eigen::Matrix3d Rie(Exp(-ang_vel_avr, dt) * R_kp);
        Eigen::Vector3d v_pt_comp_e = Rie * (Eigen::Vector3d(it_pcl->x, it_pcl->y, it_pcl->z) - tie);

        /// save Undistorted points and their rotation
        it_pcl->x = v_pt_comp_e.x();
        it_pcl->y = v_pt_comp_e.y();
        it_pcl->z = v_pt_comp_e.z();

        v_rot_pcl_.push_back(Rie);

        // if ((i++) % 100 == 0)
        // {
        //   std::cout<<i<< " current pcl time:"<< std::setprecision(8) << pt_time_abs << " and "<< imu_seg_tail_time<<std::endl;
        //   std::cout<<"rotation matrix: \n"<<v_rot_pcl_.back()<<std::endl;
        //   // std::cout<< "undistort rotation matrix:\n" << Rie <<std::endl;
        // }
      }
      R_kp = Exp(-ang_vel_avr, imu_seg_tail_time - imu_seg_head_time) * R_kp;
      double offset_t = imu_seg_head_time - imu_beg_time;
      set_pose6d(offset_t, imu_seg_head->linear_acceleration, imu_seg_head->angular_velocity, \
                      Zero3d, zero_bias_gyr, Zero3d, R_kp);
      v_rot_kp_.pose6D.push_back(rot_kp);
    }
  }

  ROS_INFO("undistort rotation angle [x, y, z]: [%.2f, %.2f, %.2f]",
           v_rot_pcl_.back().eulerAngles(0, 1, 2)[0] * 180.0 / M_PI,
           v_rot_pcl_.back().eulerAngles(0, 1, 2)[1] * 180.0 / M_PI,
           v_rot_pcl_.back().eulerAngles(0, 1, 2)[2] * 180.0 / M_PI);
  std::cout<< "v_rot_pcl_ size: "<<v_rot_pcl_.size()<<"v_rot_kp_ size: "<< v_rot_kp_.pose6D.size()<<"v_rot_ size: "<< v_rot_.size()<< std::endl;
}

void ImuProcess::Process(const MeasureGroup &meas)
{
  clock_t process_start, t1,t2,t3;

  process_start=clock();

  ROS_ASSERT(!meas.imu.empty());
  ROS_ASSERT(meas.lidar != nullptr);
  ROS_INFO("Process lidar at time: %.4f, %lu imu msgs from %.4f to %.4f",
            meas.lidar->header.stamp.toSec(), meas.imu.size(),
            meas.imu.front()->header.stamp.toSec(),
            meas.imu.back()->header.stamp.toSec());

  auto pcl_in_msg = meas.lidar;

  if (b_first_frame_)
  {
    /// The very first lidar frame
    Need_init = true;

    /// Reset
    Reset();

    /// Record first lidar, and first useful imu
    last_lidar_ = pcl_in_msg;
    last_imu_   = meas.imu.back();

    Eigen::Vector3d cur_acc(last_imu_->linear_acceleration.x, last_imu_->linear_acceleration.y,
                        last_imu_->linear_acceleration.z);
    Eigen::Vector3d cur_gyr(last_imu_->angular_velocity.x, last_imu_->angular_velocity.y,
                      last_imu_->angular_velocity.z);
    double cur_norm = cur_acc.norm();

    init_iter_num = 1;
    scale_gravity = cur_norm;
    mean_acc      = cur_acc;
    mean_gyr      = cur_gyr;

    ROS_WARN("The very first lidar frame");

    /// Do nothing more, return
    b_first_frame_ = false;

    return;
  }

  if (Need_init)
  {
    /// 1. initializing the gyro bias, acc and gyro covariance
    /// 2. normalize the acceleration measurenments to unit gravity

    double cur_norm = 1.0;
    int N           = init_iter_num;
    ROS_INFO("IMU Initializing: %.1f %%", float(N) / MAX_INI_COUNT * 100);

    for (const auto &imu : meas.imu)
    {
      Eigen::Vector3d cur_acc(imu->linear_acceleration.x, imu->linear_acceleration.y,
                        imu->linear_acceleration.z);
      Eigen::Vector3d cur_gyr(imu->angular_velocity.x, imu->angular_velocity.y,
                        imu->angular_velocity.z);
      cur_norm = cur_acc.norm();

      N = init_iter_num;

      scale_gravity += (cur_norm - scale_gravity) / N;
      mean_acc      += (cur_acc - mean_acc) / N;
      mean_gyr      += (cur_gyr - mean_gyr) / N;

      cov_acc = cov_acc * (N - 1.0) / N + (cur_acc - mean_acc).cwiseProduct(cur_acc - mean_acc) * (N - 1.0) / (N * N);
      cov_gyr = cov_gyr * (N - 1.0) / N + (cur_gyr - mean_gyr).cwiseProduct(cur_gyr - mean_gyr) * (N - 1.0) / (N * N);

      init_iter_num ++;
      // std::cout<< imu->linear_acceleration_covariance[0] <<std::endl;
    }

    if (init_iter_num > MAX_INI_COUNT)
    {
      Need_init     = false;

      scale_gravity = 1.0 / std::max(scale_gravity,0.1);
      zero_bias_gyr = mean_gyr;

      ROS_INFO("Calibration Results: Gravity_scale: %.4f; zero_bias_gyr: %.4f %.4f %.4f; acc covarience: %.4f %.4f %.4f; gry covarience: %.4f %.4f %.4f",\
               scale_gravity, zero_bias_gyr[0], zero_bias_gyr[1], zero_bias_gyr[2], cov_acc[0], cov_acc[1], cov_acc[2], cov_gyr[0], cov_gyr[1], cov_gyr[2]);
    }
  }
  else
  {
    ROS_INFO("Process IMU");
    /// Integrate all input imu message
    IntegrateGyr(meas.imu);

    /// Initial pose from IMU (with only rotation)
    t1 = clock();

    /// Undistort points： the first point is assummed as the base frame
    /// Compensate lidar points with IMU rotation (with only rotation now)
    UndistortPcl(meas, *cur_pcl_un_, BACKWARD);

    t2 = clock();

    {
      static ros::Publisher pub_UndistortPcl =
          nh.advertise<sensor_msgs::PointCloud2>("/livox_first_point", 100);
      sensor_msgs::PointCloud2 pcl_out_msg;
      pcl::toROSMsg(*laserCloudtmp, pcl_out_msg);
      pcl_out_msg.header = pcl_in_msg->header;
      pcl_out_msg.header.frame_id = "/livox";
      pub_UndistortPcl.publish(pcl_out_msg);
      laserCloudtmp->clear();
    }

    {
      static ros::Publisher pub_UndistortPcl =
          nh.advertise<sensor_msgs::PointCloud2>("/livox_undistort", 100);
      sensor_msgs::PointCloud2 pcl_out_msg;
      pcl::toROSMsg(*cur_pcl_un_, pcl_out_msg);
      pcl_out_msg.header = pcl_in_msg->header;
      pcl_out_msg.header.frame_id = "/livox";
      pub_UndistortPcl.publish(pcl_out_msg);
    }

    {
      static ros::Publisher pub_UndistortPcl =
          nh.advertise<sensor_msgs::PointCloud2>("/livox_distort", 100);
      sensor_msgs::PointCloud2 pcl_out_msg;
      pcl::toROSMsg(*cur_pcl_in_, pcl_out_msg);
      pcl_out_msg.header = pcl_in_msg->header;
      pcl_out_msg.header.frame_id = "/livox";
      pub_UndistortPcl.publish(pcl_out_msg);
    }

    {
      static ros::Publisher pub_KeyPointPose6D =
          nh.advertise<livox_loam_kp::KeyPointPose>("/KeyPointPose6D", 100);
      pub_KeyPointPose6D.publish(v_rot_kp_);
    }

    t3 = clock();
    
    std::cout<<"Points number in one steps: "<<cur_pcl_un_->points.size()<<"; Time Consumption: preintegration "\
    <<t1 - process_start<<" undistort "<<t2 - t1<<" publish "<<t3 - t2<<std::endl;

    /// Record last measurements
    last_lidar_ = pcl_in_msg;
    last_imu_ = meas.imu.back();
    cur_pcl_in_.reset(new PointCloudXYZI());
    cur_pcl_un_.reset(new PointCloudXYZI());
  }
}

/// *************ROS Node
/// To notify new data
std::mutex mtx_buffer;
std::condition_variable sig_buffer;
bool b_exit = false;
bool b_reset = false;

/// Buffers for measurements
double last_timestamp_lidar = -1;
std::deque<sensor_msgs::PointCloud2::ConstPtr> lidar_buffer;
double last_timestamp_imu = -1;
std::deque<sensor_msgs::Imu::ConstPtr> imu_buffer;

void SigHandle(int sig)
{
  b_exit = true;
  ROS_WARN("catch sig %d", sig);
  sig_buffer.notify_all();
}

void pointcloud_cbk(const sensor_msgs::PointCloud2::ConstPtr &msg) 
{
  const double timestamp = msg->header.stamp.toSec();
  ROS_DEBUG("get point cloud at time: %.6f", timestamp);

  mtx_buffer.lock();

  if (timestamp < last_timestamp_lidar)
  {
    ROS_ERROR("lidar loop back, clear buffer");
    lidar_buffer.clear();
  }
  last_timestamp_lidar = timestamp;

  lidar_buffer.push_back(msg);

  mtx_buffer.unlock();
  sig_buffer.notify_all();
}

void imu_cbk(const sensor_msgs::Imu::ConstPtr &msg_in) 
{
  sensor_msgs::Imu::Ptr msg(new sensor_msgs::Imu(*msg_in));

  double timestamp = msg->header.stamp.toSec();
  // ROS_DEBUG("get imu at time: %.6f", timestamp);

  mtx_buffer.lock();

  if (timestamp < last_timestamp_imu) {
    ROS_ERROR("imu loop back, clear buffer");
    imu_buffer.clear();
    b_reset = true;
  }
  last_timestamp_imu = timestamp;

  imu_buffer.push_back(msg);

  mtx_buffer.unlock();
  sig_buffer.notify_all();
}

bool SyncMeasure(MeasureGroup &measgroup) 
{
  if (lidar_buffer.empty() || imu_buffer.empty()) {
    /// Note: this will happen
    // ROS_INFO("NO IMU DATA");
    return false;
  }

  /* if (imu_buffer.front()->header.stamp.toSec() <
      lidar_buffer.back()->header.stamp.toSec()) 
  {
    lidar_buffer.clear();
    ROS_ERROR("clear lidar buffer, only happen at the beginning");
    return false;
  } */

  if (imu_buffer.back()->header.stamp.toSec() <
      lidar_buffer.front()->header.stamp.toSec()) 
  {
    return false;
  }

  /// Add lidar data, and pop from buffer
  measgroup.lidar = lidar_buffer.front();
  lidar_buffer.pop_front();
  pcl::PointCloud<PointType> v_pcl;
  pcl::fromROSMsg(*(measgroup.lidar), v_pcl);

  // double lidar_end_time = measgroup.lidar->header.stamp.toSec();
  double lidar_end_time = measgroup.lidar->header.stamp.toSec() + v_pcl.points.back().curvature / double(1000);

  /// Add imu data, and pop from buffer
  measgroup.imu.clear();
  int imu_cnt = 0;
  for (const auto &imu : imu_buffer)
  {
    double imu_time = imu->header.stamp.toSec();
    if (imu_time <= lidar_end_time) 
    {
      measgroup.imu.push_back(imu);
      imu_cnt++;
    }
  }

  for (int i = 0; i < imu_cnt; ++i)
  {
    imu_buffer.pop_front();
  }

  std::cout<<"imu_cnt: "<<imu_cnt<<" imu_end_time: "<<measgroup.imu.back()->header.stamp.toSec()<<"lidar_end_time"<<lidar_end_time<<std::endl;

  // ROS_DEBUG("add %d imu msg", imu_cnt);

  return true;
}

void ProcessLoop(std::shared_ptr<ImuProcess> p_imu)
{
  ROS_INFO("Start ProcessLoop");

  ros::Rate r(100);

  while (ros::ok())
  {
    MeasureGroup meas;
    std::unique_lock<std::mutex> lk(mtx_buffer);
    ROS_INFO("wait imu");
    sig_buffer.wait(lk,
                    [&meas]() -> bool { return SyncMeasure(meas) || b_exit; });
    lk.unlock();

    if (b_exit) {
      ROS_INFO("b_exit=true, exit");
      break;
    }

    if (b_reset) {
      ROS_WARN("reset when rosbag play back");
      p_imu->Reset();
      b_reset = false;
      continue;
    }
    p_imu->Process(meas);

    r.sleep();
  }
}

int main(int argc, char **argv)
{
  ros::init(argc, argv, "data_process");
  ros::NodeHandle nh;
  signal(SIGINT, SigHandle);

  ros::Subscriber sub_pcl = nh.subscribe("/laser_cloud_flat", 100, pointcloud_cbk);
  ros::Subscriber sub_imu = nh.subscribe("/livox/imu", 100, imu_cbk);

  std::shared_ptr<ImuProcess> p_imu(new ImuProcess());

  /// for debug
  p_imu->nh = nh;

  std::thread th_proc(ProcessLoop, p_imu);

  // ros::spin();
  ros::Rate r(1000);
  while (ros::ok()) {
    if (b_exit) break;

    ros::spinOnce();
    r.sleep();
  }

  ROS_INFO("Wait for process loop exit");
  if (th_proc.joinable()) th_proc.join();

  return 0;
}