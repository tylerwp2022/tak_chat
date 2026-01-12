# tak_chat

ROS2 package for TAK/ATAK chat communication via Cursor-on-Target (CoT) protocol.

## Overview

This package provides a dedicated ROS2 node (`TakChatNode`) that bridges ROS2 messaging with TAK chat. It handles:

- **Outgoing messages**: Converts `TakChat` ROS2 messages to CoT XML for the TAK server
- **Incoming messages**: Parses CoT XML from TAK and publishes `TakChat` messages
- **Broadcast support**: Destination "ALL" fans out to all allowed callsigns
- **Comms-aware delivery**: Integrates with mesh network simulation to only send when base_station is reachable
- **Reliable delivery**: Retry logic handles DDS discovery timing issues
- **Message ordering**: Timestamp management ensures replies appear after incoming messages in ATAK
- **Send queue**: Ensures distinct timestamps for proper chat ordering

## Directory Structure

```
tak_chat/
├── CMakeLists.txt
├── package.xml
├── README.md
├── config/
│   └── tak_chat.yaml              # Example configuration
├── include/
│   └── tak_chat/
│       ├── tak_chat_node.hpp      # Main node implementation
│       └── tak_chat_interface.h   # Client interface for BT nodes
├── launch/
│   └── tak_chat.launch.py         # Launch file
├── msg/
│   └── TakChat.msg                # ROS2 message definition
├── scripts/
│   └── tak_chat_console.py        # Interactive testing console
└── src/
    └── tak_chat_node.cpp          # Node entry point
```

## Installation

```bash
## In phoenix-r2 ros2 docker container
cd /phoenix

phxbuild tak_chat
source install/setup.bash
```

## Message Definition

### TakChat.msg

```
string origin       # Sender callsign (e.g., "warthog1")
string destination  # Target callsign or "ALL" for broadcast
string message      # Message text
string timestamp    # ISO8601 timestamp (e.g., "2026-01-08T15:00:34.114Z")
string uid          # Optional: for testing only (leave empty for normal use)
```

**Note:** The `uid` field is for experimental testing of ATAK's chat deduplication behavior. For normal operation, leave this field empty and the node will generate appropriate identifiers automatically.

## Usage

### Running the Node

```bash
# Using launch file (recommended)
ros2 launch tak_chat tak_chat.launch.py

# With namespace
ros2 launch tak_chat tak_chat.launch.py namespace:=warthog1

# Direct run with parameters
ros2 run tak_chat tak_chat_node --ros-args \
  -p callsign:=warthog1 \
  -p send_delay_s:=1.0
```

### Topics

| Topic | Type | Direction | Description |
|-------|------|-----------|-------------|
| `tak_chat/out` | `tak_chat/TakChat` | Subscribe | Chat requests from BT nodes |
| `tak_chat/in` | `tak_chat/TakChat` | Publish | Incoming chat messages to BT nodes |
| `send_to_tak` | `std_msgs/String` | Publish | Formatted CoT XML to TAK bridge |
| `incoming_cot` | `std_msgs/String` | Subscribe | CoT XML from TAK bridge |
| `navsat` | `sensor_msgs/NavSatFix` | Subscribe | GPS position for CoT messages |
| `comms` | `west_point_comms_sim/CommsStatus` | Subscribe | Mesh network connectivity status |

