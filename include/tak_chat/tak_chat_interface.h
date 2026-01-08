#pragma once
/**
 * =============================================================================
 * @file tak_chat_interface.h
 * @brief Simple interface for TAK chat communication in behavior tree nodes
 * =============================================================================
 *
 * DESIGN OVERVIEW
 * ---------------
 * This is a thin client interface for BehaviorTree nodes to send/receive
 * TAK chat messages. All heavy lifting is done by TakChatNode:
 *   - Retry logic and confirmation
 *   - Fan-out of "ALL" to allowed callsigns
 *   - Filtering incoming messages to only allowed senders
 *
 * RELIABILITY
 * -----------
 * This interface achieves reliable delivery by being LONG-LIVED:
 *   - Created once at startup, lives for entire mission
 *   - Constructor waits for TakChatNode to complete bidirectional discovery
 *   - All BT nodes share the same interface instance
 *   - By the time any BT node sends a message, discovery is long complete
 *
 * This is the same pattern used by CallServiceSendMissionToMaestro, which
 * is why TAK messaging from that node was always reliable.
 *
 * QoS CONFIGURATION
 * -----------------
 * Uses RELIABLE + VOLATILE QoS to match TakChatNode:
 *   - RELIABLE: Ensures delivery confirmation
 *   - VOLATILE: Works best with Fast-DDS (TRANSIENT_LOCAL has issues)
 *
 * All components MUST use the same QoS for compatibility:
 *   - tak_chat_node.hpp: VOLATILE
 *   - tak_chat_interface.h: VOLATILE
 *   - tak_chat_console.py: VOLATILE
 *
 * BROADCAST MESSAGES
 * ------------------
 * To send to all allowed callsigns, use destination "ALL":
 *   tak_chat->send("ALL", "Hello everyone");
 *
 * TakChatNode will automatically fan out to all allowed callsigns.
 *
 * INCOMING MESSAGES
 * -----------------
 * Only messages from allowed callsigns (configured in TakChatNode) will
 * appear in the inbox. Unknown senders are filtered out by TakChatNode.
 *
 * USAGE EXAMPLE
 * -------------
 *   // In main(): Create ONCE, share across all BT nodes
 *   auto tak_chat = std::make_shared<TakChatInterface>(node, "warthog1");
 *   
 *   // Register BT nodes with the shared interface
 *   factory.registerNodeType<Standby>("Standby", tak_chat);
 *   
 *   // In BT nodes: Just send - discovery is already complete
 *   tak_chat->broadcast("Standing by");
 *   
 *   // Check for incoming messages (already filtered to allowed senders)
 *   if (auto msg = tak_chat->getLatestMessage()) {
 *       std::cout << "From: " << msg->origin << std::endl;
 *   }
 *
 * =============================================================================
 */

#include <rclcpp/rclcpp.hpp>
#include <tak_chat/msg/tak_chat.hpp>

#include <mutex>
#include <deque>
#include <optional>
#include <string>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <thread>


/// Special destination that TakChatNode expands to all allowed callsigns
static const std::string TAK_BROADCAST_DESTINATION = "ALL";


class TakChatInterface {
public:
    using TakChatMsg = tak_chat::msg::TakChat;

    //==========================================================================
    // Configuration Constants
    //==========================================================================
    
    /// Maximum inbox size (oldest messages dropped when exceeded)
    static constexpr size_t MAX_INBOX_SIZE = 100;
    
    /// Default timeout waiting for TakChatNode discovery at startup
    static constexpr double DEFAULT_DISCOVERY_TIMEOUT_S = 10.0;
    
    /// Minimum time to wait for bidirectional discovery
    /// Fast-DDS needs significant time for both sides to discover each other
    static constexpr double MIN_DISCOVERY_TIME_S = 2.0;
    
    /// Additional stabilization time after discovery before sending
    static constexpr double POST_DISCOVERY_DELAY_S = 0.5;

    //==========================================================================
    // Constructor
    //==========================================================================
    
