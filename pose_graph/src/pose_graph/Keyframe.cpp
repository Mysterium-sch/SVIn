#include "pose_graph/Keyframe.h"

#include <sensor_msgs/PointCloud.h>

#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "pose_graph/Parameters.h"
#include "utils/UtilsOpenCV.h"

const int Keyframe::TH_HIGH = 100;
const int Keyframe::TH_LOW = 50;
const size_t Keyframe::briskDetectionOctaves_ = 0;                ///< The set number of brisk octaves.
const double Keyframe::briskDetectionThreshold_ = 40.0;           ///< The set BRISK detection threshold.
const double Keyframe::briskDetectionAbsoluteThreshold_ = 800;    ///< The set BRISK absolute detection threshold.
const size_t Keyframe::briskDetectionMaximumKeypoints_ = 300;     ///< The set maximum number of keypoints.
const bool Keyframe::briskDescriptionRotationInvariance_ = true;  ///< The set rotation invariance setting.
const bool Keyframe::briskDescriptionScaleInvariance_ = false;    ///< The set scale invariance setting.
const double Keyframe::briskMatchingThreshold_ = 80.0;            ///< The set BRISK matching threshold.

template <typename Derived>
static void reduceVector(std::vector<Derived>& v, std::vector<uchar> status) {  // NOLINT
  int j = 0;
  for (int i = 0; i < static_cast<int>(v.size()); i++)
    if (status[i]) v[j++] = v[i];
  v.resize(j);
}

Keyframe::Keyframe(int64_t _time_stamp,
                   std::vector<Eigen::Vector3i>& _point_ids,
                   int _index,
                   Eigen::Vector3d& _svin_T_w_i,
                   Eigen::Matrix3d& _svin_R_w_i,
                   cv::Mat& _image,
                   std::vector<cv::Point3f>& _point_3d,
                   std::vector<cv::KeyPoint>& _point_2d_uv,
                   std::map<Keyframe*, int>& KFcounter,
                   int _sequence,
                   BriefVocabulary* vocBrief,
                   const Parameters& params,
                   const bool is_vio_keyframe)
    : params_(params) {
  time_stamp = _time_stamp;

  // @Reloc
  point_ids_ = _point_ids;

  index = _index;
  svin_T_w_i = _svin_T_w_i;
  svin_R_w_i = _svin_R_w_i;
  T_w_i = svin_T_w_i;
  R_w_i = svin_R_w_i;
  origin_svin_T = svin_T_w_i;
  origin_svin_R = svin_R_w_i;
  image = _image.clone();
  point_3d = _point_3d;
  point_2d_uv = _point_2d_uv;

  has_loop = false;
  loop_index = -1;
  has_fast_point = false;
  loop_info << 0, 0, 0, 0, 0, 0, 0, 0;
  sequence = _sequence;
  is_vio_keyframe_ = is_vio_keyframe;

  if (is_vio_keyframe_) computeWindowBRIEFPoint();
  voc = vocBrief;
  computeBoW();
  KFcounter_ = KFcounter;  // for Covisibility graph
  updateConnections();     // for Covisibility graph

  computeBRIEFPoint();

  if (!params.debug_mode_) image.release();
}

Keyframe::Keyframe(int64_t _time_stamp,
                   int _index,
                   Eigen::Vector3d& _svin_T_w_i,
                   Eigen::Matrix3d& _svin_R_w_i,
                   std::map<Keyframe*, int>& KFcounter,
                   int _sequence,
                   const Parameters& params,
                   const bool is_vio_keyframe)
    : params_(params) {
  time_stamp = _time_stamp;

  index = _index;
  svin_T_w_i = _svin_T_w_i;
  svin_R_w_i = _svin_R_w_i;
  T_w_i = svin_T_w_i;
  R_w_i = svin_R_w_i;
  origin_svin_T = svin_T_w_i;
  origin_svin_R = svin_R_w_i;

  has_loop = false;
  loop_index = -1;
  has_fast_point = false;
  loop_info << 0, 0, 0, 0, 0, 0, 0, 0;
  sequence = _sequence;
  KFcounter_ = KFcounter;  // for Covisibility graph

  is_vio_keyframe_ = is_vio_keyframe;
  updateConnections();  // for Covisibility graph
}

