# tak_chat

ROS2 package for TAK/ATAK chat communication via Cursor-on-Target (CoT) protocol.

## Overview

`tak_chat` provides a dedicated ROS2 node (`TakChatNode`) that bridges ROS2 messaging with the TAK ecosystem. It handles bidirectional translation between simple `TakChat` ROS2 messages (published by BT nodes or other ROS2 components) and the CoT GeoChat XML format consumed by a TAK server / ATAK clients.

**Key capabilities:**

- **Full chat type support** — unicast, named group, team color, role, All Chat Rooms, All Groups, All Teams
- **Comms-aware delivery** — integrates with `west_point_comms_sim` and only transmits when `base_station` is reachable
- **Send queue with rate limiting** — per-destination delay prevents ATAK timestamp collisions
- **Reply-ordering guarantee** — outgoing timestamps are advanced past the incoming sender's clock so replies always appear _after_ the message they respond to in ATAK
- **Retry / delivery tracking** — messages are republished at configurable intervals until a subscriber is confirmed
- **Device UID learning** — learns ATAK device UIDs from incoming CoT and pre-seeds them from config; ensures correct `chatgrp` routing before the first inbound message
- **Backward compatibility** — empty `chat_type` is treated as unicast; `destination="ALL"` fans out to all allowed callsigns

---

## Directory Structure

```
tak_chat/
├── CMakeLists.txt
├── package.xml
├── README.md
├── config/
│   └── tak_chat.yaml                       # Example / reference configuration
├── include/
│   └── tak_chat/
│       ├── tak_chat_node.hpp               # Full TakChatNode implementation (header-only)
│       └── tak_chat_interface.h            # Shared client interface for BT nodes
├── launch/
│   └── tak_chat.launch.py                  # Single and multi-robot launch file
├── msg/
│   └── TakChat.msg                         # ROS2 message definition
├── scripts/
│   ├── cot_monitor.py                      # Filter/display matching CoT messages
│   ├── tak_chat_console.py                 # Interactive long-lived test console
│   ├── tak_chat_test.py                    # One-shot test publisher
│   └── tak_chat_test_until_send_to_tak.py  # Pipeline verification tool
└── src/
    └── tak_chat_node.cpp                   # Node entry point (thin main())
```

---

## Installation

```bash
# Inside the phoenix-r2 ROS2 Docker container
cd /phoenix
phxbuild tak_chat
source install/setup.bash
```

---

## Message Definition (`TakChat.msg`)

```
# --- Core fields (all chat types) ---
string origin           # Callsign of sender (e.g. "warthog1")
string destination      # Unicast only: recipient callsign (e.g. "TRILL")
                        # Legacy: "ALL" fans out to all allowed callsigns
string message          # Message text
string timestamp        # ISO 8601 Zulu (e.g. "2026-01-06T15:16:45.009655Z")
string uid              # Optional UID override (empty = auto-generate)
                        # Format: "event_uuid|msg_uuid" for independent control

# --- Chat type routing ---
string chat_type        # See chat types below. Empty string = "unicast" (backward compat)

# --- Group / team / role fields ---
string chatroom         # Group name, color, or role (e.g. "TEAM13", "Cyan", "HQ")
string chatroom_id      # __chat id attribute; usually = chatroom; named groups use UUID

# --- Named group membership (group chat only) ---
string[] member_uids    # UIDs of all group members (uid0, uid1, … in chatgrp)
string[] member_names   # Display names matching member_uids (for hierarchy block)
```

### Chat Types (`chat_type` field)

| Value | Description | Required fields |
|---|---|---|
| `"unicast"` | Direct message to one callsign via `<marti>` routing | `origin`, `destination`, `message` |
| `"group"` | Named group with explicit member list and hierarchy | `origin`, `message`, `chatroom`, `chatroom_id`, `member_uids` |
| `"team_color"` | TAK team color broadcast (e.g. `"Cyan"`, `"Red"`) | `origin`, `message`, `chatroom` |
| `"role"` | TAK role broadcast (e.g. `"HQ"`, `"Medic"`) | `origin`, `message`, `chatroom` |
| `"all_chat_rooms"` | Broadcast to All Chat Rooms | `origin`, `message` |
| `"all_groups"` | Broadcast to all Groups (UserGroups) | `origin`, `message` |
| `"all_teams"` | Broadcast to all Teams (TeamGroups) | `origin`, `message` |
| `""` _(empty)_ | Treated as `"unicast"` — backward compatibility | same as unicast |

