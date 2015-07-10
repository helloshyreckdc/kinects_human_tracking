//pcl includes
#include <pcl_ros/point_cloud.h>
#include <pcl/point_types.h>
#include <pcl/io/openni_grabber.h>
#include <pcl_conversions/pcl_conversions.h>
#include <pcl/conversions.h>
#include <pcl/filters/voxel_grid.h>
#include <pcl/filters/conditional_removal.h>
#include <pcl/filters/statistical_outlier_removal.h>
#include <pcl/filters/radius_outlier_removal.h>
#include <pcl/filters/extract_indices.h>
#include <pcl/registration/transforms.h>
#include <pcl/io/pcd_io.h>
#include <pcl/kdtree/kdtree.h>
#include <pcl/search/kdtree.h>
#include <pcl/segmentation/extract_clusters.h>
#include <pcl/common/common_headers.h>
#include <pcl/sample_consensus/sac_model_plane.h>

//boost includes
#include <boost/lexical_cast.hpp>
#include <boost/accumulators/accumulators.hpp>
#include <boost/accumulators/statistics/stats.hpp>
#include <boost/accumulators/statistics/mean.hpp>
#include <boost/accumulators/statistics/stats.hpp>
#include <boost/accumulators/statistics/moment.hpp>
#include <boost/accumulators/statistics/max.hpp>
#include <boost/accumulators/statistics/min.hpp>
#include <boost/accumulators/statistics/median.hpp>
#include <boost/accumulators/statistics/variance.hpp>
#include <boost/timer.hpp>
#include <boost/graph/graph_concepts.hpp>

//ros-includes
#include <ros/ros.h>
#include <tf/transform_listener.h>
#include <tf_conversions/tf_eigen.h>
#include <geometry_msgs/PointStamped.h>
#include <message_filters/subscriber.h>
#include <message_filters/synchronizer.h>
#include <message_filters/time_synchronizer.h>
#include <message_filters/sync_policies/approximate_time.h>
#include <sensor_msgs/PointCloud.h>
#include <sensor_msgs/point_cloud_conversion.h>
#include <sensor_msgs/PointCloud2.h>
#include <pcl_ros/transforms.h>


#include <kinects_human_tracking/kalmanFilter.hpp>

/**
   Subscribe to a pointCloud and figure if there is 
   a human inside. Then track him using a Kalman filter
**/

using namespace std;

// typedefs and structs
typedef pcl::PointCloud<pcl::PointXYZRGB> PointCloudSM;
typedef sensor_msgs::PointCloud2 PCMsg;
typedef message_filters::sync_policies::ApproximateTime<PCMsg, PCMsg> MySyncPolicy;
typedef Eigen::Vector3f ClusterPoint;

struct ClusterStats{
  ClusterPoint mean;
  ClusterPoint var; 
  ClusterPoint min; 
  ClusterPoint max; 
  ClusterPoint median;
};
typedef struct ClusterStats ClusterStats;

struct ClippingRule{
  string axis; 	// x, y or z
  string op;	// GT or LT
  double val;
};
typedef struct ClippingRule ClippingRule;

// functions declaration
void callback(const PCMsg::ConstPtr& human_pc_msg, const PCMsg::ConstPtr& robot_pc_msg);

// Global variables
PointCloudSM::Ptr pcl_pc1, clustered_cloud;
pcl::PointCloud<pcl::PointXYZ>::Ptr robot_pc;
ros::Publisher human_pc_pub, pc_clustered_pub, cloud_mini_pt_pub, dist_pt_pub, human_pose_pub, human_pose_obs_pub;
tf::TransformListener *tf_listener;
double voxel_size_;
int min_cluster_size_;
Eigen::Vector2f last_human_pos_;
KalmanFilter kalman_;


