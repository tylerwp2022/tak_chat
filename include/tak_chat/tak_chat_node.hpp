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
// MESSAGE FLOW
// ------------
//   A) OUTGOING (ROS -> TAK)
//      - BT nodes publish TakChat requests to tak_chat/out
//      - If destination is "ALL", fans out to all allowed callsigns
//      - Messages are queued and sent with delay between them
//      - Timestamp is generated when actually sent (not when received)
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

#include <string>
#include <vector>
#include <mutex>
#include <queue>
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
// ClockOffset - Tracks clock difference for a specific callsign
//==============================================================================
//
// ATAK devices may be disconnected from the internet and have drifting clocks.
// To ensure messages appear in correct order on their chat view, we track the
// offset between our clock and each callsign's clock.
//
// When we receive a message, we calculate:
//   offset = our_time - their_timestamp
//
// When we send TO that callsign, we adjust:
//   adjusted_timestamp = our_time - offset
//
// This makes our messages appear with timestamps relative to THEIR clock,
// ensuring proper ordering in their chat view.
//
//==============================================================================
struct ClockOffset {
    double offset_seconds;                              // our_time - their_time
    std::chrono::steady_clock::time_point last_updated; // When offset was last calculated
    int sample_count;                                   // Number of messages used to calculate
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
          current_lon_(0.0),
          last_send_time_(std::chrono::steady_clock::time_point::min())
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

        // Allowed callsigns configuration
        this->declare_parameter<std::string>("allowed_callsigns_file",
            "/phoenix/src/phoenix-tak/src/tak_bridge/config/cot_runner.yaml");

        // Retry/reliability parameters - tuned for DDS discovery timing
        this->declare_parameter<double>("retry_timeout_s", 10.0);     // Total retry window
        this->declare_parameter<double>("retry_interval_s", 1.0);     // Time between publishes
        this->declare_parameter<int>("min_retry_count", 2);           // Min publishes even with subscriber
        
        // Send queue parameters - ensures messages have distinct timestamps
        this->declare_parameter<double>("send_delay_s", 1.0);         // Min delay between sends

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
        const std::string allowed_callsigns_file = this->get_parameter("allowed_callsigns_file").as_string();

        retry_timeout_s_ = this->get_parameter("retry_timeout_s").as_double();
        retry_interval_s_ = this->get_parameter("retry_interval_s").as_double();
        min_retry_count_ = this->get_parameter("min_retry_count").as_int();
        send_delay_s_ = this->get_parameter("send_delay_s").as_double();

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
        RCLCPP_INFO(this->get_logger(), "  Send delay:           %.1fs (between messages)", send_delay_s_);
        RCLCPP_INFO(this->get_logger(), "  Retry timeout:        %.1fs", retry_timeout_s_);
        RCLCPP_INFO(this->get_logger(), "  Retry interval:       %.1fs", retry_interval_s_);
        RCLCPP_INFO(this->get_logger(), "  Min retry count:      %d", min_retry_count_);
        RCLCPP_INFO(this->get_logger(), "  Allowed callsigns:    %zu loaded", allowed_callsigns_.size());
        RCLCPP_INFO(this->get_logger(), "  Broadcast destination: '%s' -> fans out to all allowed",
                    BROADCAST_DESTINATION.c_str());
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
    // Callback: TakChat OUT (ROS -> TAK)
    //==========================================================================
    
    /**
     * @brief Handle outgoing chat requests from BT nodes.
     * 
     * Messages are added to the send queue and sent one at a time with a
     * minimum delay between them. This ensures messages have distinct
     * timestamps and arrive at ATAK in the correct order.
     */
    void takChatOutCallback(const tak_chat::msg::TakChat::SharedPtr msg)
    {
        RCLCPP_INFO(this->get_logger(),
            "[TakChat RECV] origin='%s' dest='%s' msg='%s'",
            msg->origin.c_str(), msg->destination.c_str(), msg->message.c_str());
        
        if (msg->origin != callsign_) {
            RCLCPP_WARN(this->get_logger(),
                "[TakChat REJECTED] origin='%s' does not match our callsign='%s'",
                msg->origin.c_str(), callsign_.c_str());
            return;
        }

        // Add to send queue - messages will be sent with delay between them
        // The timestamp will be generated when actually sent, not now
        if (msg->destination == BROADCAST_DESTINATION) {
            RCLCPP_INFO(this->get_logger(),
                "[TakChat QUEUED] BROADCAST | Message: %s", msg->message.c_str());
            
            // Fan out to all allowed callsigns - each gets queued separately
            for (const auto& dest : allowed_callsigns_) {
                addToSendQueue(dest, msg->message);
            }
        } else {
            RCLCPP_INFO(this->get_logger(),
                "[TakChat QUEUED] To: %s | Message: %s",
                msg->destination.c_str(), msg->message.c_str());
            
            addToSendQueue(msg->destination, msg->message);
        }
        
        // Log queue size for debugging
        std::lock_guard<std::mutex> lock(send_queue_mutex_);
        RCLCPP_INFO(this->get_logger(),
            "[TakChat QUEUE] %zu message(s) waiting to be sent", send_queue_.size());
    }

