#pragma once
//==============================================================================
// tak_chat_interface.h — Shared ROS2 interface for TAK chat BT nodes
//==============================================================================
//
// PURPOSE:
// --------
// Long-lived shared publisher/subscriber for TAK chat messaging.
// Created once at startup, shared across all BT nodes.
//
// ARCHITECTURE (post-overhaul):
// -----------------------------
// The preferred pattern is now:
//   ConstructTAKChatMessage → PublishTAKChatMessage → tak_chat/out → TakChatNode
//
// TakChatInterface is still used by:
//   - ReceiveTAKMessage (reads from inbox)
//   - Legacy nodes: BroadcastToTAK, SendTAKMessage (backward compat)
//
// The publish() method accepts a fully-assembled TakChat message object,
// allowing ConstructTAKChatMessage to set all fields including chat_type,
// chatroom, chatroom_id, member_uids, member_names before publishing.
//
// LEGACY METHODS:
// ---------------
//   send(destination, message)  — Unicast, chat_type="unicast"
//   broadcast(message)          — Legacy ALL fan-out, chat_type=""
//   Both preserved for backward compatibility with existing trees.
//
// RELIABILITY:
// ------------
//   Long-lived publisher — DDS discovery complete before any BT node ticks.
//   Created once in main(), shared via shared_ptr across all BT nodes.
//
// QoS: RELIABLE + VOLATILE — must match TakChatNode and ATAK bridge.
//
// USAGE:
// ------
//   // In main(): create once
//   auto tak_chat = std::make_shared<TakChatInterface>(node, "warthog1");
//
//   // New pattern (preferred): publish fully-assembled TakChat message
//   tak_chat::msg::TakChat msg;
//   msg.origin    = "warthog1";
//   msg.chat_type = "team_color";
//   msg.chatroom  = "Cyan";
//   msg.message   = "Moving to waypoint";
//   tak_chat->publish(msg);
//
//   // Legacy pattern (backward compat):
//   tak_chat->send("TRILL", "Confirmed!");
//   tak_chat->broadcast("Standing by");
//
//==============================================================================

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

static const std::string TAK_BROADCAST_DESTINATION = "ALL";

class TakChatInterface
{
public:
    using TakChatMsg = tak_chat::msg::TakChat;

    static constexpr size_t MAX_INBOX_SIZE          = 100;
    static constexpr double DEFAULT_DISCOVERY_TIMEOUT_S = 10.0;
    static constexpr double MIN_DISCOVERY_TIME_S    = 2.0;
    static constexpr double POST_DISCOVERY_DELAY_S  = 0.5;

    //==========================================================================
    // CONSTRUCTOR
    //==========================================================================
    TakChatInterface(
        rclcpp::Node::SharedPtr node,
        const std::string& robot_callsign,
        const std::string& out_topic = "tak_chat/out",
        const std::string& in_topic  = "tak_chat/in",
        double discovery_timeout = DEFAULT_DISCOVERY_TIMEOUT_S)
        : node_(node)
        , callsign_(robot_callsign)
        , out_topic_(out_topic)
        , in_topic_(in_topic)
        , subscriber_discovered_(false)
    {
        const auto qos = rclcpp::QoS(rclcpp::KeepLast(10))
                             .reliable()
                             .durability_volatile();

        // Publisher event callbacks for discovery diagnostics
        rclcpp::PublisherOptions pub_opts;
        pub_opts.event_callbacks.matched_callback =
            [this](rclcpp::MatchedInfo& info) {
                if (info.current_count_change > 0)
                    RCLCPP_INFO(node_->get_logger(),
                        "[TakChatInterface] TakChatNode subscribed! Total: %zu",
                        info.current_count);
                else
                    RCLCPP_WARN(node_->get_logger(),
                        "[TakChatInterface] TakChatNode unsubscribed. Total: %zu",
                        info.current_count);
            };
        pub_opts.event_callbacks.incompatible_qos_callback =
            [this](rclcpp::QOSOfferedIncompatibleQoSInfo& info) {
                RCLCPP_ERROR(node_->get_logger(),
                    "[TakChatInterface] INCOMPATIBLE QoS! Policy: %d — "
                    "messages will NOT be delivered!", info.last_policy_kind);
            };

        pub_out_ = node_->create_publisher<TakChatMsg>(out_topic_, qos, pub_opts);

        sub_in_ = node_->create_subscription<TakChatMsg>(
            in_topic_, qos,
            [this](const TakChatMsg::SharedPtr msg) { handleIncoming(msg); });

        RCLCPP_INFO(node_->get_logger(),
            "[TakChatInterface] Initialized (callsign=%s, out=%s, in=%s)",
            callsign_.c_str(), out_topic_.c_str(), in_topic_.c_str());

        waitForSubscriber(discovery_timeout);

        RCLCPP_INFO(node_->get_logger(), "[TakChatInterface] Ready!");
    }