// Templated functions declaration & definition
template<typename T> 
void pc_downsampling(boost::shared_ptr<pcl::PointCloud<T> >& pc_in, boost::shared_ptr<pcl::PointCloud<T> >& pc_out,double& voxel_size){
  
  pcl::VoxelGrid<T> vox_grid;
  vox_grid.setInputCloud(pc_in);
  vox_grid.setLeafSize(voxel_size,voxel_size,voxel_size);
  vox_grid.filter(*pc_out);
  
}

template<typename PointT> 
void pc_clipping(boost::shared_ptr<pcl::PointCloud<PointT> >& pc_in, std::vector<ClippingRule> clipping_rules , boost::shared_ptr<pcl::PointCloud<PointT> >& pc_out){
  
  typename pcl::ConditionAnd<PointT>::Ptr height_cond (new pcl::ConditionAnd<PointT> ());
  
  for(size_t i=0; i<clipping_rules.size(); i++){
  
    string axis = clipping_rules[i].axis;
    string op = clipping_rules[i].op;
    double val = clipping_rules[i].val;
    
    if ((axis == "x") || (axis == "y") || (axis == "z")){ 
      if ((op == "LT") || (op=="GT")){
	if (op == "LT")
	  height_cond->addComparison (typename pcl::FieldComparison<PointT>::ConstPtr (new pcl::FieldComparison<PointT> (axis, pcl::ComparisonOps::LT, val)));
	else
	  height_cond->addComparison (typename pcl::FieldComparison<PointT>::ConstPtr (new pcl::FieldComparison<PointT> (axis, pcl::ComparisonOps::GT, val)));
      }
      else{
      ROS_ERROR("Clipping rules not valid!!!\n Please use \'GT\' or \'LT\' for the op attribute");
      return;
      }
    }
    else{
      ROS_ERROR("Clipping rules not valid!!!\n Please use \'x\' \'y\' \'z\' for the axis");
      return;
    }
  }
  
  pcl::ConditionalRemoval<PointT> condrem (height_cond);
  condrem.setInputCloud (pc_in);
  condrem.setKeepOrganized(true);
  condrem.filter (*pc_out); 
  
}

template<typename PointT>
vector<pcl::PointIndices> pc_clustering(boost::shared_ptr<pcl::PointCloud<PointT> >& pc_in, double cluster_tolerance, boost::shared_ptr<pcl::PointCloud<PointT> >& clustered_pc){
  
  pcl::copyPointCloud(*pc_in, *clustered_pc);
  
  typename pcl::search::KdTree<PointT>::Ptr tree (new pcl::search::KdTree<PointT>);
  tree->setInputCloud (clustered_pc);
  
  std::vector<pcl::PointIndices> cluster_indices;
  typename pcl::EuclideanClusterExtraction<PointT> ec;
  ec.setClusterTolerance (cluster_tolerance);
  ec.setMinClusterSize (min_cluster_size_);
  ec.setSearchMethod (tree);
  ec.setInputCloud (clustered_pc);
  ec.extract (cluster_indices);
  
  return cluster_indices;
  
}

template<typename T> 
vector<ClusterStats> get_cluster_stats (boost::shared_ptr<pcl::PointCloud<T> >& pc, vector<pcl::PointIndices> clusters_indices){
  
  std::vector<ClusterStats> stats;
  
  for(int i = 0; i<clusters_indices.size(); i++){
    boost::accumulators::accumulator_set< float, boost::accumulators::stats<boost::accumulators::tag::mean, boost::accumulators::tag::variance, boost::accumulators::tag::min, boost::accumulators::tag::max, boost::accumulators::tag::median> > x_acc, y_acc, z_acc; 
    for(vector<int>::const_iterator pint = clusters_indices[i].indices.begin(); pint!=clusters_indices[i].indices.end(); ++pint){
      T p = pc->points[*pint];
      x_acc(p.x);
      y_acc(p.y);
      z_acc(p.z);
    }
    ClusterStats cluster_stats;
    cluster_stats.mean = ClusterPoint(boost::accumulators::mean(x_acc), boost::accumulators::mean(y_acc), boost::accumulators::mean(z_acc));
    cluster_stats.var = ClusterPoint(boost::accumulators::variance(x_acc), boost::accumulators::variance(y_acc), boost::accumulators::variance(z_acc));
    cluster_stats.min = ClusterPoint(boost::accumulators::min(x_acc), boost::accumulators::min(y_acc), boost::accumulators::min(z_acc));    
    cluster_stats.max = ClusterPoint(boost::accumulators::max(x_acc), boost::accumulators::max(y_acc), boost::accumulators::max(z_acc));    
    cluster_stats.median = ClusterPoint(boost::accumulators::median(x_acc), boost::accumulators::median(y_acc), boost::accumulators::median(z_acc));    
    
    stats.push_back(cluster_stats);
  } 
  return stats;
}

