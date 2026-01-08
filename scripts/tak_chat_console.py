#!/usr/bin/env python3
"""
tak_chat_console.py

Interactive console for sending TAK chat messages.

Unlike tak_chat_test.py (which exits after sending), this node stays alive
indefinitely. This solves the DDS discovery race condition because:
  1. Node starts and creates publisher
  2. We wait for tak_chat_node to discover us (one-time cost)
  3. All subsequent messages are delivered instantly and reliably

USAGE:
------
  ros2 run tak_chat tak_chat_console.py

  # Then type messages interactively:
  > TRILL: Hello there
  > ALL: Broadcast to everyone
  > quit

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
        
        msg = self.TakChat()
        msg.origin = self.callsign
        msg.destination = destination
        msg.message = message
        msg.timestamp = get_zulu_time()
        
        self.pub.publish(msg)
        self.message_count += 1
        
        print(f"[SENT #{self.message_count}] {self.callsign} -> {destination}: \"{message}\"")
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
        print("  /status                - Show connection status")
        print("  /quit or Ctrl+C        - Exit")
        print()
        print("Examples:")
        print("  TRILL: Hello there")
        print("  ALL: System online")
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
                
                # Handle commands
                if line.lower() in ('/quit', '/exit', 'quit', 'exit'):
                    print("Goodbye!")
                    break
                
                if line.lower() == '/status':
                    count = self.pub.get_subscription_count()
                    print(f"[STATUS] Subscribers: {count}, Messages sent: {self.message_count}")
                    continue
                
                if line.startswith('/'):
                    print(f"[ERROR] Unknown command: {line}")
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
