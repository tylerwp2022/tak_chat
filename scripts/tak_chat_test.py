#!/usr/bin/env python3
"""
tak_chat_test.py

Reliable test script for sending TAK chat messages via TakChatNode.

Since TakChatNode handles:
  - Fan-out of "ALL" to all allowed callsigns
  - Retry-until-confirmed logic
  - CoT XML conversion

This script publishes to tak_chat/out and waits for TakChatNode to be
discovered before publishing, ensuring reliable delivery.

KEY IMPROVEMENT:
----------------
Uses get_subscription_count() to wait until TakChatNode has discovered
this publisher before sending. This handles DDS discovery races that
cause messages to be lost with short-lived publisher nodes.

USAGE:
------
  # Broadcast to all allowed callsigns
  ./tak_chat_test.py "Hello everyone"

  # Send to a specific callsign
  ./tak_chat_test.py "Hello TRILL" --to TRILL

  # Different robot/namespace
  ./tak_chat_test.py "Hello" --namespace warthog2 --from warthog2
  
  # Quick mode (less reliable, for testing)
  ./tak_chat_test.py "Quick test" --discovery-timeout 1.0
"""

import argparse
import time
import uuid
from datetime import datetime, timezone

import rclpy
from rclpy.node import Node
from rclpy.qos import QoSProfile, ReliabilityPolicy, DurabilityPolicy, HistoryPolicy


def get_zulu_time() -> str:
    """
    Get current time in ISO 8601 Zulu format with milliseconds.
    
    Returns:
        String like "2024-01-15T14:30:45.123Z"
    """
    now = datetime.now(timezone.utc)
    return now.strftime("%Y-%m-%dT%H:%M:%S.%f")[:-3] + "Z"