> **Note on `uid` override:** The `uid` field accepts the format `"event_uuid|msg_uuid"` to independently control the CoT event UID and the `__chat messageId`. Both parts are optional; an empty part auto-generates a random UUID. This is for diagnostic testing only — leave `uid` empty in production.

---

## Topics

All topics are relative to the robot's namespace (e.g. `/warthog1/`).

| Topic | Type | Direction | Description |
|---|---|---|---|
| `tak_chat/out` | `tak_chat/TakChat` | Subscribe | Chat requests from BT nodes or other publishers |
| `tak_chat/in` | `tak_chat/TakChat` | Publish | Incoming messages forwarded to BT nodes |
| `send_to_tak` | `std_msgs/String` | Publish | Formatted CoT XML sent to the TAK bridge |
| `incoming_cot` | `std_msgs/String` | Subscribe | Raw CoT XML received from the TAK bridge |
| `navsat` | `sensor_msgs/NavSatFix` | Subscribe | GPS position embedded in outgoing CoT |
| `comms` | `west_point_comms_sim/CommsStatus` | Subscribe | Mesh network connectivity status |

---

## Parameters

| Parameter | Type | Default | Description |
|---|---|---|---|
| `callsign` | string | `warthog1` | This robot's TAK callsign |
| `tak_server_flow_tag_key` | string | `TAK-Server-d520...` | TAK server session flow tag |
| `outgoing_cot_topic` | string | `send_to_tak` | Topic for outgoing CoT XML |
| `incoming_cot_topic` | string | `incoming_cot` | Topic for incoming CoT XML |
| `navsat_topic` | string | `navsat` | GPS input topic (remapped in launch file) |
| `tak_chat_out_topic` | string | `tak_chat/out` | TakChat request input topic |
| `tak_chat_in_topic` | string | `tak_chat/in` | TakChat event output topic |
| `comms_topic` | string | `comms` | Mesh connectivity topic |
| `allowed_callsigns_file` | string | `.../cot_runner.yaml` | YAML file listing allowed callsigns |
| `send_delay_s` | double | `1.0` | Min delay between consecutive sends to the same destination |
| `reply_delay_s` | double | `1.0` | Buffer added past incoming timestamp before replying |
| `retry_timeout_s` | double | `10.0` | Total retry window before giving up |
| `retry_interval_s` | double | `1.0` | Interval between retry publishes |
| `min_retry_count` | int | `1` | Min publish count before marking delivered |
| `known_device_uids` | string[] | `[]` | Pre-seeded callsign→device UID map; format: `["CALLSIGN:ANDROID-hex"]` |

---

## Running the Node

```bash
# Recommended: use the launch file
ros2 launch tak_chat tak_chat.launch.py

# Specify a different robot
ros2 launch tak_chat tak_chat.launch.py robot_name:=warthog2

# Multi-robot
ros2 launch tak_chat tak_chat.launch.py \
    robot_names:="['warthog1', 'warthog2', 'warthog3']"

# Multi-robot with u-blox GPS (NAI_3 / NAI_4 condition)
ros2 launch tak_chat tak_chat.launch.py \
    robot_names:="['warthog1', 'warthog2', 'warthog3']" \
    navsat_topic:=sensors/ublox/fix

# Direct run (not recommended for production)
ros2 run tak_chat tak_chat_node --ros-args \
    -p callsign:=warthog1 \
    -r navsat:=/warthog1/sensors/ublox/fix
```

### `navsat_topic` Launch Argument

The `navsat_topic` argument is **robot-relative** (no leading slash, no namespace prefix). The ROS2 namespace mechanism prepends the robot name automatically.

| GPS hardware | Condition | Value |
|---|---|---|
| GeoFog | NAI_2, bench testing | `sensors/geofog/gps/fix` (default) |
| u-blox | NAI_3, NAI_4 | `sensors/ublox/fix` |

> **Why this matters:** `tak_chat` uses GPS to embed coordinates into outgoing CoT. If the wrong topic is mapped, TAK receives stale or zero-coordinate position data.

---

## Integration — C++ / BT Nodes

### `TakChatInterface` (preferred client)

`TakChatInterface` is a long-lived shared publisher/subscriber created once at startup and injected into all BT nodes that need TAK chat. Its constructor blocks until `TakChatNode` is discovered via DDS, so all subsequent sends are reliable.

