#ifndef TAK_CHAT_NODE_HPP
#define TAK_CHAT_NODE_HPP

//==============================================================================
// TakChatNode — Dedicated ROS2 node for TAK/ATAK chat communication
//==============================================================================
//
// WHAT THIS NODE DOES
// -------------------
// This node is the "bridge adapter" between:
//
//   1) ROS2 TakChat messages (simple: origin, destination, message, timestamp)
//   2) TAK CoT GeoChat XML (std_msgs/String) sent to your TAK bridge pipeline
//
// KEY FEATURES
// ------------
//   - BROADCAST SUPPORT: Destination "ALL" automatically fans out to all
//     allowed callsigns configured in cot_runner.yaml
//
//   - INCOMING FILTERING: Only messages from allowed callsigns are forwarded
//     to tak_chat/in. Messages from unknown senders are ignored.
//
//   - RELIABLE DELIVERY: Implements retry logic that continues publishing
//     until either a subscriber is confirmed and sufficient retries have
//     occurred, or the timeout is reached. This handles DDS discovery races.
//
//   - SEND QUEUE WITH DELAY: Messages are queued and sent one at a time with
//     a minimum delay between them (default 1 second). This ensures each
//     message has a distinct timestamp so TAK/ATAK can properly order them.
//
//   - COMMS-AWARE DELIVERY: Integrates with west_point_comms_sim to only send
//     messages to destinations that are reachable via the mesh network.
//     Messages to unreachable destinations are suppressed with a warning.
//     If comms_status is not published, defaults to allowing all destinations.
//
//   - UID OVERRIDE (for testing): If the TakChat message includes a non-empty
//     'uid' field, that UID will be used for both the event UID and the __chat
//     messageId in the CoT XML instead of generating random values. This allows
//     testing whether ATAK treats messages with identical UIDs+messageIds as
//     updates/replacements.
//
// MESSAGE FLOW
// ------------
//   A) OUTGOING (ROS -> TAK)
//      - BT nodes publish TakChat requests to tak_chat/out
//      - If destination is "ALL", fans out to all allowed callsigns
//      - Each destination is checked against comms_status transitive list
//      - Messages to unreachable destinations are suppressed
//      - Reachable messages are queued and sent with delay between them
//      - Timestamp is generated when actually sent (not when received)
//      - If TakChat.uid is non-empty, uses that UID; otherwise generates random
//      - Publishes to send_to_tak with retry logic
//
//   B) INCOMING (TAK -> ROS)
//      - TAK bridge publishes incoming CoT XML to incoming_cot
//      - Parses GeoChat messages and checks if sender is in allowed list
//      - Only forwards messages from allowed callsigns to tak_chat/in
//
// CONFIGURATION (ROS Parameters)
// ------------------------------
//   callsign:               This robot's identity (default: "warthog1")
//   android_id:             Used for ATAK UID format
//   tak_server_flow_tag_key: TAK server session key
//   outgoing_cot_topic:     Where to publish CoT XML (default: "send_to_tak")
//   incoming_cot_topic:     Where to receive CoT XML (default: "incoming_cot")
//   navsat_topic:           GPS source for location (default: "navsat")
//   tak_chat_out_topic:     Incoming requests from BT (default: "tak_chat/out")
//   tak_chat_in_topic:      Outgoing events to BT (default: "tak_chat/in")
//   comms_topic:            Mesh connectivity status (default: "comms")
//   allowed_callsigns_file: Path to YAML with allowed callsigns
//   send_delay_s:           Min delay between sends (default: 1.0)
//   retry_timeout_s:        Total time to keep retrying (default: 10.0)
//   retry_interval_s:       Interval between retries (default: 1.0)
//   min_retry_count:        Minimum retries even with subscriber (default: 2)
//
//==============================================================================

#include <rclcpp/rclcpp.hpp>
#include <std_msgs/msg/string.hpp>
#include <sensor_msgs/msg/nav_sat_fix.hpp>
#include <tak_chat/msg/tak_chat.hpp>

// Comms status from west_point_comms_sim
#include <west_point_comms_sim/msg/comms_status.hpp>

#include <string>
#include <vector>
#include <mutex>
#include <queue>
#include <deque>
#include <set>
#include <map>
#include <sstream>
#include <iomanip>
#include <chrono>
#include <random>
#include <fstream>
#include <algorithm>


//==============================================================================
// Special destination for broadcast messages
//==============================================================================
static const std::string BROADCAST_DESTINATION = "ALL";


//==============================================================================
// LastIncomingMessage - Tracks the last message received from each callsign
//==============================================================================
//
// To ensure replies appear AFTER the incoming message in ATAK's chat view,
// we track the timestamp of each callsign's last message. When we reply,
// we use their timestamp + a delay as our outgoing timestamp.
//
// This is simpler and more reliable than calculating clock offsets because:
//   1. It guarantees ordering regardless of clock drift
//   2. It handles network latency naturally
//   3. It works even if their clock jumps around
//
//==============================================================================
struct LastIncomingMessage {
    std::string timestamp;                              // ISO8601 timestamp from their message
    std::chrono::steady_clock::time_point received_at;  // When we received it (for staleness check)
};


//==============================================================================
// QueuedMessage - Message waiting in the send queue
//==============================================================================
//
// Messages are queued here first, then sent one at a time with a minimum
// delay between them. This ensures:
//   1. Messages have distinct, well-separated timestamps
//   2. Messages are sent in strict FIFO order
//   3. TAK server and ATAK can properly order messages
//
//==============================================================================
struct QueuedMessage {
    std::string destination;                // Target callsign
    std::string message;                    // Message text
    std::string override_uid;               // Optional: if non-empty, use this UID instead of random
    std::chrono::steady_clock::time_point queued_time;  // When added to queue
};


//==============================================================================
// PendingMessage - Tracks message delivery (retry logic)
//==============================================================================
// 
// After a message is sent from the queue, it moves here for retry tracking.
// This handles DDS discovery races by retrying until delivery is confirmed.
//
// DELIVERY STRATEGY:
// ------------------
// Since we can't get true end-to-end ACKs from the TAK server, we use a
// "belt and suspenders" approach:
//
//   1. Publish immediately when sent from queue
//   2. Keep retrying at regular intervals
//   3. Mark complete when BOTH conditions are met:
//      a) At least one subscriber is connected to send_to_tak
//      b) Minimum number of retries have been sent
//   4. Continue until timeout if no subscriber ever appears
//
//==============================================================================
struct PendingMessage {
    // Message identification
    std::string destination;                // Target callsign
    std::string message;                    // Message text
    std::string send_timestamp;             // Timestamp when actually sent (for CoT)
    std::string cot_xml;                    // Generated CoT XML (with unique UID)
    
    // Timing
    std::chrono::steady_clock::time_point created_time;      // When message was queued
    std::chrono::steady_clock::time_point last_publish_time; // When last publish occurred
    
    // Retry tracking
    int publish_count;                      // Total publishes so far
    bool subscriber_seen;                   // Have we ever seen a subscriber?
    bool complete;                          // Ready for removal from pending map
};


