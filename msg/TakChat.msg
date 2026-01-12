# TakChat.msg
# Message for TAK-based communication between robots and ground stations

string origin           # Callsign/UID of sender (e.g., "warthog1", "TRILL")
string destination      # Callsign/UID of recipient (e.g., "TRILL", "ALL", "warthog1")
string message          # The actual message content
string timestamp        # ISO 8601 / Zulu time format (e.g., "2026-01-06T15:16:45.009655Z")
string uid              # Optional: if non-empty, tak_chat_node will use this instead of generating random
