#!/usr/bin/env python3
"""
tak_chat_test_until_send_to_tak.py

Publish TakChat requests to /<namespace>/tak_chat/out repeatedly until the
converted CoT XML appears on /<namespace>/send_to_tak.

Why this works:
- It verifies the pipeline at the most important hop: send_to_tak.
- It keeps publishing until it sees confirmation from the actual converter path.
- It prints how many sends were required before the message showed up.

Typical usage:
  # Fan-out to allowed callsigns from cot_runner.yaml until confirmed on send_to_tak
  ./tak_chat_test_until_send_to_tak.py "Hello world"

  # Single destination
  ./tak_chat_test_until_send_to_tak.py "Hello TRILL" --to TRILL

  # Override namespace/origin
  ./tak_chat_test_until_send_to_tak.py "Test" --namespace warthog1 --from warthog1
"""

import argparse
import time
from datetime import datetime, timezone
from typing import List, Set

import rclpy
from rclpy.node import Node
from rclpy.qos import QoSProfile, ReliabilityPolicy, DurabilityPolicy, HistoryPolicy

# Optional dependency for YAML parsing
try:
    import yaml
except ImportError:
    yaml = None


def get_zulu_time_ms() -> str:
    """ISO 8601 Zulu time with millisecond precision."""
    now = datetime.now(timezone.utc)
    return now.strftime("%Y-%m-%dT%H:%M:%S.%f")[:-3] + "Z"


def _find_allowed_callsigns(obj):
    """
    Recursively search YAML-loaded object for:
      cot_msg_defaults:
        allowed: [ ... ]
    Returns list[str] or None.
    """
    if isinstance(obj, dict):
        if "cot_msg_defaults" in obj and isinstance(obj["cot_msg_defaults"], dict):
            cm = obj["cot_msg_defaults"]
            if "allowed" in cm and isinstance(cm["allowed"], list):
                return [str(x).strip() for x in cm["allowed"] if str(x).strip()]
        for v in obj.values():
            found = _find_allowed_callsigns(v)
            if found:
                return found
    elif isinstance(obj, list):
        for item in obj:
            found = _find_allowed_callsigns(item)
            if found:
                return found
    return None


def load_allowed_callsigns(yaml_path: str) -> List[str]:
    """Load the allowed callsigns list from the cot_runner.yaml config file."""
    if yaml is None:
        raise RuntimeError("PyYAML is not installed. Install with: pip install pyyaml")
    with open(yaml_path, "r", encoding="utf-8") as f:
        data = yaml.safe_load(f)

    allowed = _find_allowed_callsigns(data)
    if not allowed:
        raise RuntimeError(f"Could not find cot_msg_defaults.allowed in {yaml_path}")

    # De-duplicate while preserving order
    seen: Set[str] = set()
    out: List[str] = []
    for c in allowed:
        if c not in seen:
            seen.add(c)
            out.append(c)
    return out