class TakChatNode : public rclcpp::Node
{
public:
    //==========================================================================
    // Constructor - Initialize node, parameters, publishers, and subscribers
    //==========================================================================
    TakChatNode(const rclcpp::NodeOptions& options = rclcpp::NodeOptions())
        : Node("tak_chat_node", options),
          fix_received_(false),
          current_lat_(0.0),
          current_lon_(0.0)
    {
        //----------------------------------------------------------------------
        // Declare Parameters
        //----------------------------------------------------------------------
        
        // Identity parameters - how this robot identifies itself in TAK
        this->declare_parameter<std::string>("callsign", "warthog1");
        this->declare_parameter<std::string>("android_id", "dfac01d76beec661");
        this->declare_parameter<std::string>("tak_server_flow_tag_key",
            "TAK-Server-d520578543014e9cba1916fad77b9917");

        // Topic configuration - where messages flow
        this->declare_parameter<std::string>("outgoing_cot_topic", "send_to_tak");
        this->declare_parameter<std::string>("incoming_cot_topic", "incoming_cot");
        this->declare_parameter<std::string>("navsat_topic", "navsat");
        this->declare_parameter<std::string>("tak_chat_out_topic", "tak_chat/out");
        this->declare_parameter<std::string>("tak_chat_in_topic", "tak_chat/in");
        this->declare_parameter<std::string>("comms_topic", "comms");

        // Allowed callsigns configuration
        this->declare_parameter<std::string>("allowed_callsigns_file",
            "/phoenix/src/phoenix-tak/src/tak_bridge/config/cot_runner.yaml");

        // Retry/reliability parameters - tuned for DDS discovery timing
        this->declare_parameter<double>("retry_timeout_s", 10.0);     // Total retry window
        this->declare_parameter<double>("retry_interval_s", 1.0);     // Time between publishes
        this->declare_parameter<int>("min_retry_count", 1);           // Min publishes even with subscriber
        
        // Send queue parameters - ensures messages have distinct timestamps
        this->declare_parameter<double>("send_delay_s", 1.0);         // Min delay between sends to same dest
        this->declare_parameter<double>("reply_delay_s", 1.0);        // Buffer added to their timestamp before replying

        //----------------------------------------------------------------------
        // Read Parameters
        //----------------------------------------------------------------------
        callsign_ = this->get_parameter("callsign").as_string();
        android_id_ = this->get_parameter("android_id").as_string();
        tak_server_flow_tag_key_ = this->get_parameter("tak_server_flow_tag_key").as_string();

        const std::string outgoing_cot_topic = this->get_parameter("outgoing_cot_topic").as_string();
        const std::string incoming_cot_topic = this->get_parameter("incoming_cot_topic").as_string();
        const std::string navsat_topic = this->get_parameter("navsat_topic").as_string();
        const std::string tak_chat_out_topic = this->get_parameter("tak_chat_out_topic").as_string();
        const std::string tak_chat_in_topic = this->get_parameter("tak_chat_in_topic").as_string();
        const std::string comms_topic = this->get_parameter("comms_topic").as_string();
        const std::string allowed_callsigns_file = this->get_parameter("allowed_callsigns_file").as_string();

        retry_timeout_s_ = this->get_parameter("retry_timeout_s").as_double();
        retry_interval_s_ = this->get_parameter("retry_interval_s").as_double();
        min_retry_count_ = this->get_parameter("min_retry_count").as_int();
        send_delay_s_ = this->get_parameter("send_delay_s").as_double();
        reply_delay_s_ = this->get_parameter("reply_delay_s").as_double();

        //----------------------------------------------------------------------
        // Load Allowed Callsigns from YAML
        //----------------------------------------------------------------------
        allowed_callsigns_ = loadAllowedCallsigns(allowed_callsigns_file);
        
        // Build a set for O(1) lookup when filtering incoming messages
        for (const auto& cs : allowed_callsigns_) {
            allowed_callsigns_set_.insert(cs);
        }

        //----------------------------------------------------------------------
        // QoS Configuration
        //----------------------------------------------------------------------
        // RELIABLE: Ensures delivery confirmation
        // VOLATILE: Works best with Fast-DDS for both short and long-lived publishers
        //
        // NOTE: We use VOLATILE instead of TRANSIENT_LOCAL because:
        //   1. Fast-DDS has issues with TRANSIENT_LOCAL for short-lived publishers
        //   2. Long-lived publishers (BT nodes, TakChatInterface) work fine with VOLATILE
        //   3. All publishers and subscribers MUST match durability for compatibility
        //   4. Short-lived test scripts should use tak_chat_console.py instead
        const auto qos_reliable = rclcpp::QoS(rclcpp::KeepLast(10))
                          .reliable()
                          .durability_volatile();

        //----------------------------------------------------------------------
        // Publishers
        //----------------------------------------------------------------------
        
        // Outgoing CoT XML -> TAK bridge pipeline
        pub_cot_ = this->create_publisher<std_msgs::msg::String>(
            outgoing_cot_topic, qos_reliable);
        
        // Parsed incoming messages -> BT nodes
        pub_tak_chat_in_ = this->create_publisher<tak_chat::msg::TakChat>(
            tak_chat_in_topic, qos_reliable);

        //----------------------------------------------------------------------
        // Subscribers
        //----------------------------------------------------------------------
        
        // GPS location for embedding in outgoing CoT
        sub_navsat_ = this->create_subscription<sensor_msgs::msg::NavSatFix>(
            navsat_topic, qos_reliable,
            std::bind(&TakChatNode::navsatCallback, this, std::placeholders::_1));

        // Comms status from west_point_comms_sim
        // This tells us which destinations are reachable via mesh network
        sub_comms_status_ = this->create_subscription<west_point_comms_sim::msg::CommsStatus>(
            comms_topic, qos_reliable,
            std::bind(&TakChatNode::commsStatusCallback, this, std::placeholders::_1));

        // TakChat requests from BT nodes and test scripts
        // We use event callbacks to log when publishers connect/disconnect
        // This helps debug discovery timing issues with Fast-DDS
        rclcpp::SubscriptionOptions sub_options;
        sub_options.event_callbacks.matched_callback = 
            [this](rclcpp::MatchedInfo& info) {
                if (info.current_count_change > 0) {
                    RCLCPP_INFO(this->get_logger(),
                        "[tak_chat/out] New publisher matched! Total publishers: %zu",
                        info.current_count);
                } else {
                    RCLCPP_INFO(this->get_logger(),
                        "[tak_chat/out] Publisher unmatched. Total publishers: %zu",
                        info.current_count);
                }
            };
        sub_options.event_callbacks.incompatible_qos_callback =
            [this](rclcpp::QOSRequestedIncompatibleQoSInfo& info) {
                RCLCPP_WARN(this->get_logger(),
                    "[tak_chat/out] Incompatible QoS detected! Policy: %d, count: %d",
                    info.last_policy_kind, info.total_count);
            };
        
        sub_tak_chat_out_ = this->create_subscription<tak_chat::msg::TakChat>(
            tak_chat_out_topic, qos_reliable,
            std::bind(&TakChatNode::takChatOutCallback, this, std::placeholders::_1),
            sub_options);

        // Incoming CoT from TAK bridge
        sub_incoming_cot_ = this->create_subscription<std_msgs::msg::String>(
            incoming_cot_topic, qos_reliable,
            std::bind(&TakChatNode::incomingCotCallback, this, std::placeholders::_1));

        //----------------------------------------------------------------------
        // Timers
        //----------------------------------------------------------------------
        
        // Retry timer - handles message retries for reliability
        retry_timer_ = this->create_wall_timer(
            std::chrono::milliseconds(100),
            std::bind(&TakChatNode::retryTimerCallback, this));
        
        // Send queue timer - processes queued messages with delay
        send_queue_timer_ = this->create_wall_timer(
            std::chrono::milliseconds(100),
            std::bind(&TakChatNode::sendQueueTimerCallback, this));

        //----------------------------------------------------------------------
        // Startup Logging
        //----------------------------------------------------------------------
        RCLCPP_INFO(this->get_logger(), "TakChatNode initialized:");
        RCLCPP_INFO(this->get_logger(), "  Callsign:             %s", callsign_.c_str());
        RCLCPP_INFO(this->get_logger(), "  TakChat OUT topic:    %s", tak_chat_out_topic.c_str());
        RCLCPP_INFO(this->get_logger(), "  TakChat IN topic:     %s", tak_chat_in_topic.c_str());
        RCLCPP_INFO(this->get_logger(), "  Outgoing CoT topic:   %s", outgoing_cot_topic.c_str());
        RCLCPP_INFO(this->get_logger(), "  Incoming CoT topic:   %s", incoming_cot_topic.c_str());
        RCLCPP_INFO(this->get_logger(), "  Comms topic:          %s", comms_topic.c_str());
        RCLCPP_INFO(this->get_logger(), "  Send delay:           %.1fs (between consecutive messages)", send_delay_s_);
        RCLCPP_INFO(this->get_logger(), "  Reply delay:          %.1fs (buffer added to their timestamp)", reply_delay_s_);
        RCLCPP_INFO(this->get_logger(), "  Retry timeout:        %.1fs", retry_timeout_s_);
        RCLCPP_INFO(this->get_logger(), "  Retry interval:       %.1fs", retry_interval_s_);
        RCLCPP_INFO(this->get_logger(), "  Min retry count:      %d", min_retry_count_);
        RCLCPP_INFO(this->get_logger(), "  Allowed callsigns:    %zu loaded", allowed_callsigns_.size());
        RCLCPP_INFO(this->get_logger(), "  Broadcast destination: '%s' -> fans out to all allowed",
                    BROADCAST_DESTINATION.c_str());
        RCLCPP_INFO(this->get_logger(), "  Comms mode:           Checking for 'base_station' in comms_status");
        RCLCPP_INFO(this->get_logger(), "                        (if comms_sim not running, defaults to comms OK)");
        RCLCPP_INFO(this->get_logger(), "  UID override:         Supported via TakChat.uid field (for testing)");
        RCLCPP_INFO(this->get_logger(), "TakChatNode starting...");
    }

private:
    //==========================================================================
    // YAML Parsing - Load Allowed Callsigns
    //==========================================================================
    
