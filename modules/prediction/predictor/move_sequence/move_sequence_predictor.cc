/******************************************************************************
 * Copyright 2017 The Apollo Authors. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *****************************************************************************/

#include "modules/prediction/predictor/move_sequence/move_sequence_predictor.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <utility>

#include "Eigen/Dense"
#include "modules/common/adapters/proto/adapter_config.pb.h"
#include "cybertron/common/log.h"
#include "modules/common/math/math_utils.h"
#include "modules/common/util/file.h"
#include "modules/map/hdmap/hdmap_util.h"
#include "modules/prediction/common/prediction_gflags.h"
#include "modules/prediction/common/prediction_map.h"
#include "modules/prediction/common/prediction_util.h"
#include "modules/prediction/common/road_graph.h"
#include "modules/prediction/container/container_manager.h"
#include "modules/prediction/container/obstacles/obstacles_container.h"
#include "modules/prediction/container/pose/pose_container.h"

namespace apollo {
namespace prediction {

using ::apollo::common::PathPoint;
using ::apollo::common::Point3D;
using ::apollo::common::TrajectoryPoint;
using ::apollo::common::adapter::AdapterConfig;
using ::apollo::common::math::KalmanFilter;
using ::apollo::hdmap::LaneInfo;
using ::apollo::prediction::math_util::EvaluateQuarticPolynomial;
using ::apollo::prediction::math_util::EvaluateQuinticPolynomial;

void MoveSequencePredictor::Predict(Obstacle* obstacle) {
  Clear();

  CHECK_NOTNULL(obstacle);
  CHECK_GT(obstacle->history_size(), 0);

  const Feature& feature = obstacle->latest_feature();

  if (!feature.has_lane() || !feature.lane().has_lane_graph()) {
    AERROR << "Obstacle [" << obstacle->id() << "] has no lane graph.";
    return;
  }

  std::string lane_id = "";
  if (feature.lane().has_lane_feature()) {
    lane_id = feature.lane().lane_feature().lane_id();
  }
  int num_lane_sequence = feature.lane().lane_graph().lane_sequence_size();
  std::vector<bool> enable_lane_sequence(num_lane_sequence, true);
  FilterLaneSequences(feature, lane_id, &enable_lane_sequence);
  for (int i = 0; i < num_lane_sequence; ++i) {
    const LaneSequence& sequence = feature.lane().lane_graph().lane_sequence(i);
    if (sequence.lane_segment_size() <= 0 ||
        sequence.lane_segment(0).lane_point_size() <= 0) {
      ADEBUG << "Empty lane segments.";
      continue;
    }

    if (!enable_lane_sequence[i]) {
      ADEBUG << "Lane sequence [" << ToString(sequence)
             << "] with probability [" << sequence.probability()
             << "] is disqualified.";
      continue;
    }

    ADEBUG << "Obstacle [" << obstacle->id()
           << "] will draw a lane sequence trajectory [" << ToString(sequence)
           << "] with probability [" << sequence.probability() << "].";

    std::vector<TrajectoryPoint> points;
    DrawMoveSequenceTrajectoryPoints(*obstacle, sequence,
                                     FLAGS_prediction_duration,
                                     FLAGS_prediction_period, &points);

    Trajectory trajectory = GenerateTrajectory(points);
    trajectory.set_probability(sequence.probability());
    trajectories_.push_back(std::move(trajectory));
  }
  ADEBUG << "Obstacle [" << obstacle->id() << "] has total "
         << trajectories_.size() << " trajectories.";
}

void MoveSequencePredictor::
    DrawMoveSequenceTrajectoryPoints_UsingBestTrajectorySelection(
    const Obstacle& obstacle, const LaneSequence& lane_sequence,
    const double total_time, const double period,
    std::vector<apollo::common::TrajectoryPoint>* points) {
  // Sanity check.
  points->clear();
  const Feature& feature = obstacle.latest_feature();
  if (!feature.has_position() || !feature.has_velocity() ||
      !feature.position().has_x() || !feature.position().has_y()) {
    AERROR << "Obstacle [" << obstacle.id()
           << " is missing position or velocity";
    return;
  }

  // Generate candidate finish times.
  std::vector<double> candidate_times;
  GenerateCandidateTimes(&candidate_times);

  // Evaluate all candidates using the cost function and select the best one.

  // Set up some initial conditions.
  Eigen::Vector2d position(feature.position().x(), feature.position().y());
  double vel_heading = feature.velocity_heading();
  double feature_v = feature.speed();
  double feature_a = 0.0;
  if (FLAGS_enable_lane_sequence_acc && lane_sequence.has_acceleration()) {
    feature_a = lane_sequence.acceleration();
  }
  double lane_heading = lane_sequence.lane_segment(0).lane_point(0).heading();
  double ds0 = feature_v * std::cos(vel_heading - lane_heading);
  double dds0 = feature_a * std::cos(vel_heading - lane_heading);

  double cost_of_trajectory = 0.0;
  for (size_t j = 0; j < candidate_times.size(); ++j) {
    // Fit lateral and longitudinal polynomials.
    std::array<double, 6> lateral_coeffs;
    std::array<double, 5> longitudinal_coeffs;
    double time_to_end_state = candidate_times[j];
    double min_end_speed = std::min(FLAGS_still_obstacle_speed_threshold, ds0);
    double ds1 = std::max(min_end_speed, ds0 + dds0 * time_to_end_state);
    std::pair<double, double> lon_end_vt = {ds1, time_to_end_state};
    GetLateralPolynomial(obstacle, lane_sequence, time_to_end_state,
                         &lateral_coeffs);
    GetLongitudinalPolynomial(obstacle, lane_sequence, lon_end_vt,
                              &longitudinal_coeffs);

    // Get lane's initial conditions.
    int lane_segment_index = 0;
    std::string lane_id = lane_sequence.lane_segment(0).lane_id();
    std::shared_ptr<const LaneInfo> lane_info =
        PredictionMap::LaneById(lane_id);
    double lane_s = 0.0;
    double lane_l = 0.0;
    if (!PredictionMap::GetProjection(position, lane_info, &lane_s, &lane_l)) {
      AERROR << "Failed in getting lane s and lane l";
      return;
    }
    double prev_s = 0.0;

    // Draw each trajectory point within the total time of prediction
    double max_lat_acc = 0.0;
    size_t total_num = static_cast<size_t>(total_time / period);
    std::vector<apollo::common::TrajectoryPoint> curr_points;
    for (size_t i = 0; i < total_num; ++i) {
      double relative_time = static_cast<double>(i) * period;
      Eigen::Vector2d point;
      double theta = M_PI;

      // Evaluate the new s.
      double curr_s = EvaluateQuarticPolynomial(longitudinal_coeffs,
                                                relative_time, 0,
                                                lon_end_vt.second,
                                                lon_end_vt.first);
      // Only if new s is larger (obstacle moving forward),
      // update the lane_s and lane_l.
      lane_s += std::max(0.0, (curr_s - prev_s));
      if (curr_s + FLAGS_double_precision >= prev_s) {
        lane_l = EvaluateQuinticPolynomial(lateral_coeffs, relative_time, 0,
                                           time_to_end_state, 0.0);
        prev_s = curr_s;
      }

      // Get the universal point and theta info.
      if (!PredictionMap::SmoothPointFromLane(lane_id, lane_s, lane_l, &point,
                                              &theta)) {
        AERROR << "Unable to get smooth point from lane [" << lane_id
               << "] with s [" << lane_s << "] and l [" << lane_l << "]";
        break;
      }
      PathPoint path_point;
      path_point.set_x(point.x());
      path_point.set_y(point.y());
      path_point.set_z(0.0);
      path_point.set_theta(theta);
      path_point.set_lane_id(lane_id);
      // Get the speed and acceleration info along the lane.
      double lane_speed =
          EvaluateQuarticPolynomial(longitudinal_coeffs, relative_time, 1,
                                    lon_end_vt.second, lon_end_vt.first);
      double lane_acc =
          EvaluateQuarticPolynomial(longitudinal_coeffs, relative_time, 2,
                                    lon_end_vt.second, lon_end_vt.first);
      TrajectoryPoint trajectory_point;
      trajectory_point.mutable_path_point()->CopyFrom(path_point);
      trajectory_point.set_v(lane_speed);
      trajectory_point.set_a(lane_acc);
      trajectory_point.set_relative_time(relative_time);
      curr_points.emplace_back(std::move(trajectory_point));

      double lateral_acc =
          EvaluateQuinticPolynomial(lateral_coeffs, relative_time, 2,
                                    time_to_end_state, 0.0);
      max_lat_acc = std::max(max_lat_acc, std::abs(lateral_acc));

      // If the obstacle gets into the next lane_segment,
      // update the lane_segment accordingly.
      while (lane_s > PredictionMap::LaneById(lane_id)->total_length() &&
             lane_segment_index + 1 < lane_sequence.lane_segment_size()) {
        lane_segment_index += 1;
        lane_s = lane_s - PredictionMap::LaneById(lane_id)->total_length();
        lane_id = lane_sequence.lane_segment(lane_segment_index).lane_id();
      }
    }

    // Evaluate using the cost function, and decide whether to keep it or not.
    double curr_cost = CostFunction(max_lat_acc, time_to_end_state);
    if (j == 0 || curr_cost < cost_of_trajectory) {
      cost_of_trajectory = curr_cost;
      *points = curr_points;
    }
  }
  return;
}

/**
  * For this function:
  * Input: obstacle status, lane-sequence, the total time of prediction,
  *        and the time interval between two adjacent points when plotting.
  * Output: A vector of TrajectoryPoint
*/
void MoveSequencePredictor::DrawMoveSequenceTrajectoryPoints(
    const Obstacle& obstacle, const LaneSequence& lane_sequence,
    const double total_time, const double period,
    std::vector<TrajectoryPoint>* points) {
  // Sanity check.
  points->clear();
  const Feature& feature = obstacle.latest_feature();
  if (!feature.has_position() || !feature.has_velocity() ||
      !feature.position().has_x() || !feature.position().has_y()) {
    AERROR << "Obstacle [" << obstacle.id()
           << " is missing position or velocity";
    return;
  }

  // Fit the lateral and longitudinal polynomials.
  Eigen::Vector2d position(feature.position().x(), feature.position().y());
  double time_to_lat_end_state =
      std::max(FLAGS_default_time_to_lat_end_state,
               ComputeTimeToLatEndConditionByVelocity(obstacle, lane_sequence));

  double vel_heading = feature.velocity_heading();
  double feature_v = feature.speed();
  double feature_a = 0.0;
  if (FLAGS_enable_lane_sequence_acc && lane_sequence.has_acceleration()) {
    feature_a = lane_sequence.acceleration();
  }
  double lane_heading = lane_sequence.lane_segment(0).lane_point(0).heading();
  double s0 = 0.0;
  double ds0 = feature_v * std::cos(vel_heading - lane_heading);
  double dds0 = feature_a * std::cos(vel_heading - lane_heading);
  std::pair<double, double> lon_end_vt =
      ComputeLonEndState({s0, ds0, dds0}, lane_sequence);

  std::array<double, 6> lateral_coeffs;
  std::array<double, 5> longitudinal_coeffs;
  GetLateralPolynomial(obstacle, lane_sequence, time_to_lat_end_state,
                       &lateral_coeffs);
  GetLongitudinalPolynomial(obstacle, lane_sequence, lon_end_vt,
                            &longitudinal_coeffs);

  // Get ready for the for-loop:
  // project the obstacle's position onto the lane's Frenet coordinates.
  int lane_segment_index = 0;
  std::string lane_id =
      lane_sequence.lane_segment(lane_segment_index).lane_id();
  std::shared_ptr<const LaneInfo> lane_info = PredictionMap::LaneById(lane_id);
  double lane_s = 0.0;
  double lane_l = 0.0;
  if (!PredictionMap::GetProjection(position, lane_info, &lane_s, &lane_l)) {
    AERROR << "Failed in getting lane s and lane l";
    return;
  }
  double prev_lane_l = lane_l;

  // Draw each trajectory point within the total time of prediction
  size_t total_num = static_cast<size_t>(total_time / period);
  for (size_t i = 0; i < total_num; ++i) {
    double relative_time = static_cast<double>(i) * period;
    Eigen::Vector2d point;
    double theta = M_PI;

    lane_l = EvaluateQuinticPolynomial(lateral_coeffs, relative_time, 0,
                                       time_to_lat_end_state, 0.0);
    double curr_s =
        EvaluateQuarticPolynomial(longitudinal_coeffs, relative_time, 0,
                                  lon_end_vt.second, lon_end_vt.first);
    double prev_s = (i > 0) ? EvaluateQuarticPolynomial(
                                  longitudinal_coeffs, relative_time - period,
                                  0, lon_end_vt.second, lon_end_vt.first)
                            : 0.0;
    lane_s += std::max(0.0, (curr_s - prev_s));
    if (curr_s + FLAGS_double_precision < prev_s) {
      lane_l = prev_lane_l;
    }
    if (!PredictionMap::SmoothPointFromLane(lane_id, lane_s, lane_l, &point,
                                            &theta)) {
      AERROR << "Unable to get smooth point from lane [" << lane_id
             << "] with s [" << lane_s << "] and l [" << lane_l << "]";
      break;
    }

    prev_lane_l = lane_l;
    double lane_speed =
        EvaluateQuarticPolynomial(longitudinal_coeffs, relative_time, 1,
                                  lon_end_vt.second, lon_end_vt.first);
    double lane_acc =
        EvaluateQuarticPolynomial(longitudinal_coeffs, relative_time, 2,
                                  lon_end_vt.second, lon_end_vt.first);

    TrajectoryPoint trajectory_point;
    PathPoint path_point;
    path_point.set_x(point.x());
    path_point.set_y(point.y());
    path_point.set_z(0.0);
    path_point.set_theta(theta);
    path_point.set_lane_id(lane_id);
    trajectory_point.mutable_path_point()->CopyFrom(path_point);
    trajectory_point.set_v(lane_speed);
    trajectory_point.set_a(lane_acc);
    trajectory_point.set_relative_time(relative_time);
    points->emplace_back(std::move(trajectory_point));

    while (lane_s > PredictionMap::LaneById(lane_id)->total_length() &&
           lane_segment_index + 1 < lane_sequence.lane_segment_size()) {
      lane_segment_index += 1;
      lane_s = lane_s - PredictionMap::LaneById(lane_id)->total_length();
      lane_id = lane_sequence.lane_segment(lane_segment_index).lane_id();
    }
  }
}

bool MoveSequencePredictor::GetLongitudinalPolynomial(
    const Obstacle& obstacle, const LaneSequence& lane_sequence,
    const std::pair<double, double> lon_end_vt,
    std::array<double, 5>* coefficients) {
  // Sanity check.
  CHECK_GT(obstacle.history_size(), 0);
  CHECK_GT(lane_sequence.lane_segment_size(), 0);
  CHECK_GT(lane_sequence.lane_segment(0).lane_point_size(), 0);

  // Get obstacle info.
  const Feature& feature = obstacle.latest_feature();
  double theta = feature.velocity_heading();
  double v = feature.speed();
  double a = 0.0;

  // Get lane info.
  if (FLAGS_enable_lane_sequence_acc && lane_sequence.has_acceleration()) {
    a = lane_sequence.acceleration();
  }
  double lane_heading = lane_sequence.lane_segment(0).lane_point(0).heading();

  // Set the initial conditions for the diff. eqn.
  double s0 = 0.0;
  double ds0 = v * std::cos(theta - lane_heading);
  double dds0 = a * std::cos(theta - lane_heading);
  // double min_end_speed = std::min(FLAGS_still_obstacle_speed_threshold, ds0);
  // double ds1 = std::max(min_end_speed, ds0 + dds0 * time_to_end_state);
  double ds1 = lon_end_vt.first;
  double dds1 = 0.0;
  double p = lon_end_vt.second;  // time to lon end state

  // Solve for the coefficients.
  coefficients->operator[](0) = s0;
  coefficients->operator[](1) = ds0;
  coefficients->operator[](2) = 0.5 * dds0;
  double b0 = ds1 - dds0 * p - ds0;
  double b1 = dds1 - dds0;
  double p2 = p * p;
  double p3 = p2 * p;
  coefficients->operator[](3) = b0 / p2 - b1 / 3.0 / p;
  coefficients->operator[](4) = -0.5 / p3 * b0 + 0.25 / p2 * b1;
  return true;
}

bool MoveSequencePredictor::GetLateralPolynomial(
    const Obstacle& obstacle, const LaneSequence& lane_sequence,
    const double time_to_end_state, std::array<double, 6>* coefficients) {
  // Sanity check.
  CHECK_GT(obstacle.history_size(), 0);
  CHECK_GT(lane_sequence.lane_segment_size(), 0);
  CHECK_GT(lane_sequence.lane_segment(0).lane_point_size(), 0);

  // Get obstacle info.
  const Feature& feature = obstacle.latest_feature();
  double theta = feature.velocity_heading();
  double v = feature.speed();
  double a = feature.acc();
  Point3D position = feature.position();
  const LanePoint& start_lane_point =
      lane_sequence.lane_segment(0).lane_point(0);

  // Get lane info.
  double pos_delta_x = position.x() - start_lane_point.position().x();
  double pos_delta_y = position.y() - start_lane_point.position().y();
  double lane_heading_x = std::cos(start_lane_point.heading());
  double lane_heading_y = std::sin(start_lane_point.heading());

  // Check if obstacle is to the left(+) or right(-) of the lane.
  double cross_prod =
      lane_heading_x * pos_delta_y - lane_heading_y * pos_delta_x;
  double shift = std::hypot(pos_delta_x, pos_delta_y);

  // Set the initial conditions for solving diff. eqn.
  double l0 = (cross_prod > 0) ? shift : -shift;
  double dl0 = v * std::sin(theta - start_lane_point.heading());
  double ddl0 = a * std::sin(theta - start_lane_point.heading());
  double l1 = 0.0;
  double dl1 = 0.0;
  double ddl1 = 0.0;

  // Solve for the coefficients.
  coefficients->operator[](0) = l0;
  coefficients->operator[](1) = dl0;
  coefficients->operator[](2) = ddl0 / 2.0;
  double p = time_to_end_state;
  double p2 = p * p;
  double p3 = p2 * p;
  double c0 = (l1 - 0.5 * p2 * ddl0 - dl0 * p - l0) / p3;
  double c1 = (dl1 - ddl0 * p - dl0) / p2;
  double c2 = (ddl1 - ddl0) / p;
  coefficients->operator[](3) = 0.5 * (20.0 * c0 - 8.0 * c1 + c2);
  coefficients->operator[](4) = (-15.0 * c0 + 7.0 * c1 - c2) / p;
  coefficients->operator[](5) = (6.0 * c0 - 3.0 * c1 + 0.5 * c2) / p2;
  return true;
}

std::pair<double, double> MoveSequencePredictor::ComputeLonEndState(
    const std::array<double, 3>& init_s, const LaneSequence& lane_sequence) {
  // Get the maximum kappa of the lane.
  double max_kappa = 0.0;
  double s_at_max_kappa = 0.0;
  for (int i = 0; i < lane_sequence.path_point_size(); ++i) {
    const PathPoint& path_point = lane_sequence.path_point(i);
    if (path_point.s() < init_s[0] + FLAGS_double_precision) {
      continue;
    }
    if (max_kappa < path_point.kappa()) {
      max_kappa = path_point.kappa();
      s_at_max_kappa = path_point.s();
    }
  }

  // If the max. curvature is small (almost straight lane),
  // then predict that the obstacle will keep current speed.
  double v_init = init_s[1];
  if (max_kappa < FLAGS_turning_curvature_lower_bound) {
    return {v_init, FLAGS_prediction_duration};
  }
  // (Calculate the speed at the max. curvature point)
  double v_end = apollo::prediction::predictor_util::AdjustSpeedByCurvature(
      init_s[1], max_kappa);
  // If the calculated speed at max. curvature point is higher
  // than initial speed, don't accelerate, just predict that
  // the obstacle will maintain current speed.
  if (v_end + FLAGS_double_precision > v_init) {
    return {v_init, FLAGS_prediction_duration};
  }
  // If the obstacle is already at the max. curvature point,
  // then predict that it will maintain the current speed.
  double s_offset = s_at_max_kappa - init_s[0];
  double t = 2.0 * s_offset / (v_init + v_end);
  if (t < FLAGS_double_precision) {
    return {v_init, FLAGS_prediction_duration};
  }
  // If the deceleration is too much,
  // then predict the obstacle follows a reasonable deceleration.
  double acc = (v_end - v_init) / t;
  if (acc < FLAGS_min_acc) {
    t = v_init / (-FLAGS_min_acc);
    return {FLAGS_still_obstacle_speed_threshold, t};
  }
  // Otherwise, predict that it takes t for the obstacle to arrive at the
  // max. curvature point with v_end speed.
  return {v_end, t};
}

double MoveSequencePredictor::ComputeTimeToLatEndConditionByVelocity(
    const Obstacle& obstacle, const LaneSequence& lane_sequence) {
  // Sanity check.
  CHECK_GT(obstacle.history_size(), 0);
  CHECK_GT(lane_sequence.lane_segment_size(), 0);
  CHECK_GT(lane_sequence.lane_segment(0).lane_point_size(), 0);

  // Get the obstacle's Cartesian v_x, v_y.
  // Get the lane point's Cartesian angle, relative distance.
  // Then project the obstacle's velocity onto Frenet coordinate to get
  // lateral velocity v_l.
  const Feature& feature = obstacle.latest_feature();
  const LanePoint& first_lane_point =
      lane_sequence.lane_segment(0).lane_point(0);
  double v_x = feature.velocity().x();
  double v_y = feature.velocity().y();
  double lane_heading = first_lane_point.heading();
  double lane_l = first_lane_point.relative_l();
  double v_l = v_y * std::cos(lane_heading) - v_x * std::sin(lane_heading);

  // If laterally moving too slow or even away from lane center,
  // then use the default speed
  // Otherwise, directly calculate the time and return.
  if (std::fabs(v_l) < FLAGS_default_lateral_approach_speed ||
      lane_l * v_l < 0.0) {
    return std::fabs(lane_l / FLAGS_default_lateral_approach_speed);
  }
  return std::fabs(lane_l / v_l);
}

void MoveSequencePredictor::GenerateCandidateTimes(
    std::vector<double>* candidate_times) {
  candidate_times->clear();
  double t = FLAGS_time_lower_bound_to_lane_center;
  double time_gap = FLAGS_sample_time_gap;
  while (t <= FLAGS_time_upper_bound_to_lane_center) {
    candidate_times->push_back(t);
    t += time_gap;
  }
}

double MoveSequencePredictor::CostFunction(
    const double max_lat_acc, const double time_to_end_state) {
  double cost_of_trajectory = max_lat_acc +
                              FLAGS_cost_function_alpha * time_to_end_state;
  return (cost_of_trajectory);
}

}  // namespace prediction
}  // namespace apollo