### Parameters

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `callsign` | string | `warthog1` | This robot's TAK callsign |
| `android_id` | string | `dfac01d76beec661` | Device ID for GeoChat UID format |
| `tak_server_flow_tag_key` | string | `TAK-Server-...` | TAK server session key |
| `outgoing_cot_topic` | string | `send_to_tak` | CoT XML output topic |
| `incoming_cot_topic` | string | `incoming_cot` | CoT XML input topic |
| `navsat_topic` | string | `navsat` | GPS input topic |
| `tak_chat_out_topic` | string | `tak_chat/out` | TakChat request input topic |
| `tak_chat_in_topic` | string | `tak_chat/in` | TakChat event output topic |
| `comms_topic` | string | `comms` | Mesh network status topic |
| `allowed_callsigns_file` | string | `.../cot_runner.yaml` | YAML file with allowed callsigns |
| `send_delay_s` | double | `1.0` | Minimum delay between consecutive sends (seconds) |
| `reply_delay_s` | double | `0.5` | Buffer added to incoming timestamp for replies (seconds) |
| `retry_timeout_s` | double | `10.0` | Total retry window (seconds) |
| `retry_interval_s` | double | `1.0` | Time between retries (seconds) |
| `min_retry_count` | int | `2` | Minimum publishes before marking delivered |

## Features

### Broadcast Messages

Send to destination `"ALL"` to automatically fan out to all allowed callsigns:

```cpp
tak_chat->broadcast("Standing by, awaiting confirmation");
// Equivalent to:
tak_chat->send("ALL", "Standing by, awaiting confirmation");
```

### Incoming Message Filtering

Only messages from callsigns listed in `allowed_callsigns_file` are forwarded to `tak_chat/in`. Messages from unknown senders are silently dropped.

### Comms-Aware Delivery

Integrates with `west_point_comms_sim` to check mesh network connectivity:

- All TAK/ATAK messages go through the `base_station` (TAK server)
- If `base_station` is not in the transitive reachability list, messages are suppressed
- If `comms_sim` is not running, defaults to allowing all messages (assumes comms are up)
- Logs connectivity state changes (base_station REACHABLE/UNREACHABLE)

This ensures robots don't attempt to send TAK messages when they have no mesh connectivity to the base station.

### Send Queue

Messages are queued and sent one at a time with a configurable delay (`send_delay_s`) between consecutive sends to the **same** destination. This ensures:

1. Each message to a given destination has a distinct timestamp
2. Messages are sent in strict FIFO order
3. TAK server and ATAK can properly order messages in chat view
4. Messages to **different** destinations can be sent simultaneously (no artificial delay)

**Example:** Broadcasting to 5 callsigns sends all 5 messages immediately, not over 5 seconds.

### Message Ordering & Reply Timestamps

To ensure replies appear AFTER incoming messages in ATAK's chat view:

1. **Track incoming timestamps**: When receiving a message, store their timestamp
2. **Wait for clock sync**: Don't send replies until our clock passes their timestamp + buffer
3. **Reply with adjusted timestamp**: Use `max(their_timestamp + 2.0s, current_time + 0.5s)`
4. **Handle clock skew**: Works even if ATAK device's clock is ahead or behind ours
5. **Safety cap**: Never wait more than 5 seconds (handles wildly incorrect clocks)

This approach is simpler and more reliable than calculating clock offsets because:
- Guarantees ordering regardless of clock drift
- Handles network latency naturally
- Works even if their clock jumps around

### Reliable Delivery

Handles DDS discovery timing issues with retry logic:

1. Publish immediately when message is ready
2. Keep retrying at `retry_interval_s` intervals
3. Mark delivered when subscriber is confirmed AND minimum retries reached
4. Timeout after `retry_timeout_s` if no subscriber appears

### QoS Configuration

All topics use **RELIABLE + VOLATILE** QoS:

- **RELIABLE**: Ensures delivery confirmation
- **VOLATILE**: Works best with Fast-DDS (TRANSIENT_LOCAL has issues)

All publishers and subscribers **must match** this QoS for compatibility.

## Integration

### Using from C++ (Behavior Tree Nodes)