    /**
     * @brief Load allowed callsigns from cot_runner.yaml configuration file.
     */
    std::vector<std::string> loadAllowedCallsigns(const std::string& yaml_path)
    {
        std::vector<std::string> callsigns;
        std::ifstream file(yaml_path);
        
        if (!file.is_open()) {
            RCLCPP_ERROR(this->get_logger(), 
                "Could not open allowed callsigns file: %s", yaml_path.c_str());
            return callsigns;
        }
        
        std::string line;
        bool in_allowed_section = false;
        
        while (std::getline(file, line)) {
            if (line.find("allowed:") != std::string::npos) {
                in_allowed_section = true;
                if (line.find('[') != std::string::npos && 
                    line.find(']') != std::string::npos) {
                    extractCallsignsFromLine(line, callsigns);
                    in_allowed_section = false;
                }
                continue;
            }
            
            if (in_allowed_section) {
                if (line.find(']') != std::string::npos) {
                    extractCallsignsFromLine(line, callsigns);
                    in_allowed_section = false;
                    continue;
                }
                extractCallsignsFromLine(line, callsigns);
            }
        }
        
        file.close();
        
        std::string callsign_list;
        for (size_t i = 0; i < callsigns.size(); i++) {
            callsign_list += callsigns[i];
            if (i < callsigns.size() - 1) callsign_list += ", ";
        }
        RCLCPP_INFO(this->get_logger(), "Loaded allowed callsigns: %s", callsign_list.c_str());
        
        return callsigns;
    }
    
    void extractCallsignsFromLine(const std::string& line, std::vector<std::string>& callsigns)
    {
        size_t pos = 0;
        while ((pos = line.find('"', pos)) != std::string::npos) {
            size_t end = line.find('"', pos + 1);
            if (end != std::string::npos) {
                std::string callsign = line.substr(pos + 1, end - pos - 1);
                if (!callsign.empty()) {
                    callsigns.push_back(callsign);
                }
                pos = end + 1;
            } else {
                break;
            }
        }
    }

    //==========================================================================
    // Callback: NavSatFix - Update current GPS position
    //==========================================================================
    
    void navsatCallback(const sensor_msgs::msg::NavSatFix::SharedPtr msg)
    {
        std::lock_guard<std::mutex> lk(fix_mtx_);
        current_lat_ = msg->latitude;
        current_lon_ = msg->longitude;

        if (!fix_received_) {
            fix_received_ = true;
            RCLCPP_INFO(this->get_logger(), "GPS fix received (%.6f, %.6f)",
                        current_lat_, current_lon_);
        }
    }

    //==========================================================================
    // Callback: CommsStatus - Update mesh network connectivity
    //==========================================================================
    
    /**
     * @brief Handle comms status updates from west_point_comms_sim.
     * 
     * The CommsStatus message contains:
     *   - direct[]: Entities this robot can communicate with directly
     *   - transitive[]: Entities reachable through the mesh network
     * 
     * TAK COMMUNICATION MODEL:
     * ------------------------
     * All TAK/ATAK messages go through the base_station (the TAK server).
     * The destination callsigns (like "White Cell", "TRILL") are ATAK users
     * that are all reached through the base_station.
     * 
     * Therefore, the comms check is binary:
     *   - "base_station" in transitive → CAN send TAK messages
     *   - "base_station" NOT in transitive → CANNOT send TAK messages
     * 
     * If this callback is never called (comms_sim not running), we default
     * to allowing all messages (assumes comms are up).
     * 
     * @param msg The CommsStatus message from west_point_comms_sim
     */
    void commsStatusCallback(const west_point_comms_sim::msg::CommsStatus::SharedPtr msg)
    {
        std::lock_guard<std::mutex> lock(comms_mutex_);
        
        // Check if base_station is reachable (either directly or transitively)
        bool new_has_comms = false;
        
        // Check transitive list (includes multi-hop paths)
        for (const auto& entity : msg->transitive) {
            if (entity == "base_station") {
                new_has_comms = true;
                break;
            }
        }
        
        // Also check direct list (should be subset of transitive, but be safe)
        if (!new_has_comms) {
            for (const auto& entity : msg->direct) {
                if (entity == "base_station") {
                    new_has_comms = true;
                    break;
                }
            }
        }
        
        // First time receiving comms status - log the transition
        if (!comms_status_received_) {
            comms_status_received_ = true;
            RCLCPP_INFO(this->get_logger(),
                "[Comms] First comms_status received - base_station %s",
                new_has_comms ? "REACHABLE (TAK enabled)" : "UNREACHABLE (TAK disabled)");
        }
        // Log state changes
        else if (new_has_comms != has_base_station_comms_) {
            if (new_has_comms) {
                RCLCPP_INFO(this->get_logger(),
                    "[Comms] Connection to base_station RESTORED - TAK messaging enabled");
            } else {
                RCLCPP_WARN(this->get_logger(),
                    "[Comms] Connection to base_station LOST - TAK messaging disabled");
            }
        }
        
        has_base_station_comms_ = new_has_comms;
    }
    
    /**
     * @brief Check if TAK messages can be sent (base_station is reachable).
     * 
     * BEHAVIOR:
     *   - If comms_status has never been received (comms_sim not running):
     *     Returns true (default to allowing TAK messages)
     *   - If comms_status is active:
     *     Returns true only if "base_station" is in the transitive list
     * 
     * @return true if TAK messages can be sent
     */
    bool hasComms()
    {
        std::lock_guard<std::mutex> lock(comms_mutex_);
        
        // If we've never received comms_status, assume comms are up
        // This is the "comms_sim not running" fallback
        if (!comms_status_received_) {
            return true;
        }
        
        return has_base_station_comms_;
    }
    
    /**
     * @brief Get the current comms status for logging/debugging.
     * 
     * @return A string describing the current comms state
     */
    std::string getCommsStatusString()
    {
        std::lock_guard<std::mutex> lock(comms_mutex_);
        
        if (!comms_status_received_) {
            return "no comms_status received (defaulting to comms OK)";
        }
        
        return has_base_station_comms_ 
            ? "base_station REACHABLE" 
            : "base_station UNREACHABLE";
    }

    //==========================================================================
    // Callback: TakChat OUT (ROS -> TAK)
    //==========================================================================
    