double Keyframe::brisk_distance(const cv::Mat& a, const cv::Mat& b) {
  const unsigned char* pa = a.ptr<unsigned char>();
  const unsigned char* pb = b.ptr<unsigned char>();
  // number_of_128_bit_words or number_of_col, L = 48
  return static_cast<double>(brisk::Hamming::PopcntofXORed(pa, pb, 3 /*48 / 16*/));
}

void Keyframe::computeBRISKPoint() {
  // for searchByDescriptor to create new BRISK keypoints and descriptors
  std::shared_ptr<cv::FeatureDetector> detector(
      new brisk::ScaleSpaceFeatureDetector<brisk::HarrisScoreCalculator>(Keyframe::briskDetectionThreshold_,
                                                                         Keyframe::briskDetectionOctaves_,
                                                                         Keyframe::briskDetectionAbsoluteThreshold_,
                                                                         Keyframe::briskDetectionMaximumKeypoints_));

  std::shared_ptr<cv::DescriptorExtractor> extractor(new brisk::BriskDescriptorExtractor(
      Keyframe::briskDescriptionRotationInvariance_, Keyframe::briskDescriptionScaleInvariance_));

  detector->detect(image, brisk_keypoints);

  if (!window_keypoints.empty()) {
    extractor->compute(image, window_keypoints, window_brisk_descriptors);
  } else {
    std::cout << "window keypoints are empty. This is a problem!!" << std::endl;
  }
  extractor->compute(image, brisk_keypoints, brisk_descriptors);

  std::cout << "Size of Brisk keypoints: " << brisk_keypoints.size() << std::endl;
}

void Keyframe::computeBoW() {
  if (bowVec.empty() || featVec.empty()) {
    // Feature vector associate features with nodes in the 4th level (from leaves up)
    // We assume the vocabulary tree has 6 levels, change the 4 otherwise
    voc->transform(brief_descriptors, bowVec);
  }
}

void Keyframe::updateConnections() {
  if (KFcounter_.empty() && is_vio_keyframe_) {
    // std::cout << "KFcounter is empty for KF: " << index << " This SHOULDN't be happening except 1st frame."
    // << std::endl;
    return;
  }

  // std::cout<<"Weights for observed keyframes in Kf: "<< this->index << std::endl;
  int th_weight = 20;  // TODO(Sharmin): Move it to the Config file
  for (std::map<Keyframe*, int>::iterator mit = KFcounter_.begin(); mit != KFcounter_.end(); mit++) {
    if (mit->second > th_weight) {
      mConnectedKeyFrameWeights.insert(std::make_pair(mit->first, mit->second));
      // std::cout << "Observed Kf: " << mit->first->index << " with weight(common MapPoint): " << mit->second
      //           << std::endl;
    }
  }
}

// Note Keypoints found by okvis_estimator
void Keyframe::computeWindowBRIEFPoint() {
  BriefExtractor extractor(params_.brief_pattern_file_.c_str());

  window_keypoints = point_2d_uv;

  extractor(image, window_keypoints, window_brief_descriptors);

  for (int i = 0; i < static_cast<int>(window_keypoints.size()); i++) {
    Eigen::Vector3d tmp_p;

    project_normal(Eigen::Vector2d(window_keypoints[i].pt.x, window_keypoints[i].pt.y), tmp_p);

    cv::KeyPoint tmp_norm;
    tmp_norm.pt = cv::Point2f(tmp_p.x() / tmp_p.z(), tmp_p.y() / tmp_p.z());
    window_keypoints_norm.push_back(tmp_norm);
  }
}

void Keyframe::project_normal(Eigen::Vector2d kp, Eigen::Vector3d& point3d) const {
  const float invfx = 1.0f / params_.p_fx_;
  const float invfy = 1.0f / params_.p_fy_;

  const float u = kp[0];
  const float v = kp[1];
  point3d[0] = (u - params_.p_cx_) * invfx;
  point3d[1] = (v - params_.p_cy_) * invfy;
  point3d[2] = 1.0;
}

void Keyframe::computeBRIEFPoint() {
  BriefExtractor extractor(params_.brief_pattern_file_.c_str());
  const int fast_th = 20;  // corner detector response threshold
  if (1) {
    cv::FAST(image, keypoints, fast_th, true);
  } else {
    std::vector<cv::Point2f> tmp_pts;
    cv::goodFeaturesToTrack(image, tmp_pts, 500, 0.01, 10);
    for (int i = 0; i < static_cast<int>(tmp_pts.size()); i++) {
      cv::KeyPoint key;
      key.pt = tmp_pts[i];
      keypoints.push_back(key);
    }
  }
  extractor(image, keypoints, brief_descriptors);

  for (int i = 0; i < static_cast<int>(keypoints.size()); i++) {
    Eigen::Vector3d tmp_p;

    project_normal(Eigen::Vector2d(keypoints[i].pt.x, keypoints[i].pt.y), tmp_p);

    cv::KeyPoint tmp_norm;
    tmp_norm.pt = cv::Point2f(tmp_p.x() / tmp_p.z(), tmp_p.y() / tmp_p.z());
    keypoints_norm.push_back(tmp_norm);
  }
}