class TakChatTestPublisher(Node):
    """
    Reliable publisher for testing TAK chat messages.
    
    This node:
      1. Creates a publisher to tak_chat/out
      2. Waits for TakChatNode to discover it (subscriber count > 0)
      3. Adds a small stabilization delay after discovery
      4. Publishes the message multiple times for reliability
    
    The key improvement over a simple "publish and exit" approach is
    waiting for subscriber discovery, which handles the DDS discovery
    race condition that causes messages to be lost.
    """

    def __init__(self, topic: str, origin: str, destination: str, message: str,
                 discovery_timeout: float = 5.0, min_discovery_time: float = 0.3,
                 publish_count: int = 3, publish_interval: float = 0.2,
                 post_discovery_delay: float = 0.1):
        """
        Initialize the test publisher.
        
        Args:
            topic: Full ROS topic path (e.g., "/warthog1/tak_chat/out")
            origin: Sender callsign (must match TakChatNode's callsign)
            destination: Target callsign or "ALL" for broadcast
            message: The chat message text to send
            discovery_timeout: Max seconds to wait for subscriber discovery
            min_discovery_time: Min seconds to wait even if subscriber seen instantly
                               (handles stale DDS cache between short-lived nodes)
            publish_count: Number of times to publish (for reliability)
            publish_interval: Seconds between publishes
            post_discovery_delay: Seconds to wait after discovery before publishing
        """
        super().__init__("tak_chat_test_publisher")

        # Import TakChat message type
        # Deferred import avoids errors when running outside ROS environment
        from tak_chat.msg import TakChat
        self.TakChat = TakChat

        # Store message parameters
        self.origin = origin
        self.destination = destination
        self.message = message
        self.timestamp = get_zulu_time()
        
        # Configuration
        self.discovery_timeout = discovery_timeout
        self.min_discovery_time = min_discovery_time
        self.publish_count = publish_count
        self.publish_interval = publish_interval
        self.post_discovery_delay = post_discovery_delay
        
        # Track state
        self.publishes_sent = 0

        # QoS Configuration
        # RELIABLE: Ensures delivery confirmation
        # TRANSIENT_LOCAL: Critical for handling DDS discovery races!
        #   - We keep recent messages in a buffer
        #   - When TakChatNode discovers us, it gets our buffered messages
        #   - This solves the bidirectional discovery problem
        qos = QoSProfile(
            history=HistoryPolicy.KEEP_LAST,
            depth=10,
            reliability=ReliabilityPolicy.RELIABLE,
            durability=DurabilityPolicy.TRANSIENT_LOCAL,
        )
        
        # Create publisher
        self.pub = self.create_publisher(TakChat, topic, qos)
        self.topic = topic

        # Log configuration
        print("=" * 60)
        print("TakChat Test Publisher")
        print("=" * 60)
        print(f"  Topic:              {topic}")
        print(f"  Origin:             {origin}")
        print(f"  Destination:        {destination}")
        print(f"  Message:            {message}")
        print(f"  Discovery timeout:  {discovery_timeout}s")
        print(f"  Min discovery time: {min_discovery_time}s")
        print(f"  Publish count:      {publish_count}")
        print(f"  Publish interval:   {publish_interval}s")
        print(f"  Post-discovery delay: {post_discovery_delay}s")
        print("-" * 60)

    def wait_for_subscriber(self) -> bool:
        """
        Wait until at least one subscriber is connected to our publisher.
        
        This is CRITICAL for reliable delivery. If we publish before TakChatNode
        has discovered us via DDS, the message disappears into the void.
        
        IMPORTANT: DDS caches endpoint information between short-lived nodes.
        If we see a subscriber "instantly" (< 100ms), it's likely stale data
        from a previous run. We MUST wait for the minimum discovery time
        regardless of what get_subscription_count() reports.
        
        The method spins the node while waiting, which allows DDS discovery
        messages to be processed.
        
        Returns:
            True if a subscriber was found within the timeout
            False if the timeout was reached with no subscribers
        """
        print("[DISCOVERY] Waiting for TakChatNode to discover this publisher...")
        
        # CRITICAL: DDS caches endpoint info between runs. If we see a subscriber
        # instantly, it's probably stale. We MUST wait at least this long for
        # fresh discovery to occur, regardless of subscription count.
        min_discovery_time = self.min_discovery_time
        
        start_time = time.time()
        check_interval = 0.05  # Check every 50ms for responsiveness
        last_log_time = start_time
        subscriber_found = False
        first_seen_time = None
        
        while time.time() - start_time < self.discovery_timeout:
            # Check how many subscribers are connected
            subscriber_count = self.pub.get_subscription_count()
            elapsed = time.time() - start_time
            
            if subscriber_count > 0:
                if not subscriber_found:
                    subscriber_found = True
                    first_seen_time = time.time()
                    
                    if elapsed < 0.01:
                        # Instant discovery is suspicious - likely stale DDS cache
                        print(f"[DISCOVERY] Subscriber count = {subscriber_count} "
                              f"(instant - likely stale DDS cache)")
                        print(f"[DISCOVERY] Waiting {min_discovery_time}s for "
                              "fresh discovery...")
                    else:
                        print(f"[DISCOVERY] Subscriber count = {subscriber_count} "
                              f"after {elapsed:.3f}s")
                
                # Check if we've waited long enough for fresh discovery
                time_since_start = time.time() - start_time
                if time_since_start >= min_discovery_time:
                    # Now do the post-discovery stabilization
                    print(f"[DISCOVERY] SUCCESS - waited {time_since_start:.3f}s, "
                          f"subscriber count = {subscriber_count}")
                    
                    if self.post_discovery_delay > 0:
                        print(f"[DISCOVERY] Additional {self.post_discovery_delay}s "
                              "stabilization delay...")
                        stabilize_until = time.time() + self.post_discovery_delay
                        while time.time() < stabilize_until:
                            rclpy.spin_once(self, timeout_sec=0.01)
                        
                        # Re-check subscriber count after delay
                        new_count = self.pub.get_subscription_count()
                        print(f"[DISCOVERY] After stabilization: {new_count} subscriber(s)")
                    
                    return True
            
            # Log progress every second so user knows we're waiting
            if time.time() - last_log_time >= 1.0:
                print(f"[DISCOVERY] Still waiting... ({elapsed:.1f}s elapsed, "
                      f"subscribers: {subscriber_count})")
                last_log_time = time.time()
            
            # Spin to process DDS discovery messages
            # This is essential - without spinning, discovery won't progress
            rclpy.spin_once(self, timeout_sec=check_interval)
        
        # Timeout reached
        print(f"[DISCOVERY] TIMEOUT - No subscribers found after "
              f"{self.discovery_timeout}s!")
        print("[DISCOVERY] Publishing anyway, but message will likely be LOST")
        return False

    def publish_message(self):
        """
        Publish the TakChat message multiple times for reliability.
        
        Builds and publishes the message the configured number of times
        with intervals between each publish. This helps handle any
        remaining race conditions after discovery.
        """
        # Build the message
        msg = self.TakChat()
        msg.origin = self.origin
        msg.destination = self.destination
        msg.message = self.message
        msg.timestamp = self.timestamp

        print(f"[PUBLISH] Sending {self.publish_count} publish(es) with "
              f"{self.publish_interval}s interval...")

        # Publish the configured number of times
        for i in range(self.publish_count):
            # Check subscriber count before each publish
            sub_count = self.pub.get_subscription_count()
            
            self.pub.publish(msg)
            self.publishes_sent += 1
            
            print(f"[PUBLISH #{i+1}] {self.origin} -> {self.destination}: "
                  f"\"{self.message}\" (subscribers: {sub_count})")
            
            # Spin and wait between publishes
            if i < self.publish_count - 1:
                wait_until = time.time() + self.publish_interval
                while time.time() < wait_until:
                    rclpy.spin_once(self, timeout_sec=0.01)
        
        print("-" * 60)
        print(f"[DONE] Sent {self.publishes_sent} publish(es)")
        print("TakChatNode should now handle retry logic and delivery to TAK.")

    def flush(self, duration: float = 0.5):
        """
        Spin for a bit to ensure messages are flushed through DDS.
        
        With TRANSIENT_LOCAL QoS, the message is buffered and will be
        delivered when the subscriber discovers us. We still spin briefly
        to allow DDS to process the publish.
        
        Args:
            duration: How long to spin in seconds
        """
        print(f"[FLUSH] Spinning for {duration}s to ensure delivery...")
        flush_start = time.time()
        while rclpy.ok() and time.time() - flush_start < duration:
            rclpy.spin_once(self, timeout_sec=0.05)
        
        final_count = self.pub.get_subscription_count()
        print(f"[FLUSH] Complete. Final subscriber count: {final_count}")