    //==========================================================================
    // DISCOVERY
    //==========================================================================
    bool waitForSubscriber(double timeout_s = DEFAULT_DISCOVERY_TIMEOUT_S)
    {
        RCLCPP_INFO(node_->get_logger(),
            "[TakChatInterface] Waiting for TakChatNode (min %.1fs)...",
            MIN_DISCOVERY_TIME_S);

        auto start = std::chrono::steady_clock::now();
        bool seen  = false;
        double last_log = 0.0;

        while (true)
        {
            double elapsed = std::chrono::duration<double>(
                std::chrono::steady_clock::now() - start).count();

            if (elapsed >= timeout_s)
            {
                if (seen)
                {
                    RCLCPP_WARN(node_->get_logger(),
                        "[TakChatInterface] Timeout but subscriber was seen — proceeding");
                    subscriber_discovered_ = true;
                    return true;
                }
                RCLCPP_ERROR(node_->get_logger(),
                    "[TakChatInterface] Timeout — NO SUBSCRIBER. "
                    "Is TakChatNode running?");
                subscriber_discovered_ = false;
                return false;
            }

            size_t count = pub_out_->get_subscription_count();
            if (count > 0)
            {
                if (!seen)
                {
                    seen = true;
                    RCLCPP_INFO(node_->get_logger(),
                        "[TakChatInterface] Subscriber seen at %.3fs", elapsed);
                }
                if (elapsed >= MIN_DISCOVERY_TIME_S)
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(
                        static_cast<int>(POST_DISCOVERY_DELAY_S * 1000)));
                    subscriber_discovered_ = true;
                    RCLCPP_INFO(node_->get_logger(),
                        "[TakChatInterface] Discovery complete (%.3fs)", elapsed);
                    return true;
                }
            }