    /**
     * @brief Handle outgoing chat requests from BT nodes.
     * 
     * Messages are checked for base_station connectivity before being queued:
     *   - If base_station is reachable (or comms_sim not running): queue message
     *   - If base_station is unreachable: suppress with warning
     * 
     * For broadcast messages ("ALL"), all destinations are queued if comms are up,
     * or all are suppressed if comms are down.
     * 
     * UID OVERRIDE SUPPORT:
     * ---------------------
     * If the TakChat message includes a non-empty 'uid' field, that UID will be
     * passed through to the CoT generation instead of generating a random one.
     * This allows testing whether ATAK treats messages with identical UIDs as
     * updates/replacements rather than new messages.
     */
    void takChatOutCallback(const tak_chat::msg::TakChat::SharedPtr msg)
    {
        // Parse the uid field which encodes: "event_uid|message_id"
        std::string uid_field = msg->uid;
        std::string event_uid_part;
        std::string message_id_part;
        
        size_t separator_pos = uid_field.find('|');
        if (separator_pos != std::string::npos) {
            // Format: "event_uid|message_id"
            event_uid_part = uid_field.substr(0, separator_pos);
            message_id_part = uid_field.substr(separator_pos + 1);
        } else if (!uid_field.empty()) {
            // Legacy format: use same value for both (backward compatible)
            event_uid_part = uid_field;
            message_id_part = uid_field;
        }
        
        // Build log string
        std::string uid_info = "";
        if (!event_uid_part.empty() || !message_id_part.empty()) {
            std::ostringstream oss;
            oss << " [";
            if (!event_uid_part.empty()) {
                oss << "EventUID=" << event_uid_part.substr(0, std::min(size_t(8), event_uid_part.length())) << "...";
            } else {
                oss << "EventUID=random";
            }
            oss << ", ";
            if (!message_id_part.empty()) {
                oss << "MsgID=" << message_id_part.substr(0, std::min(size_t(8), message_id_part.length())) << "...";
            } else {
                oss << "MsgID=random";
            }
            oss << "]";
            uid_info = oss.str();
        }
        
        RCLCPP_INFO(this->get_logger(),
            "[TakChat RECV] origin='%s' dest='%s' msg='%s'%s",
            msg->origin.c_str(), msg->destination.c_str(), msg->message.c_str(), uid_info.c_str());
        
        // Verify the message is from this robot
        if (msg->origin != callsign_) {
            RCLCPP_WARN(this->get_logger(),
                "[TakChat REJECTED] origin='%s' does not match our callsign='%s'",
                msg->origin.c_str(), callsign_.c_str());
            return;
        }

        // Check if we have comms to base_station (TAK server)
        if (!hasComms()) {
            RCLCPP_WARN(this->get_logger(),
                "[TakChat SUPPRESSED] No comms to base_station - message not sent: '%s'",
                msg->message.c_str());
            return;
        }

        // Pass the uid field through (will be decoded again in buildGeoChatCoT)
        std::string override_uid = msg->uid;

        // Handle broadcast vs single destination
        if (msg->destination == BROADCAST_DESTINATION) {
            // Fan out to all allowed callsigns
            RCLCPP_INFO(this->get_logger(),
                "[TakChat BROADCAST] Queuing to %zu destinations | Message: %s%s",
                allowed_callsigns_.size(), msg->message.c_str(), uid_info.c_str());
            
            for (const auto& dest : allowed_callsigns_) {
                addToSendQueue(dest, msg->message, override_uid);
            }
            
        } else {
            // Single destination
            RCLCPP_INFO(this->get_logger(),
                "[TakChat QUEUED] To: %s | Message: %s%s",
                msg->destination.c_str(), msg->message.c_str(), uid_info.c_str());
            
            addToSendQueue(msg->destination, msg->message, override_uid);
        }
        
        // Log queue size for debugging
        std::lock_guard<std::mutex> lock(send_queue_mutex_);
        if (!send_queue_.empty()) {
            RCLCPP_INFO(this->get_logger(),
                "[TakChat QUEUE] %zu message(s) waiting to be sent", send_queue_.size());
        }
    }

    //==========================================================================
    // Send Queue - Ensures messages are sent with delay between them
    //==========================================================================
    
    /**
     * @brief Add a message to the send queue.
     * 
     * Messages wait in the queue until either:
     *   - It's the first message to this destination, OR
     *   - send_delay_s has elapsed since the last send to this destination
     * 
     * This allows simultaneous sends to DIFFERENT destinations while
     * maintaining proper timestamp ordering for the SAME destination.
     * 
     * @param destination Target callsign
     * @param message Message text to send
     * @param override_uid Optional UID to use instead of random generation
     */
    void addToSendQueue(const std::string& destination, 
                       const std::string& message,
                       const std::string& override_uid = "")
    {
        QueuedMessage queued;
        queued.destination = destination;
        queued.message = message;
        queued.override_uid = override_uid;
        queued.queued_time = std::chrono::steady_clock::now();
        
        std::lock_guard<std::mutex> lock(send_queue_mutex_);
        send_queue_.push_back(queued);
    }
    
    /**
     * @brief Timer callback to process the send queue.
     * 
     * SIMULTANEOUS MULTI-DESTINATION SENDING:
     * ---------------------------------------
     * Messages to DIFFERENT destinations can be sent simultaneously because
     * each destination has its own independent chat thread.
     * 
     * The delay (send_delay_s) only applies when sending consecutive messages
     * to the SAME destination, ensuring distinct timestamps for proper ordering.
     * 
     * For example, a broadcast to 5 callsigns will send all 5 messages at once
     * (or within the same timer tick), rather than taking 5 seconds.
     */
    void sendQueueTimerCallback()
    {
        auto now = std::chrono::steady_clock::now();
        auto wall_now = std::chrono::system_clock::now();
        double current_wall_time = std::chrono::duration<double>(
            wall_now.time_since_epoch()).count();
        
        // Collect messages that are ready to send
        std::vector<QueuedMessage> ready_to_send;
        
        {
            std::lock_guard<std::mutex> lock(send_queue_mutex_);
            
            if (send_queue_.empty()) {
                return;  // Nothing to send
            }
            
            // Iterate through queue and collect messages ready to send
            // We'll remove them after collecting to avoid iterator invalidation
            auto it = send_queue_.begin();
            while (it != send_queue_.end()) {
                const std::string& dest = it->destination;
                
                // Check both send delay (between consecutive sends) AND reply delay
                // (waiting until we've passed their message timestamp)
                bool can_send = true;
                
                // Check 1: Have we sent to this destination recently?
                auto last_send_it = last_send_time_per_dest_.find(dest);
                if (last_send_it != last_send_time_per_dest_.end()) {
                    double since_last_send = std::chrono::duration<double>(
                        now - last_send_it->second).count();
                    
                    if (since_last_send < send_delay_s_) {
                        // Not ready yet - need to wait between consecutive sends
                        can_send = false;
                    }
                }
                
                // Check 2: Have we passed their message timestamp?
                // This handles clock skew - we wait until our wall clock has passed
                // their timestamp + buffer, ensuring our reply arrives at TAK server
                // after their message's server-assigned timestamp.
                auto earliest_it = earliest_reply_time_per_dest_.find(dest);
                if (can_send && earliest_it != earliest_reply_time_per_dest_.end()) {
                    double earliest_reply_time = earliest_it->second;
                    
                    if (current_wall_time < earliest_reply_time) {
                        // Not ready yet - our clock hasn't passed their timestamp
                        double wait_remaining = earliest_reply_time - current_wall_time;
                        RCLCPP_DEBUG(this->get_logger(),
                            "[Reply Delay] Waiting %.1fs more before sending to %s (clock skew handling)",
                            wait_remaining, dest.c_str());
                        can_send = false;
                    }
                }
                
                if (can_send) {
                    ready_to_send.push_back(*it);
                    // Update last send time for this destination
                    last_send_time_per_dest_[dest] = now;
                    // Remove from queue
                    it = send_queue_.erase(it);
                } else {
                    ++it;
                }
            }
        }
        
        // Now send all ready messages (outside the lock)
        for (auto& queued : ready_to_send) {
            // Re-check connectivity at send time
            if (!hasComms()) {
                RCLCPP_WARN(this->get_logger(),
                    "[TakChat DROPPED] Comms to base_station lost while queued - "
                    "message dropped: '%s' (to: %s)",
                    queued.message.c_str(), queued.destination.c_str());
                continue;
            }
            
            std::string uid_info = queued.override_uid.empty() ? "" : " [UID: " + queued.override_uid + "]";
            RCLCPP_INFO(this->get_logger(),
                "[TakChat SENDING] To: %s | Message: '%s'%s",
                queued.destination.c_str(), queued.message.c_str(), uid_info.c_str());
            
            // Send the message (timestamp determined by sendMessage based on
            // any incoming messages from this destination)
            sendMessage(queued.destination, queued.message, queued.override_uid);
        }
        
        // Log if there are still messages waiting
        {
            std::lock_guard<std::mutex> lock(send_queue_mutex_);
            if (!send_queue_.empty()) {
                RCLCPP_DEBUG(this->get_logger(),
                    "[TakChat QUEUE] %zu message(s) still waiting (rate-limited)",
                    send_queue_.size());
            }
        }
    }

    //==========================================================================
    // Send a message with retry logic
    //==========================================================================
    
