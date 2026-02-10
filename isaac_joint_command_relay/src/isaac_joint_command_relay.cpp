// isaac_joint_command_relay_node.cpp
#include <rclcpp/rclcpp.hpp>
#include <sensor_msgs/msg/joint_state.hpp>
#include <control_msgs/msg/joint_trajectory_controller_state.hpp>

#include <unordered_map>
#include <vector>
#include <string>
#include <mutex>
#include <algorithm>

using control_msgs::msg::JointTrajectoryControllerState;
using sensor_msgs::msg::JointState;

class IsaacJointCommandRelay : public rclcpp::Node {
public:
  IsaacJointCommandRelay()
  : Node("isaac_joint_command_relay")
  {
    // ───────────────── Parameters ─────────────────
    arm_state_topic_ = this->declare_parameter<std::string>(
        "arm_state_topic", "/hanford_manipulator_controller/state");
    lin_state_topic_ = this->declare_parameter<std::string>(
        "lin_state_topic", "/linear_axis_controller/state");
    publish_rate_hz_ = this->declare_parameter<double>("publish_rate_hz", 50.0);

    // Optional: choose source order at runtime (comma-separated)
    // default = "reference,output,desired,actual"
    const auto order_str = this->declare_parameter<std::string>(
        "source_preference", "reference,output,desired,actual");
    parse_source_order(order_str);

    // Fixed Isaac joint order (must match Isaac’s subscriber & your URDF)
    all_joints_ = {
      "insert_into_pipe",
      "rotate_in_pipe",
      "joint_1",
      "joint_2",
      "end_effector_joint",
      "joint_3_pulley_spin"
    };

    // ───────────────── QoS ─────────────────
    // Controller state publishers are RELIABLE + TRANSIENT_LOCAL, but a VOLATILE
    // subscriber is compatible. Keep it small latency.
    rclcpp::QoS sub_qos(10);
    sub_qos.reliability(RMW_QOS_POLICY_RELIABILITY_RELIABLE);
    sub_qos.durability(RMW_QOS_POLICY_DURABILITY_VOLATILE);

    // Isaac typically wants RELIABLE command stream; 1 is fine (last-value-wins).
    rclcpp::QoS pub_qos(1);
    pub_qos.reliability(RMW_QOS_POLICY_RELIABILITY_RELIABLE);
    pub_qos.durability(RMW_QOS_POLICY_DURABILITY_VOLATILE);

    // ───────────────── Pub & Subs ─────────────────
    pub_ = this->create_publisher<JointState>("/isaac_joint_command", pub_qos);

    // Subscribe to BOTH “state” and “controller_state” paths for each controller.
    // (Whichever arrives will update the aggregator.)
    make_sub(arm_state_topic_, sub_qos);
    make_sub(mirror_to_controller_state(arm_state_topic_), sub_qos);
    make_sub(lin_state_topic_, sub_qos);
    make_sub(mirror_to_controller_state(lin_state_topic_), sub_qos);

    // ───────────────── Timer ─────────────────
    timer_ = this->create_wall_timer(
      std::chrono::duration<double>(1.0 / std::max(1e-3, publish_rate_hz_)),
      std::bind(&IsaacJointCommandRelay::tick, this));

    RCLCPP_INFO(get_logger(),
      "Relaying joint setpoints from [%s] & [%s] (and their /controller_state twins) → /isaac_joint_command @ %.1f Hz\n  Source preference: %s",
      arm_state_topic_.c_str(), lin_state_topic_.c_str(), publish_rate_hz_,
      order_str.c_str());
  }

private:
  // Build a sibling topic name “…/controller_state” if the input ends with “…/state”
  static std::string mirror_to_controller_state(const std::string& t) {
    static const std::string tail = "/state";
    if (t.size() >= tail.size() && t.compare(t.size()-tail.size(), tail.size(), tail) == 0) {
      return t.substr(0, t.size()-tail.size()) + "/controller_state";
    }
    return t; // unchanged
  }

  void make_sub(const std::string& topic, const rclcpp::QoS& qos) {
    // Avoid duplicate subscriptions to identical topic strings
    if (std::find(subscribed_topics_.begin(), subscribed_topics_.end(), topic) != subscribed_topics_.end())
      return;
    subscribed_topics_.push_back(topic);

    auto sub = this->create_subscription<JointTrajectoryControllerState>(
      topic, qos,
      [this, topic](JointTrajectoryControllerState::ConstSharedPtr msg){
        this->on_state(topic, *msg);
      });
    subs_.push_back(sub);
    RCLCPP_INFO(get_logger(), "Subscribed: %s", topic.c_str());
  }

