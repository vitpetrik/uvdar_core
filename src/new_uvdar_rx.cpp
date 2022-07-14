#include <ros/ros.h>
#include <ros/package.h>
#include <mrs_lib/param_loader.h>
#include <cv_bridge/cv_bridge.h>
#include <mrs_msgs/ImagePointsWithFloatStamped.h>
#include <mrs_lib/transformer.h>
#include <mrs_msgs/String.h>
#include <std_msgs/Float32.h>
#include <uvdar_core/RecMsg.h>
#include <string>
#include <cmath>
#include <mutex>

#include <ht4dbt/ht4d.h>

#define SCFFE 30
#define MAX_CLUSTER 100
#define CHANNEL_OFF 20
#define SB 15
#define MAX_FRAME_SIZE 60
#define MIN_FRAME_SIZE 30
#define SIGNAL_LEN 100

std::string    uav_name;
std::string    recieved_topic;
int            uav_id = 0;
ros::Publisher pub_rec_msg;
// int            scffe = 30;  // samples count for framerate estimate

std::vector<int> points_loaded;

/* float                       frequencies[] = {10, 15, 30, 5}; */
std::vector<std::string>    points_seen_topics;
std::vector<std::string>    blinkers_seen_topics;
std::vector<std::string>    estimated_framerate_topics;
std::vector<ros::Publisher> pub_blinkers_seen;
std::vector<ros::Publisher> pub_estimated_framerate;
using points_seen_callback = boost::function<void(const mrs_msgs::ImagePointsWithFloatStampedConstPtr&)>;
std::vector<points_seen_callback> callbacks_points_seen;
std::vector<ros::Subscriber>      subscribers_points_seen;

std::vector<std::shared_ptr<uvdar::HT4DBlinkerTracker>> ht4dbt_trackers_;

int _accumulator_length_ = 15;
int _sig_step_ = _accumulator_length_ / 5;
/* int _accumulator_length_ = 24; */
/* int _sig_step_ = _accumulator_length_ / 6; */
int _pitch_steps_ = 16;
int _yaw_steps_ = 16;
int _max_pixel_shift_ = 4;
int _nullify_radius_ = 5;
int _reasonable_radius_ = 6;

bool _debug_ = false;
bool _visual_debug_ = false;

namespace RX
{

class RX_processor {
public:
  RX_processor(ros::NodeHandle& nh) {
    mrs_lib::ParamLoader param_loader(nh, "UVDARrx");

    param_loader.loadParam("uav_name", uav_name);
    param_loader.loadParam("uav_id", uav_id);
    param_loader.loadParam("recieved_topic", recieved_topic);
    param_loader.loadParam("points_seen_topics", points_seen_topics, points_seen_topics);
    param_loader.loadParam("blinkers_seen_topics", blinkers_seen_topics, blinkers_seen_topics);
    param_loader.loadParam("estimated_framerate_topics", estimated_framerate_topics, estimated_framerate_topics);
    if (points_seen_topics.empty()) {
      ROS_WARN("[RX_processor]: No topics of points_seen_topics were supplied. Returning.");
      return;
    }

    // make publishers for viktors pose calculator and filter
    for (size_t i = 0; i < blinkers_seen_topics.size(); ++i) {
      pub_blinkers_seen.push_back(nh.advertise<mrs_msgs::ImagePointsWithFloatStamped>(blinkers_seen_topics[i], 1));
    }
    for (size_t i = 0; i < estimated_framerate_topics.size(); ++i) {
      pub_estimated_framerate.push_back(nh.advertise<std_msgs::Float32>(estimated_framerate_topics[i], 1));
    }

    pub_rec_msg = nh.advertise<uvdar_core::RecMsg>(recieved_topic, 1);  // to publish decoded msgs for uvdar node

    std::vector<std::vector<PointSeen>> tmp;
    std::vector<RecSignal> init_signal; 
    
    ROS_WARN("Not initialized yet");

    for (int i = 0; i < (int)points_seen_topics.size(); ++i) {
      recieved_signals.push_back(init_signal);
      point_seen.push_back(tmp);  // initialize points_seen vector for number of camera used
      ROS_INFO("[RX_Processor]: Added camera %d on topic %s", i, points_seen_topics[i].c_str());
      CamInfo ci_new;
      ci_new.cam_id = i;
      cam_info.push_back(ci_new);
      // callback of individual image frames
      points_seen_callback callback = [i, this](const mrs_msgs::ImagePointsWithFloatStampedConstPtr& pointsMessage) { VisiblePoints(pointsMessage, i); };
      callbacks_points_seen.push_back(callback);
      subscribers_points_seen.push_back(nh.subscribe(points_seen_topics[i], 1, callbacks_points_seen[i]));
      
      ht4dbt_trackers_.push_back(std::make_shared<uvdar::HT4DBlinkerTracker>(_accumulator_length_, _pitch_steps_, _yaw_steps_, _max_pixel_shift_, cv::Size(0, 0), _nullify_radius_, _reasonable_radius_));
      ht4dbt_trackers_.back()->setDebug(_debug_, _visual_debug_);
      
      signal_data_.push_back(SignalData());
      /* SignalData sd_new; */
      /* sd_new.retrieved_blinkers.push_back({cv::Point2d(0,0),1}); */
      /* signal_data_.push_back(sd_new); */

      points_loaded.push_back(0);
    }

    ROS_INFO("Node initialized");
  }

private:

  static int abs_function (int x, int y) {return x+abs(y);}
  
  int DataFrameCheck(std::vector<int>& received_msg_corrected) {
    int rmc_size = received_msg_corrected.size();

    for (int bs = (rmc_size - 1); bs >= 3; bs--) {  // check of Bit Stuffing and BS bits separation
      if (received_msg_corrected[bs - 1] == received_msg_corrected[bs - 2] && received_msg_corrected[bs - 2] == received_msg_corrected[bs - 3]) {
        if (received_msg_corrected[bs - 1] != received_msg_corrected[bs]) {
          received_msg_corrected.erase(received_msg_corrected.begin() + bs);
          // bs += 2;
          rmc_size--;
        } else {
          // std::cout << " BS fail |";
          return 1;
          break;
        }
      }
    }

    received_msg_corrected.erase(received_msg_corrected.begin());  // separation of the rest of SOF needed for BS check
    rmc_size--;
    if ((rmc_size != 8) && (rmc_size != 9) && (rmc_size != 13)) {  // check of frame length
      // std::cout << " FL fail " << rmc_size << " |";
      return 1;
    }

    int parity_check = 0;

    for (auto& cnt : received_msg_corrected) {  // parity check
      if (cnt > 0)
        parity_check++;
    }

    if (parity_check % 2 != 1) {
      // std::cout << " PA fail |";
      return 1;
    }

    return 0;
  }

  float transferValues(int pl_value, int pl_size, int pl_index, int pl_type) {
    // float real_value = transferValues(tmp_pl, payload_size, pl, rec_dtype);
    float payload          = -1.0;
    float lu_table_1[1][4] = {{0.1, 22.5, -1.0, -1.0}};                            //[pl_index][pl_type]
    float lu_table_2[2][4] = {{22.5, -1.0, -1.0, -1.0}, {0.1, -1.0, -1.0, -1.0}};  //[pl_index][pl_type]

    switch (pl_size) {
      case 1:
        payload = (float)pl_value * lu_table_1[pl_index][pl_type];
        break;
      case 2:
        payload = (float)pl_value * lu_table_2[pl_index][pl_type];
        break;
      default:
        ROS_ERROR("Unexpected payload size");
        break;
    }
    return payload;
  }