def main():
    """
    Main entry point for the TAK chat test script.
    
    Parses command line arguments, initializes ROS, creates the publisher,
    waits for subscriber discovery, and sends the message.
    """
    ap = argparse.ArgumentParser(
        description="Send a TAK chat message for testing",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  # Broadcast to all allowed callsigns:
  ./tak_chat_test.py "Hello everyone"

  # Send to specific callsign:
  ./tak_chat_test.py "Hello TRILL" --to TRILL

  # Different namespace/origin:
  ./tak_chat_test.py "Hello" --namespace warthog2 --from warthog2
  
  # Shorter timeout for quick testing:
  ./tak_chat_test.py "Quick test" --discovery-timeout 2.0
  
  # More publishes for extra reliability:
  ./tak_chat_test.py "Important message" --publish-count 5

  # Add unique suffix to avoid deduplication:
  ./tak_chat_test.py "Test message" --unique
"""
    )
    
    # Required argument: the message text
    ap.add_argument("message", help="Message to send")
    
    # Identity and routing arguments
    ap.add_argument("--namespace", default="warthog1",
                    help="Robot namespace (default: warthog1)")
    ap.add_argument("--from", dest="origin", default="warthog1",
                    help="Origin callsign - must match TakChatNode's callsign "
                         "(default: warthog1)")
    ap.add_argument("--to", default="ALL",
                    help="Destination: callsign or 'ALL' for broadcast (default: ALL)")
    ap.add_argument("--topic", default="tak_chat/out",
                    help="Topic relative to namespace (default: tak_chat/out)")
    
    # Reliability tuning arguments
    # NOTE: With TRANSIENT_LOCAL QoS, we don't need multiple publishes.
    # The publisher buffers messages and delivers them when the subscriber
    # completes discovery. Single publish is sufficient.
    ap.add_argument("--discovery-timeout", type=float, default=5.0,
                    help="Max seconds to wait for TakChatNode discovery (default: 5.0)")
    ap.add_argument("--min-discovery-time", type=float, default=0.5,
                    help="Minimum seconds to wait even if subscriber seen instantly "
                         "(default: 0.5) - handles stale DDS cache")
    ap.add_argument("--publish-count", type=int, default=1,
                    help="Number of times to publish the message (default: 1)")
    ap.add_argument("--publish-interval", type=float, default=0.5,
                    help="Seconds between publishes (default: 0.5)")
    ap.add_argument("--post-discovery-delay", type=float, default=0.3,
                    help="Seconds to wait after discovery before publishing (default: 0.3)")
    
    # Debugging helpers
    ap.add_argument("--unique", action="store_true",
                    help="Append unique ID to message to avoid deduplication")

    args = ap.parse_args()

    # Build fully-qualified topic name under the robot's namespace
    # e.g., "/warthog1/tak_chat/out"
    ns = args.namespace.strip().strip("/")
    full_topic = f"/{ns}/{args.topic}".replace("//", "/")

    # Clean up destination
    destination = args.to.strip()
    
    # Optionally make message unique to avoid deduplication issues
    message = args.message
    if args.unique:
        unique_id = str(uuid.uuid4())[:8]
        message = f"{args.message} [{unique_id}]"
        print(f"[NOTE] Added unique suffix: {message}")

    # Initialize ROS2
    rclpy.init()
    
    # Create the publisher node
    node = TakChatTestPublisher(
        topic=full_topic,
        origin=args.origin,
        destination=destination,
        message=message,
        discovery_timeout=args.discovery_timeout,
        min_discovery_time=args.min_discovery_time,
        publish_count=args.publish_count,
        publish_interval=args.publish_interval,
        post_discovery_delay=args.post_discovery_delay,
    )

    exit_code = 0
    try:
        # Step 1: Wait for TakChatNode to discover us
        subscriber_found = node.wait_for_subscriber()
        
        if not subscriber_found:
            print("\n*** WARNING: No subscriber found! ***")
            print("Make sure TakChatNode is running:")
            print(f"  ros2 run tak_chat tak_chat_node --ros-args -p callsign:={args.origin}")
            print()
            exit_code = 1
        
        # Step 2: Publish the message (even if no subscriber, for debugging)
        node.publish_message()
        
        # Step 3: Flush to ensure messages are sent
        node.flush()
            
    except KeyboardInterrupt:
        print("\nInterrupted by user")
        exit_code = 130
    finally:
        node.destroy_node()
        rclpy.shutdown()
    
    return exit_code


if __name__ == "__main__":
    exit(main())