void BriefExtractor::operator()(const cv::Mat& im,
                                std::vector<cv::KeyPoint>& keys,
                                std::vector<DVision::BRIEF256::bitset>& descriptors) const {
  m_brief.compute(im, keys, descriptors);
}

bool Keyframe::matchBrisk(const cv::Mat& window_descriptor,
                          const cv::Mat& descriptors_old,
                          const std::vector<cv::KeyPoint>& keypoints_old,
                          cv::Point2f& best_match) {
  cv::Point2f best_pt;
  int bestDist = 256;
  int bestIndex = -1;
  for (size_t i = 0; i < descriptors_old.rows; i++) {
    double dis = brisk_distance(window_descriptor, descriptors_old.row(i));
    if (dis < bestDist) {
      bestDist = dis;
      bestIndex = i;
    }
  }
  // printf("best dist %d", bestDist);
  if (bestIndex != -1 && bestDist < briskMatchingThreshold_) {
    best_match = keypoints_old[bestIndex].pt;
    return true;
  } else {
    return false;
  }
}

void Keyframe::searchByBRISKDescriptor(std::vector<cv::Point2f>& matched_2d_old,
                                       std::vector<uchar>& status,
                                       const cv::Mat& descriptors_old,
                                       const std::vector<cv::KeyPoint>& keypoints_old) {
  for (size_t i = 0; i < window_brisk_descriptors.rows; i++) {
    cv::Point2f pt(0.f, 0.f);
    if (matchBrisk(window_brisk_descriptors.row(i), descriptors_old, keypoints_old, pt))
      status.push_back(1);
    else
      status.push_back(0);
    matched_2d_old.push_back(pt);
  }
}

bool Keyframe::searchInAera(const DVision::BRIEF256::bitset window_descriptor,
                            const std::vector<DVision::BRIEF256::bitset>& descriptors_old,
                            const std::vector<cv::KeyPoint>& keypoints_old,
                            const std::vector<cv::KeyPoint>& keypoints_old_norm,
                            cv::Point2f& best_match,
                            cv::Point2f& best_match_norm) {
  cv::Point2f best_pt;
  int bestDist = 128;
  int bestIndex = -1;
  for (int i = 0; i < static_cast<int>(descriptors_old.size()); i++) {
    int dis = HammingDis(window_descriptor, descriptors_old[i]);
    if (dis < bestDist) {
      bestDist = dis;
      bestIndex = i;
    }
  }
  // printf("best dist %d", bestDist);
  if (bestIndex != -1 && bestDist < 80) {
    best_match = keypoints_old[bestIndex].pt;
    best_match_norm = keypoints_old_norm[bestIndex].pt;
    return true;
  } else {
    return false;
  }
}

void Keyframe::searchByBRIEFDes(std::vector<cv::Point2f>& matched_2d_old,
                                std::vector<cv::Point2f>& matched_2d_old_norm,
                                std::vector<uchar>& status,
                                const std::vector<DVision::BRIEF256::bitset>& descriptors_old,
                                const std::vector<cv::KeyPoint>& keypoints_old,
                                const std::vector<cv::KeyPoint>& keypoints_old_norm) {
  for (int i = 0; i < static_cast<int>(window_brief_descriptors.size()); i++) {
    cv::Point2f pt(0.f, 0.f);
    cv::Point2f pt_norm(0.f, 0.f);
    if (searchInAera(window_brief_descriptors[i], descriptors_old, keypoints_old, keypoints_old_norm, pt, pt_norm))
      status.push_back(1);
    else
      status.push_back(0);
    matched_2d_old.push_back(pt);
    matched_2d_old_norm.push_back(pt_norm);
  }
}