    /**
     * @brief Send a message and add to pending for retry tracking.
     * 
     * This is called from the send queue timer when it's time to actually
     * send a message. The timestamp is determined by getResponseTimestamp()
     * to ensure proper ordering relative to any incoming messages from this
     * destination.
     * 
     * @param destination Target callsign
     * @param message Message text to send
     * @param override_uid Optional UID to use instead of random generation
     */
    void sendMessage(const std::string& destination, 
                     const std::string& message,
                     const std::string& override_uid = "")
    {
        std::string key = destination + "|" + message;

        {
            std::lock_guard<std::mutex> lock(pending_mutex_);
            
            if (pending_messages_.find(key) != pending_messages_.end()) {
                RCLCPP_WARN(this->get_logger(),
                    "[TakChat DEDUP] Message to %s already pending, skipping: '%s'",
                    destination.c_str(), message.c_str());
                return;
            }
        }

        // Get the appropriate timestamp for this destination
        // This ensures our reply appears AFTER their last message in ATAK
        std::string timestamp = getResponseTimestamp(destination);

        // Build CoT XML with the response timestamp and optional UID override
        std::string cot_xml = buildGeoChatCoT(callsign_, destination, message, timestamp, override_uid);

        PendingMessage pending;
        pending.destination = destination;
        pending.message = message;
        pending.send_timestamp = timestamp;
        pending.cot_xml = cot_xml;
        pending.created_time = std::chrono::steady_clock::now();
        pending.last_publish_time = std::chrono::steady_clock::time_point::min();
        pending.publish_count = 0;
        pending.subscriber_seen = false;
        pending.complete = false;

        {
            std::lock_guard<std::mutex> lock(pending_mutex_);
            pending_messages_[key] = pending;
        }

        doPublish(key);
    }

    //==========================================================================
    // Retry Timer - Core reliability mechanism
    //==========================================================================
    
    void retryTimerCallback()
    {
        auto now = std::chrono::steady_clock::now();
        std::vector<std::string> to_remove;
        std::vector<std::string> to_publish;
        
        size_t subscriber_count = pub_cot_->get_subscription_count();
        
        {
            std::lock_guard<std::mutex> lock(pending_mutex_);
            
            for (auto& [key, pending] : pending_messages_) {
                if (pending.complete) {
                    to_remove.push_back(key);
                    continue;
                }
                
                double elapsed_s = std::chrono::duration<double>(
                    now - pending.created_time).count();
                
                if (elapsed_s >= retry_timeout_s_) {
                    if (pending.subscriber_seen) {
                        RCLCPP_WARN(this->get_logger(),
                            "[TakChat TIMEOUT] To: %s after %d publishes (%.1fs) - "
                            "subscriber was seen, message likely delivered",
                            pending.destination.c_str(), pending.publish_count, elapsed_s);
                    } else {
                        RCLCPP_ERROR(this->get_logger(),
                            "[TakChat TIMEOUT] To: %s after %d publishes (%.1fs) - "
                            "NO SUBSCRIBER EVER SEEN, message likely LOST",
                            pending.destination.c_str(), pending.publish_count, elapsed_s);
                    }
                    to_remove.push_back(key);
                    continue;
                }
                
                if (subscriber_count > 0) {
                    if (!pending.subscriber_seen) {
                        RCLCPP_DEBUG(this->get_logger(),
                            "Subscriber discovered for message to %s",
                            pending.destination.c_str());
                    }
                    pending.subscriber_seen = true;
                }
                
                if (pending.subscriber_seen && pending.publish_count >= min_retry_count_) {
                    RCLCPP_INFO(this->get_logger(),
                        "[TakChat DELIVERED] To: %s after %d publish(es)",
                        pending.destination.c_str(), pending.publish_count);
                    pending.complete = true;
                    to_remove.push_back(key);
                    continue;
                }
                
                if (pending.publish_count > 0) {
                    double since_last_publish_s = std::chrono::duration<double>(
                        now - pending.last_publish_time).count();
                    
                    if (since_last_publish_s >= retry_interval_s_) {
                        to_publish.push_back(key);
                    }
                }
            }
        }
        
        for (const auto& key : to_publish) {
            doPublish(key);
        }
        
        {
            std::lock_guard<std::mutex> lock(pending_mutex_);
            for (const auto& key : to_remove) {
                pending_messages_.erase(key);
            }
        }
    }

    //==========================================================================
    // Publish CoT XML
    //==========================================================================
    
    void doPublish(const std::string& key)
    {
        std::string cot_xml;
        std::string destination;
        std::string send_timestamp;
        int publish_count;
        size_t subscriber_count;
        
        {
            std::lock_guard<std::mutex> lock(pending_mutex_);
            
            auto it = pending_messages_.find(key);
            if (it == pending_messages_.end()) {
                return;
            }
            
            PendingMessage& pending = it->second;
            pending.publish_count++;
            pending.last_publish_time = std::chrono::steady_clock::now();
            
            cot_xml = pending.cot_xml;
            destination = pending.destination;
            send_timestamp = pending.send_timestamp;
            publish_count = pending.publish_count;
        }
        
        subscriber_count = pub_cot_->get_subscription_count();
        
        std_msgs::msg::String out_msg;
        out_msg.data = cot_xml;
        pub_cot_->publish(out_msg);
        
        if (publish_count == 1) {
            RCLCPP_INFO(this->get_logger(),
                "[TakChat SEND #1] To: %s | timestamp=%s | subscribers: %zu",
                destination.c_str(), send_timestamp.c_str(), subscriber_count);
        } else {
            RCLCPP_INFO(this->get_logger(),
                "[TakChat RETRY #%d] To: %s | subscribers: %zu",
                publish_count, destination.c_str(), subscriber_count);
        }
    }

    //==========================================================================
    // Callback: Incoming CoT (TAK -> ROS)
    //==========================================================================
    
    void incomingCotCallback(const std_msgs::msg::String::SharedPtr msg)
    {
        // Only process GeoChat messages (type="b-t-f")
        if (msg->data.find("type=\"b-t-f\"") == std::string::npos) {
            return;
        }

        std::string sender, recipient, message, timestamp;
        if (!parseGeoChatCoT(msg->data, sender, recipient, message, timestamp)) {
            RCLCPP_WARN(this->get_logger(), "Failed to parse incoming GeoChat CoT");
            return;
        }

        // Ignore our own messages
        if (sender == callsign_) {
            return;
        }

        // Filter to only allowed callsigns
        if (allowed_callsigns_set_.find(sender) == allowed_callsigns_set_.end()) {
            RCLCPP_DEBUG(this->get_logger(),
                "[TakChat IN] IGNORED - sender '%s' not in allowed list",
                sender.c_str());
            return;
        }

        // Record this message's timestamp so when we reply, our response
        // will have a timestamp that appears AFTER theirs in ATAK's chat
        recordIncomingMessage(sender, timestamp);

        RCLCPP_INFO(this->get_logger(),
            "[TakChat IN] From: %s | To: %s | Message: %s",
            sender.c_str(), recipient.c_str(), message.c_str());

        tak_chat::msg::TakChat tak_msg;
        tak_msg.origin = sender;
        tak_msg.destination = recipient;
        tak_msg.message = message;
        tak_msg.timestamp = timestamp;
        tak_msg.uid = "";  // Not needed for incoming messages

        pub_tak_chat_in_->publish(tak_msg);
    }
    
    //==========================================================================
    // Last Incoming Message Tracking (for proper response ordering)
    //==========================================================================
    