template<typename T> 
ClusterStats get_cluster_stats (boost::shared_ptr<pcl::PointCloud<T> >& pc){
  
  boost::accumulators::accumulator_set< float, boost::accumulators::stats<boost::accumulators::tag::mean, boost::accumulators::tag::variance, boost::accumulators::tag::min, boost::accumulators::tag::max, boost::accumulators::tag::median> > x_acc, y_acc, z_acc; 
  for(int i=0; i< pc->points.size();i++ ){
    T p = pc->points[i];
    x_acc(p.x);
    y_acc(p.y);
    z_acc(p.z);
  }
  ClusterStats cluster_stats;
  cluster_stats.mean = ClusterPoint(boost::accumulators::mean(x_acc), boost::accumulators::mean(y_acc), boost::accumulators::mean(z_acc));
  cluster_stats.var = ClusterPoint(boost::accumulators::variance(x_acc), boost::accumulators::variance(y_acc), boost::accumulators::variance(z_acc));
  cluster_stats.min = ClusterPoint(boost::accumulators::min(x_acc), boost::accumulators::min(y_acc), boost::accumulators::min(z_acc));    
  cluster_stats.max = ClusterPoint(boost::accumulators::max(x_acc), boost::accumulators::max(y_acc), boost::accumulators::max(z_acc));    
  cluster_stats.median = ClusterPoint(boost::accumulators::median(x_acc), boost::accumulators::median(y_acc), boost::accumulators::median(z_acc));    
    
  return cluster_stats;
}

template<typename T, typename T2> 
void pc_to_pc_min_dist(boost::shared_ptr<pcl::PointCloud<T> >& pc1, boost::shared_ptr<pcl::PointCloud<T2> >& pc2, double& min_dist, geometry_msgs::PointStamped& pc1_pt_min, geometry_msgs::PointStamped& pc2_pt_min){
  
  min_dist = 100000000;
  float dist = 0; 
  int min_idx = 0;
  int min_jdx = 0;
  for (size_t i = 0; i < pc1->points.size (); ++i){
    for (size_t j = 0; j < pc2->points.size (); ++j){  
      pcl::Vector4fMap pt1 = pc1->points[i].getVector4fMap();
      pcl::Vector4fMap pt2 = pc2->points[j].getVector4fMap();
      dist = (pt2 - pt1).norm ();
      if (dist < min_dist){
	min_idx = i;
	min_jdx = j;
	min_dist = dist;
      }
    }
  }
  
  pc1_pt_min.header.frame_id = pc1->header.frame_id;
  pc1_pt_min.point.x = pc1->points[min_idx].x;
  pc1_pt_min.point.y = pc1->points[min_idx].y;
  pc1_pt_min.point.z = pc1->points[min_idx].z; 
  
  pc2_pt_min.header.frame_id = clustered_cloud->header.frame_id;
  pc2_pt_min.point.x = pc2->points[min_jdx].x;
  pc2_pt_min.point.y = pc2->points[min_jdx].y;
  pc2_pt_min.point.z = pc2->points[min_jdx].z;
  
}