    /**
     * @brief Construct a TakChatInterface for a specific robot callsign.
     *
     * The constructor waits for TakChatNode to discover this publisher before
     * returning. This ensures the first message sent won't be lost to DDS
     * discovery races.
     *
     * IMPORTANT: This interface should be created ONCE at startup and shared
     * across all BT nodes. This ensures it's long-lived, which is critical
     * for reliable message delivery.
     *
     * @param node              ROS2 node to create publishers/subscribers from
     * @param robot_callsign    Callsign used as 'origin' for outgoing messages
     * @param out_topic         Topic for outgoing requests (default: tak_chat/out)
     * @param in_topic          Topic for incoming events (default: tak_chat/in)
     * @param discovery_timeout Max seconds to wait for TakChatNode discovery
     */
    TakChatInterface(
        rclcpp::Node::SharedPtr node,
        const std::string& robot_callsign,
        const std::string& out_topic = "tak_chat/out",
        const std::string& in_topic = "tak_chat/in",
        double discovery_timeout = DEFAULT_DISCOVERY_TIMEOUT_S)
        : node_(node)
        , callsign_(robot_callsign)
        , out_topic_(out_topic)
        , in_topic_(in_topic)
        , subscriber_discovered_(false)
    {
        // ---------------------------------------------------------------------
        // QoS Configuration
        // ---------------------------------------------------------------------
        // RELIABLE: Ensures delivery confirmation once both sides discover
        // VOLATILE: Works best with Fast-DDS (TRANSIENT_LOCAL has issues)
        //
        // CRITICAL: This QoS MUST match TakChatNode's subscription QoS!
        // Mismatched durability will cause "incompatible QoS" warnings and
        // messages will not be delivered.
        auto qos = rclcpp::QoS(rclcpp::KeepLast(10))
                     .reliable()
                     .durability_volatile();

        // ---------------------------------------------------------------------
        // Create Publisher with Event Callbacks
        // ---------------------------------------------------------------------
        // Event callbacks help debug discovery and QoS issues
        rclcpp::PublisherOptions pub_options;
        pub_options.event_callbacks.matched_callback =
            [this](rclcpp::MatchedInfo& info) {
                if (info.current_count_change > 0) {
                    RCLCPP_INFO(node_->get_logger(),
                        "[TakChatInterface] TakChatNode subscribed! Total: %zu",
                        info.current_count);
                } else {
                    RCLCPP_WARN(node_->get_logger(),
                        "[TakChatInterface] TakChatNode unsubscribed. Total: %zu",
                        info.current_count);
                }
            };
        pub_options.event_callbacks.incompatible_qos_callback =
            [this](rclcpp::QOSOfferedIncompatibleQoSInfo& info) {
                RCLCPP_ERROR(node_->get_logger(),
                    "[TakChatInterface] INCOMPATIBLE QoS with subscriber! "
                    "Policy: %d, count: %d. Messages will NOT be delivered!",
                    info.last_policy_kind, info.total_count);
            };

        pub_out_ = node_->create_publisher<TakChatMsg>(out_topic_, qos, pub_options);

        // ---------------------------------------------------------------------
        // Create Subscription for Incoming Messages
        // ---------------------------------------------------------------------
        sub_in_ = node_->create_subscription<TakChatMsg>(
            in_topic_, qos,
            [this](const TakChatMsg::SharedPtr msg) { handleIncoming(msg); }
        );

        RCLCPP_INFO(node_->get_logger(),
                    "[TakChatInterface] Initialized (callsign=%s)",
                    callsign_.c_str());
        RCLCPP_INFO(node_->get_logger(),
                    "[TakChatInterface]   OUT topic: %s", out_topic_.c_str());
        RCLCPP_INFO(node_->get_logger(),
                    "[TakChatInterface]   IN topic:  %s", in_topic_.c_str());

        // ---------------------------------------------------------------------
        // Wait for TakChatNode Discovery
        // ---------------------------------------------------------------------
        // This is critical for reliable first-message delivery.
        // Since this interface is long-lived, this is a one-time cost.
        waitForSubscriber(discovery_timeout);
        
        RCLCPP_INFO(node_->get_logger(),
                    "[TakChatInterface] Ready for messaging!");
    }

    //==========================================================================
    // Discovery Management
    //==========================================================================