    /**
     * @brief Record the timestamp of an incoming message from a callsign.
     * 
     * This function serves TWO purposes:
     * 
     * 1. TIMESTAMP TRACKING: Records their message timestamp so that when we
     *    reply, we can use their_timestamp + RESPONSE_DELAY_S to ensure our
     *    reply appears AFTER their message in ATAK's chat view.
     * 
     * 2. SEND DELAY ENFORCEMENT: Stores their parsed timestamp so the send
     *    queue can wait until our clock has passed their timestamp before
     *    sending. This handles clock skew between devices - if their clock
     *    is ahead of ours, we wait longer; if behind, we can send sooner.
     * 
     *    This is necessary because ATAK sorts messages by the `event time`
     *    attribute, which the TAK server sets based on when IT receives the
     *    message. With clock skew, a fixed delay doesn't work - we need to
     *    ensure our message arrives at the TAK server AFTER their message's
     *    timestamp.
     * 
     * @param callsign The sender's callsign
     * @param their_timestamp ISO8601 timestamp from their message
     */
    void recordIncomingMessage(const std::string& callsign, const std::string& their_timestamp)
    {
        // Parse their timestamp to get seconds since epoch
        double their_time_seconds = parseISOTimestamp(their_timestamp);
        
        // Record their timestamp for proper response timestamp calculation
        {
            std::lock_guard<std::mutex> lock(last_incoming_mutex_);
            
            LastIncomingMessage msg;
            msg.timestamp = their_timestamp;
            msg.received_at = std::chrono::steady_clock::now();
            
            last_incoming_messages_[callsign] = msg;
            
            RCLCPP_DEBUG(this->get_logger(),
                "[Timestamp] Recorded message from %s at %s",
                callsign.c_str(), their_timestamp.c_str());
        }
        
        // CRITICAL: Store their parsed timestamp for reply delay enforcement
        // The send queue will wait until our clock passes their_time + buffer
        // before sending any reply. This handles clock skew automatically:
        // - If their clock is ahead: we wait longer
        // - If their clock is behind: we can send sooner
        //
        // SAFETY CAP: We never wait more than MAX_REPLY_WAIT_S to handle cases
        // where a device's clock is wildly off (hours/days ahead).
        {
            std::lock_guard<std::mutex> lock(send_queue_mutex_);
            
            auto now = std::chrono::system_clock::now();
            double current_time = std::chrono::duration<double>(
                now.time_since_epoch()).count();
            
            if (their_time_seconds > 0) {
                // Calculate when we'd ideally reply (their time + small buffer)
                double ideal_reply_time = their_time_seconds + reply_delay_s_;
                
                // Calculate the maximum time we're willing to wait
                double max_reply_time = current_time + MAX_REPLY_WAIT_S;
                
                // Use the earlier of the two (cap the wait time)
                double earliest_reply_time = std::min(ideal_reply_time, max_reply_time);
                earliest_reply_time_per_dest_[callsign] = earliest_reply_time;
                
                // Calculate how long we'll wait from now (for logging)
                double wait_time = earliest_reply_time - current_time;
                
                if (ideal_reply_time > max_reply_time) {
                    // Clock skew is large - we're capping the wait
                    RCLCPP_WARN(this->get_logger(),
                        "[Reply Delay] Their clock is %.1fs ahead! Capping wait to %.1fs for %s",
                        (their_time_seconds - current_time), MAX_REPLY_WAIT_S, callsign.c_str());
                } else if (wait_time > 0) {
                    RCLCPP_INFO(this->get_logger(),
                        "[Reply Delay] Will wait %.1fs before sending to %s (their clock is %.1fs ahead)",
                        wait_time, callsign.c_str(), (their_time_seconds - current_time));
                } else {
                    RCLCPP_INFO(this->get_logger(),
                        "[Reply Delay] Can send to %s immediately (their clock is %.1fs behind)",
                        callsign.c_str(), (current_time - their_time_seconds));
                }
            } else {
                // Failed to parse their timestamp - fall back to current time + small delay
                earliest_reply_time_per_dest_[callsign] = current_time + reply_delay_s_;
                
                RCLCPP_WARN(this->get_logger(),
                    "[Reply Delay] Could not parse timestamp from %s, using fixed %.1fs delay",
                    callsign.c_str(), reply_delay_s_);
            }
        }
    }
    
    /**
     * @brief Get the appropriate timestamp for replying to a callsign.
     * 
     * RESPONSE ORDERING LOGIC:
     * ------------------------
     * To ensure our reply appears AFTER their message in ATAK's chat, we use
     * the MAXIMUM of two candidate timestamps:
     * 
     *   1. their_timestamp + RESPONSE_DELAY_S  (relative to their message)
     *   2. current_time + CURRENT_TIME_BUFFER_S (relative to now)
     * 
     * This dual approach handles CLOCK SKEW between devices:
     *   - If their clock is behind ours: (1) might be in our past, so we use (2)
     *   - If their clock is ahead of ours: (1) ensures we're after their message
     * 
     * CRITICAL: After calculating a response timestamp, we UPDATE the stored
     * timestamp to the response value. This ensures that if we send MULTIPLE
     * messages in quick succession, each one gets a progressively LATER timestamp.
     * 
     * STALENESS CHECKS:
     * -----------------
     * We consider a stored timestamp "stale" if it was received more than 5
     * minutes ago. After that, we only use current time.
     * 
     * @param destination The callsign we're replying to
     * @return ISO8601 timestamp to use for our outgoing message
     */
    std::string getResponseTimestamp(const std::string& destination)
    {
        std::lock_guard<std::mutex> lock(last_incoming_mutex_);
        
        // Get current time - we'll need this regardless
        auto now = std::chrono::system_clock::now();
        double current_time_seconds = std::chrono::duration<double>(
            now.time_since_epoch()).count();
        
        auto it = last_incoming_messages_.find(destination);
        
        if (it == last_incoming_messages_.end()) {
            // No recorded message from this destination - use current time + buffer
            double response_time = current_time_seconds + CURRENT_TIME_BUFFER_S;
            std::string response_timestamp = secondsToISO(response_time);
            RCLCPP_DEBUG(this->get_logger(),
                "[Timestamp] No record for %s, using current time + buffer = %s",
                destination.c_str(), response_timestamp.c_str());
            return response_timestamp;
        }
        
        // Check if the record is stale (received more than 5 minutes ago)
        auto age = std::chrono::steady_clock::now() - it->second.received_at;
        if (age > std::chrono::minutes(5)) {
            double response_time = current_time_seconds + CURRENT_TIME_BUFFER_S;
            std::string response_timestamp = secondsToISO(response_time);
            RCLCPP_DEBUG(this->get_logger(),
                "[Timestamp] Record from %s is stale (%.1f min), using current time + buffer = %s",
                destination.c_str(),
                std::chrono::duration<double, std::ratio<60>>(age).count(),
                response_timestamp.c_str());
            return response_timestamp;
        }
        
        // Parse their stored timestamp
        double their_time_seconds = parseISOTimestamp(it->second.timestamp);
        if (their_time_seconds < 0) {
            double response_time = current_time_seconds + CURRENT_TIME_BUFFER_S;
            std::string response_timestamp = secondsToISO(response_time);
            RCLCPP_WARN(this->get_logger(),
                "[Timestamp] Failed to parse timestamp from %s, using current time + buffer = %s",
                destination.c_str(), response_timestamp.c_str());
            return response_timestamp;
        }
        
        // Calculate both candidate timestamps
        double candidate_from_their_time = their_time_seconds + RESPONSE_DELAY_S;
        double candidate_from_current_time = current_time_seconds + CURRENT_TIME_BUFFER_S;
        
        // Use the MAXIMUM to handle clock skew in either direction
        double response_time_seconds = std::max(candidate_from_their_time, candidate_from_current_time);
        
        // Convert back to ISO format
        std::string response_timestamp = secondsToISO(response_time_seconds);
        
        // Log which timestamp we chose
        if (candidate_from_their_time >= candidate_from_current_time) {
            RCLCPP_INFO(this->get_logger(),
                "[Timestamp] Reply to %s: using their_time + delay = %s (their=%s + %.1fs)",
                destination.c_str(), response_timestamp.c_str(),
                it->second.timestamp.c_str(), RESPONSE_DELAY_S);
        } else {
            RCLCPP_INFO(this->get_logger(),
                "[Timestamp] Reply to %s: using current_time + buffer = %s (clock skew detected: their_time was %.1fs behind)",
                destination.c_str(), response_timestamp.c_str(),
                current_time_seconds - their_time_seconds);
        }
        
        // CRITICAL: Update the stored timestamp to be our response timestamp
        // This ensures that the NEXT message to this destination will have
        // a timestamp AFTER this one, maintaining proper ordering
        it->second.timestamp = response_timestamp;
        // Don't reset received_at - we want staleness based on original incoming message
        
        return response_timestamp;
    }
    
    /**
     * @brief Parse an ISO8601 timestamp string to seconds since epoch.
     * 
     * Handles formats like:
     *   - 2026-01-08T15:00:34.114Z
     *   - 2026-01-08T15:00:34Z
     * 
     * @param iso_timestamp The timestamp string
     * @return Seconds since epoch, or -1.0 on parse error
     */
    double parseISOTimestamp(const std::string& iso_timestamp)
    {
        if (iso_timestamp.empty()) return -1.0;
        
        std::tm tm = {};
        double fractional_seconds = 0.0;
        
        // Find the 'T' separator
        size_t t_pos = iso_timestamp.find('T');
        if (t_pos == std::string::npos) return -1.0;
        
        // Parse date: YYYY-MM-DD
        if (sscanf(iso_timestamp.c_str(), "%d-%d-%d",
                   &tm.tm_year, &tm.tm_mon, &tm.tm_mday) != 3) {
            return -1.0;
        }
        tm.tm_year -= 1900;  // Years since 1900
        tm.tm_mon -= 1;      // Months are 0-based
        
        // Parse time: HH:MM:SS
        if (sscanf(iso_timestamp.c_str() + t_pos + 1, "%d:%d:%d",
                   &tm.tm_hour, &tm.tm_min, &tm.tm_sec) != 3) {
            return -1.0;
        }
        
        // Parse optional fractional seconds
        size_t dot_pos = iso_timestamp.find('.', t_pos);
        if (dot_pos != std::string::npos) {
            size_t end_pos = iso_timestamp.find('Z', dot_pos);
            if (end_pos == std::string::npos) {
                end_pos = iso_timestamp.length();
            }
            std::string frac_str = iso_timestamp.substr(dot_pos, end_pos - dot_pos);
            fractional_seconds = std::stod(frac_str);
        }
        
        // Convert to time_t (seconds since epoch, UTC)
        time_t seconds = timegm(&tm);
        if (seconds == -1) return -1.0;
        
        return static_cast<double>(seconds) + fractional_seconds;
    }

