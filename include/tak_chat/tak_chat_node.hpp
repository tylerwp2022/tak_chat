#ifndef TAK_CHAT_NODE_HPP
#define TAK_CHAT_NODE_HPP

//==============================================================================
// tak_chat_node.hpp — ROS2 node bridging TakChat messages to TAK CoT XML
//==============================================================================
//
// PURPOSE:
// --------
// Bridges between simple ROS2 TakChat messages (from BT nodes) and the
// TAK CoT GeoChat XML format required by the TAK server / ATAK clients.
//
// SUPPORTED CHAT TYPES (outgoing):
// ---------------------------------
//   "unicast"        — Direct message to one callsign via <marti> routing
//   "group"          — Named group with explicit member list + hierarchy
//   "team_color"     — TAK team color broadcast (e.g. "Cyan", "Red")
//   "role"           — TAK role broadcast (e.g. "HQ", "Medic")
//   "all_chat_rooms" — Broadcast to All Chat Rooms
//   "all_groups"     — Broadcast to all Groups (UserGroups)
//   "all_teams"      — Broadcast to all Teams (TeamGroups)
//
// BACKWARD COMPATIBILITY:
// -----------------------
//   Empty chat_type is treated as "unicast".
//   destination="ALL" triggers legacy fan-out to all allowed callsigns.
//   This preserves behavior for existing trees using BroadcastToTAK /
//   SendTAKMessage / old ConstructTAKChatMessage nodes.
//
// INCOMING PARSING:
// -----------------
//   All incoming b-t-f CoT from allowed senders is parsed and forwarded
//   to tak_chat/in with chat_type, chatroom, chatroom_id populated.
//   TAK server handles group membership routing — if we receive it,
//   we forward it. No local membership tracking needed.
//
// MESSAGE FLOW:
// -------------
//   OUTGOING: BT node → tak_chat/out → TakChatNode → CoT XML → send_to_tak
//   INCOMING: incoming_cot → TakChatNode → parse → tak_chat/in → BT node
//
// CONFIGURATION (ROS Parameters):
// --------------------------------
//   callsign                 Robot callsign (default: "warthog1")
//   tak_server_flow_tag_key  TAK server session key
//   outgoing_cot_topic       Where to publish CoT XML (default: "send_to_tak")
//   incoming_cot_topic       Where to receive CoT XML (default: "incoming_cot")
//   navsat_topic             GPS source (default: "navsat")
//   tak_chat_out_topic       Requests from BT (default: "tak_chat/out")
//   tak_chat_in_topic        Events to BT (default: "tak_chat/in")
//   comms_topic              Mesh connectivity (default: "comms")
//   allowed_callsigns_file   Path to YAML with allowed callsigns
//   send_delay_s             Min delay between sends to same dest (default: 1.0)
//   retry_timeout_s          Total retry window (default: 10.0)
//   retry_interval_s         Interval between retries (default: 1.0)
//   min_retry_count          Min publishes even with subscriber (default: 1)
//   reply_delay_s            Buffer added to their timestamp (default: 1.0)
//
//==============================================================================

#include <rclcpp/rclcpp.hpp>
#include <std_msgs/msg/string.hpp>
#include <sensor_msgs/msg/nav_sat_fix.hpp>
#include <tak_chat/msg/tak_chat.hpp>
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
// Chat type string constants
// Used in TakChat.msg chat_type field and throughout this node.
//==============================================================================
namespace ChatType
{
    static const std::string UNICAST = "unicast";
    static const std::string GROUP = "group";
    static const std::string TEAM_COLOR = "team_color";
    static const std::string ROLE = "role";
    static const std::string ALL_CHAT_ROOMS = "all_chat_rooms";
    static const std::string ALL_GROUPS = "all_groups";
    static const std::string ALL_TEAMS = "all_teams";
}

//==============================================================================
// __chat parent attribute constants
// These are fixed TAK protocol values — do not change.
//==============================================================================
namespace ChatParent
{
    static const std::string ROOT_CONTACT_GROUP = "RootContactGroup";
    static const std::string USER_GROUPS = "UserGroups";
    static const std::string TEAM_GROUPS = "TeamGroups";
}

//==============================================================================
// Well-known chatroom/id constants for broadcast types
//==============================================================================
namespace ChatRoom
{
    static const std::string ALL_CHAT_ROOMS = "All Chat Rooms";
    static const std::string ALL_GROUPS = "Groups";
    static const std::string ALL_GROUPS_ID = "UserGroups";
    static const std::string ALL_TEAMS = "Teams";
    static const std::string ALL_TEAMS_ID = "TeamGroups";
}

//==============================================================================
// Legacy broadcast destination sentinel
//==============================================================================
static const std::string BROADCAST_DESTINATION = "ALL";

//==============================================================================
// Internal structs
//==============================================================================

/// Message waiting in the send queue before being dispatched
struct QueuedMessage
{
    tak_chat::msg::TakChat tak_msg;                    // Full TakChat message
    std::chrono::steady_clock::time_point queued_time; // When added to queue
};

/// Message being tracked for retry/delivery confirmation
struct PendingMessage
{
    std::string destination;    // Target callsign (unicast) or type label
    std::string message;        // Message text (for logging)
    std::string send_timestamp; // Timestamp embedded in CoT
    std::string cot_xml;        // Generated CoT XML

    std::chrono::steady_clock::time_point created_time;
    std::chrono::steady_clock::time_point last_publish_time;

    int publish_count;
    bool subscriber_seen;
    bool complete;
};

/// Tracks the last message received from a callsign (for reply ordering)
struct LastIncomingMessage
{
    std::string timestamp;
    std::chrono::steady_clock::time_point received_at;
};

