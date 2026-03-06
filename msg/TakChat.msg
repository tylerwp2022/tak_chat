# TakChat.msg
# =============================================================================
# Message for TAK-based communication between robots and ground stations.
#
# CHAT TYPES (chat_type field):
# -----------------------------
#   "unicast"        — Direct message to one callsign (destination required)
#   "group"          — Named group (e.g. "TEAM13") with explicit member list
#   "team_color"     — TAK team color broadcast (e.g. "Cyan", "Red")
#   "role"           — TAK role broadcast (e.g. "HQ", "Medic", "Team Leader")
#   "all_chat_rooms" — Broadcast to All Chat Rooms
#   "all_groups"     — Broadcast to all Groups
#   "all_teams"      — Broadcast to all Teams
#
# FIELD USAGE BY CHAT TYPE:
# -------------------------
#   Field          unicast  group  team_color  role  all_*
#   origin         YES      YES    YES         YES   YES
#   destination    YES      no     no          no    no
#   message        YES      YES    YES         YES   YES
#   timestamp      YES      YES    YES         YES   YES
#   uid            opt      opt    opt         opt   opt
#   chat_type      YES      YES    YES         YES   YES
#   chatroom       no       YES    YES         YES   no
#   chatroom_id    no       YES    YES         YES   no
#   member_uids    no       YES    no          no    no
#   member_names   no       YES    no          no    no
#
# BACKWARD COMPATIBILITY:
# -----------------------
#   chat_type defaults to empty string. TakChatNode treats empty chat_type
#   as "unicast" for backward compatibility with existing trees that use
#   BroadcastToTAK / SendTAKMessage / old ConstructTAKChatMessage.
# =============================================================================

# --- Core fields (all types) ---
string origin           # Callsign/UID of sender (e.g. "warthog1")
string destination      # Unicast only: recipient callsign (e.g. "TRILL")
                        # Legacy broadcast: "ALL" triggers fan-out in TakChatNode
string message          # The actual message content
string timestamp        # ISO 8601 / Zulu (e.g. "2026-01-06T15:16:45.009655Z")
string uid              # Optional UID override (empty = auto-generate)

# --- Chat type routing ---
string chat_type        # See CHAT TYPES above. Empty = "unicast" (backward compat)

# --- Group/team/role fields ---
string chatroom         # Group name, color, role, etc. (e.g. "TEAM13", "Cyan", "HQ")
string chatroom_id      # __chat id attr. Usually = chatroom; named group uses UUID

# --- Named group membership (group chat only) ---
string[] member_uids    # UIDs of all group members (uid0, uid1, ... in chatgrp)
string[] member_names   # Display names matching member_uids (for hierarchy block)