void Keyframe::PnPRANSAC(const std::vector<cv::Point2f>& matched_2d_old,
                         const std::vector<cv::Point3f>& matched_3d,
                         std::vector<uchar>& status,
                         Eigen::Vector3d& PnP_T_old,
                         Eigen::Matrix3d& PnP_R_old) {
  cv::Mat r, rvec, t, tmp_r;
  cv::Mat K = (cv::Mat_<double>(3, 3) << params_.p_fx_, 0, params_.p_cx_, 0, params_.p_fy_, params_.p_cy_, 0, 0, 1.0);
  Eigen::Matrix3d R_inital;
  Eigen::Vector3d P_inital;

  Eigen::Matrix3d R_w_c = origin_svin_R;
  Eigen::Vector3d T_w_c = origin_svin_T;

  R_inital = R_w_c.inverse();
  P_inital = -(R_inital * T_w_c);

  cv::eigen2cv(R_inital, tmp_r);
  cv::Rodrigues(tmp_r, rvec);
  cv::eigen2cv(P_inital, t);

  cv::Mat inliers;

  // bjoshi
  // Temporary fix for https://github.com/opencv/opencv/issues/17799
  // This is a bug in opencv. The bug is fixed in opencv master branch.
  try {
    solvePnPRansac(matched_3d,
                   matched_2d_old,
                   K,
                   params_.distortion_coeffs_,
                   rvec,
                   t,
                   false,
                   params_.loop_closure_params_.pnp_ransac_iterations,
                   params_.loop_closure_params_.pnp_reprojection_thresh,
                   0.99,
                   inliers);
  } catch (cv::Exception e) {
    // std::cout << "Caught exception in PnPRANSAC:" << e.what() << std::endl;
    inliers.setTo(cv::Scalar(0));
  }

  for (int i = 0; i < static_cast<int>(matched_2d_old.size()); i++) status.push_back(0);

  for (int i = 0; i < inliers.rows; i++) {
    int n = inliers.at<int>(i);
    status[n] = 1;
  }

  cv::Rodrigues(rvec, r);
  Eigen::Matrix3d R_pnp, R_w_c_old;
  cv::cv2eigen(r, R_pnp);
  R_w_c_old = R_pnp.transpose();
  Eigen::Vector3d T_pnp, T_w_c_old;
  cv::cv2eigen(t, T_pnp);
  T_w_c_old = R_w_c_old * (-T_pnp);

  PnP_R_old = R_w_c_old;
  PnP_T_old = T_w_c_old;
}

