"""
LoRa Gateway Protocol — Luckfox side.

Implements the GATEWAY role of the protocol used by nano nodes (ESP32-S3,
code_board_v03).  Message format matches main.cpp exactly:

    [COMMAND] payload

Pairing phase:
    Gateway → broadcast  [DISCOVER]
    Node    → reply      [HELLO] <raw_node_id>
    Gateway → assign     [ASSIGN] N1

Normal operation (round-robin every request_interval_sec):
    Gateway → poll       [REQUEST] N1
    Node    → respond    [DATA] {"node_id":"N1","type":"sensor","sensors":{...}}
    Gateway → confirm    [ACK] N1
"""

import re
import time
import json
import logging

from .sx1278 import SX1278

logger = logging.getLogger(__name__)

_MSG_RE = re.compile(r'^\[([A-Z]+)\]\s*(.*)', re.DOTALL)


def _parse(raw: str):
    """Split '[CMD] payload' → (cmd, payload).  Returns (None, None) on mismatch."""
    m = _MSG_RE.match(raw.strip())
    return (m.group(1), m.group(2).strip()) if m else (None, None)


class LoRaGateway:
    """
    Gateway-side protocol handler.

    Usage:
        gw = LoRaGateway(lora)
        gw.on_data = my_callback          # optional: called on every DATA packet
        gw.discover(duration_sec=60)      # pairing phase
        while True:
            gw.poll_all()
            time.sleep(gw.request_interval)
    """

    def __init__(self,
                 lora: SX1278,
                 discover_interval_sec: float = 5.0,
                 request_interval_sec:  float = 30.0,
                 ack_timeout_sec:       float = 5.0,
                 max_retries:           int   = 3):

        self.lora              = lora
        self.discover_interval = discover_interval_sec
        self.request_interval  = request_interval_sec
        self.ack_timeout       = ack_timeout_sec
        self.max_retries       = max_retries

        # raw_id (string sent in HELLO) → node info dict
        self.nodes: dict = {}
        self._next_id    = 1

        # Optional callback: on_data(node_id: str, data: dict)
        self.on_data = None

    # -----------------------------------------------------------------------
    # Internal helpers
    # -----------------------------------------------------------------------

    def _send(self, cmd: str, payload: str = ""):
        msg = f"[{cmd}] {payload}" if payload else f"[{cmd}]"
        self.lora.send(msg)

    def _recv(self, timeout_ms: int = 500):
        """Returns (cmd, payload, rssi, snr) or (None, None, None, None)."""
        pkt = self.lora.receive(timeout_ms)
        if pkt is None:
            return None, None, None, None
        cmd, payload = _parse(pkt["data"])
        return cmd, payload, pkt["rssi"], pkt["snr"]

    # -----------------------------------------------------------------------
    # Pairing phase
    # -----------------------------------------------------------------------

    def discover(self, duration_sec: float = 60.0) -> list:
        """
        Broadcast [DISCOVER] every discover_interval seconds.
        Collect [HELLO] replies and assign sequential IDs (N1, N2, …).
        Already-known nodes are silently re-confirmed (no new ASSIGN sent).

        Returns list of newly assigned node IDs for this call.
        """
        logger.info(f"[Discover] Starting  duration={duration_sec}s")
        newly_assigned = []
        deadline       = time.time() + duration_sec

        while time.time() < deadline:
            self._send("DISCOVER")
            logger.debug("[Discover] Sent [DISCOVER]")

            listen_end = min(time.time() + self.discover_interval, deadline)
            while time.time() < listen_end:
                cmd, payload, rssi, snr = self._recv(timeout_ms=500)

                if cmd != "HELLO" or not payload:
                    continue

                raw_id = payload.strip()

                if raw_id in self.nodes:
                    # node already known — re-send ASSIGN so it locks in
                    node_id = self.nodes[raw_id]["id"]
                    self._send("ASSIGN", node_id)
                    logger.debug(f"[Discover] Re-confirmed {raw_id} → {node_id}")
                    continue

                node_id = f"N{self._next_id}"
                self._next_id += 1
                self.nodes[raw_id] = {
                    "id":        node_id,
                    "raw_id":    raw_id,
                    "last_seen": time.time(),
                    "rssi":      rssi,
                    "snr":       snr,
                }
                self._send("ASSIGN", node_id)
                newly_assigned.append(node_id)
                logger.info(
                    f"[Discover] New node '{raw_id}' → {node_id}  "
                    f"RSSI={rssi}  SNR={snr:.1f}"
                )

        logger.info(
            f"[Discover] Done  total_nodes={len(self.nodes)}  "
            f"new_this_call={newly_assigned}"
        )
        return newly_assigned

    # -----------------------------------------------------------------------
    # Normal operation
    # -----------------------------------------------------------------------

    def request_data(self, raw_id: str) -> dict | None:
        """
        Poll one node: [REQUEST] → wait for [DATA] → send [ACK].
        Retries up to max_retries times.

        Returns parsed data dict, or None on total failure.
        """
        node = self.nodes.get(raw_id)
        if node is None:
            logger.warning(f"[Poll] Unknown raw_id={raw_id!r}")
            return None

        node_id = node["id"]

        for attempt in range(1, self.max_retries + 1):
            self._send("REQUEST", node_id)
            logger.debug(
                f"[Poll] [{node_id}] REQUEST  attempt {attempt}/{self.max_retries}"
            )

            cmd, payload, rssi, snr = self._recv(
                timeout_ms=int(self.ack_timeout * 1000)
            )

            if cmd == "DATA" and payload:
                try:
                    data = json.loads(payload)
                except json.JSONDecodeError:
                    data = {"raw": payload}

                self._send("ACK", node_id)
                node.update({"last_seen": time.time(), "rssi": rssi, "snr": snr})

                logger.info(
                    f"[Poll] [{node_id}] DATA received  "
                    f"RSSI={rssi}  SNR={snr:.1f}  payload={data}"
                )

                if self.on_data:
                    try:
                        self.on_data(node_id, data)
                    except Exception as e:
                        logger.error(f"[Poll] on_data callback error: {e}")

                return data

            logger.debug(f"[Poll] [{node_id}] No DATA (got cmd={cmd!r})  retrying...")
            time.sleep(0.3)

        logger.warning(
            f"[Poll] [{node_id}] No response after {self.max_retries} retries"
        )
        return None

    def poll_all(self):
        """Poll every known node once, with a short gap between nodes."""
        for raw_id in list(self.nodes):
            self.request_data(raw_id)
            time.sleep(0.5)

    # -----------------------------------------------------------------------
    # Status helpers
    # -----------------------------------------------------------------------

    def is_online(self, raw_id: str, timeout_sec: float = 120.0) -> bool:
        node = self.nodes.get(raw_id)
        return node is not None and (time.time() - node.get("last_seen", 0)) < timeout_sec

    def get_status(self) -> dict:
        """
        Returns dict keyed by assigned node ID, e.g.:
          {"N1": {"online": True, "last_seen": 1718000000, "rssi": -72, "snr": 8.5}}
        """
        return {
            info["id"]: {
                "online":    self.is_online(raw_id),
                "last_seen": info.get("last_seen"),
                "rssi":      info.get("rssi"),
                "snr":       info.get("snr"),
            }
            for raw_id, info in self.nodes.items()
        }