//==============================================================================
// TakChatNode
//==============================================================================
class TakChatNode : public rclcpp::Node
{
public:
    //==========================================================================
    // CONSTRUCTOR
    //==========================================================================
    explicit TakChatNode(const rclcpp::NodeOptions &options = rclcpp::NodeOptions())
        : Node("tak_chat_node", options), fix_received_(false), current_lat_(0.0), current_lon_(0.0), comms_status_received_(false), has_base_station_comms_(false)
    {
        //----------------------------------------------------------------------
        // Declare parameters
        //----------------------------------------------------------------------
        declare_parameter<std::string>("callsign", "warthog1");
        declare_parameter<std::string>("tak_server_flow_tag_key",
                                       "TAK-Server-d520578543014e9cba1916fad77b9917");
        declare_parameter<std::string>("outgoing_cot_topic", "send_to_tak");
        declare_parameter<std::string>("incoming_cot_topic", "incoming_cot");
        declare_parameter<std::string>("navsat_topic", "navsat");
        declare_parameter<std::string>("tak_chat_out_topic", "tak_chat/out");
        declare_parameter<std::string>("tak_chat_in_topic", "tak_chat/in");
        declare_parameter<std::string>("comms_topic", "comms");
        declare_parameter<std::string>("allowed_callsigns_file",
                                       "/phoenix/src/phoenix-tak/src/tak_bridge/config/cot_runner.yaml");
        declare_parameter<double>("retry_timeout_s", 10.0);
        declare_parameter<double>("retry_interval_s", 1.0);
        declare_parameter<int>("min_retry_count", 1);
        declare_parameter<double>("send_delay_s", 1.0);
        declare_parameter<double>("reply_delay_s", 1.0);
        declare_parameter<std::vector<std::string>>("known_device_uids",
                                                    std::vector<std::string>{});

        //----------------------------------------------------------------------
        // Read parameters
        //----------------------------------------------------------------------
        callsign_ = get_parameter("callsign").as_string();
        tak_server_flow_tag_key_ = get_parameter("tak_server_flow_tag_key").as_string();
        retry_timeout_s_ = get_parameter("retry_timeout_s").as_double();
        retry_interval_s_ = get_parameter("retry_interval_s").as_double();
        min_retry_count_ = get_parameter("min_retry_count").as_int();
        send_delay_s_ = get_parameter("send_delay_s").as_double();
        reply_delay_s_ = get_parameter("reply_delay_s").as_double();

        const auto outgoing_cot_topic = get_parameter("outgoing_cot_topic").as_string();
        const auto incoming_cot_topic = get_parameter("incoming_cot_topic").as_string();
        const auto navsat_topic = get_parameter("navsat_topic").as_string();
        const auto tak_chat_out_topic = get_parameter("tak_chat_out_topic").as_string();
        const auto tak_chat_in_topic = get_parameter("tak_chat_in_topic").as_string();
        const auto comms_topic = get_parameter("comms_topic").as_string();
        const auto allowed_callsigns_file = get_parameter("allowed_callsigns_file").as_string();

        //----------------------------------------------------------------------
        // Load allowed callsigns
        //----------------------------------------------------------------------
        allowed_callsigns_ = loadAllowedCallsigns(allowed_callsigns_file);
        for (const auto &cs : allowed_callsigns_)
        {
            allowed_callsigns_set_.insert(cs);
        }

        //----------------------------------------------------------------------
        // QoS — RELIABLE + VOLATILE to match TakChatInterface and ATAK bridge
        //----------------------------------------------------------------------
        const auto qos = rclcpp::QoS(rclcpp::KeepLast(10))
                             .reliable()
                             .durability_volatile();

        //----------------------------------------------------------------------
        // Publishers
        //----------------------------------------------------------------------
        pub_cot_ = create_publisher<std_msgs::msg::String>(
            outgoing_cot_topic, qos);
        pub_tak_chat_in_ = create_publisher<tak_chat::msg::TakChat>(
            tak_chat_in_topic, qos);

        //----------------------------------------------------------------------
        // Subscribers
        //----------------------------------------------------------------------
        sub_navsat_ = create_subscription<sensor_msgs::msg::NavSatFix>(
            navsat_topic, qos,
            std::bind(&TakChatNode::navsatCallback, this, std::placeholders::_1));

        sub_comms_status_ = create_subscription<west_point_comms_sim::msg::CommsStatus>(
            comms_topic, qos,
            std::bind(&TakChatNode::commsStatusCallback, this, std::placeholders::_1));

        // Event callbacks on tak_chat/out subscription for discovery diagnostics
        rclcpp::SubscriptionOptions sub_opts;
        sub_opts.event_callbacks.matched_callback =
            [this](rclcpp::MatchedInfo &info)
        {
            RCLCPP_INFO(get_logger(),
                        "[tak_chat/out] Publisher count changed: %zu", info.current_count);
        };
        sub_opts.event_callbacks.incompatible_qos_callback =
            [this](rclcpp::QOSRequestedIncompatibleQoSInfo &info)
        {
            RCLCPP_WARN(get_logger(),
                        "[tak_chat/out] Incompatible QoS! Policy: %d", info.last_policy_kind);
        };

        sub_tak_chat_out_ = create_subscription<tak_chat::msg::TakChat>(
            tak_chat_out_topic, qos,
            std::bind(&TakChatNode::takChatOutCallback, this, std::placeholders::_1),
            sub_opts);

        sub_incoming_cot_ = create_subscription<std_msgs::msg::String>(
            incoming_cot_topic, qos,
            std::bind(&TakChatNode::incomingCotCallback, this, std::placeholders::_1));

        //----------------------------------------------------------------------
        // Timers
        //----------------------------------------------------------------------
        retry_timer_ = create_wall_timer(
            std::chrono::milliseconds(100),
            std::bind(&TakChatNode::retryTimerCallback, this));

        send_queue_timer_ = create_wall_timer(
            std::chrono::milliseconds(100),
            std::bind(&TakChatNode::sendQueueTimerCallback, this));

        //----------------------------------------------------------------------
        // Startup log
        //----------------------------------------------------------------------
        //----------------------------------------------------------------------
        // Pre-populate callsign → device UID map from config
        // WHY: The map is normally learned from incoming messages, but the BT
        // tree may send outgoing unicasts before any message has been received.
        // Format: ["CALLSIGN:DEVICE_UID", "TRILL:ANDROID-49c8964ab97f24bc"]
        //----------------------------------------------------------------------
        const auto known_uids = get_parameter("known_device_uids").as_string_array();
        for (const auto &entry : known_uids)
        {
            size_t sep = entry.find(':');
            if (sep != std::string::npos)
            {
                const std::string cs  = entry.substr(0, sep);
                const std::string uid = entry.substr(sep + 1);
                callsign_to_device_uid_[cs] = uid;
                RCLCPP_INFO(get_logger(),
                    "[UID Map] Pre-loaded: %s → %s", cs.c_str(), uid.c_str());
            }
            else
            {
                RCLCPP_WARN(get_logger(),
                    "[UID Map] Skipping malformed entry '%s' — expected 'CALLSIGN:DEVICE_UID'",
                    entry.c_str());
            }
        }

        //----------------------------------------------------------------------
        // Startup log
        //----------------------------------------------------------------------
        RCLCPP_INFO(get_logger(), "TakChatNode initialized:");
        RCLCPP_INFO(get_logger(), "  callsign:          %s", callsign_.c_str());
        RCLCPP_INFO(get_logger(), "  outgoing CoT:      %s", outgoing_cot_topic.c_str());
        RCLCPP_INFO(get_logger(), "  incoming CoT:      %s", incoming_cot_topic.c_str());
        RCLCPP_INFO(get_logger(), "  tak_chat/out:      %s", tak_chat_out_topic.c_str());
        RCLCPP_INFO(get_logger(), "  tak_chat/in:       %s", tak_chat_in_topic.c_str());
        RCLCPP_INFO(get_logger(), "  allowed callsigns: %zu", allowed_callsigns_.size());
        RCLCPP_INFO(get_logger(), "  send_delay_s:      %.1f", send_delay_s_);
        RCLCPP_INFO(get_logger(), "  retry_timeout_s:   %.1f", retry_timeout_s_);
        RCLCPP_INFO(get_logger(), "  min_retry_count:   %d", min_retry_count_);
        RCLCPP_INFO(get_logger(), "  known UIDs:        %zu pre-loaded", callsign_to_device_uid_.size());
    }

private:
    //==========================================================================
    // SECTION: YAML LOADING
    //==========================================================================

    std::vector<std::string> loadAllowedCallsigns(const std::string &yaml_path)
    {
        std::vector<std::string> callsigns;
        std::ifstream file(yaml_path);

        if (!file.is_open())
        {
            RCLCPP_ERROR(get_logger(),
                         "Could not open allowed callsigns file: %s", yaml_path.c_str());
            return callsigns;
        }

        std::string line;
        bool in_allowed_section = false;

        while (std::getline(file, line))
        {
            if (line.find("allowed:") != std::string::npos)
            {
                in_allowed_section = true;
                if (line.find('[') != std::string::npos &&
                    line.find(']') != std::string::npos)
                {
                    extractCallsignsFromLine(line, callsigns);
                    in_allowed_section = false;
                }
                continue;
            }

            if (in_allowed_section)
            {
                if (line.find(']') != std::string::npos)
                {
                    extractCallsignsFromLine(line, callsigns);
                    in_allowed_section = false;
                    continue;
                }
                extractCallsignsFromLine(line, callsigns);
            }
        }

        std::string list;
        for (size_t i = 0; i < callsigns.size(); ++i)
        {
            list += callsigns[i];
            if (i + 1 < callsigns.size())
                list += ", ";
        }
        RCLCPP_INFO(get_logger(), "Allowed callsigns: [%s]", list.c_str());
        return callsigns;
    }

    void extractCallsignsFromLine(const std::string &line,
                                  std::vector<std::string> &out)
    {
        size_t pos = 0;
        while ((pos = line.find('"', pos)) != std::string::npos)
        {
            size_t end = line.find('"', pos + 1);
            if (end == std::string::npos)
                break;
            std::string cs = line.substr(pos + 1, end - pos - 1);
            if (!cs.empty())
                out.push_back(cs);
            pos = end + 1;
        }
    }

    //==========================================================================
    // SECTION: SUBSCRIBER CALLBACKS
    //==========================================================================

    // -------------------------------------------------------------------------
    // GPS
    // -------------------------------------------------------------------------
    void navsatCallback(const sensor_msgs::msg::NavSatFix::SharedPtr msg)
    {
        std::lock_guard<std::mutex> lk(fix_mtx_);
        current_lat_ = msg->latitude;
        current_lon_ = msg->longitude;
        if (!fix_received_)
        {
            fix_received_ = true;
            RCLCPP_INFO(get_logger(), "GPS fix received (%.6f, %.6f)",
                        current_lat_, current_lon_);
        }
    }

    // -------------------------------------------------------------------------
    // Comms status
    // -------------------------------------------------------------------------
    void commsStatusCallback(
        const west_point_comms_sim::msg::CommsStatus::SharedPtr msg)
    {
        std::lock_guard<std::mutex> lock(comms_mutex_);

        bool new_has_comms = false;
        for (const auto &e : msg->transitive)
            if (e == "base_station")
            {
                new_has_comms = true;
                break;
            }
        if (!new_has_comms)
            for (const auto &e : msg->direct)
                if (e == "base_station")
                {
                    new_has_comms = true;
                    break;
                }

        if (!comms_status_received_)
        {
            comms_status_received_ = true;
            RCLCPP_INFO(get_logger(), "[Comms] base_station %s",
                        new_has_comms ? "REACHABLE" : "UNREACHABLE");
        }
        else if (new_has_comms != has_base_station_comms_)
        {
            RCLCPP_INFO(get_logger(), "[Comms] base_station %s",
                        new_has_comms ? "RESTORED" : "LOST");
        }

        has_base_station_comms_ = new_has_comms;
    }