            if (elapsed - last_log >= 1.0)
            {
                RCLCPP_INFO(node_->get_logger(),
                    "[TakChatInterface] Waiting... %.1fs, %zu subscriber(s)",
                    elapsed, count);
                last_log = elapsed;
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

    bool isSubscriberConnected() const
    {
        return pub_out_->get_subscription_count() > 0;
    }

    //==========================================================================
    // PUBLISH — preferred method (new architecture)
    //==========================================================================
    /**
     * @brief Publish a fully-assembled TakChat message to tak_chat/out.
     *
     * This is the preferred method post-overhaul. The caller (typically
     * PublishTAKChatMessage BT node) provides a fully-assembled TakChat
     * message with all fields set including chat_type, chatroom, etc.
     *
     * @param msg Fully-assembled TakChat message
     */
    void publish(const TakChatMsg& msg)
    {
        if (pub_out_->get_subscription_count() == 0)
        {
            RCLCPP_WARN(node_->get_logger(),
                "[TakChatInterface] No subscribers — message may be lost: '%s'",
                msg.message.c_str());
        }
        pub_out_->publish(msg);
    }

    //==========================================================================
    // LEGACY SEND METHODS — backward compatibility
    //==========================================================================

    /**
     * @brief [LEGACY] Send a unicast message to a specific callsign.
     *
     * Preserved for backward compatibility with BroadcastToTAK / SendTAKMessage.
     * New code should use ConstructTAKChatMessage + PublishTAKChatMessage instead.
     */
    void send(const std::string& destination, const std::string& message)
    {
        TakChatMsg msg;
        msg.origin      = callsign_;
        msg.destination = destination;
        msg.message     = message;
        msg.timestamp   = nowISO();
        msg.chat_type   = "unicast";
        // chatroom, chatroom_id, member_uids, member_names left empty

        if (pub_out_->get_subscription_count() == 0)
        {
            RCLCPP_WARN(node_->get_logger(),
                "[TakChatInterface] No subscribers — message may be lost");
        }

        RCLCPP_INFO(node_->get_logger(),
            "[TakChat SEND legacy] %s -> %s: \"%s\"",
            callsign_.c_str(), destination.c_str(), message.c_str());

        pub_out_->publish(msg);
    }

    /**
     * @brief [LEGACY] Broadcast to all allowed callsigns via "ALL" destination.
     *
     * Preserved for backward compatibility. TakChatNode fans out to all
     * allowed callsigns when destination="ALL".
     */
    void broadcast(const std::string& message)
    {
        TakChatMsg msg;
        msg.origin      = callsign_;
        msg.destination = TAK_BROADCAST_DESTINATION;
        msg.message     = message;
        msg.timestamp   = nowISO();
        msg.chat_type   = "";  // Empty → TakChatNode treats as legacy unicast fan-out

        if (pub_out_->get_subscription_count() == 0)
        {
            RCLCPP_WARN(node_->get_logger(),
                "[TakChatInterface] No subscribers — broadcast may be lost");
        }

        RCLCPP_INFO(node_->get_logger(),
            "[TakChat BROADCAST legacy] %s -> ALL: \"%s\"",
            callsign_.c_str(), message.c_str());

        pub_out_->publish(msg);
    }

    //==========================================================================
    // INBOX
    //==========================================================================
    std::optional<TakChatMsg> getLatestMessage(const std::string& from = "")
    {
        std::lock_guard<std::mutex> lock(inbox_mutex_);
        if (inbox_.empty()) return std::nullopt;

        if (from.empty())
        {
            auto msg = inbox_.front();
            inbox_.pop_front();
            return msg;
        }

        for (auto it = inbox_.begin(); it != inbox_.end(); ++it)
        {
            if (it->origin == from)
            {
                auto msg = *it;
                inbox_.erase(it);
                return msg;
            }
        }
        return std::nullopt;
    }

    bool hasMessage(const std::string& from = "")
    {
        std::lock_guard<std::mutex> lock(inbox_mutex_);
        if (from.empty()) return !inbox_.empty();
        for (const auto& msg : inbox_)
            if (msg.origin == from) return true;
        return false;
    }

    void clearInbox()
    {
        std::lock_guard<std::mutex> lock(inbox_mutex_);
        inbox_.clear();
    }

    size_t getInboxSize()
    {
        std::lock_guard<std::mutex> lock(inbox_mutex_);
        return inbox_.size();
    }

    std::string getCallsign()  const { return callsign_; }
    std::string getOutTopic()  const { return out_topic_; }
    std::string getInTopic()   const { return in_topic_; }
    bool wasSubscriberDiscovered() const { return subscriber_discovered_; }

private:
    //==========================================================================
    // INCOMING HANDLER
    //==========================================================================
    void handleIncoming(const TakChatMsg::SharedPtr msg)
    {
        // Ignore our own messages
        if (msg->origin == callsign_) return;

        // Accept messages addressed to us, broadcast, or any group/team/role
        // message (TAK server already filtered delivery to us)
        const bool is_unicast = (msg->chat_type == "unicast" || msg->chat_type.empty());
        if (is_unicast)
        {
            if (msg->destination != callsign_ &&
                msg->destination != TAK_BROADCAST_DESTINATION)
            {
                return;
            }
        }
        // Non-unicast types (group, team_color, role, all_*) are always accepted
        // — TAK server already decided we should receive them

        RCLCPP_INFO(node_->get_logger(),
            "[TakChat RECV] type='%s' from='%s' chatroom='%s' msg='%s'",
            msg->chat_type.c_str(), msg->origin.c_str(),
            msg->chatroom.c_str(), msg->message.c_str());

        std::lock_guard<std::mutex> lock(inbox_mutex_);
        inbox_.push_back(*msg);
        while (inbox_.size() > MAX_INBOX_SIZE) inbox_.pop_front();
    }

    //==========================================================================
    // TIMESTAMP UTILITY
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
        auto us = duration_cast<microseconds>(now.time_since_epoch()).count() % 1000000;
        oss << "." << std::setw(6) << std::setfill('0') << us << "Z";
        return oss.str();
    }

    //==========================================================================
    // MEMBER VARIABLES
    //==========================================================================
    rclcpp::Node::SharedPtr                              node_;
    rclcpp::Publisher<TakChatMsg>::SharedPtr             pub_out_;
    rclcpp::Subscription<TakChatMsg>::SharedPtr          sub_in_;

    std::string callsign_;
    std::string out_topic_;
    std::string in_topic_;
    bool        subscriber_discovered_;

    std::deque<TakChatMsg> inbox_;
    std::mutex             inbox_mutex_;
};