    //==========================================================================
    // CoT Parsing
    //==========================================================================
    
    bool parseGeoChatCoT(const std::string& xml,
                         std::string& sender,
                         std::string& recipient,
                         std::string& message,
                         std::string& timestamp)
    {
        // Extract sender callsign from __chat element (required)
        sender = extractXmlAttribute(xml, "senderCallsign");
        if (sender.empty()) return false;

        // Extract recipient - try chatroom first, then to attribute
        recipient = extractXmlAttribute(xml, "chatroom");
        if (recipient.empty()) recipient = extractXmlAttribute(xml, "to");
        if (recipient.empty()) recipient = callsign_;  // Default to us

        // Extract message text from <remarks> element (required)
        message = extractXmlElementContent(xml, "remarks");
        if (message.empty()) return false;
        message = unescapeXml(message);

        // Extract timestamp - prefer remarks time (when user sent message)
        // over event time (when server processed it)
        // 
        // CoT has multiple timestamps:
        //   - event time: When TAK server received/processed message
        //   - remarks time: When ATAK user actually sent the message
        //   - start: When message becomes "valid"
        //
        // The remarks time is most accurate for chat message ordering.
        timestamp = extractRemarksTimeAttribute(xml);
        if (timestamp.empty()) {
            // Fallback to event time if remarks time not found
            timestamp = extractXmlAttribute(xml, "time");
        }
        if (timestamp.empty()) {
            timestamp = nowISO();
        }

        return true;
    }
    
    /**
     * @brief Extract the time attribute from the <remarks> element.
     * 
     * The remarks element looks like:
     *   <remarks source="..." to="..." time="2026-01-08T15:00:34.114Z">message</remarks>
     * 
     * This time is when the ATAK user actually sent the message, which is
     * more accurate than the event time (set by TAK server).
     * 
     * @param xml The full CoT XML string
     * @return The remarks time attribute value, or empty string if not found
     */
    std::string extractRemarksTimeAttribute(const std::string& xml)
    {
        // Find the <remarks element
        size_t remarks_start = xml.find("<remarks");
        if (remarks_start == std::string::npos) return "";
        
        // Find the closing > of the remarks opening tag
        size_t remarks_end = xml.find(">", remarks_start);
        if (remarks_end == std::string::npos) return "";
        
        // Extract just the remarks opening tag
        std::string remarks_tag = xml.substr(remarks_start, remarks_end - remarks_start);
        
        // Find time=" within the remarks tag
        size_t time_start = remarks_tag.find("time=\"");
        if (time_start == std::string::npos) return "";
        time_start += 6;  // Skip past 'time="'
        
        size_t time_end = remarks_tag.find("\"", time_start);
        if (time_end == std::string::npos) return "";
        
        return remarks_tag.substr(time_start, time_end - time_start);
    }

    std::string extractXmlAttribute(const std::string& xml, const std::string& attr_name)
    {
        const std::string search = attr_name + "=\"";
        size_t start = xml.find(search);
        if (start == std::string::npos) return "";
        start += search.length();
        size_t end = xml.find('"', start);
        if (end == std::string::npos) return "";
        return xml.substr(start, end - start);
    }

    std::string extractXmlElementContent(const std::string& xml, const std::string& element_name)
    {
        const std::string open_tag = "<" + element_name;
        size_t tag_start = xml.find(open_tag);
        if (tag_start == std::string::npos) return "";
        
        size_t content_start = xml.find('>', tag_start);
        if (content_start == std::string::npos) return "";
        content_start++;
        
        const std::string close_tag = "</" + element_name + ">";
        size_t content_end = xml.find(close_tag, content_start);
        if (content_end == std::string::npos) return "";
        
        return xml.substr(content_start, content_end - content_start);
    }

    std::string unescapeXml(const std::string& s)
    {
        std::string result = s;
        size_t pos;
        
        while ((pos = result.find("&lt;")) != std::string::npos)   result.replace(pos, 4, "<");
        while ((pos = result.find("&gt;")) != std::string::npos)   result.replace(pos, 4, ">");
        while ((pos = result.find("&quot;")) != std::string::npos) result.replace(pos, 6, "\"");
        while ((pos = result.find("&apos;")) != std::string::npos) result.replace(pos, 6, "'");
        while ((pos = result.find("&amp;")) != std::string::npos)  result.replace(pos, 5, "&");
        
        return result;
    }

    //==========================================================================
    // CoT Building
    //==========================================================================
    
    /**
     * @brief Build a GeoChat CoT XML message.
     * 
     * The timestamp should already be correctly adjusted by the caller using
     * getResponseTimestamp() to ensure proper message ordering in ATAK.
     * 
     * UID OVERRIDE SUPPORT:
     * ---------------------
     * The override_uid parameter uses the format: "event_uid|message_id"
     * 
     * This allows independent control of each identifier:
     *   - "uuid1|uuid2" → Event UID uses uuid1, messageId uses uuid2
     *   - "uuid1|"      → Event UID uses uuid1, messageId is random
     *   - "|uuid2"      → Event UID is random, messageId uses uuid2
     *   - "uuid1"       → Both use uuid1 (legacy, backward compatible)
     *   - ""            → Both random (default)
     * 
     * This enables testing which identifier ATAK uses as the primary key:
     *   /reuse-both  → Tests if matching both IDs causes overwrites
     *   /reuse-event → Tests if only event UID matters
     *   /reuse-msgid → Tests if only messageId matters
     * 
     * @param sender This robot's callsign
     * @param destination Target callsign
     * @param message The chat message text
     * @param timestamp The timestamp to use (already adjusted for ordering)
     * @param override_uid Encoded as "event_uid|message_id"
     * @return Complete CoT XML string
     */
    std::string buildGeoChatCoT(const std::string& sender,
                                const std::string& destination,
                                const std::string& message,
                                const std::string& timestamp,
                                const std::string& override_uid = "")
    {
        const std::string time_now = timestamp;
        
        // Stale time is calculated from actual current time
        // This ensures the message doesn't expire prematurely
        const std::string time_stale = futureISO(60);

        double lat = 0.0, lon = 0.0;
        {
            std::lock_guard<std::mutex> lk(fix_mtx_);
            lat = current_lat_;
            lon = current_lon_;
        }

        // Parse override_uid field: "event_uid|message_id"
        // This allows independent control of each identifier for testing
        std::string event_uid_override;
        std::string message_id_override;
        
        if (!override_uid.empty()) {
            size_t separator_pos = override_uid.find('|');
            if (separator_pos != std::string::npos) {
                // Format: "event_uid|message_id"
                event_uid_override = override_uid.substr(0, separator_pos);
                message_id_override = override_uid.substr(separator_pos + 1);
            } else {
                // Legacy format: use same value for both (backward compatible)
                event_uid_override = override_uid;
                message_id_override = override_uid;
            }
        }
        
        // Generate UID - use override if provided, otherwise generate random
        std::string uid;
        std::string chat_id;
        
        if (!event_uid_override.empty()) {
            // Use the provided event UID
            uid = std::string("GeoChat.") + sender + ".ANDROID-" + android_id_ + "." + event_uid_override;
        } else {
            // Generate random event UID (normal operation)
            uid = std::string("GeoChat.") + sender + ".ANDROID-" + android_id_ + "." + randUUID();
        }
        
        if (!message_id_override.empty()) {
            // Use the provided messageId
            chat_id = message_id_override;
        } else {
            // Generate random messageId (normal operation)
            chat_id = randUUID();
        }

        std::ostringstream xml;
        
        xml << "<event version=\"2.0\" uid=\"" << uid
            << "\" type=\"b-t-f\" how=\"h-g-i-g-o\" time=\"" << time_now
            << "\" start=\"" << time_now << "\" stale=\"" << time_stale
            << "\" access=\"Undefined\">";

        xml << "<point lat=\"" << std::fixed << std::setprecision(6) << lat
            << "\" lon=\"" << std::fixed << std::setprecision(6) << lon
            << "\" hae=\"0\" ce=\"10\" le=\"10\"/>";

        xml << "<detail>";
        
        xml << "<_flow_tags_ " << tak_server_flow_tag_key_ << "=\"" << time_now << "\"/>";

        xml << "<__chat parent=\"RootContactGroup\" groupOwner=\"false\""
            << " messageId=\"" << chat_id
            << "\" chatroom=\"" << destination
            << "\" id=\"" << destination
            << "\" senderCallsign=\"" << sender << "\"/>";

        xml << "<marti><dest callsign=\"" << destination << "\"/></marti>";

        xml << "<remarks source=\"" << sender << "\" time=\"" << time_now
            << "\">" << escapeXml(message) << "</remarks>";

        xml << "</detail></event>";

        return xml.str();
    }

