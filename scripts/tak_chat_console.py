#!/usr/bin/env python3
"""
tak_chat_console.py

Interactive console for sending TAK chat messages with granular UID control.

Unlike tak_chat_test.py (which exits after sending), this node stays alive
indefinitely. This solves the DDS discovery race condition because:
  1. Node starts and creates publisher
  2. We wait for tak_chat_node to discover us (one-time cost)
  3. All subsequent messages are delivered instantly and reliably

NEW FEATURE: Granular UID Reuse for Testing
--------------------------------------------
You can independently control event UID and messageId to determine which
identifier ATAK uses as the primary key for deduplication:

  /reuse-both    - Reuse both event UID and messageId
  /reuse-event   - Reuse event UID only (messageId random)
  /reuse-msgid   - Reuse messageId only (event UID random)
  /random        - Random UIDs (default)

This allows systematic testing of ATAK's chat message deduplication logic.

USAGE:
------
  ros2 run tak_chat tak_chat_console.py

  # Test 1: Do matching event UID + messageId cause overwrites?
  > /reuse-both
  > TRILL: test1
  > TRILL: test2    (same event UID, same messageId)
  
  # Test 2: Does event UID alone matter?
  > /random
  > /reuse-event
  > TRILL: test3    (generates event UID, random messageId)
  > TRILL: test4    (same event UID, different messageId)
  
  # Test 3: Does messageId alone matter?
  > /random
  > /reuse-msgid
  > TRILL: test5    (random event UID, generates messageId)
  > TRILL: test6    (different event UID, same messageId)

Or with a different namespace:
  ros2 run tak_chat tak_chat_console.py --namespace warthog2 --from warthog2
"""

import argparse
import sys
import time
import threading
from datetime import datetime, timezone

import rclpy
from rclpy.node import Node
from rclpy.qos import QoSProfile, ReliabilityPolicy, DurabilityPolicy, HistoryPolicy


def get_zulu_time() -> str:
    """Get current time in ISO 8601 Zulu format."""
    now = datetime.now(timezone.utc)
    return now.strftime("%Y-%m-%dT%H:%M:%S.%f")[:-3] + "Z"