```cpp
#include <tak_chat/tak_chat_interface.h>

// In main() - create ONCE at startup
auto tak_chat = std::make_shared<TakChatInterface>(
    node,           // ROS2 node
    "warthog1",     // This robot's callsign
    "/warthog1/tak_chat/out",
    "/warthog1/tak_chat/in"
);

// Constructor waits for TakChatNode discovery (~2-3 seconds)
// After that, all sends are reliable

// Send to specific callsign
tak_chat->send("TRILL", "Hello operator");

// Broadcast to all allowed callsigns
tak_chat->broadcast("Mission started");

// Check for incoming messages
if (auto msg = tak_chat->getLatestMessage()) {
    std::cout << "From: " << msg->origin << std::endl;
    std::cout << "Message: " << msg->message << std::endl;
}

// Check for message from specific sender
if (tak_chat->hasMessage("TRILL")) {
    auto msg = tak_chat->getLatestMessage("TRILL");
}
```

### Registering BT Nodes

```cpp
// Share the same tak_chat instance across all BT nodes
factory.registerNodeType<Standby>("Standby", tak_chat);
factory.registerNodeType<ContinueMission>("ContinueMission", tak_chat);
```

### Using from Another Package

Add to your `package.xml`:
```xml
<depend>tak_chat</depend>
```

Add to your `CMakeLists.txt`:
```cmake
find_package(tak_chat REQUIRED)
ament_target_dependencies(your_target tak_chat)
```

## Testing

### Interactive Console

A long-lived interactive console for testing (recommended over short-lived scripts):

```bash
ros2 run tak_chat tak_chat_console.py

# Basic commands:
> TRILL: Hello there          # Send to TRILL
> ALL: Broadcast message      # Send to all
> /status                     # Show connection status
> /quit                       # Exit

# Experimental UID testing commands (for ATAK research):
> /reuse-event                # Test event UID deduplication
> /reuse-msgid                # Test messageId deduplication
> /reuse-both                 # Test both together
> /random                     # Return to normal (random IDs)
```

**Note on UID Testing:** Through experimentation, we've learned that:
- ATAK uses the `messageId` (in the `<__chat>` element) as the primary deduplication key
- Messages with duplicate `messageId` are silently dropped (not updated)
- ATAK chat messages are **immutable** - updates are not supported
- The event UID doesn't affect chat deduplication
- Normal operation should always use random IDs (default behavior)

### Command Line

```bash
# Send a message
ros2 topic pub /warthog1/tak_chat/out tak_chat/msg/TakChat \
  "{origin: 'warthog1', destination: 'TRILL', message: 'Hello', uid: ''}" --once

# Monitor incoming messages
ros2 topic echo /warthog1/tak_chat/in

# Monitor outgoing CoT XML
ros2 topic echo /warthog1/send_to_tak
```

## ATAK Chat Behavior (Discovered Through Testing)

Based on systematic testing with controlled UIDs and messageIds:

### Chat Message Deduplication

- **Primary Key**: ATAK uses the `messageId` attribute in `<__chat>` for deduplication
- **Event UID**: Does NOT affect chat deduplication (only messageId matters)
- **Behavior**: Messages with duplicate messageId are **dropped**, not updated
- **Immutability**: Chat messages cannot be edited/updated in ATAK

### Implications for Design