```cpp
#include <tak_chat/tak_chat_interface.h>

// Create once in main() — inject into BT nodes via registerBuilder
auto tak_chat = std::make_shared<TakChatInterface>(
    node,           // rclcpp::Node::SharedPtr
    "warthog1",     // robot callsign
    "tak_chat/out", // outgoing topic (relative to node's namespace)
    "tak_chat/in"   // incoming topic (relative to node's namespace)
);
// Constructor blocks ~2–3 s for DDS discovery, then returns.

// --- NEW (preferred): publish a fully-assembled TakChat message ---
tak_chat::msg::TakChat msg;
msg.origin    = "warthog1";
msg.chat_type = "team_color";
msg.chatroom  = "Cyan";
msg.message   = "Moving to waypoint";
tak_chat->publish(msg);

// --- LEGACY: unicast convenience wrapper ---
tak_chat->send("TRILL", "Confirmed!");

// --- LEGACY: broadcast to all allowed callsigns ---
tak_chat->broadcast("Standing by");

// --- Inbox ---
if (tak_chat->hasMessage("TRILL")) {
    auto msg = tak_chat->getLatestMessage("TRILL");
    // msg->origin, msg->message, msg->chat_type, etc.
}
```

### Registering BT Nodes

```cpp
// In node_registration.h — inject the same tak_chat instance into every node
factory.registerBuilder<ConstructTAKChatMessage>(
    "ConstructTAKChatMessage",
    [tak_chat, robot_name](const std::string& name, const BT::NodeConfig& config) {
        return std::make_unique<ConstructTAKChatMessage>(name, config, tak_chat, robot_name);
    });

factory.registerBuilder<PublishTAKChatMessage>(
    "PublishTAKChatMessage",
    [tak_chat](const std::string& name, const BT::NodeConfig& config) {
        return std::make_unique<PublishTAKChatMessage>(name, config, tak_chat);
    });
```

### Adding as a Dependency

`package.xml`:
```xml
<depend>tak_chat</depend>
```

`CMakeLists.txt`:
```cmake
find_package(tak_chat REQUIRED)
ament_target_dependencies(your_target tak_chat)
```

---

## Features In Depth

### Send Queue and Rate Limiting

Messages are placed in a send queue and dispatched by a timer. Rate limiting is applied per **destination** for unicast (keyed by callsign) and per **chat type** for all other types. This ensures:

- Each message to a given destination has a distinct timestamp, preventing ATAK ordering issues.
- A broadcast to 5 callsigns sends all 5 messages at once — there is no artificial stacking delay across different destinations.

### Reply Timestamp Ordering

To guarantee that a reply appears _after_ the message it responds to in ATAK's chat view (even when device clocks are skewed):

1. When an incoming message is received, its timestamp is parsed and stored.
2. Before dispatching a reply to that callsign, the node waits until wall time has advanced past `their_timestamp + reply_delay_s`.
3. The outgoing timestamp is set to `max(their_timestamp + 2.0s, now + 0.5s)`.
4. If their clock is more than `MAX_REPLY_WAIT_S` (5 s) ahead of ours, the wait is capped to avoid indefinite blocking.
5. Consecutive replies to the same callsign keep advancing the stored timestamp so they also remain in order.

### Device UID Learning

ATAK routes unicast `chatgrp` elements by **device UID** (e.g. `ANDROID-49c8964ab97f24bc`), not callsign. `TakChatNode` learns the mapping from incoming CoT (`chatgrp uid0`) and pre-seeds it from the `known_device_uids` parameter. If a device UID is not yet known when sending a unicast, the node falls back to the callsign and logs a warning.

### Comms-Aware Delivery

The node subscribes to `comms` (`west_point_comms_sim/CommsStatus`). A message is dispatched only when `base_station` appears in the transitive or direct reachability list. Messages are suppressed (with a log warning) when connectivity is lost. If `west_point_comms_sim` is not running, the node defaults to allowing all sends.

### Incoming Message Filtering

Incoming `b-t-f` CoT is forwarded to `tak_chat/in` only if the `senderCallsign` attribute appears in the `allowed_callsigns_file`. Messages from unknown senders are silently dropped (visible at DEBUG log level).

### QoS

All topics use **RELIABLE + VOLATILE** QoS. This must match `TakChatInterface`, the TAK bridge subscriber, and all test tools. TRANSIENT_LOCAL is intentionally avoided because it causes issues with Fast-DDS.

---

## Scripts

