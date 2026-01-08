#!/usr/bin/env python3
"""
Monitor a high-traffic ROS2 topic and print only matching CoT (Cursor on Target) messages.

Default behavior:
- Subscribes to a std_msgs/msg/String topic (default: /warthog1/send_to_tak)
- Only prints messages that contain "GeoChat"
- Attempts to parse CoT XML and prints a compact summary (chatroom, senderCallsign, remarks, etc.)

Usage examples:
  python3 cot_monitor.py
  python3 cot_monitor.py --topic /warthog1/send_to_tak
  python3 cot_monitor.py --contains GeoChat --chatroom TRILL
  python3 cot_monitor.py --regex 'GeoChat.*senderCallsign="warthog1"'
  python3 cot_monitor.py --raw
  python3 cot_monitor.py --out cot_hits.log
"""

import argparse
import re
import sys
import time
import xml.etree.ElementTree as ET
from typing import Optional, Dict, Any

import rclpy
from rclpy.node import Node
from std_msgs.msg import String


def _strip_ros_echo_prefix(s: str) -> str:
    """
    If the string looks like a ros2 topic echo style:
        data: <event ...>...</event>
    return just the XML part.
    """
    # Your sample includes: "data: <event ...</event>"
    # In ROS messages, msg.data will normally be ONLY the XML, but some systems wrap it.
    s = s.strip()
    if s.startswith("data:"):
        s = s[len("data:"):].lstrip()
    return s


def _safe_text(elem: Optional[ET.Element]) -> str:
    if elem is None or elem.text is None:
        return ""
    return elem.text.strip()


def parse_cot(xml_text: str) -> Dict[str, Any]:
    """
    Parse common CoT fields from an <event> message.
    Returns dict with best-effort fields; missing fields are None/"".
    """
    out: Dict[str, Any] = {
        "uid": None,
        "type": None,
        "how": None,
        "time": None,
        "start": None,
        "stale": None,
        "lat": None,
        "lon": None,
        "hae": None,
        "ce": None,
        "le": None,
        "chatroom": None,
        "senderCallsign": None,
        "remarks": None,
        "messageId": None,
        "raw_event_tag": None,
    }

    # Some payloads might contain leading junk; try to find "<event"
    idx = xml_text.find("<event")
    if idx > 0:
        xml_text = xml_text[idx:]

    root = ET.fromstring(xml_text)
    out["raw_event_tag"] = root.tag

    # Usually root is <event ...>
    out["uid"] = root.attrib.get("uid")
    out["type"] = root.attrib.get("type")
    out["how"] = root.attrib.get("how")
    out["time"] = root.attrib.get("time")
    out["start"] = root.attrib.get("start")
    out["stale"] = root.attrib.get("stale")

    # <point lat=".." lon=".." ... />
    point = root.find("point")
    if point is not None:
        out["lat"] = point.attrib.get("lat")
        out["lon"] = point.attrib.get("lon")
        out["hae"] = point.attrib.get("hae")
        out["ce"] = point.attrib.get("ce")
        out["le"] = point.attrib.get("le")

    # <detail> ... </detail>
    detail = root.find("detail")
    if detail is not None:
        # chat metadata is often under <__chat ... />
        chat = detail.find("__chat")
        if chat is not None:
            out["chatroom"] = chat.attrib.get("chatroom") or chat.attrib.get("id")
            out["senderCallsign"] = chat.attrib.get("senderCallsign")
            out["messageId"] = chat.attrib.get("messageId")

        # remarks text is usually under <remarks>...</remarks>
        remarks = detail.find("remarks")
        if remarks is not None:
            out["remarks"] = _safe_text(remarks)

    return out


def format_compact(fields: Dict[str, Any]) -> str:
    """
    Single-line readable summary.
    """
    uid = fields.get("uid") or "?"
    cot_type = fields.get("type") or "?"
    t = fields.get("time") or "?"
    chatroom = fields.get("chatroom") or "?"
    sender = fields.get("senderCallsign") or "?"
    lat = fields.get("lat") or "?"
    lon = fields.get("lon") or "?"
    remarks = fields.get("remarks") or ""
    if len(remarks) > 140:
        remarks = remarks[:137] + "..."

    return (
        f"[CoT] time={t} type={cot_type} chatroom={chatroom} sender={sender} "
        f"lat={lat} lon={lon} uid={uid} remarks={remarks}"
    )