1. **Always use random messageId** for each message (default behavior ✓)
2. **Messages appear as new entries** in chat history (can't be updated)
3. **For status updates**, send new messages with context:
   - ✅ "Checkpoint 1 reached" → "Checkpoint 2 reached"
   - ✅ "Mission Update #1: ..." → "Mission Update #2: ..."
   - ❌ Can't update existing message "Status: ..." to show new value

4. **For updating displays**, consider alternatives to chat:
   - Use Self SA (Situational Awareness) remarks field
   - Use custom CoT event types
   - Send new chat messages with clear context

## Architecture

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                              OUTGOING FLOW                                  │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                             │
│  ┌──────────────┐     TakChat msg      ┌──────────────────────────────────┐ │
│  │ BehaviorTree │ ───────────────────> │         TakChatNode              │ │
│  │    Nodes     │   tak_chat/out       │                                  │ │
│  └──────────────┘                      │  1. Check base_station comms     │ │
│                                        │  2. Queue message                │ │
│  ┌──────────────┐     TakChat msg      │  3. Wait for send_delay_s        │ │
│  │    Other     │ ───────────────────> │  4. Generate timestamp           │ │
│  │  ROS2 Nodes  │                      │  5. Adjust for reply ordering    │ │
│  └──────────────┘                      │  6. Build CoT XML                │ │
│                                        │  7. Publish with retry           │ │
│                                        └───────────────┬──────────────────┘ │
│                                                        │ CoT XML            │
│                                                        ▼                    │
│                                        ┌──────────────────────────────────┐ │
│                                        │       TAK Bridge / Server        │ │
│                                        └──────────────────────────────────┘ │
│                                                                             │
├─────────────────────────────────────────────────────────────────────────────┤
│                              INCOMING FLOW                                  │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                             │
│  ┌──────────────────────────────────┐                                       │
│  │       TAK Bridge / Server        │                                       │
│  └───────────────┬──────────────────┘                                       │
│                  │ CoT XML                                                  │
│                  ▼                                                          │
│  ┌──────────────────────────────────┐     TakChat msg      ┌─────────────┐ │
│  │         TakChatNode              │ ───────────────────> │ BehaviorTree│ │
│  │                                  │   tak_chat/in        │    Nodes    │ │
│  │  1. Parse GeoChat CoT            │                      └─────────────┘ │
│  │  2. Filter by allowed callsigns  │                                      │
│  │  3. Record timestamp             │                                      │
│  │  4. Publish TakChat message      │                                      │
│  └──────────────────────────────────┘                                       │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

## Troubleshooting

### Messages Not Delivered

1. **Check QoS compatibility**: All components must use RELIABLE + VOLATILE
   ```
   [WARN] Incompatible QoS detected! Policy: DURABILITY
   ```
   Solution: Rebuild all packages with matching QoS

2. **Check subscriber discovery**: TakChatInterface waits for discovery at startup
   ```
   [ERROR] Discovery timeout - NO SUBSCRIBER FOUND!
   ```
   Solution: Ensure tak_chat_node is running before BT nodes

3. **Check allowed callsigns**: Incoming messages from unknown senders are dropped
   ```
   [DEBUG] IGNORED - sender 'UNKNOWN' not in allowed list
   ```
   Solution: Add callsign to `cot_runner.yaml`

4. **Check comms connectivity**: Messages suppressed when base_station unreachable
   ```
   [WARN] No comms to base_station - message not sent
   ```
   Solution: Check mesh network connectivity in comms_sim

### Messages Out of Order

1. **Check reply delays**: Logs show timing decisions
   ```
   [INFO] [Reply Delay] Will wait 2.5s before sending to TRILL (their clock is ahead)
   ```
   This is handled automatically - messages wait to ensure correct ordering

2. **Increase send_delay_s**: If messages to same destination appear out of order
   ```yaml
   send_delay_s: 2.0  # Increase from default 1.0
   ```

3. **Check for clock skew warnings**: Large skew might indicate device clock issues
   ```
   [WARN] Their clock is 300.0s ahead! Capping wait to 5.0s
   ```

### High Latency

1. **Reduce send_delay_s**: For faster message throughput to same destination
   ```yaml
   send_delay_s: 0.5  # 500ms between messages
   ```

2. **Reduce reply_delay_s**: For faster replies (but may affect ordering)
   ```yaml
   reply_delay_s: 0.25  # Smaller buffer (default 0.5s)
   ```

3. **Check retry settings**: Reduce if TAK bridge is reliable
   ```yaml
   min_retry_count: 1
   retry_interval_s: 0.5
   ```

### Messages Appearing Out of Order in ATAK

If your replies appear BEFORE the incoming message in ATAK:

1. **Increase reply_delay_s**: Give more buffer for clock skew
   ```yaml
   reply_delay_s: 1.0  # More conservative (default 0.5s)
   ```

2. **Check the logs**: Look for clock skew warnings
   ```
   [INFO] [Reply Delay] Their clock is 10.2s ahead
   ```

3. **Wait longer before replying**: The node waits until our clock passes their timestamp

## License

MIT