class TakChatUntilSendToTak(Node):
    """
    Publishes TakChat messages repeatedly until send_to_tak shows the converted CoT.
    
    This is a diagnostic tool to verify the full pipeline:
      BT/Test -> tak_chat/out -> TakChatNode -> send_to_tak -> TAK bridge
    
    The node keeps publishing at a fixed rate until:
      1. All (or any, depending on --require-all) destinations are confirmed, OR
      2. The timeout is reached
    """

    def __init__(
        self,
        tak_chat_out_topic: str,
        send_to_tak_topic: str,
        origin: str,
        destinations: List[str],
        message: str,
        publish_hz: float,
        timeout_s: float,
        pre_spin_s: float,
        require_all_destinations: bool,
    ):
        super().__init__("tak_chat_test_until_send_to_tak")

        # Import message types (deferred to avoid import errors if not in ROS env)
        from tak_chat.msg import TakChat  # type: ignore
        from std_msgs.msg import String  # type: ignore

        self.TakChat = TakChat

        # Store configuration
        self.origin = origin
        self.destinations = destinations
        self.message_text = message
        self.require_all = require_all_destinations

        # Correlation token: timestamp we inject into TakChat messages
        # We'll look for this message text in the CoT XML on send_to_tak
        self.sent_stamp = get_zulu_time_ms()

        # Tracking state
        self.matched_destinations: Set[str] = set()  # Destinations confirmed via send_to_tak
        self.send_count_total = 0
        self.send_count_by_dest = {d: 0 for d in destinations}

        # Timing
        self.timeout_deadline = time.time() + timeout_s
        self.pre_spin_deadline = time.time() + pre_spin_s

        # QoS: RELIABLE to match tak_chat_node's subscription
        qos_reliable = QoSProfile(
            history=HistoryPolicy.KEEP_LAST,
            depth=10,
            reliability=ReliabilityPolicy.RELIABLE,
            durability=DurabilityPolicy.VOLATILE,
        )

        # Publisher: TakChat requests -> tak_chat_node
        self.pub = self.create_publisher(TakChat, tak_chat_out_topic, qos_reliable)

        # Subscriber: CoT XML emitted by tak_chat_node -> send_to_tak topic
        self.sub = self.create_subscription(
            String, send_to_tak_topic, self._send_to_tak_cb, qos_reliable
        )

        # Timer for periodic publishing
        self.period_s = 1.0 / max(0.5, publish_hz)  # Cap at >= 0.5 Hz minimum
        self.timer = self.create_timer(self.period_s, self._tick)

        # Flag to track if we've finished (prevents double shutdown)
        self.finished = False

        # Print startup info
        print("=" * 60)
        print("TakChat Pipeline Test - Until send_to_tak Confirmation")
        print("=" * 60)
        print(f"TakChat OUT topic:  {tak_chat_out_topic}")
        print(f"send_to_tak topic:  {send_to_tak_topic}")
        print(f"Origin:             {origin}")
        print(f"Destinations ({len(destinations)}): {', '.join(destinations)}")
        print(f"Message:            {message}")
        print(f"Correlation stamp:  {self.sent_stamp}")
        print(f"Mode:               {'require ALL destinations' if self.require_all else 'require ANY destination'}")
        print(f"Publish rate:       {publish_hz} Hz (every {self.period_s:.1f}s)")
        print(f"Timeout:            {timeout_s} s")
        print(f"Pre-spin warmup:    {pre_spin_s} s")
        print("-" * 60)

    def _cot_matches(self, cot_xml: str, dest: str) -> bool:
        """
        Check if this CoT XML matches our request to the given destination.

        We match on:
          - Message text present in <remarks> (or anywhere in XML)
          - Destination callsign present (callsign="DEST", chatroom="DEST", or id="DEST")
        """
        # Must contain our message text
        if self.message_text not in cot_xml:
            return False

        # Must contain the destination in one of the expected attribute patterns
        patterns = [
            f'callsign="{dest}"',
            f'chatroom="{dest}"',
            f'id="{dest}"',
        ]
        if not any(p in cot_xml for p in patterns):
            return False

        return True

    def _send_to_tak_cb(self, msg):
        """
        Callback for messages received on send_to_tak topic.
        
        Checks if the CoT XML matches any of our pending destinations.
        """
        if self.finished:
            return

        cot_xml = msg.data

        # Check for any destinations that match this CoT
        for dest in self.destinations:
            # Skip already-confirmed destinations
            if dest in self.matched_destinations:
                continue

            if self._cot_matches(cot_xml, dest):
                self.matched_destinations.add(dest)
                print(f"[CONFIRMED] send_to_tak received message for dest={dest} "
                      f"(after {self.send_count_by_dest[dest]} sends to this dest, "
                      f"{self.send_count_total} total publishes)")

        # Check exit conditions
        if self.require_all:
            # Need all destinations confirmed
            if len(self.matched_destinations) == len(self.destinations):
                self._finish_success()
        else:
            # Need at least one destination confirmed
            if len(self.matched_destinations) >= 1:
                self._finish_success()

    def _tick(self):
        """
        Timer callback - publishes TakChat messages to unconfirmed destinations.
        
        KEY BEHAVIOR: Keeps retrying until the destination is CONFIRMED on send_to_tak,
        not just until it's been sent once. This handles DDS discovery races.
        """
        if self.finished:
            return

        now = time.time()

        # Check timeout
        if now >= self.timeout_deadline:
            self._finish_timeout()
            return

        # Wait for pre-spin discovery grace period
        if now < self.pre_spin_deadline:
            return

        # Publish to each destination that hasn't been confirmed yet
        for dest in self.destinations:
            # IMPORTANT: Skip only if CONFIRMED, not if already sent
            # This is the key difference - we keep retrying until confirmed
            if dest in self.matched_destinations:
                continue

            msg = self.TakChat()
            msg.origin = self.origin
            msg.destination = dest
            msg.message = self.message_text
            msg.timestamp = self.sent_stamp

            self.pub.publish(msg)

            self.send_count_total += 1
            self.send_count_by_dest[dest] += 1
            print(f"[SENT #{self.send_count_by_dest[dest]}] "
                  f"Published to {dest} (total: {self.send_count_total})")

    def _finish_success(self):
        """Called when all required destinations have been confirmed."""
        if self.finished:
            return
        self.finished = True
        self.timer.cancel()

        print("-" * 60)
        print("SUCCESS: send_to_tak confirmed delivery!")
        print(f"Total publishes: {self.send_count_total}")
        print("Per-destination breakdown:")
        for d in self.destinations:
            status = "CONFIRMED" if d in self.matched_destinations else "not confirmed"
            print(f"  {d}: {self.send_count_by_dest[d]} sends - {status}")
        print("=" * 60)

    def _finish_timeout(self):
        """Called when timeout is reached without full confirmation."""
        if self.finished:
            return
        self.finished = True
        self.timer.cancel()

        print("-" * 60)
        print("TIMEOUT: Did not see confirmation on send_to_tak before deadline.")
        print(f"Total publishes: {self.send_count_total}")
        confirmed_list = sorted(self.matched_destinations) if self.matched_destinations else ["None"]
        print(f"Confirmed destinations: {', '.join(confirmed_list)}")
        print("Per-destination breakdown:")
        for d in self.destinations:
            status = "CONFIRMED" if d in self.matched_destinations else "NOT confirmed"
            print(f"  {d}: {self.send_count_by_dest[d]} sends - {status}")
        print("=" * 60)