### `tak_chat_console.py` — Interactive Console

A long-lived interactive console for manual testing. Because the node stays alive, DDS discovery is a one-time startup cost, and all subsequent messages are delivered reliably.

```bash
ros2 run tak_chat tak_chat_console.py
ros2 run tak_chat tak_chat_console.py --namespace warthog2 --from warthog2
```

**Console commands:**

| Command | Description |
|---|---|
| `DESTINATION: message` | Send a unicast to the given callsign |
| `ALL: message` | Fan-out to all allowed callsigns |
| `/status` | Show subscriber count, mode, and stored UIDs |
| `/reuse-event` | Reuse the CoT event UID across sends (messageId random) |
| `/reuse-msgid` | Reuse the `__chat messageId` across sends (event UID random) |
| `/reuse-both` | Reuse both identifiers |
| `/random` | Reset to fully random UIDs (default) |
| `/quit` | Exit |

The UID reuse modes are for systematically testing ATAK's chat deduplication behavior. See _ATAK Chat Behavior_ below.

---

### `tak_chat_test.py` — One-Shot Publisher

Sends a single message and exits. Useful for scripted testing.

```bash
# Broadcast to all allowed callsigns
ros2 run tak_chat tak_chat_test.py "Hello everyone"

# Unicast
ros2 run tak_chat tak_chat_test.py "Hello TRILL" --to TRILL

# Different namespace
ros2 run tak_chat tak_chat_test.py "Hello" --namespace warthog2 --from warthog2

# Append unique suffix to avoid ATAK deduplication
ros2 run tak_chat tak_chat_test.py "Test" --unique
```

Key arguments: `--discovery-timeout`, `--min-discovery-time`, `--publish-count`, `--post-discovery-delay`.

---

### `tak_chat_test_until_send_to_tak.py` — Pipeline Verification

Publishes to `tak_chat/out` repeatedly until the converted CoT XML appears on `send_to_tak`. Useful for verifying the full BT→TakChatNode→TAK bridge pipeline.

```bash
# Verify fan-out to all allowed callsigns
ros2 run tak_chat tak_chat_test_until_send_to_tak.py "Hello world"

# Verify unicast pipeline
ros2 run tak_chat tak_chat_test_until_send_to_tak.py "Hello TRILL" --to TRILL

# Require ALL destinations confirmed before exiting
ros2 run tak_chat tak_chat_test_until_send_to_tak.py "Broadcast" --require-all
```

---

### `cot_monitor.py` — CoT Filter / Display

Subscribes to a high-traffic `std_msgs/String` topic (default: `/warthog1/send_to_tak`) and prints only messages matching the specified filters.

```bash
# Default: print all GeoChat CoT on warthog1/send_to_tak
ros2 run tak_chat cot_monitor.py

# Filter by chatroom
ros2 run tak_chat cot_monitor.py --chatroom TRILL

# Filter by sender
ros2 run tak_chat cot_monitor.py --sender warthog1

# Print raw XML instead of compact summary
ros2 run tak_chat cot_monitor.py --raw

# Save matching messages to a file
ros2 run tak_chat cot_monitor.py --out /tmp/cot_hits.log

# Different topic
ros2 run tak_chat cot_monitor.py --topic /warthog2/send_to_tak
```

---

## ATAK Chat Behavior (Discovered Through Testing)

### Deduplication

Through systematic testing with controlled UIDs and messageIds:

- **Primary deduplication key:** ATAK uses the `messageId` attribute inside `<__chat>` to deduplicate incoming messages.
- **Event UID:** Does _not_ affect chat deduplication. Two messages with the same event UID but different messageIds both appear.
- **Behavior:** A message with a duplicate messageId is **silently dropped** — it does not update or replace the existing message.
- **Immutability:** Chat messages in ATAK cannot be edited or updated after delivery.

### Implications for BT Message Design

1. Always use a random `messageId` for each outgoing message (default behavior — leave `uid` field empty).
2. Status updates must be sent as new messages with context in the text, not as modifications to a prior message:
   - ✅ `"Checkpoint 1 reached"` → `"Checkpoint 2 reached"`
   - ✅ `"Mission Update #1: ..."` → `"Mission Update #2: ..."`
   - ❌ Cannot update `"Status: pending"` to `"Status: complete"`

---

## Architecture