class CoTMonitor(Node):
    def __init__(
        self,
        topic: str,
        contains: Optional[str],
        regex: Optional[re.Pattern],
        chatroom: Optional[str],
        sender: Optional[str],
        cot_type: Optional[str],
        raw: bool,
        out_path: Optional[str],
        throttle_sec: float,
    ):
        super().__init__("cot_monitor")

        self.topic = topic
        self.contains = contains
        self.regex = regex
        self.chatroom = chatroom
        self.sender = sender
        self.cot_type = cot_type
        self.raw = raw
        self.out_path = out_path
        self.throttle_sec = throttle_sec

        self._last_print = 0.0
        self._hits = 0
        self._seen = 0

        self._fh = None
        if self.out_path:
            self._fh = open(self.out_path, "a", encoding="utf-8")

        self.sub = self.create_subscription(String, self.topic, self.cb, 50)
        self.get_logger().info(
            f"Listening on {self.topic}. Filters: "
            f"contains={self.contains!r}, regex={bool(self.regex)}, chatroom={self.chatroom!r}, "
            f"sender={self.sender!r}, cot_type={self.cot_type!r}"
        )

    def destroy_node(self):
        if self._fh:
            try:
                self._fh.close()
            except Exception:
                pass
        super().destroy_node()

    def _passes_text_filters(self, payload: str) -> bool:
        if self.contains and self.contains not in payload:
            return False
        if self.regex and not self.regex.search(payload):
            return False
        return True

    def _passes_parsed_filters(self, fields: Dict[str, Any]) -> bool:
        if self.chatroom and (fields.get("chatroom") != self.chatroom):
            return False
        if self.sender and (fields.get("senderCallsign") != self.sender):
            return False
        if self.cot_type and (fields.get("type") != self.cot_type):
            return False
        return True

    def cb(self, msg: String):
        self._seen += 1

        payload = msg.data or ""
        payload = _strip_ros_echo_prefix(payload)

        # Quick filters first (cheap)
        if not self._passes_text_filters(payload):
            return

        # Parse and apply field-based filters
        try:
            fields = parse_cot(payload)
        except Exception as e:
            # If it contains GeoChat but isn't valid XML, optionally show a hint
            self.get_logger().debug(f"Failed to parse XML: {e}")
            return

        if not self._passes_parsed_filters(fields):
            return

        # Throttle output if requested
        now = time.time()
        if self.throttle_sec > 0 and (now - self._last_print) < self.throttle_sec:
            return
        self._last_print = now

        self._hits += 1

        if self.raw:
            line = payload.strip()
        else:
            line = format_compact(fields)

        print(line, flush=True)

        if self._fh:
            self._fh.write(line + "\n")
            self._fh.flush()


def main():
    ap = argparse.ArgumentParser(description="Filter CoT messages on a high-traffic ROS2 String topic.")
    ap.add_argument("--topic", default="/warthog1/send_to_tak", help="Topic name (std_msgs/msg/String).")
    ap.add_argument("--contains", default="GeoChat", help='Only pass messages containing this substring (default "GeoChat").')
    ap.add_argument("--regex", default=None, help="Optional regex filter applied to the raw payload.")
    ap.add_argument("--chatroom", default=None, help='Only pass messages with __chat chatroom="...".')
    ap.add_argument("--sender", default=None, help='Only pass messages with __chat senderCallsign="...".')
    ap.add_argument("--cot-type", dest="cot_type", default=None, help='Only pass messages with event type="...". (e.g. b-t-f)')
    ap.add_argument("--raw", action="store_true", help="Print raw XML instead of a compact summary.")
    ap.add_argument("--out", default=None, help="Append matching lines to this file.")
    ap.add_argument("--throttle", type=float, default=0.0, help="Minimum seconds between prints (0 = no throttle).")
    args = ap.parse_args()

    rx = re.compile(args.regex) if args.regex else None

    rclpy.init(args=None)
    node = CoTMonitor(
        topic=args.topic,
        contains=args.contains if args.contains else None,
        regex=rx,
        chatroom=args.chatroom,
        sender=args.sender,
        cot_type=args.cot_type,
        raw=args.raw,
        out_path=args.out,
        throttle_sec=args.throttle,
    )

    try:
        rclpy.spin(node)
    except KeyboardInterrupt:
        pass
    finally:
        node.destroy_node()
        rclpy.shutdown()


if __name__ == "__main__":
    main()