    bool hasComms()
    {
        std::lock_guard<std::mutex> lock(comms_mutex_);
        // If comms_sim is not running, default to comms OK
        return !comms_status_received_ || has_base_station_comms_;
    }

    // -------------------------------------------------------------------------
    // tak_chat/out — main entry point from BT nodes
    // -------------------------------------------------------------------------
    void takChatOutCallback(const tak_chat::msg::TakChat::SharedPtr msg)
    {
        // Resolve chat_type — empty string means unicast (backward compat)
        const std::string chat_type = msg->chat_type.empty()
                                          ? ChatType::UNICAST
                                          : msg->chat_type;

        RCLCPP_INFO(get_logger(),
                    "[TakChat OUT] type='%s' origin='%s' dest='%s' chatroom='%s' msg='%s'",
                    chat_type.c_str(), msg->origin.c_str(), msg->destination.c_str(),
                    msg->chatroom.c_str(), msg->message.c_str());

        // Verify origin matches our callsign
        if (msg->origin != callsign_)
        {
            RCLCPP_WARN(get_logger(),
                        "[TakChat OUT] REJECTED — origin '%s' != callsign '%s'",
                        msg->origin.c_str(), callsign_.c_str());
            return;
        }

        // Comms gate — all TAK messages go through base_station
        if (!hasComms())
        {
            RCLCPP_WARN(get_logger(),
                        "[TakChat OUT] SUPPRESSED — no comms to base_station");
            return;
        }

        // ------------------------------------------------------------------
        // BACKWARD COMPAT: legacy destination="ALL" fan-out
        // Only applies when chat_type is empty or "unicast" AND dest is "ALL"
        // ------------------------------------------------------------------
        if ((chat_type == ChatType::UNICAST) &&
            (msg->destination == BROADCAST_DESTINATION))
        {
            RCLCPP_INFO(get_logger(),
                        "[TakChat OUT] Legacy ALL broadcast — fanning out to %zu callsigns",
                        allowed_callsigns_.size());

            for (const auto &dest : allowed_callsigns_)
            {
                tak_chat::msg::TakChat unicast = *msg;
                unicast.destination = dest;
                unicast.chat_type = ChatType::UNICAST;
                addToSendQueue(unicast);
            }
            return;
        }

        // All other types go directly to the send queue as-is
        addToSendQueue(*msg);
    }

    //==========================================================================
    // SECTION: SEND QUEUE
    //==========================================================================

    void addToSendQueue(const tak_chat::msg::TakChat &msg)
    {
        QueuedMessage q;
        q.tak_msg = msg;
        q.queued_time = std::chrono::steady_clock::now();

        std::lock_guard<std::mutex> lock(send_queue_mutex_);
        send_queue_.push_back(q);

        RCLCPP_DEBUG(get_logger(),
                     "[Queue] Added. Queue size: %zu", send_queue_.size());
    }

    void sendQueueTimerCallback()
    {
        auto now = std::chrono::steady_clock::now();
        auto wall_now = std::chrono::system_clock::now();
        double current_wall = std::chrono::duration<double>(
                                  wall_now.time_since_epoch())
                                  .count();

        std::vector<QueuedMessage> ready;

        {
            std::lock_guard<std::mutex> lock(send_queue_mutex_);
            if (send_queue_.empty())
                return;

            auto it = send_queue_.begin();
            while (it != send_queue_.end())
            {
                // ----------------------------------------------------------
                // Rate-limit key: for unicast use destination callsign,
                // for all other types use chat_type (they don't have a
                // per-callsign destination so we rate-limit per type).
                // ----------------------------------------------------------
                const std::string &chat_type = it->tak_msg.chat_type.empty()
                                                   ? ChatType::UNICAST
                                                   : it->tak_msg.chat_type;

                const std::string rate_key =
                    (chat_type == ChatType::UNICAST)
                        ? it->tak_msg.destination
                        : chat_type;

                bool can_send = true;

                // Check send delay per rate_key
                auto last_it = last_send_time_per_dest_.find(rate_key);
                if (last_it != last_send_time_per_dest_.end())
                {
                    double since_last = std::chrono::duration<double>(
                                            now - last_it->second)
                                            .count();
                    if (since_last < send_delay_s_)
                        can_send = false;
                }

                // Check reply delay (unicast only — reply ordering)
                if (can_send && chat_type == ChatType::UNICAST)
                {
                    auto earliest_it = earliest_reply_time_per_dest_.find(
                        it->tak_msg.destination);
                    if (earliest_it != earliest_reply_time_per_dest_.end() &&
                        current_wall < earliest_it->second)
                    {
                        can_send = false;
                    }
                }

                if (can_send)
                {
                    ready.push_back(*it);
                    last_send_time_per_dest_[rate_key] = now;
                    it = send_queue_.erase(it);
                }
                else
                {
                    ++it;
                }
            }
        }

        for (auto &q : ready)
        {
            if (!hasComms())
            {
                RCLCPP_WARN(get_logger(),
                            "[Queue] Comms lost while queued — dropping '%s'",
                            q.tak_msg.message.c_str());
                continue;
            }
            dispatchMessage(q.tak_msg);
        }
    }

    //==========================================================================
    // SECTION: DISPATCH — build CoT and add to pending
    //==========================================================================