```
┌────────────────────────────────────────────────────────────────────┐
│                          OUTGOING FLOW                             │
├────────────────────────────────────────────────────────────────────┤
│                                                                    │
│  ┌──────────────┐  TakChat msg   ┌────────────────────────────┐   │
│  │ BehaviorTree │ ─────────────> │       TakChatNode          │   │
│  │    Nodes     │  tak_chat/out  │                            │   │
│  └──────────────┘                │  1. Validate origin        │   │
│                                  │  2. Check base_station     │   │
│  ┌──────────────┐  TakChat msg   │     comms (gate)           │   │
│  │  Other ROS2  │ ─────────────> │  3. Fan-out ALL (legacy)   │   │
│  │    Nodes     │                │  4. Enqueue message        │   │
│  └──────────────┘                │  5. Rate-limit per dest    │   │
│                                  │  6. Wait reply-order delay │   │
│                                  │  7. Build CoT XML          │   │
│                                  │  8. Publish + retry loop   │   │
│                                  └─────────────┬──────────────┘   │
│                                                │ CoT XML          │
│                                                ▼                  │
│                                  ┌────────────────────────────┐   │
│                                  │   TAK Bridge / TAK Server  │   │
│                                  └────────────────────────────┘   │
│                                                                    │
├────────────────────────────────────────────────────────────────────┤
│                          INCOMING FLOW                             │
├────────────────────────────────────────────────────────────────────┤
│                                                                    │
│  ┌────────────────────────────┐                                    │
│  │   TAK Bridge / TAK Server  │                                    │
│  └─────────────┬──────────────┘                                    │
│                │ CoT XML                                           │
│                ▼                                                   │
│  ┌────────────────────────────┐  TakChat msg   ┌──────────────┐   │
│  │       TakChatNode          │ ─────────────> │ BehaviorTree │   │
│  │                            │  tak_chat/in   │    Nodes     │   │
│  │  1. Filter b-t-f type      │                └──────────────┘   │
│  │  2. Check comms gate       │                                    │
│  │  3. Filter allowed senders │                                    │
│  │  4. Learn device UID       │                                    │
│  │  5. Record for reply order │                                    │
│  │  6. Determine chat_type    │                                    │
│  │  7. Publish TakChat msg    │                                    │
│  └────────────────────────────┘                                    │
│                                                                    │
└────────────────────────────────────────────────────────────────────┘
```

---

## Troubleshooting

### Messages Not Delivered to TAK

1. **QoS mismatch** — all components must use RELIABLE + VOLATILE.
   ```
   [tak_chat/out] Incompatible QoS! Policy: ...
   ```
   Rebuild all packages and confirm QoS settings match.

2. **TakChatNode not running** — `TakChatInterface` logs at startup:
   ```
   [TakChatInterface] Timeout — NO SUBSCRIBER. Is TakChatNode running?
   ```
   Ensure `tak_chat_node` is started before BT trees begin ticking.

3. **No comms to base_station** — check `west_point_comms_sim`:
   ```
   [TakChat OUT] SUPPRESSED — no comms to base_station
   ```
   Verify that the comms sim is running and that the robot is within range.

4. **Origin mismatch** — origin must equal the node's callsign:
   ```
   [TakChat OUT] REJECTED — origin 'X' != callsign 'Y'
   ```
   Ensure `msg.origin` is set to the correct robot name.

### Incoming Messages Not Forwarded

1. **Sender not in allowed list:**
   ```
   [Incoming] IGNORED — sender 'CALLSIGN' not in allowed list
   ```
   Add the callsign to `cot_runner.yaml` under `cot_msg_defaults.allowed`.

2. **No GPS fix** — the node subscribes to `navsat` (remapped in launch). If GPS is silent, the node still functions but outgoing CoT will have `lat=0, lon=0`.

### Reply Appears Before Incoming Message in ATAK

Increase `reply_delay_s` to give a wider buffer:
```yaml
reply_delay_s: 2.0  # Default is 1.0
```
Check logs for clock-skew warnings:
```
[Comms] base_station RESTORED
```

### Unicast Missing Device UID

If you see:
```
[Unicast] No device UID known for 'TRILL' — falling back to callsign
```
Pre-seed the UID in the launch file via `known_device_uids`, or have the operator send a message first so the node can learn their UID from the incoming `chatgrp uid0`.

### FastDDS Stale Lock Files

If the node fails to start after a hard kill, stale lock files from a previous run may be blocking DDS:
```bash
rm -f /dev/shm/fastrtps_*
```
The simulation launch and shutdown scripts include this cleanup automatically.
