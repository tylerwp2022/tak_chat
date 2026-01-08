# tak_chat

ROS2 package for TAK/ATAK chat communication via Cursor-on-Target (CoT) protocol.

## Overview

This package provides a dedicated ROS2 node (`TakChatNode`) that bridges ROS2 messaging with TAK chat. It handles:

- **Outgoing messages**: Converts `TakChat` ROS2 messages to CoT XML for the TAK server
- **Incoming messages**: Parses CoT XML from TAK and publishes `TakChat` messages
- **Broadcast support**: Destination "ALL" fans out to all allowed callsigns
- **Reliable delivery**: Retry logic handles DDS discovery timing issues
- **Clock synchronization**: Per-callsign clock offset tracking for devices with drifting clocks
- **Message ordering**: Send queue ensures distinct timestamps for proper chat ordering

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
```

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
| `allowed_callsigns_file` | string | `.../cot_runner.yaml` | YAML file with allowed callsigns |
| `send_delay_s` | double | `1.0` | Minimum delay between sends (seconds) |
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

### Send Queue

Messages are queued and sent one at a time with a configurable delay (`send_delay_s`) between them. This ensures:

1. Each message has a distinct timestamp
2. Messages are sent in strict FIFO order
3. TAK server and ATAK can properly order messages in chat view

### Clock Offset Tracking

ATAK devices may be disconnected from the internet with drifting clocks. To ensure messages appear in correct order on their devices:

1. When receiving a message, we calculate the offset between our clock and theirs
2. When sending TO that callsign, we adjust our timestamp to be relative to their clock
3. Each callsign has its own tracked offset
4. Uses exponential moving average to smooth out network jitter
5. Offsets older than 1 hour are considered stale

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

# Commands:
> TRILL: Hello there          # Send to TRILL
> ALL: Broadcast message      # Send to all
> /status                     # Show connection status
> /quit                       # Exit
```

### Command Line

```bash
# Send a message
ros2 topic pub /warthog1/tak_chat/out tak_chat/msg/TakChat \
  "{origin: 'warthog1', destination: 'TRILL', message: 'Hello'}" --once

# Monitor incoming messages
ros2 topic echo /warthog1/tak_chat/in

# Monitor outgoing CoT XML
ros2 topic echo /warthog1/send_to_tak
```

## Architecture

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                              OUTGOING FLOW                                  │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                             │
│  ┌──────────────┐     TakChat msg      ┌──────────────────────────────────┐ │
│  │ BehaviorTree │ ───────────────────> │         TakChatNode              │ │
│  │    Nodes     │   tak_chat/out       │                                  │ │
│  └──────────────┘                      │  1. Queue message                │ │
│                                        │  2. Wait for send_delay_s        │ │
│  ┌──────────────┐     TakChat msg      │  3. Generate timestamp           │ │
│  │    Other     │ ───────────────────> │  4. Apply clock offset           │ │
│  │  ROS2 Nodes  │                      │  5. Build CoT XML                │ │
│  └──────────────┘                      │  6. Publish with retry           │ │
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
│  │  3. Update clock offset          │                                      │
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

### Messages Out of Order

1. **Check clock offset**: Large offsets indicate clock drift
   ```
   [INFO] [ClockOffset] NEW TRILL: 300.00s (their clock is behind)
   ```
   This is handled automatically - messages are adjusted

2. **Increase send_delay_s**: If messages still appear out of order
   ```yaml
   send_delay_s: 2.0  # Increase from default 1.0
   ```

### High Latency

1. **Reduce send_delay_s**: For faster message throughput
   ```yaml
   send_delay_s: 0.5  # 500ms between messages
   ```

2. **Check retry settings**: Reduce if TAK bridge is reliable
   ```yaml
   min_retry_count: 1
   retry_interval_s: 0.5
   ```

## License

MIT