  void VisiblePoints(const mrs_msgs::ImagePointsWithFloatStampedConstPtr& points_seen_msg, size_t camera_index) {
    /*
     *Estimation and publishing of camera framerate
     * */
    if (cam_info[camera_index].samples == 0)
      cam_info[camera_index].last_stamp = points_seen_msg->stamp;
    cam_info[camera_index].samples += 1;
    if (cam_info[camera_index].samples == SCFFE) {
      cam_info[camera_index].framerate = (SCFFE - 1) / (points_seen_msg->stamp.toSec() - cam_info[camera_index].last_stamp.toSec());
      cam_info[camera_index].samples   = 0;
      
      ht4dbt_trackers_[camera_index]->updateFramerate(cam_info[camera_index].framerate);

      std_msgs::Float32 msgFramerate;
      msgFramerate.data = cam_info[camera_index].framerate;
      pub_estimated_framerate[camera_index].publish(msgFramerate);
    }





    /*
     *Expand clusters with a new camera frame
     * */
    int cluster_count = point_seen[camera_index].size();  // count of point cluster currently in point_seen

    for (int i = 0; i < cluster_count; i++) {  // add new blank record for each cluster in new camera frame
      PointSeen tmp_ps;
      tmp_ps.decoded            = point_seen[camera_index][i].back().decoded;
      tmp_ps.id                 = point_seen[camera_index][i].back().id;
      /* tmp_ps.frequency          = point_seen[camera_index][i].back().frequency; */
      tmp_ps.cnt_last_published = point_seen[camera_index][i].back().cnt_last_published + 1;
      tmp_ps.position           = cv::Point2i(point_seen[camera_index][i].back().position.x, point_seen[camera_index][i].back().position.y);
      int last_val_SOF          = point_seen[camera_index][i].back().start_frame_index;
      if (last_val_SOF >= 0)
        tmp_ps.start_frame_index = ++last_val_SOF;  // incrementing of SOF index for frame detection
      tmp_ps.count       = 0;
      tmp_ps.sample_time = points_seen_msg->stamp;
      point_seen[camera_index][i].push_back(tmp_ps);

      if (point_seen[camera_index][i].size() > MAX_CLUSTER) {  // if there are more records than 100 in one cluster, pop the oldest one
        point_seen[camera_index][i].erase(point_seen[camera_index][i].begin());
      }

      for (int j = 0; j < CHANNEL_OFF; j++) {  // if leds in cluster were not visible in last 20 frames, remove the cluster
        if (point_seen[camera_index][i][point_seen[camera_index][i].size() - 1 - j].count > 1)
          break;
        if (j == (CHANNEL_OFF - 1)) {
          point_seen[camera_index].erase(point_seen[camera_index].begin() + i);
          cluster_count--;
          if (cluster_count > 0)
            i--;
        }
      }
    }




    /*
     *Process new points
     * */
    std::vector<cv::Point2i> new_points;
    for (auto& point : points_seen_msg->points) {
      new_points.push_back(cv::Point2d(point.x, point.y));
      // if there is no cluster in the current camera frame, add point into new cluster and add this cluster into camera container
      if (point_seen[camera_index].empty()) {
        std::vector<PointSeen> tmp_vec;
        PointSeen              tmp_ps;
        tmp_ps.position = cv::Point2i(point.x, point.y);
        tmp_ps.positions.push_back(cv::Point2i(point.x, point.y));
        tmp_ps.count       = 1;
        tmp_ps.sample_time = points_seen_msg->stamp;
        tmp_vec.push_back(tmp_ps);
        point_seen[camera_index].push_back(tmp_vec);
        continue;
      }

      // if the new point is close to one of old clusters, add it to this cluser. Will be replaced by nearest neighbor in the future
      bool point_added = false;

      int closest_id   = 0;
      int closest_dist = -1;

      // std::cout << "Camera: " << camera_index << "- ";

      int cluster_count = point_seen[camera_index].size();
      for (int i = 0; i < cluster_count; i++) {
        // int dfc = sqrt(pow(point_seen[camera_index][i].back().position.x - point.x, 2) +
        //                       pow(point_seen[camera_index][i].back().position.y - point.y, 2));  // distance from cluster

        int go_back_n = 0;
        while (point_seen[camera_index][i].rbegin()[go_back_n].count <= 1 && (int)point_seen[camera_index][i].size() > go_back_n + 1) {
          go_back_n++;
        }

        for (auto& pnt : point_seen[camera_index][i].rbegin()[go_back_n].positions) {
          int actual_dist = abs(point.x - pnt.x) + abs(point.y - pnt.y);
          // std::cout << actual_dist << "x" << i << " ";
          if (closest_dist == -1) {
            closest_dist = actual_dist;
            closest_id   = i;
          }
          if (closest_dist > actual_dist) {
            closest_dist = actual_dist;
            closest_id   = i;
          }
        }
      }

      // std::cout << "- " << closest_dist << std::endl;

      if (closest_dist < 35) {
        point_seen[camera_index][closest_id].back().positions.push_back(cv::Point2i(point.x, point.y));
        point_seen[camera_index][closest_id].back().count += 1;
        point_added = true;
        // ROS_INFO("My pos: %f, %f. Cl id: %d, dist %d ", point.x, point.y, closest_id, closest_dist);
      } else {
        point_added = false;
      }

      // if the point was too far from each cluster and it was not added, make a new cluster
      if (!point_added) {
        std::vector<PointSeen> tmp_vec;
        PointSeen              tmp_ps;
        tmp_ps.position = cv::Point2i(point.x, point.y);
        tmp_ps.positions.push_back(cv::Point2i(point.x, point.y));
        tmp_ps.count       = 1;
        tmp_ps.sample_time = points_seen_msg->stamp;
        tmp_vec.push_back(tmp_ps);
        point_seen[camera_index].push_back(tmp_vec);
        continue;
      }
    }
   
    /* update resolution */   
    if(cam_info[camera_index].im_size.width <= 0 || cam_info[camera_index].im_size.height <= 0){
      cam_info[camera_index].im_size.height = points_seen_msg->image_height;
      cam_info[camera_index].im_size.width = points_seen_msg->image_width;
      ht4dbt_trackers_[camera_index]->updateResolution(cam_info[camera_index].im_size);
    }
    
    ht4dbt_trackers_[camera_index]->insertFrame(new_points);
    points_loaded[camera_index]=points_loaded[camera_index]+1;
      
    bool init_signals = false;

    if(recieved_signals[camera_index].empty()){
      init_signals = true; 
    }

    int step_ = _sig_step_;
    if(init_signals) step_ = _accumulator_length_;

    if(points_loaded[camera_index] >= step_){
    /* if(points_loaded[camera_index] >= _accumulator_length_){ */
      /* ROS_ERROR("Orig points %d", (int)new_points.size()); */
      points_loaded[camera_index] = 0;
      {
        std::scoped_lock lock(*(signal_data_[camera_index].mutex_retrieved_blinkers));
        signal_data_[camera_index].retrieved_blinkers = ht4dbt_trackers_[camera_index]->getResults();
      }

      std::vector<int> rec_signal;
   
      int retr_size = (int)signal_data_[camera_index].retrieved_blinkers.size();
      
      for(int i = 0; i < (int)recieved_signals[camera_index].size(); i++){
        recieved_signals[camera_index][i].updated = false;
      }
      
      for (int i = 0; i<retr_size; i++) {
        rec_signal = ht4dbt_trackers_[camera_index]->getSignal(i);
        
        bool added = false;

        RecSignal new_signal;
        new_signal.signal_ = rec_signal;
        new_signal.updated = true;
        new_signal.position = signal_data_[camera_index].retrieved_blinkers[i].first;
   
        if(init_signals){
          recieved_signals[camera_index].push_back(new_signal);
          ROS_ERROR("init");
          continue;
        }

        for(int i = 0; i < (int)recieved_signals[camera_index].size(); i++){
          if(recieved_signals[camera_index][i].updated) continue;
          double d2origin = sqrt((new_signal.position.x - recieved_signals[camera_index][i].position.x)^(1/2) + (new_signal.position.y - recieved_signals[camera_index][i].position.y)^(1/2));
          if(d2origin > 100) continue;

          std::vector<int> signal_local = {new_signal.signal_.begin() + _sig_step_, new_signal.signal_.end()};
          std::transform(signal_local.begin(), signal_local.end(), recieved_signals[camera_index][i].signal_.begin(), signal_local.begin(), std::minus<int>()); 
          int correl = std::accumulate(signal_local.begin(), signal_local.end(), 0, abs_function);
          /* ROS_WARN("xx"); */
          /* for(int j = 0; j < (int)new_signal.signal_.size(); j++){ */
          /*   std::cout << recieved_signals[camera_index][i].signal_[j]; */
          /* } */
          /*   std::cout << std::endl; */          
          if(correl <= 3){
            added = true;
            recieved_signals[camera_index][i].updated = true;
            recieved_signals[camera_index][i].position = new_signal.position;
            for(int j = _sig_step_-1; j >= 0; j--){
              recieved_signals[camera_index][i].signal_.insert(recieved_signals[camera_index][i].signal_.begin(), new_signal.signal_[j]);
            }
            break;
          }
          /* std::cout << correl; */ 
          /* signal_local = {new_signal.signal_.begin() + _sig_step_, new_signal.signal_.end()}; */
          /* std::cout << correl << std::endl; */          
          /* for(int j = 0; j < (int)new_signal.signal_.size(); j++){ */
          /*   std::cout << new_signal.signal_[j]; */
          /* } */
          /*   std::cout << std::endl; */          
        }
          /* std::cout << std::endl; */ 
        
        if(!added){
          ROS_ERROR("added");
          recieved_signals[camera_index].push_back(new_signal);
          continue;
        }

        for(int j = 0; j<(int)rec_signal.size(); j++){
          /* std::cout << rec_signal[j]; */
        }
        /* std::cout << "a"; */
        /* std::cout << std::endl; */
      }
      
      for(int i = 0; i < (int)recieved_signals[camera_index].size(); i++){
        if(!recieved_signals[camera_index][i].updated){
          recieved_signals[camera_index].erase(recieved_signals[camera_index].begin() + i);
          ROS_WARN("signal deleted");
        }
      
        if((int)recieved_signals[camera_index][i].signal_.size() > SIGNAL_LEN){
          int ex_b = (int)recieved_signals[camera_index][i].signal_.size() - SIGNAL_LEN;
          recieved_signals[camera_index][i].signal_.erase(recieved_signals[camera_index][i].signal_.end() - ex_b, recieved_signals[camera_index][i].signal_.end());
        }
   
        int sb_min = 12;
        int min_frame = 45;
        std::vector<int> sof_mask(sb_min, 1);
        std::vector<int> eof_mask(sb_min, 1);
        sof_mask.insert(sof_mask.begin(), 0);
        eof_mask.push_back(0);
        
        for(int j = 0; j < _sig_step_; j++){
          std::vector<int> eof_mask_local = eof_mask;
          std::transform(eof_mask_local.begin(), eof_mask_local.end(), recieved_signals[camera_index][i].signal_.begin() + j, eof_mask_local.begin(), std::minus<int>()); 
          int eof_correl = std::accumulate(eof_mask_local.begin(), eof_mask_local.end(), 0, abs_function);
          /* ROS_WARN("correl %d", eof_correl); */
          if(eof_correl == 0){
            /* ROS_INFO("EOF found"); */
            for(int k = min_frame; k < (int)recieved_signals[camera_index][i].signal_.size(); k++){
              std::vector<int> sof_mask_local = sof_mask;
              std::transform(sof_mask_local.begin(), sof_mask_local.end(), recieved_signals[camera_index][i].signal_.begin() + k, sof_mask_local.begin(), std::minus<int>()); 
              int sof_correl = std::accumulate(sof_mask_local.begin(), sof_mask_local.end(), 0, abs_function);
                if(sof_correl == 0){
            
                  /* ROS_INFO("SOF found %d, %d", j, k); */
                  /* for(int l = 0; l < (int)recieved_signals[camera_index][i].signal_.size(); l++){ */
                  /*   std::cout << recieved_signals[camera_index][i].signal_[l]; */ 
                  /* } */
                  /* std::cout << std::endl; */
      
                  std::vector<int> msg_raw = {recieved_signals[camera_index][i].signal_.begin() + j + sb_min, recieved_signals[camera_index][i].signal_.begin() + k + 1};
                  /* std::vector<int> msg_raw = {recieved_signals[camera_index][i].signal_.begin() + 3, recieved_signals[camera_index][i].signal_.end() - 3}; */
                  /* std::vector<int> msg_raw; */ 
                  /* copy(recieved_signals[camera_index][i].signal_.begin(), recieved_signals[camera_index][i].signal_.end(), back_inserter(msg_raw)); */
                  /* copy(recieved_signals[camera_index][i].signal_.begin(), recieved_signals[camera_index][i].signal_.end(), msg_raw); */

                  /* for(int l = 0; l < (int)msg_raw.size(); l++){ */
                  /*   std::cout << msg_raw[l]; */
                  /* } */
                  /* std::cout << std::endl; */

                  while (msg_raw.front() == 0) {  // start synchronize, the second part of SOF (bit 1) remains for BS check
                    msg_raw.erase(msg_raw.begin());
                  }

                  while (msg_raw.back() == 0) {  // end synchronization
                    msg_raw.pop_back();
                  }
                
                  std::reverse(msg_raw.begin(), msg_raw.end());

                  /* for(int l = 0; l < (int)msg_raw.size(); l++){ */
                  /*   std::cout << msg_raw[l]; */
                  /* } */
                  /* std::cout << std::endl; */
      
                  std::vector<std::vector<int>> sub_frames;

                  std::vector<int> sub_frame;

                  int curr_bit;

                  while (!msg_raw.empty()) {
                    curr_bit = msg_raw.back();
                    while (msg_raw.back() == curr_bit && !msg_raw.empty()) {
                      sub_frame.push_back(curr_bit);
                      msg_raw.pop_back();
                    }
                    sub_frames.push_back(sub_frame);
                    sub_frame.clear();
                  }

                  for (auto& frame : sub_frames) {
                    int extra_bits = (int)frame.size() % 3;
                    switch (extra_bits) {
                      case 1:
                        if (frame.size() == 1) {
                        // frame.index
                          size_t front_index = &frame - &sub_frames.front();
                          size_t back_index  = &sub_frames.back() - &frame;
                          if (front_index == 0 || back_index == 0) {
                            frame.push_back(frame.front());
                            frame.push_back(frame.front());
                          } else if ((int)sub_frames[front_index - 1].size() > 2 || (int)sub_frames[front_index + 1].size() > 2) {
                            frame.push_back(frame.front());
                            frame.push_back(frame.front());
                          } else {
                            frame.pop_back();
                          }

                        } else {
                            frame.pop_back();
                        }
                        break;
                      case 2:
                        frame.push_back(frame.front());
                        break;
                      default:
                        // ROS_INFO("Ok");
                        break;
                    }
                  }

                  msg_raw.clear();

                  while (!sub_frames.empty()) {
                    int bit_cnt = sub_frames.back().size() / 3;
                    if (bit_cnt > 3) {
                      bit_cnt = 3;
                      // ROS_INFO("Correction from hodne to 3");
                    }
                    for (int p = 0; p < bit_cnt; p++) {
                      msg_raw.push_back(sub_frames.back().back());
                    }
                    sub_frames.pop_back();
                  }
                  msg_raw.pop_back();
                  
                  for(int l = 0; l < (int)msg_raw.size(); l++){
                    std::cout << msg_raw[l];
                  }
                  std::cout << std::endl;
                  
                  break;
                }
            }
            break;
          }
        }
      }

      /* std::vector<int> r_frame; */

      /* std::cout << std::endl; */
      /* for(int i = 0; i < (int)recieved_signals[camera_index].size(); i++){ */
      /*   /1* std::cout << recieved_signals[camera_index][i].position; *1/ */
      /*   /1* std::cout << " | "; *1/ */
      /*   for(int j = 0; j < (int)recieved_signals[camera_index][i].signal_.size(); j++){ */
      /*     std::cout << recieved_signals[camera_index][i].signal_[j]; */
      /*   } */
      /*   std::cout << std::endl; */
      /* } */
    }

    




    /*
     * detection of SOF and EOF and decoding follow*
     * */
    cluster_count = point_seen[camera_index].size();
    for (int i = 0; i < cluster_count; i++) {
      if (point_seen[camera_index][i].size() < (MAX_CLUSTER / 2))
        continue;  // we have stable channel connection
      // publishing on blinkers seen topic
      if (point_seen[camera_index][i].back().decoded) {
        if (!point_seen[camera_index][i].back().positions.empty()) {
          if (point_seen[camera_index][i].back().cnt_last_published >= 5) {
            mrs_msgs::ImagePointsWithFloatStamped msg;
            /* msg.stamp        = point_seen[camera_index][i].back().sample_time; */
            msg.stamp        = ros::Time::now(); 
            msg.image_width  = points_seen_msg->image_width;
            msg.image_height = points_seen_msg->image_height;

            for (auto& blinker : point_seen[camera_index][i].back().positions) {
              mrs_msgs::Point2DWithFloat point;
              point.x     = blinker.x;
              point.y     = blinker.y;
              point.value = point_seen[camera_index][i].back().id;
              msg.points.push_back(point);
            }

            pub_blinkers_seen[camera_index].publish(msg);

            point_seen[camera_index][i].back().cnt_last_published = 0;
          }
        }
      }


      /* for(auto& bit : point_seen[camera_index][i]){ */
      /*   std::cout << bit.count; */
      /* } */
      /* std::cout << std::endl; */


      /*
       * SOF and EOF detection
       * */

      if (point_seen[camera_index][i].rbegin()[0].count == 0 && point_seen[camera_index][i].rbegin()[1].count) {  // possible start of frame detection
        for (int j = 1; j < (SB + 1); j++) {
          if (point_seen[camera_index][i].rbegin()[j].count == 0)
            break;
          if (j == SB) {  // SOF validation
            // ROS_INFO("SOF");
            point_seen[camera_index][i].back().start_frame_index = 0;
          }
        }
      }

      int j = 0;
      if (point_seen[camera_index][i].rbegin()[SB].count == 0 && point_seen[camera_index][i].rbegin()[SB + 1].count == 0) {  // possible end of frame detection
        for (j = 0; j < SB; j++) {
          if (point_seen[camera_index][i].rbegin()[j].count == 0)
            break;
        }
      }

      if (j != (SB - 1))
        continue;  // EOF validation

      int sof = point_seen[camera_index][i].back().start_frame_index;

      if (sof < SB || sof >= (int)point_seen[camera_index][i].size()-1)
        continue;

      // ROS_INFO("EOF");

      std::vector<PointSeen> received_msg;

      std::copy(point_seen[camera_index][i].rbegin() + SB - 1, point_seen[camera_index][i].rbegin() + sof + 1,
                back_inserter(received_msg));  // cut of the frame in range of SOF-EOF
      std::reverse(received_msg.begin(), received_msg.end());

      int rmr_size = received_msg.size();

      if ((rmr_size > MAX_FRAME_SIZE) || (rmr_size < MIN_FRAME_SIZE))
        continue;



      /*
       *Cleaning of received msg
       */
      std::vector<int> received_msg_raw;
      for (auto& bits : received_msg) {
        int bit_val = bits.count;
        if (bit_val < 2) {
          received_msg_raw.push_back(0);
        } else {
          received_msg_raw.push_back(1);
        }
      }

      while (received_msg_raw.front() == 0) {  // start synchronize, the second part of SOF (bit 1) remains for BS check
        received_msg_raw.erase(received_msg_raw.begin());
      }

      while (received_msg_raw.back() == 0) {  // end synchronization
        received_msg_raw.pop_back();
      }



      /*
       * Bit corrections
       */

      std::vector<std::vector<int>> sub_frames;

      std::vector<int> sub_frame;

      int curr_bit;

      while (!received_msg_raw.empty()) {
        curr_bit = received_msg_raw.back();
        while (received_msg_raw.back() == curr_bit && !received_msg_raw.empty()) {
          sub_frame.push_back(curr_bit);
          received_msg_raw.pop_back();
        }
        sub_frames.push_back(sub_frame);
        sub_frame.clear();
      }

      for (auto& frame : sub_frames) {
        int extra_bits = (int)frame.size() % 3;
        switch (extra_bits) {
          case 1:
            if (frame.size() == 1) {
              // frame.index
              size_t front_index = &frame - &sub_frames.front();
              size_t back_index  = &sub_frames.back() - &frame;
              if (front_index == 0 || back_index == 0) {
                frame.push_back(frame.front());
                frame.push_back(frame.front());
              } else if ((int)sub_frames[front_index - 1].size() > 2 || (int)sub_frames[front_index + 1].size() > 2) {
                frame.push_back(frame.front());
                frame.push_back(frame.front());
              } else {
                frame.pop_back();
              }

            } else {
              frame.pop_back();
            }
            break;
          case 2:
            frame.push_back(frame.front());
            break;
          default:
            // ROS_INFO("Ok");
            break;
        }
      }

      received_msg_raw.clear();

      while (!sub_frames.empty()) {
        int bit_cnt = sub_frames.back().size() / 3;
        if (bit_cnt > 3) {
          bit_cnt = 3;
          // ROS_INFO("Correction from hodne to 3");
        }
        for (int p = 0; p < bit_cnt; p++) {
          received_msg_raw.push_back(sub_frames.back().back());
        }
        sub_frames.pop_back();
      }
      received_msg_raw.pop_back();
                  
      /* for(int l = 0; l < (int)received_msg_raw.size(); l++){ */
      /*   std::cout << received_msg_raw[l]; */
      /* } */
      /* std::cout << " ej"; */
      /* std::cout << std::endl; */

      int faults;
      faults = DataFrameCheck(received_msg_raw);  // verify if the data frame is correct

      if (faults != 0) {  // if both corrections were not succesful, ignore received frame
        ROS_WARN("Not able to decode msg");
        received_msg_raw.clear();
        continue;
      }

      /*
       * Validations and corrections are OK, decode data frame
       *
       * Predelat vse odsud dale
       *
       * */
      uvdar_core::RecMsg rm_pub;

      int rec_id = 2 * received_msg_raw[0] + received_msg_raw[1];
      if (rec_id == uav_id) {
        ROS_ERROR("My ID %d, redirecting to ID: %d", rec_id, point_seen[camera_index][i].back().id);
        rec_id = point_seen[camera_index][i].back().id;
      }
      if (point_seen[camera_index][i].back().id < 0)
        point_seen[camera_index][i].back().id = rec_id;
      if (point_seen[camera_index][i].back().id != rec_id) {
        // ROS_ERROR("Ignoring msg, bad ID %d", rec_id);
        ROS_ERROR("Bad ID %d, redirecting to ID: %d", rec_id, point_seen[camera_index][i].back().id);
        // continue;
        rec_id = point_seen[camera_index][i].back().id;
      }
      rm_pub.uav_id = rec_id;
      float              rec_heading;
      int                rec_dtype;
      int                rmc_size = received_msg_raw.size();
      std::vector<float> payload;
      if (rmc_size == 8) {
        rec_heading        = 22.5 * (8 * received_msg_raw[3] + 4 * received_msg_raw[4] + 2 * received_msg_raw[5] + received_msg_raw[6]);
        rm_pub.pl_carrying = false;
        rm_pub.heading     = rec_heading;
        if(received_msg_raw[2]==1){
          rm_pub.msg_type = 1;
        }
        else{
          rm_pub.msg_type = 0;
        }
        /* ROS_INFO("Recieved id: %d, hd: %f, vis: %d", rec_id, rec_heading, rm_pub.msg_type); */
      } else {
        int payload_size = (rmc_size - 5) / 4;
        rec_dtype        = 2 * received_msg_raw[2] + received_msg_raw[3];
        if (payload_size > 2 || payload_size < 0) {
          ROS_ERROR("Ignoring msg, bad payload size %d", payload_size);
          continue;
        }
        if (rec_dtype > 3 || rec_dtype < 0) {
          ROS_ERROR("Ignoring msg, bad msg type %d", rec_dtype);
          continue;
        }
        bool valid_data = true;
        for (int pl = 0; pl < payload_size; pl++) {
          int tmp_pl = (8 * received_msg_raw[4 + 4 * pl] + 4 * received_msg_raw[5 + 4 * pl] + 2 * received_msg_raw[6 + 4 * pl] + received_msg_raw[7 + 4 * pl]);
          float real_value = transferValues(tmp_pl, payload_size, pl, rec_dtype);
          if (real_value < 0)
            valid_data = false;
          payload.push_back(real_value);
        }
        if (!valid_data)
          continue;
        rm_pub.payload     = payload;
        rm_pub.pl_carrying = true;
        rm_pub.msg_type    = rec_dtype;
        /* ROS_INFO("Msg from UAV id: %d, msg_type: %d, payload: %f", rec_id, rec_dtype, payload[0]); */
      }

      point_seen[camera_index][i].back().decoded   = true;
      /* point_seen[camera_index][i].back().frequency = frequencies[rec_id]; */

      // publish decoded message
      pub_rec_msg.publish(rm_pub);

      received_msg.clear();
      received_msg_raw.clear();
    }
  }
  