class TakChatConsole(Node):
    """
    Long-lived interactive console for TAK chat testing.
    
    This node stays alive, which means DDS discovery only needs to happen
    once at startup. All subsequent messages are delivered reliably.
    
    Supports granular UID reuse for testing ATAK's deduplication logic:
    - /reuse-both: Reuse event UID + messageId (test both together)
    - /reuse-event: Reuse event UID only (test event UID as primary key)
    - /reuse-msgid: Reuse messageId only (test messageId as primary key)
    - /random: Random UIDs (default behavior)
    """

    def __init__(self, topic: str, callsign: str):
        """
        Initialize the console node.
        
        Args:
            topic: Full ROS topic path (e.g., "/warthog1/tak_chat/out")
            callsign: Our callsign for outgoing messages
        """
        super().__init__("tak_chat_console")

        # Import TakChat message type
        from tak_chat.msg import TakChat
        self.TakChat = TakChat

        self.callsign = callsign
        self.topic = topic

        # QoS matching tak_chat_node
        # RELIABLE + VOLATILE is fine for long-lived publishers
        qos = QoSProfile(
            history=HistoryPolicy.KEEP_LAST,
            depth=10,
            reliability=ReliabilityPolicy.RELIABLE,
            durability=DurabilityPolicy.VOLATILE,
        )
        
        # Create publisher
        self.pub = self.create_publisher(TakChat, topic, qos)
        
        # Track message count
        self.message_count = 0
        
        # UID control - allows testing message overwrites in ATAK
        # We track event UID and messageId separately to test which one ATAK uses
        # as the primary key for deduplication
        self.reuse_event_uid = False   # Reuse the event UID?
        self.reuse_message_id = False  # Reuse the __chat messageId?
        self.last_event_uid = None     # Stored event UID
        self.last_message_id = None    # Stored messageId

        self.get_logger().info(f"TakChatConsole initialized")
        self.get_logger().info(f"  Topic:    {topic}")
        self.get_logger().info(f"  Callsign: {callsign}")

    def wait_for_subscriber(self, timeout: float = 10.0) -> bool:
        """
        Wait for tak_chat_node to discover this publisher.
        
        This is a one-time cost at startup. Once discovered, all
        subsequent messages are delivered instantly.
        """
        print("[DISCOVERY] Waiting for TakChatNode...")
        start = time.time()
        
        while time.time() - start < timeout:
            count = self.pub.get_subscription_count()
            if count > 0:
                elapsed = time.time() - start
                print(f"[DISCOVERY] TakChatNode found! ({count} subscriber(s), {elapsed:.2f}s)")
                
                # Extra stabilization for bidirectional discovery
                print("[DISCOVERY] Waiting 1s for bidirectional discovery...")
                time.sleep(1.0)
                
                return True
            
            # Spin to process DDS discovery
            rclpy.spin_once(self, timeout_sec=0.1)
        
        print(f"[DISCOVERY] TIMEOUT after {timeout}s - no subscribers found!")
        return False

    def send(self, destination: str, message: str) -> bool:
        """
        Send a message to a destination.
        
        Args:
            destination: Target callsign or "ALL"
            message: Message text
            
        Returns:
            True if sent successfully
        """
        sub_count = self.pub.get_subscription_count()
        
        if sub_count == 0:
            print(f"[ERROR] No subscribers! Message not sent.")
            return False
        
        # Create the TakChat message
        msg = self.TakChat()
        msg.origin = self.callsign
        msg.destination = destination
        msg.message = message
        msg.timestamp = get_zulu_time()
        
        # Handle UID logic based on reuse modes
        # We encode both event_uid and message_id into the single uid field
        # Format: "event_uid|message_id"
        # This allows independent control of each identifier
        
        import uuid
        
        # Generate or reuse event UID
        if self.reuse_event_uid:
            if self.last_event_uid is None:
                self.last_event_uid = str(uuid.uuid4())
                print(f"[EVENT UID] Generated: {self.last_event_uid[:16]}...")
            event_uid = self.last_event_uid
        else:
            event_uid = ""
            self.last_event_uid = None
        
        # Generate or reuse messageId
        if self.reuse_message_id:
            if self.last_message_id is None:
                self.last_message_id = str(uuid.uuid4())
                print(f"[MESSAGE ID] Generated: {self.last_message_id[:16]}...")
            message_id = self.last_message_id
        else:
            message_id = ""
            self.last_message_id = None
        
        # Encode both into uid field: "event_uid|message_id"
        msg.uid = f"{event_uid}|{message_id}"
        
        self.pub.publish(msg)
        self.message_count += 1
        
        # Log the send with UID info
        uid_parts = []
        if self.reuse_event_uid:
            uid_parts.append(f"EventUID={self.last_event_uid[:8]}...")
        else:
            uid_parts.append("EventUID=random")
        
        if self.reuse_message_id:
            uid_parts.append(f"MsgID={self.last_message_id[:8]}...")
        else:
            uid_parts.append("MsgID=random")
        
        uid_info = f" [{', '.join(uid_parts)}]"
        print(f"[SENT #{self.message_count}] {self.callsign} -> {destination}: \"{message}\"{uid_info}")
        return True

    def run_console(self):
        """
        Run the interactive console loop.
        """
        print()
        print("=" * 60)
        print("TakChat Console - Interactive Mode")
        print("=" * 60)
        print("Commands:")
        print("  DESTINATION: message   - Send to specific destination")
        print("  ALL: message           - Broadcast to all callsigns")
        print("  /reuse-event           - Reuse event UID only")
        print("  /reuse-msgid           - Reuse messageId only")
        print("  /reuse-both            - Reuse both (event UID + messageId)")
        print("  /random                - Random UIDs (default)")
        print("  /status                - Show connection status")
        print("  /quit or Ctrl+C        - Exit")
        print()
        print("Testing Workflow:")
        print("  /reuse-both            Test: both IDs match")
        print("  TRILL: msg1            (generates IDs)")
        print("  TRILL: msg2            (reuses both IDs)")
        print()
        print("  /random                Reset")
        print("  /reuse-event           Test: only event UID matches")
        print("  TRILL: msg3            (new event UID, random msgId)")
        print("  TRILL: msg4            (same event UID, random msgId)")
        print()
        print("  /random                Reset")
        print("  /reuse-msgid           Test: only messageId matches")
        print("  TRILL: msg5            (random event UID, new msgId)")
        print("  TRILL: msg6            (random event UID, same msgId)")
        print("=" * 60)
        print()
        
        # Start a background thread for spinning
        spin_thread = threading.Thread(target=self._spin_loop, daemon=True)
        spin_thread.start()
        
        try:
            while rclpy.ok():
                try:
                    line = input("> ").strip()
                except EOFError:
                    break
                
                if not line:
                    continue
                
                # Handle quit commands
                if line.lower() in ('/quit', '/exit', 'quit', 'exit'):
                    print("Goodbye!")
                    break
                
                # Handle status command
                if line.lower() == '/status':
                    count = self.pub.get_subscription_count()
                    
                    # Build status string
                    if self.reuse_event_uid and self.reuse_message_id:
                        mode = "REUSE BOTH"
                        details = f"EventUID={self.last_event_uid[:16] if self.last_event_uid else 'not yet generated'}..., MsgID={self.last_message_id[:16] if self.last_message_id else 'not yet generated'}..."
                    elif self.reuse_event_uid:
                        mode = "REUSE EVENT UID ONLY"
                        details = f"EventUID={self.last_event_uid[:16] if self.last_event_uid else 'not yet generated'}..."
                    elif self.reuse_message_id:
                        mode = "REUSE MESSAGE ID ONLY"
                        details = f"MsgID={self.last_message_id[:16] if self.last_message_id else 'not yet generated'}..."
                    else:
                        mode = "RANDOM"
                        details = "All IDs randomly generated"
                    
                    print(f"[STATUS] Subscribers: {count}, Messages sent: {self.message_count}")
                    print(f"[STATUS] Mode: {mode}")
                    print(f"[STATUS] {details}")
                    continue
                
                # Handle reuse-event command
                if line.lower() in ('/reuse-event', '/reuse-uid'):
                    self.reuse_event_uid = True
                    self.reuse_message_id = False
                    self.last_message_id = None  # Clear msgId so it generates new ones
                    print(f"[MODE] REUSE EVENT UID ONLY")
                    print(f"[MODE] Event UID will be reused, messageId will be random")
                    print(f"[MODE] This tests if ATAK uses event UID as primary key")
                    continue
                
                # Handle reuse-msgid command
                if line.lower() in ('/reuse-msgid', '/reuse-messageid'):
                    self.reuse_event_uid = False
                    self.reuse_message_id = True
                    self.last_event_uid = None  # Clear event UID so it generates new ones
                    print(f"[MODE] REUSE MESSAGE ID ONLY")
                    print(f"[MODE] MessageId will be reused, event UID will be random")
                    print(f"[MODE] This tests if ATAK uses messageId as primary key")
                    continue
                
                # Handle reuse-both command
                if line.lower() in ('/reuse-both', '/reuse-all'):
                    self.reuse_event_uid = True
                    self.reuse_message_id = True
                    print(f"[MODE] REUSE BOTH (event UID + messageId)")
                    print(f"[MODE] Both identifiers will be reused")
                    print(f"[MODE] This tests if ATAK treats identical IDs as updates")
                    continue
                
                # Handle random command (disable all reuse)
                if line.lower() in ('/random', '/new-uid', '/clear'):
                    self.reuse_event_uid = False
                    self.reuse_message_id = False
                    self.last_event_uid = None
                    self.last_message_id = None
                    print(f"[MODE] RANDOM (default)")
                    print(f"[MODE] All identifiers will be randomly generated")
                    continue
                
                # Handle unknown commands
                if line.startswith('/'):
                    print(f"[ERROR] Unknown command: {line}")
                    print("        Valid: /reuse-event, /reuse-msgid, /reuse-both, /random, /status, /quit")
                    continue
                
                # Parse "DESTINATION: message" format
                if ':' not in line:
                    print("[ERROR] Format: DESTINATION: message")
                    print("        Example: TRILL: Hello there")
                    continue
                
                parts = line.split(':', 1)
                destination = parts[0].strip().upper()
                message = parts[1].strip()
                
                if not destination:
                    print("[ERROR] Destination cannot be empty")
                    continue
                
                if not message:
                    print("[ERROR] Message cannot be empty")
                    continue
                
                self.send(destination, message)
                
        except KeyboardInterrupt:
            print("\nInterrupted. Goodbye!")

    def _spin_loop(self):
        """Background thread to keep ROS spinning."""
        while rclpy.ok():
            rclpy.spin_once(self, timeout_sec=0.1)