    //==========================================================================
    // Send Queue - Ensures messages are sent with delay between them
    //==========================================================================
    
    /**
     * @brief Add a message to the send queue.
     * 
     * Messages wait in the queue until the send delay has elapsed since
     * the last message was sent. This ensures distinct timestamps.
     * 
     * @param destination Target callsign
     * @param message Message text to send
     */
    void addToSendQueue(const std::string& destination, const std::string& message)
    {
        QueuedMessage queued;
        queued.destination = destination;
        queued.message = message;
        queued.queued_time = std::chrono::steady_clock::now();
        
        std::lock_guard<std::mutex> lock(send_queue_mutex_);
        send_queue_.push(queued);
    }
    
    /**
     * @brief Timer callback to process the send queue.
     * 
     * Sends one message from the queue if:
     *   1. The queue is not empty
     *   2. At least send_delay_s has elapsed since the last send
     * 
     * This ensures messages have distinct, well-separated timestamps
     * so TAK server and ATAK can properly order them.
     */
    void sendQueueTimerCallback()
    {
        auto now = std::chrono::steady_clock::now();
        
        // First check if there's anything to send
        size_t queue_size;
        {
            std::lock_guard<std::mutex> lock(send_queue_mutex_);
            queue_size = send_queue_.size();
        }
        
        if (queue_size == 0) {
            return;  // Nothing to send
        }
        
        // Check if enough time has passed since last send
        // Special case: if last_send_time_ is min (never sent), allow immediate send
        bool first_send = (last_send_time_ == std::chrono::steady_clock::time_point::min());
        
        if (!first_send) {
            double since_last_send = std::chrono::duration<double>(
                now - last_send_time_).count();
            
            if (since_last_send < send_delay_s_) {
                return;  // Not time to send yet
            }
        }
        
        // Get next message from queue
        QueuedMessage queued;
        {
            std::lock_guard<std::mutex> lock(send_queue_mutex_);
            if (send_queue_.empty()) {
                return;  // Race condition - queue was emptied
            }
            queued = send_queue_.front();
            send_queue_.pop();
            queue_size = send_queue_.size();
        }
        
        // Generate timestamp NOW (when actually sending)
        std::string timestamp = nowISO();
        
        RCLCPP_INFO(this->get_logger(),
            "[TakChat SENDING] To: %s | Message: '%s' | timestamp=%s",
            queued.destination.c_str(), queued.message.c_str(), timestamp.c_str());
        
        // Send the message (moves to pending for retry tracking)
        sendMessage(queued.destination, queued.message, timestamp);
        
        // Update last send time
        last_send_time_ = now;
        
        // Log remaining queue size
        if (queue_size > 0) {
            RCLCPP_INFO(this->get_logger(),
                "[TakChat QUEUE] %zu message(s) still waiting (next in %.1fs)",
                queue_size, send_delay_s_);
        }
    }

    //==========================================================================
    // Send a message with retry logic
    //==========================================================================
    
    /**
     * @brief Send a message and add to pending for retry tracking.
     * 
     * This is called from the send queue timer when it's time to actually
     * send a message. The timestamp is generated fresh at send time.
     * 
     * @param destination Target callsign
     * @param message Message text to send
     * @param timestamp Timestamp to use in the CoT XML
     */
    void sendMessage(const std::string& destination, 
                     const std::string& message,
                     const std::string& timestamp)
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

        // Build CoT XML with the current timestamp
        std::string cot_xml = buildGeoChatCoT(callsign_, destination, message, timestamp);

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

        // Calculate clock offset for this sender
        // This helps us send responses with timestamps that appear in correct order
        // on their device, even if their clock is drifting
        updateClockOffset(sender, timestamp);

        RCLCPP_INFO(this->get_logger(),
            "[TakChat IN] From: %s | To: %s | Message: %s",
            sender.c_str(), recipient.c_str(), message.c_str());