  struct SignalData
  {
    std::vector<std::pair<cv::Point2d,int>>      retrieved_blinkers;
    std::shared_ptr<std::mutex>   mutex_retrieved_blinkers;
    
    SignalData(){mutex_retrieved_blinkers = std::make_shared<std::mutex>();}
    ~SignalData(){mutex_retrieved_blinkers.reset();}
  };

  struct RecSignal
  {
    cv::Point2i position;
    bool updated;
    std::vector<int> signal_;
  };

  struct CamInfo
  {
    bool      init      = false;
    int       cam_id    = 0;
    double    framerate = 80.0;
    ros::Time last_stamp;
    int       samples = 0;
    
    cv::Size im_size = cv::Size(-1,-1);
  };

  struct PointSeen
  {
    bool                     decoded            = false;
    int                      id                 = -1;
    /* double                   frequency          = 0; */
    int                      cnt_last_published = 0;
    std::vector<cv::Point2i> positions;

    cv::Point2i position;                // position in the camera frame
    int         count;                   // number of visible leds in current frame
    ros::Time   sample_time;             // time of current frame
    int         start_frame_index = -1;  // marker of SOF
  };
  std::vector<std::vector<std::vector<PointSeen>>> point_seen;  // ith camera, jth cluster, kth time of visible point
  std::vector<CamInfo>                             cam_info;
  std::vector<SignalData>                         signal_data_;
  std::vector<std::vector<RecSignal>> recieved_signals;
};
}  // namespace RX
int main(int argc, char** argv) {
  ros::init(argc, argv, "UVDARrx");
  ros::NodeHandle  nh("~");
  RX::RX_processor rxko(nh);
  ROS_INFO("[RX_processor] Node initialized");
  ros::spin();
  return 0;
}