    //==========================================================================
    // Utility Functions
    //==========================================================================
    
    static std::string escapeXml(const std::string& s)
    {
        std::string result;
        result.reserve(static_cast<size_t>(s.size() * 1.1));
        
        for (char c : s) {
            switch (c) {
                case '&':  result += "&amp;";  break;
                case '<':  result += "&lt;";   break;
                case '>':  result += "&gt;";   break;
                case '"':  result += "&quot;"; break;
                case '\'': result += "&apos;"; break;
                default:   result += c;        break;
            }
        }
        return result;
    }

    static std::string nowISO()
    {
        using namespace std::chrono;
        auto now = system_clock::now();
        std::time_t t = system_clock::to_time_t(now);
        std::tm tm{};
        gmtime_r(&t, &tm);
        
        std::ostringstream oss;
        oss << std::put_time(&tm, "%Y-%m-%dT%H:%M:%S");
        
        auto micros = duration_cast<microseconds>(now.time_since_epoch()).count() % 1000000;
        oss << "." << std::setw(6) << std::setfill('0') << micros << "Z";
        
        return oss.str();
    }

    static std::string futureISO(int sec)
    {
        using namespace std::chrono;
        auto fut = system_clock::now() + seconds(sec);
        std::time_t t = system_clock::to_time_t(fut);
        std::tm tm{};
        gmtime_r(&t, &tm);
        
        std::ostringstream oss;
        oss << std::put_time(&tm, "%Y-%m-%dT%H:%M:%S");
        
        auto micros = duration_cast<microseconds>(fut.time_since_epoch()).count() % 1000000;
        oss << "." << std::setw(6) << std::setfill('0') << micros << "Z";
        
        return oss.str();
    }
    
    /**
     * @brief Convert seconds since epoch to ISO8601 timestamp string.
     * 
     * @param seconds_since_epoch Time in seconds since Unix epoch (UTC)
     * @return ISO8601 formatted timestamp (e.g., "2026-01-08T15:00:34.114000Z")
     */
    std::string secondsToISO(double seconds_since_epoch)
    {
        using namespace std::chrono;
        
        auto tp = system_clock::time_point(
            duration_cast<system_clock::duration>(
                duration<double>(seconds_since_epoch)));
        
        std::time_t t = system_clock::to_time_t(tp);
        std::tm tm{};
        gmtime_r(&t, &tm);
        
        std::ostringstream oss;
        oss << std::put_time(&tm, "%Y-%m-%dT%H:%M:%S");
        
        // Preserve microsecond precision
        auto micros = duration_cast<microseconds>(tp.time_since_epoch()).count() % 1000000;
        oss << "." << std::setw(6) << std::setfill('0') << micros << "Z";
        
        return oss.str();
    }

    static std::string randUUID()
    {
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<int> dis(0, 15);
        std::uniform_int_distribution<int> dis2(8, 11);
        
        std::stringstream ss;
        ss << std::hex;
        
        for (int i = 0; i < 8; ++i) ss << dis(gen);
        ss << '-';
        for (int i = 0; i < 4; ++i) ss << dis(gen);
        ss << "-4";
        for (int i = 0; i < 3; ++i) ss << dis(gen);
        ss << '-';
        ss << dis2(gen);
        for (int i = 0; i < 3; ++i) ss << dis(gen);
        ss << '-';
        for (int i = 0; i < 12; ++i) ss << dis(gen);
        
        return ss.str();
    }

    //==========================================================================
    // Member Variables
    //==========================================================================
    
    // --- Identity ---
    std::string callsign_;
    std::string android_id_;
    std::string tak_server_flow_tag_key_;
    
    // --- Allowed callsigns (for filtering incoming messages) ---
    std::vector<std::string> allowed_callsigns_;
    std::set<std::string> allowed_callsigns_set_;
    
    // --- Retry/timing configuration ---
    double retry_timeout_s_;
    double retry_interval_s_;
    int min_retry_count_;
    double send_delay_s_;      // Min delay between consecutive sends to same dest
    double reply_delay_s_;     // Min delay after receiving before sending reply

    // --- GPS position ---
    std::mutex fix_mtx_;
    bool fix_received_;
    double current_lat_;
    double current_lon_;

    // --- Comms status (base_station connectivity) ---
    // Tracks whether we can reach the base_station (TAK server) via the mesh network
    // All TAK/ATAK messages go through base_station, so this is a binary check
    std::mutex comms_mutex_;
    bool comms_status_received_{false};     // Have we ever received comms_status?
    bool has_base_station_comms_{false};    // Is base_station in transitive list?

    // --- Send queue (ensures delay between messages to SAME destination) ---
    std::deque<QueuedMessage> send_queue_;
    std::mutex send_queue_mutex_;
    // Track last send time PER DESTINATION - allows simultaneous sends to different destinations
    std::map<std::string, std::chrono::steady_clock::time_point> last_send_time_per_dest_;
    // Track earliest reply time per destination (wall-clock seconds since epoch)
    // This handles clock skew - we wait until our clock passes their timestamp + buffer
    std::map<std::string, double> earliest_reply_time_per_dest_;

    // --- Pending messages (for retry tracking) ---
    std::map<std::string, PendingMessage> pending_messages_;
    std::mutex pending_mutex_;
    
    // --- Last incoming message per callsign (for proper response ordering) ---
    // When replying to someone, we use their timestamp + delay to ensure our
    // response appears AFTER their message in ATAK's chat view
    std::map<std::string, LastIncomingMessage> last_incoming_messages_;
    std::mutex last_incoming_mutex_;
    
    // Minimum delay (seconds) to add to incoming timestamp when replying.
    // Must be large enough to account for network latency + processing time.
    static constexpr double RESPONSE_DELAY_S = 2.0;
    
    // Buffer (seconds) to add to current time when using current-time-based timestamp.
    // This handles cases where their clock is behind ours.
    static constexpr double CURRENT_TIME_BUFFER_S = 0.5;
    
    // Maximum time (seconds) to wait before replying, regardless of clock skew.
    // This prevents excessive delays if a device's clock is wildly off (hours/days ahead).
    // Set to a reasonable value that handles typical clock drift but caps extreme cases.
    static constexpr double MAX_REPLY_WAIT_S = 5.0;

    // --- Publishers ---
    rclcpp::Publisher<std_msgs::msg::String>::SharedPtr pub_cot_;
    rclcpp::Publisher<tak_chat::msg::TakChat>::SharedPtr pub_tak_chat_in_;

    // --- Subscribers ---
    rclcpp::Subscription<sensor_msgs::msg::NavSatFix>::SharedPtr sub_navsat_;
    rclcpp::Subscription<tak_chat::msg::TakChat>::SharedPtr sub_tak_chat_out_;
    rclcpp::Subscription<std_msgs::msg::String>::SharedPtr sub_incoming_cot_;
    rclcpp::Subscription<west_point_comms_sim::msg::CommsStatus>::SharedPtr sub_comms_status_;

    // --- Timers ---
    rclcpp::TimerBase::SharedPtr retry_timer_;
    rclcpp::TimerBase::SharedPtr send_queue_timer_;
};

#endif // TAK_CHAT_NODE_HPP