def main():
    ap = argparse.ArgumentParser(
        description="Interactive TAK chat console (stays running for reliable delivery)"
    )
    
    ap.add_argument("--namespace", default="warthog1",
                    help="Robot namespace (default: warthog1)")
    ap.add_argument("--from", dest="callsign", default="warthog1",
                    help="Our callsign (default: warthog1)")
    ap.add_argument("--topic", default="tak_chat/out",
                    help="Topic relative to namespace (default: tak_chat/out)")
    ap.add_argument("--discovery-timeout", type=float, default=10.0,
                    help="Seconds to wait for TakChatNode discovery (default: 10.0)")

    args = ap.parse_args()

    # Build full topic name
    ns = args.namespace.strip().strip("/")
    full_topic = f"/{ns}/{args.topic}".replace("//", "/")

    # Initialize ROS2
    rclpy.init()
    
    node = TakChatConsole(
        topic=full_topic,
        callsign=args.callsign,
    )

    try:
        # Wait for discovery (one-time cost)
        if not node.wait_for_subscriber(args.discovery_timeout):
            print("\n[WARNING] TakChatNode not found. Messages may not be delivered.")
            print("Make sure TakChatNode is running:")
            print(f"  ros2 launch tak_chat tak_chat.launch.py")
            print()
        
        # Run interactive console
        node.run_console()
        
    finally:
        node.destroy_node()
        rclpy.shutdown()


if __name__ == "__main__":
    main()