    /**
     * @brief Wait for TakChatNode to discover this publisher.
     *
     * This handles DDS discovery timing issues. With Fast-DDS and VOLATILE QoS,
     * we need BIDIRECTIONAL discovery - not just us seeing them, but them
     * seeing us too. We wait for:
     *   1. At least one subscriber is connected, AND
     *   2. A minimum time has elapsed (for bidirectional discovery)
     *
     * Since this interface is long-lived (created once at startup), this
     * discovery wait only happens once. All subsequent messages are delivered
     * instantly and reliably.
     *
     * @param timeout_s Maximum seconds to wait for discovery
     * @return true if subscriber was found, false if timeout reached
     */
    bool waitForSubscriber(double timeout_s = DEFAULT_DISCOVERY_TIMEOUT_S)
    {
        RCLCPP_INFO(node_->get_logger(),
                    "[TakChatInterface] Waiting for TakChatNode discovery "
                    "(min %.1fs for bidirectional)...",
                    MIN_DISCOVERY_TIME_S);

        auto start_time = std::chrono::steady_clock::now();
        bool subscriber_seen = false;
        double last_log_time = 0.0;
        
        while (true) {
            auto elapsed = std::chrono::duration<double>(
                std::chrono::steady_clock::now() - start_time).count();
            
            // -----------------------------------------------------------------
            // Check timeout
            // -----------------------------------------------------------------
            if (elapsed >= timeout_s) {
                if (subscriber_seen) {
                    RCLCPP_WARN(node_->get_logger(),
                        "[TakChatInterface] Discovery timeout but subscriber was seen - "
                        "proceeding (messages should be delivered)");
                    subscriber_discovered_ = true;
                    return true;
                } else {
                    RCLCPP_ERROR(node_->get_logger(),
                        "[TakChatInterface] Discovery timeout - NO SUBSCRIBER FOUND!");
                    RCLCPP_ERROR(node_->get_logger(),
                        "[TakChatInterface] Is TakChatNode running? Check:");
                    RCLCPP_ERROR(node_->get_logger(),
                        "[TakChatInterface]   ros2 launch tak_chat tak_chat.launch.py");
                    subscriber_discovered_ = false;
                    return false;
                }
            }
            
            // -----------------------------------------------------------------
            // Check subscriber count
            // -----------------------------------------------------------------
            size_t sub_count = pub_out_->get_subscription_count();
            
            if (sub_count > 0) {
                if (!subscriber_seen) {
                    subscriber_seen = true;
                    RCLCPP_INFO(node_->get_logger(),
                        "[TakChatInterface] Subscriber seen after %.3fs, "
                        "waiting for bidirectional discovery...",
                        elapsed);
                }
                
                // Ensure minimum discovery time has elapsed
                // This is critical for Fast-DDS bidirectional discovery
                if (elapsed >= MIN_DISCOVERY_TIME_S) {
                    RCLCPP_INFO(node_->get_logger(),
                        "[TakChatInterface] TakChatNode discovered! "
                        "(waited %.3fs, %zu subscriber(s))",
                        elapsed, sub_count);
                    
                    // Additional delay for bidirectional discovery
                    RCLCPP_INFO(node_->get_logger(),
                        "[TakChatInterface] Final stabilization delay (%.1fs)...",
                        POST_DISCOVERY_DELAY_S);
                    std::this_thread::sleep_for(
                        std::chrono::milliseconds(
                            static_cast<int>(POST_DISCOVERY_DELAY_S * 1000)));
                    
                    subscriber_discovered_ = true;
                    return true;
                }
            }
            
            // -----------------------------------------------------------------
            // Log progress every second
            // -----------------------------------------------------------------
            if (elapsed - last_log_time >= 1.0) {
                RCLCPP_INFO(node_->get_logger(),
                    "[TakChatInterface] Waiting... %.1fs elapsed, %zu subscriber(s)",
                    elapsed, sub_count);
                last_log_time = elapsed;
            }
            
            // Brief sleep to avoid busy-waiting
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

    /**
     * @brief Check if TakChatNode has been discovered.
     *
     * @return true if subscriber is connected
     */
    bool isSubscriberConnected() const
    {
        return pub_out_->get_subscription_count() > 0;
    }

    //==========================================================================
    // Send Methods
    //==========================================================================

    /**
     * @brief Send a message to a destination.
     *
     * Use "ALL" as destination to broadcast to all allowed callsigns.
     * TakChatNode handles fan-out and retry logic.
     *
     * Since this interface is long-lived (created at startup), DDS discovery
     * is already complete by the time any BT node calls this method.
     * Messages are delivered reliably with a single publish.
     *
     * @param destination  Target callsign, or "ALL" for broadcast
     * @param message      Message text
     */
    void send(const std::string& destination, const std::string& message)
    {
        TakChatMsg msg;
        msg.origin = callsign_;
        msg.destination = destination;
        msg.message = message;
        msg.timestamp = nowISO();

        size_t sub_count = pub_out_->get_subscription_count();
        
        // DEBUG: Log the exact timestamp being embedded in the message
        RCLCPP_INFO(node_->get_logger(),
                    "[TakChat SEND] %s -> %s: \"%s\" timestamp=%s (subscribers: %zu)",
                    callsign_.c_str(), destination.c_str(), message.c_str(),
                    msg.timestamp.c_str(), sub_count);

        if (sub_count == 0) {
            RCLCPP_WARN(node_->get_logger(),
                "[TakChat SEND] WARNING: No subscribers! Message may be lost. "
                "Is TakChatNode running?");
        }

        pub_out_->publish(msg);
    }

    /**
     * @brief Broadcast a message to all allowed callsigns.
     *
     * Convenience method that sends to "ALL".
     * TakChatNode fans out to all configured allowed callsigns.
     *
     * @param message  Message text
     */
    void broadcast(const std::string& message)
    {
        send(TAK_BROADCAST_DESTINATION, message);
    }

    //==========================================================================
    // Inbox Methods
    //==========================================================================

    /**
     * @brief Retrieve and remove the oldest message from the inbox.
     *
     * Note: Only messages from allowed callsigns appear here (TakChatNode
     * filters out unknown senders).
     *
     * @param from  Optional filter - only return messages from this callsign
     * @return The message, or std::nullopt if inbox is empty/no match
     */
    std::optional<TakChatMsg> getLatestMessage(const std::string& from = "")
    {
        std::lock_guard<std::mutex> lock(inbox_mutex_);

        if (inbox_.empty()) {
            return std::nullopt;
        }

        if (from.empty()) {
            auto msg = inbox_.front();
            inbox_.pop_front();
            return msg;
        }

        for (auto it = inbox_.begin(); it != inbox_.end(); ++it) {
            if (it->origin == from) {
                auto msg = *it;
                inbox_.erase(it);
                return msg;
            }
        }

        return std::nullopt;
    }

    /**
     * @brief Check if there's a message waiting in the inbox.
     */
    bool hasMessage(const std::string& from = "")
    {
        std::lock_guard<std::mutex> lock(inbox_mutex_);

        if (from.empty()) {
            return !inbox_.empty();
        }

        for (const auto& msg : inbox_) {
            if (msg.origin == from) {
                return true;
            }
        }
        return false;
    }

    /**
     * @brief Clear all messages from the inbox.
     */
    void clearInbox()
    {
        std::lock_guard<std::mutex> lock(inbox_mutex_);
        inbox_.clear();
    }

    /**
     * @brief Get the current number of messages in the inbox.
     */
    size_t getInboxSize()
    {
        std::lock_guard<std::mutex> lock(inbox_mutex_);
        return inbox_.size();
    }

    //==========================================================================
    // Accessors
    //==========================================================================

    std::string getCallsign() const { return callsign_; }
    std::string getOutTopic() const { return out_topic_; }
    std::string getInTopic() const { return in_topic_; }
    bool wasSubscriberDiscovered() const { return subscriber_discovered_; }

private:
    //==========================================================================
    // Incoming Message Handler
    //==========================================================================
    
    void handleIncoming(const TakChatMsg::SharedPtr msg)
    {
        // Ignore our own messages
        if (msg->origin == callsign_) {
            return;
        }

        // Accept messages addressed to us OR broadcast messages
        // TakChatNode already filtered to allowed senders
        if (msg->destination != callsign_ && msg->destination != TAK_BROADCAST_DESTINATION) {
            return;
        }

        RCLCPP_INFO(node_->get_logger(),
                    "[TakChat RECV] From %s: \"%s\"",
                    msg->origin.c_str(), msg->message.c_str());

        std::lock_guard<std::mutex> lock(inbox_mutex_);
        inbox_.push_back(*msg);

        while (inbox_.size() > MAX_INBOX_SIZE) {
            inbox_.pop_front();
        }
    }

    //==========================================================================
    // Utility
    //==========================================================================

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

    //==========================================================================
    // Member Variables
    //==========================================================================

    rclcpp::Node::SharedPtr node_;
    rclcpp::Publisher<TakChatMsg>::SharedPtr pub_out_;
    rclcpp::Subscription<TakChatMsg>::SharedPtr sub_in_;

    std::string callsign_;
    std::string out_topic_;
    std::string in_topic_;
    
    bool subscriber_discovered_;  // Whether TakChatNode was found at startup

    std::deque<TakChatMsg> inbox_;
    std::mutex inbox_mutex_;
};