        tak_chat::msg::TakChat tak_msg;
        tak_msg.origin = sender;
        tak_msg.destination = recipient;
        tak_msg.message = message;
        tak_msg.timestamp = timestamp;

        pub_tak_chat_in_->publish(tak_msg);
    }
    
    //==========================================================================
    // Clock Offset Management
    //==========================================================================
    
    /**
     * @brief Update the clock offset for a callsign based on their message timestamp.
     * 
     * Calculates the difference between our clock and theirs:
     *   offset = our_time - their_time
     * 
     * If their clock is behind ours, offset is positive.
     * If their clock is ahead of ours, offset is negative.
     * 
     * Uses exponential moving average to smooth out network jitter.
     * 
     * @param callsign The sender's callsign
     * @param their_timestamp ISO8601 timestamp from their message
     */
    void updateClockOffset(const std::string& callsign, const std::string& their_timestamp)
    {
        // Parse their timestamp to get seconds since epoch
        double their_time_seconds = parseISOTimestamp(their_timestamp);
        if (their_time_seconds < 0) {
            RCLCPP_WARN(this->get_logger(),
                "[ClockOffset] Failed to parse timestamp from %s: %s",
                callsign.c_str(), their_timestamp.c_str());
            return;
        }
        
        // Get our current time in seconds since epoch
        auto now = std::chrono::system_clock::now();
        double our_time_seconds = std::chrono::duration<double>(
            now.time_since_epoch()).count();
        
        // Calculate offset: positive means their clock is behind
        double new_offset = our_time_seconds - their_time_seconds;
        
        std::lock_guard<std::mutex> lock(clock_offsets_mutex_);
        
        auto it = clock_offsets_.find(callsign);
        if (it == clock_offsets_.end()) {
            // First message from this callsign
            ClockOffset offset;
            offset.offset_seconds = new_offset;
            offset.last_updated = std::chrono::steady_clock::now();
            offset.sample_count = 1;
            clock_offsets_[callsign] = offset;
            
            RCLCPP_INFO(this->get_logger(),
                "[ClockOffset] NEW %s: %.2fs (their clock is %s)",
                callsign.c_str(), new_offset,
                new_offset > 0 ? "behind" : "ahead");
        } else {
            // Update with exponential moving average (alpha = 0.3)
            // This smooths out network latency jitter
            const double alpha = 0.3;
            double old_offset = it->second.offset_seconds;
            it->second.offset_seconds = alpha * new_offset + (1.0 - alpha) * old_offset;
            it->second.last_updated = std::chrono::steady_clock::now();
            it->second.sample_count++;
            
            // Only log if offset changed significantly (>1 second)
            if (std::abs(it->second.offset_seconds - old_offset) > 1.0) {
                RCLCPP_INFO(this->get_logger(),
                    "[ClockOffset] UPDATED %s: %.2fs -> %.2fs",
                    callsign.c_str(), old_offset, it->second.offset_seconds);
            }
        }
    }
    
    /**
     * @brief Get the clock offset for a callsign.
     * 
     * @param callsign The callsign to look up
     * @return The offset in seconds (positive = their clock behind), or 0.0 if unknown
     */
    double getClockOffset(const std::string& callsign)
    {
        std::lock_guard<std::mutex> lock(clock_offsets_mutex_);
        
        auto it = clock_offsets_.find(callsign);
        if (it == clock_offsets_.end()) {
            return 0.0;  // No offset known, use our clock
        }
        
        // Check if offset is stale (older than 1 hour)
        // Clocks can drift, so old offsets may be inaccurate
        auto age = std::chrono::steady_clock::now() - it->second.last_updated;
        if (age > std::chrono::hours(1)) {
            RCLCPP_DEBUG(this->get_logger(),
                "[ClockOffset] Offset for %s is stale (%.1f hours old), using 0",
                callsign.c_str(), 
                std::chrono::duration<double, std::ratio<3600>>(age).count());
            return 0.0;
        }
        
        return it->second.offset_seconds;
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
     * The timestamp is adjusted based on the clock offset for the destination
     * callsign. This ensures messages appear in correct chronological order
     * on their device, even if their clock is drifting.
     * 
     * @param sender This robot's callsign
     * @param destination Target callsign
     * @param message The chat message text
     * @param base_timestamp Our timestamp (from nowISO())
     * @return Complete CoT XML string
     */
    std::string buildGeoChatCoT(const std::string& sender,
                                const std::string& destination,
                                const std::string& message,
                                const std::string& base_timestamp)
    {
        // Adjust timestamp based on clock offset for this destination
        // If their clock is behind, we subtract from our time so messages
        // appear in correct order on their device
        double offset = getClockOffset(destination);
        std::string adjusted_timestamp = base_timestamp;
        
        if (std::abs(offset) > 1.0) {  // Only adjust if offset > 1 second
            adjusted_timestamp = adjustTimestamp(base_timestamp, -offset);
            RCLCPP_DEBUG(this->get_logger(),
                "[TakChat] Adjusted timestamp for %s: %s -> %s (offset: %.1fs)",
                destination.c_str(), base_timestamp.c_str(), 
                adjusted_timestamp.c_str(), offset);
        }
        
        const std::string time_now = adjusted_timestamp;
        
        // Stale time is calculated from NOW (not adjusted timestamp)
        // This ensures the message doesn't expire prematurely
        const std::string time_stale = futureISO(60);

        double lat = 0.0, lon = 0.0;
        {
            std::lock_guard<std::mutex> lk(fix_mtx_);
            lat = current_lat_;
            lon = current_lon_;
        }

        const std::string uid = std::string("GeoChat.") + sender +
                                ".ANDROID-" + android_id_ + "." + randUUID();
        const std::string chat_id = randUUID();

        std::ostringstream xml;
        
        xml << "<event version=\"2.0\" uid=\"" << uid
            << "\" type=\"b-t-f\" how=\"h-g-i-g-o\" time=\"" << time_now
            << "\" start=\"" << time_now << "\" stale=\"" << time_stale
            << "\" access=\"Undefined\">";

        xml << "<point lat=\"" << std::fixed << std::setprecision(6) << lat
            << "\" lon=\"" << std::fixed << std::setprecision(6) << lon
            << "\" hae=\"0\" ce=\"10\" le=\"10\"/>";

        xml << "<detail>";
        
        // Flow tag uses the original timestamp for consistency
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
     * @brief Adjust an ISO8601 timestamp by a number of seconds.
     * 
     * Used to apply clock offset when sending to callsigns with drifting clocks.
     * 
     * @param iso_timestamp The original timestamp (e.g., "2026-01-08T15:00:34.114Z")
     * @param adjustment_seconds Seconds to add (negative to subtract)
     * @return Adjusted timestamp in ISO8601 format
     */
    std::string adjustTimestamp(const std::string& iso_timestamp, double adjustment_seconds)
    {
        // Parse the timestamp
        double seconds_since_epoch = parseISOTimestamp(iso_timestamp);
        if (seconds_since_epoch < 0) {
            return iso_timestamp;  // Parse failed, return original
        }
        
        // Apply adjustment
        seconds_since_epoch += adjustment_seconds;
        
        // Convert back to ISO format
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
    
    std::string callsign_;
    std::string android_id_;
    std::string tak_server_flow_tag_key_;
    
    std::vector<std::string> allowed_callsigns_;
    std::set<std::string> allowed_callsigns_set_;
    
    double retry_timeout_s_;
    double retry_interval_s_;
    int min_retry_count_;
    double send_delay_s_;

    std::mutex fix_mtx_;
    bool fix_received_;
    double current_lat_;
    double current_lon_;

    // Send queue - messages wait here before being sent
    std::queue<QueuedMessage> send_queue_;
    std::mutex send_queue_mutex_;
    std::chrono::steady_clock::time_point last_send_time_;

    // Pending messages - for retry tracking after sending
    std::map<std::string, PendingMessage> pending_messages_;
    std::mutex pending_mutex_;
    
    // Clock offsets - tracks time difference per callsign for proper message ordering
    // Key: callsign, Value: ClockOffset with offset_seconds = our_time - their_time
    std::map<std::string, ClockOffset> clock_offsets_;
    std::mutex clock_offsets_mutex_;

    rclcpp::Publisher<std_msgs::msg::String>::SharedPtr pub_cot_;
    rclcpp::Publisher<tak_chat::msg::TakChat>::SharedPtr pub_tak_chat_in_;

    rclcpp::Subscription<sensor_msgs::msg::NavSatFix>::SharedPtr sub_navsat_;
    rclcpp::Subscription<tak_chat::msg::TakChat>::SharedPtr sub_tak_chat_out_;
    rclcpp::Subscription<std_msgs::msg::String>::SharedPtr sub_incoming_cot_;

    rclcpp::TimerBase::SharedPtr retry_timer_;
    rclcpp::TimerBase::SharedPtr send_queue_timer_;
};

#endif // TAK_CHAT_NODE_HPP