bool Keyframe::findConnection(Keyframe* old_kf) {
  if (!old_kf->is_vio_keyframe_) return false;

  std::vector<cv::KeyPoint> matched_2d_cur;
  std::vector<cv::Point2f> matched_2d_old;
  std::vector<cv::Point2f> matched_2d_old_norm;
  std::vector<cv::Point3f> matched_3d;
  std::vector<Eigen::Vector3i> matched_ids;  // Reloc
  std::vector<uchar> status;

  matched_3d = point_3d;
  matched_2d_cur = point_2d_uv;
  matched_ids = point_ids_;

  if (params_.debug_mode_) {
    cv::Mat old_img = UtilsOpenCV::DrawCircles(old_kf->image, old_kf->keypoints);
    cv::Mat cur_image = UtilsOpenCV::DrawCircles(image, point_2d_uv);
    std::string loop_candidate_directory = params_.debug_output_path_ + "/loop_candidates/";
    std::string filename = loop_candidate_directory + "loop_candidate_" + std::to_string(index) + "_" +
                           std::to_string(old_kf->index) + ".png";
    UtilsOpenCV::showImagesSideBySide(cur_image, old_img, "loop closing candidates", false, true, filename);
  }

  searchByBRIEFDes(matched_2d_old,
                   matched_2d_old_norm,
                   status,
                   old_kf->brief_descriptors,
                   old_kf->keypoints,
                   old_kf->keypoints_norm);
  reduceVector(matched_2d_old, status);
  reduceVector(matched_3d, status);
  reduceVector(matched_2d_cur, status);
  reduceVector(matched_2d_old_norm, status);
  reduceVector(matched_ids, status);
  status.clear();

  if (params_.debug_mode_) {
    cv::Mat corners_match_image =
        UtilsOpenCV::DrawCornersMatches(image, matched_2d_cur, old_kf->image, matched_2d_old, true);
    std::string dscriptor_match_dir = params_.debug_output_path_ + "/descriptor_matched/";
    std::string filename = dscriptor_match_dir + "descriptor_match_" + std::to_string(index) + "_" +
                           std::to_string(old_kf->index) + ".png";
    cv::imwrite(filename, corners_match_image);
  }

  // std::cout << "Size Before RANSAC: " << matched_2d_cur.size() << std::endl;

  // opengv::transformation_t T_w_c_old;
  // if (LoopClosureUtils::geometricVerificationNister(
  //         matched_2d_cur, matched_2d_old, status, params_.loop_closure_params_.min_correspondences, &T_w_c_old)) {
  //   reduceVector(matched_2d_old, status);
  //   reduceVector(matched_3d, status);
  //   reduceVector(matched_2d_cur, status);
  //   reduceVector(matched_2d_old_norm, status);
  //   reduceVector(matched_ids, status);
  //   status.clear();

  //   if (params_.debug_image_) {
  //     cv::Mat corners_match_image =
  //         UtilsOpenCV::DrawCornersMatches(image, matched_2d_cur, old_kf->image, matched_2d_old, true);
  //     std::string dscriptor_match_dir = pkg_path + "/output_logs/geometric_verification/";
  //     std::string filename = dscriptor_match_dir + "geometric_verification_" + std::to_string(index) + "_" +
  //                            std::to_string(old_kf->index) + ".png";
  //     cv::imwrite(filename, corners_match_image);
  //   }
  // } else {
  //   return false;
  // }

  Eigen::Vector3d PnP_T_old;
  Eigen::Matrix3d PnP_R_old;
  Eigen::Vector3d relative_t;
  Eigen::Quaterniond relative_q;
  double relative_yaw;

  if (static_cast<int>(matched_2d_cur.size()) > params_.loop_closure_params_.min_correspondences) {
    PnPRANSAC(matched_2d_old, matched_3d, status, PnP_T_old, PnP_R_old);
    reduceVector(matched_2d_cur, status);
    reduceVector(matched_2d_old, status);
    reduceVector(matched_2d_old_norm, status);
    reduceVector(matched_3d, status);
    reduceVector(matched_ids, status);
    status.clear();

    if (params_.debug_mode_) {
      cv::Mat pnp_verified_image =
          UtilsOpenCV::DrawCornersMatches(image, matched_2d_cur, old_kf->image, matched_2d_old, true);
      cv::Mat notation(50, pnp_verified_image.cols, CV_8UC3, cv::Scalar(255, 255, 255));
      putText(notation,
              "current frame: " + std::to_string(index),
              cv::Point2f(20, 30),
              cv::FONT_HERSHEY_SIMPLEX,
              1,
              cv::Scalar(255),
              3);

      putText(notation,
              "previous frame: " + std::to_string(old_kf->index),
              cv::Point2f(20 + pnp_verified_image.cols / 2, 30),
              cv::FONT_HERSHEY_SIMPLEX,
              1,
              cv::Scalar(255),
              3);
      cv::vconcat(notation, pnp_verified_image, pnp_verified_image);
      std::string pnp_verified_dir = params_.debug_output_path_ + "/pnp_verified/";
      std::string filename =
          pnp_verified_dir + "pnp_verified_" + std::to_string(index) + "_" + std::to_string(old_kf->index) + ".png";
      cv::imwrite(filename, pnp_verified_image);
    }
  }

  // std::cout<< "Size after RANSAC "<< matched_2d_cur.size() << std::endl;

  if (static_cast<int>(matched_2d_cur.size()) > params_.loop_closure_params_.min_correspondences) {
    relative_t = PnP_R_old.transpose() * (origin_svin_T - PnP_T_old);
    relative_q = PnP_R_old.transpose() * origin_svin_R;

    relative_yaw = Utility::normalizeAngle(Utility::R2ypr(origin_svin_R).x() - Utility::R2ypr(PnP_R_old).x());

    if (abs(relative_yaw) < 25.0 && relative_t.norm() < 15.0) {
      if (params_.debug_mode_) {
        cv::Mat loop_image =
            UtilsOpenCV::DrawCornersMatches(image, matched_2d_cur, old_kf->image, matched_2d_old, true);
        cv::Mat notation(50, loop_image.cols, CV_8UC3, cv::Scalar(255, 255, 255));
        putText(notation,
                "current frame: " + std::to_string(index),
                cv::Point2f(20, 30),
                cv::FONT_HERSHEY_SIMPLEX,
                1,
                cv::Scalar(255),
                3);

        putText(
            notation,
            "previous frame: " + std::to_string(old_kf->index) + " matches: " + std::to_string(matched_2d_cur.size()),
            cv::Point2f(20 + loop_image.cols / 2, 30),
            cv::FONT_HERSHEY_SIMPLEX,
            1,
            cv::Scalar(255),
            3);
        cv::vconcat(notation, loop_image, loop_image);
        std::string pnp_verified_dir = params_.debug_output_path_ + "/loop_closure/";
        std::string filename =
            pnp_verified_dir + "loop_closure_" + std::to_string(index) + "_" + std::to_string(old_kf->index) + ".png";
        cv::imwrite(filename, loop_image);
        std::string loop_closure_stats = params_.debug_output_path_ + "/loop_closure.txt";
        std::ofstream loop_closure_file(loop_closure_stats, std::ios::app);
        loop_closure_file.setf(std::ios::fixed, std::ios::floatfield);
        Eigen::Vector3d relative_ypr = Utility::R2ypr(relative_q.toRotationMatrix());
        loop_closure_file.precision(9);
        loop_closure_file << index << " " << time_stamp << " " << old_kf->index << " " << old_kf->time_stamp << " "
                          << relative_t.x() << " " << relative_t.y() << " " << relative_t.z() << " "
                          << relative_ypr.transpose() << std::endl;
        loop_closure_file.close();
      }
      has_loop = true;
      loop_index = old_kf->index;
      loop_info << relative_t.x(), relative_t.y(), relative_t.z(), relative_q.w(), relative_q.x(), relative_q.y(),
          relative_q.z(), relative_yaw;
      return true;
    }
  }

  return false;
}