  // Choose the first non-empty vector among preferred sources
  const std::vector<double>* pick_positions(const JointTrajectoryControllerState& msg,
                                            std::string& picked) const {
    for (auto s : source_order_) {
      switch (s) {
        case Source::REFERENCE:
          if (!msg.reference.positions.empty()) { picked = "reference"; return &msg.reference.positions; }
          break;
        case Source::OUTPUT:
          if (!msg.output.positions.empty())    { picked = "output";    return &msg.output.positions; }
          break;
        case Source::DESIRED:
          if (!msg.desired.positions.empty())   { picked = "desired";   return &msg.desired.positions; }
          break;
        case Source::ACTUAL:
          if (!msg.feedback.positions.empty())  { picked = "actual";    return &msg.feedback.positions; }
          break;
      }
    }
    picked.clear();
    return nullptr;
  }

  void on_state(const std::string& topic, const JointTrajectoryControllerState& msg)
  {
    std::string picked;
    const auto* vec = pick_positions(msg, picked);
    if (!vec) {
      // One-time throttled warn if nothing usable
      RCLCPP_WARN_THROTTLE(get_logger(), *get_clock(), 3000,
        "No usable positions (reference/output/desired/actual) in message from [%s].", topic.c_str());
      return;
    }

    const auto& names = msg.joint_names;
    if (vec->size() != names.size()) {
      RCLCPP_WARN_THROTTLE(get_logger(), *get_clock(), 3000,
        "Name/position size mismatch from [%s]: names=%zu positions=%zu (source=%s)",
        topic.c_str(), names.size(), vec->size(), picked.c_str());
      return;
    }

    {
      std::lock_guard<std::mutex> lk(mux_);
      for (size_t i = 0; i < names.size(); ++i) {
        latest_positions_[names[i]] = (*vec)[i];
      }
      have_any_ = true;
    }
  }

  void tick()
  {
    std::unordered_map<std::string,double> snapshot;
    {
      std::lock_guard<std::mutex> lk(mux_);
      if (!have_any_) return;
      snapshot = latest_positions_;  // copy for lockless publish
    }

    JointState js;
    js.header.stamp = this->get_clock()->now();
    js.name = all_joints_;
    js.position.resize(all_joints_.size());

    for (size_t i = 0; i < all_joints_.size(); ++i) {
      auto it = snapshot.find(all_joints_[i]);
      js.position[i] = (it != snapshot.end()) ? it->second : 0.0;
    }
    pub_->publish(js);
  }

  enum class Source { REFERENCE, OUTPUT, DESIRED, ACTUAL };

  void parse_source_order(const std::string& s) {
    source_order_.clear();
    auto to_lower = [](std::string t){ std::transform(t.begin(), t.end(), t.begin(), ::tolower); return t; };
    size_t start = 0;
    while (start <= s.size()) {
      auto comma = s.find(',', start);
      auto token = to_lower(s.substr(start, comma == std::string::npos ? s.size()-start : comma-start));
      token.erase(std::remove_if(token.begin(), token.end(), ::isspace), token.end());
      if (token == "reference") source_order_.push_back(Source::REFERENCE);
      else if (token == "output")   source_order_.push_back(Source::OUTPUT);
      else if (token == "desired")  source_order_.push_back(Source::DESIRED);
      else if (token == "actual" || token == "feedback") source_order_.push_back(Source::ACTUAL);
      start = (comma == std::string::npos) ? s.size()+1 : comma + 1;
    }
    if (source_order_.empty()) {
      source_order_ = {Source::REFERENCE, Source::OUTPUT, Source::DESIRED, Source::ACTUAL};
    }
  }

  // ───────── Params ─────────
  std::string arm_state_topic_;
  std::string lin_state_topic_;
  double publish_rate_hz_{50.0};

  // ───────── Comms ─────────
  std::vector<rclcpp::Subscription<JointTrajectoryControllerState>::SharedPtr> subs_;
  std::vector<std::string> subscribed_topics_;
  rclcpp::Publisher<JointState>::SharedPtr pub_;
  rclcpp::TimerBase::SharedPtr timer_;

  // ───────── State ─────────
  std::vector<std::string> all_joints_;
  std::unordered_map<std::string,double> latest_positions_;
  std::mutex mux_;
  bool have_any_{false};
  std::vector<Source> source_order_;
};

int main(int argc, char** argv)
{
  rclcpp::init(argc, argv);
  rclcpp::spin(std::make_shared<IsaacJointCommandRelay>());
  rclcpp::shutdown();
  return 0;
}