def main():
    ap = argparse.ArgumentParser(
        description="Send TakChat messages until send_to_tak confirms receipt",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  # Test with fan-out to all allowed callsigns:
  ./tak_chat_test_until_send_to_tak.py "Hello world"

  # Test to a single destination:
  ./tak_chat_test_until_send_to_tak.py "Hello TRILL" --to TRILL

  # Require ALL destinations to be confirmed (for fan-out):
  ./tak_chat_test_until_send_to_tak.py "Broadcast test" --require-all

  # Custom namespace and origin:
  ./tak_chat_test_until_send_to_tak.py "Test" --namespace warthog2 --from warthog2
"""
    )
    ap.add_argument("message", type=str, help="Message text to send")

    ap.add_argument("--namespace", default="warthog1",
                    help="Robot namespace (default: warthog1)")
    ap.add_argument("--from", dest="origin", default="warthog1",
                    help="Origin callsign (default: warthog1)")
    ap.add_argument("--to", default="ALL",
                    help="Destination callsign, or 'ALL' for fan-out (default: ALL)")
    ap.add_argument("--config",
                    default="/phoenix/src/phoenix-tak/src/tak_bridge/config/cot_runner.yaml",
                    help="Path to cot_runner.yaml for loading allowed callsigns")

    # Topic configuration
    ap.add_argument("--tak-chat-out-topic", default="tak_chat/out",
                    help="TakChat OUT topic relative to namespace (default: tak_chat/out)")
    ap.add_argument("--send-to-tak-topic", default="send_to_tak",
                    help="send_to_tak topic relative to namespace (default: send_to_tak)")

    # Timing/retry behavior
    ap.add_argument("--rate", type=float, default=0.5,
                    help="Publish rate in Hz (default: 0.5 = every 2 seconds)")
    ap.add_argument("--timeout", type=float, default=12.0,
                    help="Timeout in seconds (default: 12)")
    ap.add_argument("--pre-spin", type=float, default=0.75,
                    help="Discovery warmup time in seconds (default: 0.75)")

    # Confirmation semantics
    ap.add_argument("--require-all", action="store_true",
                    help="Require confirmation for ALL destinations (default: ANY one)")

    args = ap.parse_args()

    # Build fully qualified topic names under namespace
    ns = args.namespace.strip().strip("/")
    tak_chat_out = f"/{ns}/{args.tak_chat_out_topic}".replace("//", "/")
    send_to_tak = f"/{ns}/{args.send_to_tak_topic}".replace("//", "/")

    # Resolve destinations
    to_arg = (args.to or "").strip()
    if to_arg.upper() == "ALL":
        dests = load_allowed_callsigns(args.config)
        print(f"Loaded {len(dests)} allowed callsigns from {args.config}")
    else:
        dests = [to_arg]

    # Initialize ROS and run
    rclpy.init()
    node = TakChatUntilSendToTak(
        tak_chat_out_topic=tak_chat_out,
        send_to_tak_topic=send_to_tak,
        origin=args.origin,
        destinations=dests,
        message=args.message,
        publish_hz=args.rate,
        timeout_s=args.timeout,
        pre_spin_s=args.pre_spin,
        require_all_destinations=args.require_all,
    )

    # Spin until the node finishes (success or timeout)
    try:
        while rclpy.ok() and not node.finished:
            rclpy.spin_once(node, timeout_sec=0.1)
    except KeyboardInterrupt:
        print("\nInterrupted by user")
    finally:
        if not node.finished:
            node.destroy_node()
        rclpy.shutdown()


if __name__ == "__main__":
    main()