int Keyframe::HammingDis(const DVision::BRIEF256::bitset& a, const DVision::BRIEF256::bitset& b) {
  DVision::BRIEF256::bitset xor_of_bitset = a ^ b;
  int dis = xor_of_bitset.count();
  return dis;
}

void Keyframe::getSVInPose(Eigen::Vector3d& _T_w_i, Eigen::Matrix3d& _R_w_i) {
  _T_w_i = svin_T_w_i;
  _R_w_i = svin_R_w_i;
}

void Keyframe::getPose(Eigen::Vector3d& _T_w_i, Eigen::Matrix3d& _R_w_i) {
  _T_w_i = T_w_i;
  _R_w_i = R_w_i;
}

void Keyframe::updatePose(const Eigen::Vector3d& _T_w_i, const Eigen::Matrix3d& _R_w_i) {
  T_w_i = _T_w_i;
  R_w_i = _R_w_i;
}

void Keyframe::updateSVInPose(const Eigen::Vector3d& _T_w_i, const Eigen::Matrix3d& _R_w_i) {
  svin_T_w_i = _T_w_i;
  svin_R_w_i = _R_w_i;
  T_w_i = svin_T_w_i;
  R_w_i = svin_R_w_i;
}

Eigen::Vector3d Keyframe::getLoopRelativeT() { return Eigen::Vector3d(loop_info(0), loop_info(1), loop_info(2)); }

Eigen::Quaterniond Keyframe::getLoopRelativeQ() {
  return Eigen::Quaterniond(loop_info(3), loop_info(4), loop_info(5), loop_info(6));
}

double Keyframe::getLoopRelativeYaw() { return loop_info(7); }

void Keyframe::updateLoop(Eigen::Matrix<double, 8, 1>& _loop_info) {
  if (abs(_loop_info(7)) < 30.0 && Eigen::Vector3d(_loop_info(0), _loop_info(1), _loop_info(2)).norm() < 20.0) {
    // printf("update loop info\n");
    loop_info = _loop_info;
  }
}

BriefExtractor::BriefExtractor(const std::string& pattern_file) {
  // The DVision::BRIEF extractor computes a random pattern by default when
  // the object is created.
  // We load the pattern that we used to build the vocabulary, to make
  // the descriptors compatible with the predefined vocabulary

  // loads the pattern
  cv::FileStorage fs(pattern_file.c_str(), cv::FileStorage::READ);
  if (!fs.isOpened()) throw std::string("Could not open file ") + pattern_file;

  std::vector<int> x1, y1, x2, y2;
  fs["x1"] >> x1;
  fs["x2"] >> x2;
  fs["y1"] >> y1;
  fs["y2"] >> y2;

  m_brief.importPairs(x1, y1, x2, y2);
}

void Keyframe::setRelocalizationPCLCallback(const PointCloudCallback& pcl_callback) {
  relocalization_pcl_callback_ = pcl_callback;
}