    /**
     * @brief Build CoT XML for the given TakChat message and add to pending.
     *
     * This is the central dispatch point. It resolves the chat_type and
     * calls the appropriate CoT builder. The resulting CoT is stored in
     * pending_messages_ for retry tracking.
     *
     * PENDING KEY:
     *   For unicast: "destination|message" (existing behavior)
     *   For all other types: "chat_type|chatroom|message"
     *   This prevents duplicate sends while allowing the same message
     *   to be sent to different groups simultaneously.
     */
    void dispatchMessage(const tak_chat::msg::TakChat &msg)
    {
        const std::string chat_type = msg.chat_type.empty()
                                          ? ChatType::UNICAST
                                          : msg.chat_type;

        // Build pending key
        std::string key;
        if (chat_type == ChatType::UNICAST)
            key = msg.destination + "|" + msg.message;
        else
            key = chat_type + "|" + msg.chatroom + "|" + msg.message;

        {
            std::lock_guard<std::mutex> lock(pending_mutex_);
            if (pending_messages_.count(key))
            {
                RCLCPP_WARN(get_logger(),
                            "[Dispatch] Duplicate message already pending, skipping: '%s'",
                            msg.message.c_str());
                return;
            }
        }

        // Get timestamp (reply-ordered for unicast, current for all others)
        const std::string timestamp =
            (chat_type == ChatType::UNICAST)
                ? getResponseTimestamp(msg.destination)
                : nowISO();

        // Build CoT XML based on chat type
        std::string cot_xml;
        std::string log_dest;

        if (chat_type == ChatType::UNICAST)
        {
            cot_xml = buildUnicastCoT(msg, timestamp);
            log_dest = msg.destination;
        }
        else if (chat_type == ChatType::GROUP)
        {
            cot_xml = buildGroupCoT(msg, timestamp);
            log_dest = "group:" + msg.chatroom;
        }
        else if (chat_type == ChatType::TEAM_COLOR)
        {
            cot_xml = buildTeamColorCoT(msg, timestamp);
            log_dest = "team_color:" + msg.chatroom;
        }
        else if (chat_type == ChatType::ROLE)
        {
            cot_xml = buildRoleCoT(msg, timestamp);
            log_dest = "role:" + msg.chatroom;
        }
        else if (chat_type == ChatType::ALL_CHAT_ROOMS)
        {
            cot_xml = buildAllChatRoomsCoT(msg, timestamp);
            log_dest = "all_chat_rooms";
        }
        else if (chat_type == ChatType::ALL_GROUPS)
        {
            cot_xml = buildAllGroupsCoT(msg, timestamp);
            log_dest = "all_groups";
        }
        else if (chat_type == ChatType::ALL_TEAMS)
        {
            cot_xml = buildAllTeamsCoT(msg, timestamp);
            log_dest = "all_teams";
        }
        else
        {
            RCLCPP_ERROR(get_logger(),
                         "[Dispatch] Unknown chat_type '%s' — dropping message",
                         chat_type.c_str());
            return;
        }

        // Add to pending for retry tracking
        PendingMessage pending;
        pending.destination = log_dest;
        pending.message = msg.message;
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
    // SECTION: COT BUILDERS
    //==========================================================================

    // -------------------------------------------------------------------------
    // Shared: GPS position helper
    // -------------------------------------------------------------------------
    void getPosition(double &lat, double &lon)
    {
        std::lock_guard<std::mutex> lk(fix_mtx_);
        lat = current_lat_;
        lon = current_lon_;
    }

    // -------------------------------------------------------------------------
    // Shared: event header + point block
    //
    // event uid format varies by type:
    //   unicast:        GeoChat.{sender}.{dest}.{uuid}
    //   group:          GeoChat.{sender}.{group_uuid}.{uuid}
    //   team_color:     GeoChat.{sender}.{color}.{uuid}
    //   role:           GeoChat.{sender}.{role}.{uuid}
    //   all_chat_rooms: GeoChat.{sender}.All Chat Rooms.{uuid}
    //   all_groups:     GeoChat.{sender}.UserGroups.{uuid}
    //   all_teams:      GeoChat.{sender}.TeamGroups.{uuid}
    // -------------------------------------------------------------------------
    std::string buildEventHeader(const std::string &uid_middle_segment,
                                 const std::string &timestamp,
                                 const std::string &override_uid = "")
    {
        double lat = 0.0, lon = 0.0;
        getPosition(lat, lon);

        // Resolve message UUID
        std::string msg_uuid = override_uid.empty() ? randUUID() : override_uid;

        // Build event UID
        const std::string event_uid =
            "GeoChat." + callsign_ + "." + uid_middle_segment + "." + msg_uuid;

        // Stale = 24 hours from now
        const std::string stale = futureISO(86400);

        std::ostringstream xml;
        xml << std::fixed << std::setprecision(6);
        xml << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
            << "<event version=\"2.0\""
            << " uid=\"" << event_uid << "\""
            << " type=\"b-t-f\""
            << " how=\"h-g-i-g-o\""
            << " time=\"" << timestamp << "\""
            << " start=\"" << timestamp << "\""
            << " stale=\"" << stale << "\""
            << " access=\"Undefined\">"
            << "<point lat=\"" << lat << "\""
            << " lon=\"" << lon << "\""
            << " hae=\"0\""
            << " ce=\"9999999.0\""
            << " le=\"9999999.0\"/>";

        return xml.str();
    }

    // -------------------------------------------------------------------------
    // Shared: remarks element
    // -------------------------------------------------------------------------
    std::string buildRemarks(const std::string &sender,
                             const std::string &message,
                             const std::string &timestamp,
                             const std::string &to_attr = "")
    {
        std::ostringstream xml;
        xml << "<remarks"
            << " source=\"BAO.F.ATAK." << sender << "\""
            << (to_attr.empty() ? "" : " to=\"" + to_attr + "\"")
            << " time=\"" << timestamp << "\">"
            << escapeXml(message)
            << "</remarks>";
        return xml.str();
    }

    // -------------------------------------------------------------------------
    // Shared: link element (present in all types except unicast)
    // -------------------------------------------------------------------------
    std::string buildLink(const std::string &sender_uid)
    {
        return "<link uid=\"" + sender_uid + "\""
                                             " type=\"a-f-G-U-C\""
                                             " relation=\"p-p\"/>";
    }

    // -------------------------------------------------------------------------
    // Shared: flow tags
    // -------------------------------------------------------------------------
    std::string buildFlowTags(const std::string &timestamp)
    {
        return "<_flow_tags_ " + tak_server_flow_tag_key_ + "=\"" + timestamp + "\"/>";
    }

    // =========================================================================
    // UNICAST CoT
    // =========================================================================
    //
    // Structure:
    //   __chat parent="RootContactGroup" groupOwner="false"
    //          chatroom="{dest}" id="{dest}"
    //     chatgrp uid0="{sender}" uid1="{dest}" id="{dest}"
    //   <marti><dest callsign="{dest}"/></marti>
    //   <remarks ...>{message}</remarks>
    //
    // NOTE: No <link> element for unicast (matches observed ATAK behavior).
    // =========================================================================
    std::string buildUnicastCoT(const tak_chat::msg::TakChat &msg,
                                const std::string &timestamp)
    {
        auto [event_uuid, msg_uuid] = resolveUidOverride(msg.uid);

        const std::string &dest = msg.destination;
        const std::string &sender = msg.origin;

        // Destination device UID — learned from incoming messages.
        // Used as the middle segment of the event UID, __chat id, chatgrp uid1,
        // chatgrp id, and remarks to= attribute.
        // Falls back to callsign if not yet learned.
        std::string dest_device_uid;
        {
            std::lock_guard<std::mutex> lock(callsign_uid_mutex_);
            auto it = callsign_to_device_uid_.find(dest);
            dest_device_uid = (it != callsign_to_device_uid_.end())
                                  ? it->second
                                  : dest;
        }

        if (dest_device_uid == dest)
        {
            RCLCPP_WARN(get_logger(),
                        "[Unicast] No device UID known for '%s' — falling back to callsign. "
                        "Have '%s' send a message to us first to teach us their UID.",
                        dest.c_str(), dest.c_str());
        }

        const std::string msg_uuid_resolved = msg_uuid.empty() ? randUUID() : msg_uuid;

        std::ostringstream xml;
        // Event UID middle segment = dest device UID (not callsign)
        xml << buildEventHeader(dest_device_uid, timestamp, event_uuid);
        xml << "<detail>";
        xml << buildFlowTags(timestamp);
        xml << "<__chat"
            << " parent=\"" << ChatParent::ROOT_CONTACT_GROUP << "\""
            << " groupOwner=\"false\""
            << " messageId=\"" << msg_uuid_resolved << "\""
            << " chatroom=\"" << dest << "\""
            << " id=\"" << dest_device_uid << "\"" // dest device UID, not callsign
            << " senderCallsign=\"" << sender << "\">"
            << "<chatgrp"
            << " uid0=\"" << sender << "\""          // sender callsign, not device UID
            << " uid1=\"" << dest_device_uid << "\"" // dest device UID
            << " id=\"" << dest_device_uid << "\"/>" // dest device UID
            << "</__chat>";
        xml << "<marti><dest callsign=\"" << dest << "\"/></marti>";
        xml << buildLink(sender); // <link uid="warthog1"/> — was missing
        xml << buildRemarks(sender, msg.message, timestamp,
                            dest_device_uid); // to= attr with dest device UID
        xml << "</detail></event>";
        return xml.str();
    }

    // =========================================================================
    // NAMED GROUP CoT
    // =========================================================================
    //
    // Structure:
    //   __chat parent="UserGroups" groupOwner="true"
    //          chatroom="{group_name}" id="{group_id}"
    //     chatgrp uid0="{sender}" uid1..N="{members}" id="{group_id}"
    //     hierarchy > group(UserGroups) > group({group_id}) > contacts
    //   <link uid="{sender}"/>
    //   <remarks ...>{message}</remarks>
    //
    // member_uids[0] is always the sender. Additional members follow.
    // =========================================================================
    std::string buildGroupCoT(const tak_chat::msg::TakChat &msg,
                              const std::string &timestamp)
    {
        auto [event_uuid, msg_uuid] = resolveUidOverride(msg.uid);

        const std::string &sender = msg.origin;
        const std::string &group_name = msg.chatroom;
        const std::string &group_id = msg.chatroom_id;

        std::ostringstream xml;
        xml << buildEventHeader(group_id, timestamp, event_uuid);
        xml << "<detail>";
        xml << buildFlowTags(timestamp);

        // __chat opening tag
        xml << "<__chat"
            << " parent=\"" << ChatParent::USER_GROUPS << "\""
            << " groupOwner=\"true\""
            << " messageId=\"" << (msg_uuid.empty() ? randUUID() : msg_uuid) << "\""
            << " chatroom=\"" << group_name << "\""
            << " id=\"" << group_id << "\""
            << " senderCallsign=\"" << sender << "\">";

        // chatgrp — sender is uid0, members follow
        xml << "<chatgrp uid0=\"" << sender << "\"";
        for (size_t i = 0; i < msg.member_uids.size(); ++i)
        {
            // Skip if member_uid is the sender (already uid0)
            if (msg.member_uids[i] == sender)
                continue;
            xml << " uid" << (i + 1) << "=\"" << msg.member_uids[i] << "\"";
        }
        xml << " id=\"" << group_id << "\"/>";

        // hierarchy block
        xml << "<hierarchy>"
            << "<group uid=\"" << ChatParent::USER_GROUPS << "\" name=\"Groups\">"
            << "<group uid=\"" << group_id << "\" name=\"" << group_name << "\">";

        // Sender contact first
        xml << "<contact uid=\"" << sender << "\" name=\"" << sender << "\"/>";

        // Member contacts
        for (size_t i = 0; i < msg.member_uids.size(); ++i)
        {
            if (msg.member_uids[i] == sender)
                continue;
            const std::string &name = (i < msg.member_names.size())
                                          ? msg.member_names[i]
                                          : msg.member_uids[i];
            xml << "<contact uid=\"" << msg.member_uids[i]
                << "\" name=\"" << name << "\"/>";
        }

        xml << "</group></group></hierarchy>";
        xml << "</__chat>";

        xml << buildLink(sender);
        xml << buildRemarks(sender, msg.message, timestamp);
        xml << "</detail></event>";
        return xml.str();
    }

    // =========================================================================
    // TEAM COLOR CoT
    // =========================================================================
    //
    // Structure:
    //   __chat parent="TeamGroups" groupOwner="false"
    //          chatroom="{color}" id="{color}"
    //     chatgrp uid0="{sender}" id="{color}"
    //   <link uid="{sender}"/>
    //   <remarks ...>{message}</remarks>
    //
    // chatroom and id are both the color name (e.g. "Cyan", "Red").
    // =========================================================================
    std::string buildTeamColorCoT(const tak_chat::msg::TakChat &msg,
                                  const std::string &timestamp)
    {
        auto [event_uuid, msg_uuid] = resolveUidOverride(msg.uid);

        const std::string &sender = msg.origin;
        const std::string &color = msg.chatroom;

        std::ostringstream xml;
        xml << buildEventHeader(color, timestamp, event_uuid);
        xml << "<detail>";
        xml << buildFlowTags(timestamp);
        xml << "<__chat"
            << " parent=\"" << ChatParent::TEAM_GROUPS << "\""
            << " groupOwner=\"false\""
            << " messageId=\"" << (msg_uuid.empty() ? randUUID() : msg_uuid) << "\""
            << " chatroom=\"" << color << "\""
            << " id=\"" << color << "\""
            << " senderCallsign=\"" << sender << "\">"
            << "<chatgrp uid0=\"" << sender << "\" id=\"" << color << "\"/>"
            << "</__chat>";
        xml << buildLink(sender);
        xml << buildRemarks(sender, msg.message, timestamp);
        xml << "</detail></event>";
        return xml.str();
    }

    // =========================================================================
    // ROLE CoT
    // =========================================================================
    //
    // Structure:
    //   __chat parent="RootContactGroup" groupOwner="false"
    //          chatroom="{role}" id="{role}"
    //     chatgrp uid0="{sender}" id="{role}"
    //   <link uid="{sender}"/>
    //   <remarks ...>{message}</remarks>
    //
    // Structurally identical to team_color except parent="RootContactGroup".
    // =========================================================================
    std::string buildRoleCoT(const tak_chat::msg::TakChat &msg,
                             const std::string &timestamp)
    {
        auto [event_uuid, msg_uuid] = resolveUidOverride(msg.uid);

        const std::string &sender = msg.origin;
        const std::string &role = msg.chatroom;

        std::ostringstream xml;
        xml << buildEventHeader(role, timestamp, event_uuid);
        xml << "<detail>";
        xml << buildFlowTags(timestamp);
        xml << "<__chat"
            << " parent=\"" << ChatParent::ROOT_CONTACT_GROUP << "\""
            << " groupOwner=\"false\""
            << " messageId=\"" << (msg_uuid.empty() ? randUUID() : msg_uuid) << "\""
            << " chatroom=\"" << role << "\""
            << " id=\"" << role << "\""
            << " senderCallsign=\"" << sender << "\">"
            << "<chatgrp uid0=\"" << sender << "\" id=\"" << role << "\"/>"
            << "</__chat>";
        xml << buildLink(sender);
        xml << buildRemarks(sender, msg.message, timestamp);
        xml << "</detail></event>";
        return xml.str();
    }

    // =========================================================================
    // ALL CHAT ROOMS CoT
    // =========================================================================
    //
    // Structure:
    //   __chat parent="RootContactGroup" groupOwner="false"
    //          chatroom="All Chat Rooms" id="All Chat Rooms"
    //     chatgrp uid0="{sender}" uid1="All Chat Rooms" id="All Chat Rooms"
    //   <link uid="{sender}"/>
    //   <remarks to="All Chat Rooms" ...>{message}</remarks>
    //
    // NOTE: remarks has a to="All Chat Rooms" attribute — unique to this type.
    // =========================================================================
    std::string buildAllChatRoomsCoT(const tak_chat::msg::TakChat &msg,
                                     const std::string &timestamp)
    {
        auto [event_uuid, msg_uuid] = resolveUidOverride(msg.uid);

        const std::string &sender = msg.origin;

        std::ostringstream xml;
        xml << buildEventHeader(ChatRoom::ALL_CHAT_ROOMS, timestamp, event_uuid);
        xml << "<detail>";
        xml << buildFlowTags(timestamp);
        xml << "<__chat"
            << " parent=\"" << ChatParent::ROOT_CONTACT_GROUP << "\""
            << " groupOwner=\"false\""
            << " messageId=\"" << (msg_uuid.empty() ? randUUID() : msg_uuid) << "\""
            << " chatroom=\"" << ChatRoom::ALL_CHAT_ROOMS << "\""
            << " id=\"" << ChatRoom::ALL_CHAT_ROOMS << "\""
            << " senderCallsign=\"" << sender << "\">"
            << "<chatgrp"
            << " uid0=\"" << sender << "\""
            << " uid1=\"" << ChatRoom::ALL_CHAT_ROOMS << "\""
            << " id=\"" << ChatRoom::ALL_CHAT_ROOMS << "\"/>"
            << "</__chat>";
        xml << buildLink(sender);
        // NOTE: to= attribute is unique to All Chat Rooms
        xml << buildRemarks(sender, msg.message, timestamp, ChatRoom::ALL_CHAT_ROOMS);
        xml << "</detail></event>";
        return xml.str();
    }

    // =========================================================================
    // ALL GROUPS CoT
    // =========================================================================
    //
    // Structure:
    //   __chat parent="RootContactGroup" groupOwner="false"
    //          chatroom="Groups" id="UserGroups"
    //     chatgrp uid0="{sender}" id="UserGroups"
    //   <link uid="{sender}"/>
    //   <remarks ...>{message}</remarks>
    //
    // NOTE: chatroom != id here ("Groups" vs "UserGroups").
    // =========================================================================
    std::string buildAllGroupsCoT(const tak_chat::msg::TakChat &msg,
                                  const std::string &timestamp)
    {
        auto [event_uuid, msg_uuid] = resolveUidOverride(msg.uid);

        const std::string &sender = msg.origin;

        std::ostringstream xml;
        xml << buildEventHeader(ChatRoom::ALL_GROUPS_ID, timestamp, event_uuid);
        xml << "<detail>";
        xml << buildFlowTags(timestamp);
        xml << "<__chat"
            << " parent=\"" << ChatParent::ROOT_CONTACT_GROUP << "\""
            << " groupOwner=\"false\""
            << " messageId=\"" << (msg_uuid.empty() ? randUUID() : msg_uuid) << "\""
            << " chatroom=\"" << ChatRoom::ALL_GROUPS << "\""
            << " id=\"" << ChatRoom::ALL_GROUPS_ID << "\""
            << " senderCallsign=\"" << sender << "\">"
            << "<chatgrp uid0=\"" << sender << "\" id=\"" << ChatRoom::ALL_GROUPS_ID << "\"/>"
            << "</__chat>";
        xml << buildLink(sender);
        xml << buildRemarks(sender, msg.message, timestamp);
        xml << "</detail></event>";
        return xml.str();
    }

    // =========================================================================
    // ALL TEAMS CoT
    // =========================================================================
    //
    // Structure:
    //   __chat parent="RootContactGroup" groupOwner="false"
    //          chatroom="Teams" id="TeamGroups"
    //     chatgrp uid0="{sender}" id="TeamGroups"
    //   <link uid="{sender}"/>
    //   <remarks ...>{message}</remarks>
    //
    // NOTE: chatroom != id here ("Teams" vs "TeamGroups").
    // =========================================================================
    std::string buildAllTeamsCoT(const tak_chat::msg::TakChat &msg,
                                 const std::string &timestamp)
    {
        auto [event_uuid, msg_uuid] = resolveUidOverride(msg.uid);

        const std::string &sender = msg.origin;

        std::ostringstream xml;
        xml << buildEventHeader(ChatRoom::ALL_TEAMS_ID, timestamp, event_uuid);
        xml << "<detail>";
        xml << buildFlowTags(timestamp);
        xml << "<__chat"
            << " parent=\"" << ChatParent::ROOT_CONTACT_GROUP << "\""
            << " groupOwner=\"false\""
            << " messageId=\"" << (msg_uuid.empty() ? randUUID() : msg_uuid) << "\""
            << " chatroom=\"" << ChatRoom::ALL_TEAMS << "\""
            << " id=\"" << ChatRoom::ALL_TEAMS_ID << "\""
            << " senderCallsign=\"" << sender << "\">"
            << "<chatgrp uid0=\"" << sender << "\" id=\"" << ChatRoom::ALL_TEAMS_ID << "\"/>"
            << "</__chat>";
        xml << buildLink(sender);
        xml << buildRemarks(sender, msg.message, timestamp);
        xml << "</detail></event>";
        return xml.str();
    }

    //==========================================================================
    // SECTION: RETRY LOGIC
    //==========================================================================

    void retryTimerCallback()
    {
        auto now = std::chrono::steady_clock::now();
        std::vector<std::string> to_remove;
        std::vector<std::string> to_publish;

        size_t sub_count = pub_cot_->get_subscription_count();

        {
            std::lock_guard<std::mutex> lock(pending_mutex_);

            for (auto &[key, pending] : pending_messages_)
            {
                if (pending.complete)
                {
                    to_remove.push_back(key);
                    continue;
                }

                double elapsed = std::chrono::duration<double>(
                                     now - pending.created_time)
                                     .count();

                if (elapsed >= retry_timeout_s_)
                {
                    if (pending.subscriber_seen)
                    {
                        RCLCPP_WARN(get_logger(),
                                    "[Retry] Timeout after %d publishes — subscriber seen, "
                                    "likely delivered: '%s'",
                                    pending.publish_count, pending.destination.c_str());
                    }
                    else
                    {
                        RCLCPP_ERROR(get_logger(),
                                     "[Retry] Timeout after %d publishes — NO SUBSCRIBER, "
                                     "message likely LOST: '%s'",
                                     pending.publish_count, pending.destination.c_str());
                    }
                    to_remove.push_back(key);
                    continue;
                }

                if (sub_count > 0)
                    pending.subscriber_seen = true;

                if (pending.subscriber_seen &&
                    pending.publish_count >= min_retry_count_)
                {
                    RCLCPP_INFO(get_logger(),
                                "[Retry] Delivered to '%s' after %d publish(es)",
                                pending.destination.c_str(), pending.publish_count);
                    pending.complete = true;
                    to_remove.push_back(key);
                    continue;
                }

                if (pending.publish_count > 0)
                {
                    double since_last = std::chrono::duration<double>(
                                            now - pending.last_publish_time)
                                            .count();
                    if (since_last >= retry_interval_s_)
                        to_publish.push_back(key);
                }
            }
        }

        for (const auto &key : to_publish)
            doPublish(key);

        {
            std::lock_guard<std::mutex> lock(pending_mutex_);
            for (const auto &key : to_remove)
                pending_messages_.erase(key);
        }
    }

    void doPublish(const std::string &key)
    {
        std::string cot_xml;
        std::string destination;
        int publish_count;

        {
            std::lock_guard<std::mutex> lock(pending_mutex_);
            auto it = pending_messages_.find(key);
            if (it == pending_messages_.end())
                return;

            PendingMessage &p = it->second;
            p.publish_count++;
            p.last_publish_time = std::chrono::steady_clock::now();

            cot_xml = p.cot_xml;
            destination = p.destination;
            publish_count = p.publish_count;
        }

        std_msgs::msg::String out;
        out.data = cot_xml;
        pub_cot_->publish(out);

        size_t sub_count = pub_cot_->get_subscription_count();

        if (publish_count == 1)
        {
            RCLCPP_INFO(get_logger(),
                        "[Publish #1] To: '%s' | subscribers: %zu",
                        destination.c_str(), sub_count);
        }
        else
        {
            RCLCPP_INFO(get_logger(),
                        "[Publish #%d] To: '%s' | subscribers: %zu",
                        publish_count, destination.c_str(), sub_count);
        }
    }

    //==========================================================================
    // SECTION: INCOMING COT PARSING
    //==========================================================================

    /**
     * @brief Parse incoming b-t-f CoT and forward to tak_chat/in.
     *
     * TAK server handles group membership routing. If we receive a message,
     * the server decided to deliver it to us. We forward all b-t-f messages
     * from allowed senders without local membership filtering.
     *
     * Populates chat_type, chatroom, chatroom_id on the forwarded TakChat
     * message so BT nodes can filter/route based on message type.
     */
    void incomingCotCallback(const std_msgs::msg::String::SharedPtr msg)
    {
        // Only process GeoChat messages
        if (msg->data.find("type=\"b-t-f\"") == std::string::npos)
            return;

        // Parse all fields
        std::string sender, chatroom, chatroom_id, chat_parent,
            message, timestamp, chat_type;

        if (!parseGeoChatCoT(msg->data, sender, chatroom, chatroom_id,
                             chat_parent, message, timestamp, chat_type))
        {
            RCLCPP_WARN(get_logger(), "[Incoming] Failed to parse GeoChat CoT");
            return;
        }

        // Ignore our own echoes
        if (sender == callsign_)
            return;

        // Comms gate
        if (!hasComms())
        {
            RCLCPP_WARN(get_logger(),
                        "[Incoming] SUPPRESSED — no comms to base_station | from: '%s'",
                        sender.c_str());
            return;
        }

        // Filter to allowed senders
        if (allowed_callsigns_set_.find(sender) == allowed_callsigns_set_.end())
        {
            RCLCPP_DEBUG(get_logger(),
                         "[Incoming] IGNORED — sender '%s' not in allowed list", sender.c_str());
            return;
        }

        // Record for reply ordering (unicast only)
        if (chat_type == ChatType::UNICAST)
        {
            recordIncomingMessage(sender, timestamp);
        }

        // Learn sender's device UID from chatgrp uid0.
        // uid0 is always the sender's device UID (e.g. ANDROID-49c8964ab97f24bc).
        // We cache this so outgoing unicasts can populate uid1 correctly.
        {
            const auto member_uids = parseChatGrpMembers(msg->data);
            if (!member_uids.empty() && member_uids[0] != sender)
            {
                std::lock_guard<std::mutex> lock(callsign_uid_mutex_);
                callsign_to_device_uid_[sender] = member_uids[0];
                RCLCPP_DEBUG(get_logger(),
                             "[Incoming] Learned UID: %s → %s",
                             sender.c_str(), member_uids[0].c_str());
            }
        }

        RCLCPP_INFO(get_logger(),
                    "[Incoming] type='%s' from='%s' chatroom='%s' msg='%s'",
                    chat_type.c_str(), sender.c_str(), chatroom.c_str(), message.c_str());

        // Forward to BT nodes
        tak_chat::msg::TakChat out;
        out.origin = sender;
        out.destination = callsign_;
        out.message = message;
        out.timestamp = timestamp;
        out.chat_type = chat_type;
        out.chatroom = chatroom;
        out.chatroom_id = chatroom_id;

        // Populate uid from messageId (useful for downstream deduplication)
        out.uid = extractXmlAttr(msg->data, "messageId");

        // Populate member lists from chatgrp uid0..N attributes
        out.member_uids = parseChatGrpMembers(msg->data);

        // For group type, resolve display names from <hierarchy><contact> elements.
        // For all other types, fall back to using uids as names.
        if (chat_type == ChatType::GROUP)
        {
            auto name_map = parseHierarchyNames(msg->data);
            out.member_names.reserve(out.member_uids.size());
            for (const auto &uid : out.member_uids)
            {
                auto it = name_map.find(uid);
                out.member_names.push_back(
                    (it != name_map.end()) ? it->second : uid);
            }
        }
        else
        {
            // Non-group types have no hierarchy block — names mirror uids
            out.member_names = out.member_uids;
        }

        pub_tak_chat_in_->publish(out);
    }

    /**
     * @brief Parse a b-t-f GeoChat CoT XML string into its component fields.
     *
     * Determines chat_type by examining __chat parent and chatroom/id values:
     *
     *   parent=UserGroups                          → group
     *   parent=TeamGroups                          → team_color
     *   parent=RootContactGroup, chatroom=chatroom_id:
     *     chatroom == "All Chat Rooms"             → all_chat_rooms
     *     chatroom == "Groups"                     → all_groups
     *     chatroom == "Teams"                      → all_teams
     *     otherwise                                → role
     *   parent=RootContactGroup, chatroom!=chatroom_id → unicast
     *     (unicast: chatroom=dest callsign, id=dest callsign — they match,
     *      but we distinguish from role by checking if chatroom is in
     *      allowed_callsigns_set_)
     *
     * NOTE ON UNICAST vs ROLE DISAMBIGUATION:
     *   Both unicast and role have parent=RootContactGroup and chatroom==id.
     *   We distinguish them by checking if chatroom is a known callsign.
     *   If chatroom is in allowed_callsigns_set_ → unicast.
     *   Otherwise → role.
     */
    bool parseGeoChatCoT(const std::string &xml,
                         std::string &sender,
                         std::string &chatroom,
                         std::string &chatroom_id,
                         std::string &chat_parent,
                         std::string &message,
                         std::string &timestamp,
                         std::string &chat_type)
    {
        // Required: senderCallsign
        sender = extractXmlAttr(xml, "senderCallsign");
        if (sender.empty())
            return false;

        // Required: message text from <remarks>
        message = extractXmlElementContent(xml, "remarks");
        if (message.empty())
            return false;
        message = unescapeXml(message);

        // Chatroom and id attributes from __chat
        chatroom = extractXmlAttr(xml, "chatroom");
        chatroom_id = extractXmlAttr(xml, "id");
        chat_parent = extractXmlAttr(xml, "parent");

        // Timestamp — prefer remarks time, fall back to event time
        timestamp = extractRemarksTime(xml);
        if (timestamp.empty())
            timestamp = extractXmlAttr(xml, "time");
        if (timestamp.empty())
            timestamp = nowISO();

        // ------------------------------------------------------------------
        // Determine chat_type from structural attributes
        // ------------------------------------------------------------------
        if (chat_parent == ChatParent::USER_GROUPS)
        {
            chat_type = ChatType::GROUP;
        }
        else if (chat_parent == ChatParent::TEAM_GROUPS)
        {
            chat_type = ChatType::TEAM_COLOR;
        }
        else if (chat_parent == ChatParent::ROOT_CONTACT_GROUP)
        {
            if (chatroom == ChatRoom::ALL_CHAT_ROOMS)
            {
                chat_type = ChatType::ALL_CHAT_ROOMS;
            }
            else if (chatroom == ChatRoom::ALL_GROUPS)
            {
                chat_type = ChatType::ALL_GROUPS;
            }
            else if (chatroom == ChatRoom::ALL_TEAMS)
            {
                chat_type = ChatType::ALL_TEAMS;
            }
            else
            {
                // Unicast vs Role disambiguation:
                // If chatroom is a known callsign → unicast
                // Otherwise → role
                if (allowed_callsigns_set_.count(chatroom) ||
                    chatroom == callsign_)
                {
                    chat_type = ChatType::UNICAST;
                }
                else
                {
                    chat_type = ChatType::ROLE;
                }
            }
        }
        else
        {
            // Unknown parent — treat as unicast for safety
            chat_type = ChatType::UNICAST;
        }

        return true;
    }

    //==========================================================================
    // SECTION: REPLY TIMESTAMP ORDERING
    //==========================================================================

    void recordIncomingMessage(const std::string &callsign,
                               const std::string &their_timestamp)
    {
        {
            std::lock_guard<std::mutex> lock(last_incoming_mutex_);
            LastIncomingMessage rec;
            rec.timestamp = their_timestamp;
            rec.received_at = std::chrono::steady_clock::now();
            last_incoming_messages_[callsign] = rec;
        }

        double their_time = parseISOTimestamp(their_timestamp);

        {
            std::lock_guard<std::mutex> lock(send_queue_mutex_);
            auto now = std::chrono::system_clock::now();
            double current = std::chrono::duration<double>(
                                 now.time_since_epoch())
                                 .count();

            if (their_time > 0)
            {
                double ideal = their_time + reply_delay_s_;
                double max_wait = current + MAX_REPLY_WAIT_S;
                earliest_reply_time_per_dest_[callsign] =
                    std::min(ideal, max_wait);
            }
            else
            {
                earliest_reply_time_per_dest_[callsign] = current + reply_delay_s_;
            }
        }
    }

    std::string getResponseTimestamp(const std::string &destination)
    {
        std::lock_guard<std::mutex> lock(last_incoming_mutex_);

        auto now = std::chrono::system_clock::now();
        double current = std::chrono::duration<double>(
                             now.time_since_epoch())
                             .count();

        auto it = last_incoming_messages_.find(destination);
        if (it == last_incoming_messages_.end())
            return secondsToISO(current + CURRENT_TIME_BUFFER_S);

        // Stale after 5 minutes
        auto age = std::chrono::steady_clock::now() - it->second.received_at;
        if (age > std::chrono::minutes(5))
            return secondsToISO(current + CURRENT_TIME_BUFFER_S);

        double their_time = parseISOTimestamp(it->second.timestamp);
        if (their_time < 0)
            return secondsToISO(current + CURRENT_TIME_BUFFER_S);

        double response = std::max(their_time + RESPONSE_DELAY_S,
                                   current + CURRENT_TIME_BUFFER_S);

        // Update stored timestamp so consecutive replies keep advancing
        it->second.timestamp = secondsToISO(response);

        return secondsToISO(response);
    }

    //==========================================================================
    // SECTION: XML UTILITIES
    //==========================================================================

    std::string extractXmlAttr(const std::string &xml,
                               const std::string &attr_name)
    {
        const std::string search = " " + attr_name + "=\"";
        size_t start = xml.find(search);
        if (start == std::string::npos)
            return "";
        start += search.size();
        size_t end = xml.find('"', start);
        if (end == std::string::npos)
            return "";
        return xml.substr(start, end - start);
    }

    std::string extractXmlElementContent(const std::string &xml,
                                         const std::string &element)
    {
        const std::string open = "<" + element;
        size_t tag_start = xml.find(open);
        if (tag_start == std::string::npos)
            return "";
        size_t content_start = xml.find('>', tag_start);
        if (content_start == std::string::npos)
            return "";
        ++content_start;
        const std::string close = "</" + element + ">";
        size_t content_end = xml.find(close, content_start);
        if (content_end == std::string::npos)
            return "";
        return xml.substr(content_start, content_end - content_start);
    }

    std::string extractRemarksTime(const std::string &xml)
    {
        size_t start = xml.find("<remarks");
        if (start == std::string::npos)
            return "";
        size_t end = xml.find('>', start);
        if (end == std::string::npos)
            return "";
        std::string tag = xml.substr(start, end - start);

        size_t t = tag.find("time=\"");
        if (t == std::string::npos)
            return "";
        t += 6;
        size_t te = tag.find('"', t);
        if (te == std::string::npos)
            return "";
        return tag.substr(t, te - t);
    }

    static std::string unescapeXml(const std::string &s)
    {
        std::string r = s;
        size_t pos;
        while ((pos = r.find("<")) != std::string::npos)
            r.replace(pos, 4, "<");
        while ((pos = r.find(">")) != std::string::npos)
            r.replace(pos, 4, ">");
        while ((pos = r.find("&quot;")) != std::string::npos)
            r.replace(pos, 6, "\"");
        while ((pos = r.find("&apos;")) != std::string::npos)
            r.replace(pos, 6, "'");
        while ((pos = r.find("'")) != std::string::npos)
            r.replace(pos, 6, "'");
        while ((pos = r.find("&")) != std::string::npos)
            r.replace(pos, 5, "&");
        return r;
    }

    static std::string escapeXml(const std::string &s)
    {
        std::string r;
        r.reserve(static_cast<size_t>(s.size() * 1.1));
        for (char c : s)
        {
            switch (c)
            {
            case '&':
                r += "&";
                break;
            case '<':
                r += "<";
                break;
            case '>':
                r += ">";
                break;
            case '"':
                r += "&quot;";
                break;
            case '\'':
                r += "&apos;";
                break;
            default:
                r += c;
                break;
            }
        }
        return r;
    }

    // -------------------------------------------------------------------------
    // parseChatGrpMembers()
    // -------------------------------------------------------------------------
    // Extracts uid0, uid1, uid2... from the <chatgrp> element.
    // WHY: chatgrp encodes group membership for all chat types. We parse all
    // sequential uidN attributes until one is missing.
    // -------------------------------------------------------------------------
    static std::vector<std::string> parseChatGrpMembers(const std::string &xml)
    {
        std::vector<std::string> uids;

        // Find the chatgrp element bounds first to avoid matching uid attributes
        // from other elements (e.g. <link uid="...">)
        size_t grp_start = xml.find("<chatgrp");
        if (grp_start == std::string::npos)
            return uids;
        size_t grp_end = xml.find("/>", grp_start);
        if (grp_end == std::string::npos)
            return uids;
        const std::string grp = xml.substr(grp_start, grp_end - grp_start);

        // Extract uid0, uid1, uid2... until one is missing
        for (int i = 0; i < 64; ++i)
        {
            const std::string key = " uid" + std::to_string(i) + "=\"";
            size_t pos = grp.find(key);
            if (pos == std::string::npos)
                break;
            pos += key.size();
            size_t end = grp.find('"', pos);
            if (end == std::string::npos)
                break;
            uids.push_back(grp.substr(pos, end - pos));
        }

        return uids;
    }

    // -------------------------------------------------------------------------
    // parseHierarchyNames()
    // -------------------------------------------------------------------------
    // Extracts uid→name pairs from <contact uid="..." name="..."/> elements
    // inside the <hierarchy> block (present in group-type CoT only).
    // Returns a map from uid to display name.
    // -------------------------------------------------------------------------
    static std::map<std::string, std::string> parseHierarchyNames(
        const std::string &xml)
    {
        std::map<std::string, std::string> names;

        size_t hier_start = xml.find("<hierarchy>");
        size_t hier_end = xml.find("</hierarchy>");
        if (hier_start == std::string::npos || hier_end == std::string::npos)
            return names;

        const std::string hier = xml.substr(hier_start, hier_end - hier_start);

        size_t pos = 0;
        while ((pos = hier.find("<contact", pos)) != std::string::npos)
        {
            size_t tag_end = hier.find("/>", pos);
            if (tag_end == std::string::npos)
                break;
            const std::string tag = hier.substr(pos, tag_end - pos);

            // Extract uid and name from this <contact> tag
            std::string uid, name;

            size_t u = tag.find(" uid=\"");
            if (u != std::string::npos)
            {
                u += 6;
                size_t ue = tag.find('"', u);
                if (ue != std::string::npos)
                    uid = tag.substr(u, ue - u);
            }

            size_t n = tag.find(" name=\"");
            if (n != std::string::npos)
            {
                n += 7;
                size_t ne = tag.find('"', n);
                if (ne != std::string::npos)
                    name = tag.substr(n, ne - n);
            }

            if (!uid.empty())
                names[uid] = name.empty() ? uid : name;
            pos = tag_end + 2;
        }

        return names;
    }

    //==========================================================================
    // SECTION: TIMESTAMP UTILITIES
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

    static std::string futureISO(int seconds_ahead)
    {
        using namespace std::chrono;
        auto fut = system_clock::now() + seconds(seconds_ahead);
        std::time_t t = system_clock::to_time_t(fut);
        std::tm tm{};
        gmtime_r(&t, &tm);
        std::ostringstream oss;
        oss << std::put_time(&tm, "%Y-%m-%dT%H:%M:%S");
        auto us = duration_cast<microseconds>(fut.time_since_epoch()).count() % 1000000;
        oss << "." << std::setw(6) << std::setfill('0') << us << "Z";
        return oss.str();
    }

    std::string secondsToISO(double seconds_since_epoch)
    {
        using namespace std::chrono;
        auto tp = system_clock::time_point(
            duration_cast<system_clock::duration>(duration<double>(seconds_since_epoch)));
        std::time_t t = system_clock::to_time_t(tp);
        std::tm tm{};
        gmtime_r(&t, &tm);
        std::ostringstream oss;
        oss << std::put_time(&tm, "%Y-%m-%dT%H:%M:%S");
        auto us = duration_cast<microseconds>(tp.time_since_epoch()).count() % 1000000;
        oss << "." << std::setw(6) << std::setfill('0') << us << "Z";
        return oss.str();
    }

    static double parseISOTimestamp(const std::string &ts)
    {
        if (ts.empty())
            return -1.0;
        std::tm tm = {};
        double frac = 0.0;

        size_t t_pos = ts.find('T');
        if (t_pos == std::string::npos)
            return -1.0;

        if (sscanf(ts.c_str(), "%d-%d-%d",
                   &tm.tm_year, &tm.tm_mon, &tm.tm_mday) != 3)
            return -1.0;
        tm.tm_year -= 1900;
        tm.tm_mon -= 1;

        if (sscanf(ts.c_str() + t_pos + 1, "%d:%d:%d",
                   &tm.tm_hour, &tm.tm_min, &tm.tm_sec) != 3)
            return -1.0;

        size_t dot = ts.find('.', t_pos);
        if (dot != std::string::npos)
        {
            size_t z = ts.find('Z', dot);
            if (z == std::string::npos)
                z = ts.size();
            frac = std::stod(ts.substr(dot, z - dot));
        }

        time_t sec = timegm(&tm);
        if (sec == -1)
            return -1.0;
        return static_cast<double>(sec) + frac;
    }

    //==========================================================================
    // SECTION: UUID + UID UTILITIES
    //==========================================================================

    static std::string randUUID()
    {
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<int> d(0, 15);
        std::uniform_int_distribution<int> d2(8, 11);

        std::ostringstream ss;
        ss << std::hex;
        for (int i = 0; i < 8; ++i)
        {
            ss << d(gen);
        }
        ss << '-';
        for (int i = 0; i < 4; ++i)
        {
            ss << d(gen);
        }
        ss << "-4";
        for (int i = 0; i < 3; ++i)
        {
            ss << d(gen);
        }
        ss << '-';
        ss << d2(gen);
        for (int i = 0; i < 3; ++i)
        {
            ss << d(gen);
        }
        ss << '-';
        for (int i = 0; i < 12; ++i)
        {
            ss << d(gen);
        }
        return ss.str();
    }

    /**
     * @brief Resolve the uid field into event_uuid and msg_uuid components.
     *
     * uid field format: "event_uuid|msg_uuid"
     *   - Both parts optional. Empty part → auto-generate.
     *   - Legacy format (no '|'): use same value for both.
     *   - Empty string: both auto-generated (normal operation).
     *
     * @return pair<event_uuid, msg_uuid> — either may be empty (→ auto-generate)
     */
    static std::pair<std::string, std::string> resolveUidOverride(
        const std::string &uid_field)
    {
        if (uid_field.empty())
            return {"", ""};

        size_t sep = uid_field.find('|');
        if (sep != std::string::npos)
            return {uid_field.substr(0, sep), uid_field.substr(sep + 1)};

        // Legacy: no separator — use same value for both
        return {uid_field, uid_field};
    }

    //==========================================================================
    // MEMBER VARIABLES
    //==========================================================================

    // --- Identity ---
    std::string callsign_;
    std::string tak_server_flow_tag_key_;

    // Callsign → device UID learned from incoming CoT.
    // WHY: ATAK routes chatgrp by device UID, not callsign.
    // Populated passively when we receive messages from a callsign.
    std::map<std::string, std::string> callsign_to_device_uid_;
    std::mutex callsign_uid_mutex_;

    // --- Allowed callsigns ---
    std::vector<std::string> allowed_callsigns_;
    std::set<std::string> allowed_callsigns_set_;

    // --- Timing configuration ---
    double retry_timeout_s_;
    double retry_interval_s_;
    int min_retry_count_;
    double send_delay_s_;
    double reply_delay_s_;

    // --- GPS ---
    std::mutex fix_mtx_;
    bool fix_received_;
    double current_lat_;
    double current_lon_;

    // --- Comms status ---
    std::mutex comms_mutex_;
    bool comms_status_received_;
    bool has_base_station_comms_;

    // --- Send queue ---
    std::deque<QueuedMessage> send_queue_;
    std::mutex send_queue_mutex_;
    std::map<std::string, std::chrono::steady_clock::time_point> last_send_time_per_dest_;
    std::map<std::string, double> earliest_reply_time_per_dest_;

    // --- Pending messages (retry tracking) ---
    std::map<std::string, PendingMessage> pending_messages_;
    std::mutex pending_mutex_;

    // --- Reply ordering ---
    std::map<std::string, LastIncomingMessage> last_incoming_messages_;
    std::mutex last_incoming_mutex_;

    static constexpr double RESPONSE_DELAY_S = 2.0;
    static constexpr double CURRENT_TIME_BUFFER_S = 0.5;
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