import paho.mqtt.client as mqtt
import json
import time
import logging
import threading

class ThingsBoardSender:
    def __init__(self, host, port, access_token, topic="v1/gateway/telemetry"):
        self.host = host
        self.port = port
        self.access_token = access_token
        self.topic = topic
        self.client = None
        self.connected = False
        self.connection_lock = threading.Lock()

        # RPC state
        self.rpc_enabled = False
        self.rpc_methods = {}         # {"method_name": callable}
        self.rpc_validators = {}      # {"method_name": {"required": [...], "types": {...}}}

        # Setup logging
        logging.basicConfig(level=logging.DEBUG)
        self.logger = logging.getLogger(__name__)

        # Initialize MQTT client
        self._init_client()
        self._reconnect_running = False

    # ------------- MQTT base -------------
    def _init_client(self):
        """Initialize MQTT client"""
        try:
            self.client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
            self.client.username_pw_set(self.access_token)
            # Set callbacks
            self.client.on_connect = self._on_connect
            self.client.on_disconnect = self._on_disconnect
            self.client.on_publish = self._on_publish
            # RPC (will be activated when start_rpc_handler is called)
            self.client.on_message = self._on_message
            self.logger.debug("MQTT client initialized")
        except Exception as e:
            self.logger.error(f"Failed to initialize MQTT client: {e}")

    def _on_connect(self, client, userdata, flags, rc, properties=None):
        """Callback for successful connection"""
        if rc == 0:
            self.connected = True
            self.logger.debug("Connected to ThingsBoard")
            # If RPC mode enabled previously, re-subscribe on reconnect
            if self.rpc_enabled:
                try:
                    client.subscribe("v1/devices/me/rpc/request/+", qos=1)
                    self.logger.debug("Re-subscribed to RPC topic after reconnect")
                except Exception as e:
                    self.logger.error(f"Failed to subscribe RPC on reconnect: {e}")
        else:
            self.connected = False
            self.logger.error(f"Failed to connect to ThingsBoard: {rc}")

    def _on_disconnect(self, client, userdata, disconnect_flags, reason_code, properties=None):
        """Callback for disconnection"""
        self.connected = False
        self.logger.debug(f"Disconnected from ThingsBoard: reason_code={reason_code}")
        if not self._reconnect_running:
            self._reconnect_running = True

            def _reconnector():
                backoff = 1
                max_backoff = 30
                while not self.connected:
                    try:
                        self.logger.debug("Trying to reconnect MQTT...")
                        self.client.reconnect()
                    except Exception as e:
                        self.logger.error(f"Reconnect failed: {e}")
                    if self.connected:
                        break
                    time.sleep(backoff)
                    backoff = backoff * 2 if backoff < max_backoff else max_backoff
                self._reconnect_running = False

            t = threading.Thread(target=_reconnector, daemon=True)
            t.start()


    def _on_publish(self, client, userdata, mid, reason_code=None, properties=None):
        """Callback for successful publish"""
        self.logger.debug(f"Message published successfully: {mid}")

    # ------------- RPC core -------------
    def start_rpc_handler(self, request_topic="v1/devices/me/rpc/request/+"):
        """
        Enable RPC handling: subscribe to RPC topic and allow incoming RPC processing.
        Call this after connect() returns True, or call before connect() (it will subscribe on connect).
        """
        self.rpc_enabled = True
        self.rpc_request_topic = request_topic
        if not self.connected:
            self.logger.debug("RPC enabled; will subscribe after connect.")
            return True
        try:
            self.client.subscribe(self.rpc_request_topic, qos=1)
            self.logger.debug("Subscribed to RPC: {self.rpc_request_topic}")
            return True
        except Exception as e:
            self.logger.error(f"Failed to subscribe RPC topic: {e}")
            return False

    def stop_rpc_handler(self):
        """Disable RPC handling (unsubscribe RPC topic)."""
        self.rpc_enabled = False
        if not self.connected:
            return True
        try:
            self.client.unsubscribe("v1/devices/me/rpc/request/+")
            self.logger.debug("Unsubscribed from RPC topic.")
            return True
        except Exception as e:
            self.logger.error(f"Failed to unsubscribe RPC topic: {e}")
            return False

    def register_rpc_method(self, method_name, handler_function, validation_rules=None):
        """
        Register RPC method handler.
        handler_function signature: def handler(method: str, params: dict) -> dict
        validation_rules example:
        {
          "required": ["param"],
          "types": {"param": "bool"}  # supports: "bool", "int", "float", "str"
        }
        """
        self.rpc_methods[method_name] = handler_function
        if validation_rules:
            self.rpc_validators[method_name] = validation_rules
        self.logger.debug(f"Registered RPC method: {method_name}")

    def _on_message(self, client, userdata, message):
        """Unified message callback (used for RPC)."""
        if not self.rpc_enabled:
            return
        try:
            topic = message.topic
            payload = message.payload.decode("utf-8")
            self.logger.debug(f"[RPC] Incoming topic={topic} payload={payload}")
            if topic.startswith("v1/devices/me/rpc/request/"):
                request_id = topic.split("/")[-1]
                self._handle_device_rpc_request(payload, request_id)
        except Exception as e:
            self.logger.error(f"[RPC] Error processing message: {e}")

    def _handle_device_rpc_request(self, payload: str, request_id: str):
        """Process RPC request: validate, dispatch to handler, and send response."""
        try:
            rpc_data = json.loads(payload)
        except json.JSONDecodeError as e:
            self.logger.error(f"[RPC] JSON decode error: {e}")
            self._send_rpc_response(request_id, {"error": f"JSON decode error: {e}"})
            return

        method = rpc_data.get("method")
        params = rpc_data.get("params", {})
        self.logger.debug(f"[RPC] method={method} params={params} req_id={request_id}")

        if method not in self.rpc_methods:
            self._send_rpc_response(request_id, {
                "error": f"Unsupported method: {method}",
                "available_methods": list(self.rpc_methods.keys())
            })
            return

        # validate params if rules exist
        validation = self._validate_rpc_params(method, params)
        if not validation["valid"]:
            self._send_rpc_response(request_id, {
                "error": f"Invalid parameters: {validation['error']}",
                "required_params": validation.get("required_params", [])
            })
            return

        # dispatch
        try:
            handler = self.rpc_methods[method]
            response = handler(method, params)
        except Exception as e:
            response = {"error": f"Method execution failed: {str(e)}"}

        self._send_rpc_response(request_id, response)

    def _validate_rpc_params(self, method, params):
        """Simple validator like in rpc_receive."""
        rules = self.rpc_validators.get(method)
        if not rules:
            return {"valid": True}

        # required
        required = rules.get("required", [])
        for r in required:
            if r not in params:
                return {"valid": False, "error": f"Missing required parameter: {r}", "required_params": required}

        # types
        types = rules.get("types", {})
        for key, t in types.items():
            if key in params:
                v = params[key]
                if t == "bool" and not isinstance(v, bool):
                    return {"valid": False, "error": f"Parameter '{key}' must be boolean"}
                if t == "int" and not isinstance(v, int):
                    return {"valid": False, "error": f"Parameter '{key}' must be int"}
                if t == "float" and not (isinstance(v, float) or isinstance(v, int)):
                    return {"valid": False, "error": f"Parameter '{key}' must be float"}
                if t == "str" and not isinstance(v, str):
                    return {"valid": False, "error": f"Parameter '{key}' must be str"}

        return {"valid": True}

    def _send_rpc_response(self, request_id: str, response: dict):
        """Publish RPC response to ThingsBoard."""
        if not self.connected:
            self.logger.error("[RPC] Cannot send response: not connected")
            return False
        try:
            topic = f"v1/devices/me/rpc/response/{request_id}"
            msg = json.dumps(response, ensure_ascii=False)
            res = self.client.publish(topic, msg, qos=1)
            if res.rc == mqtt.MQTT_ERR_SUCCESS:
                self.logger.debug(f"[RPC] Response sent: {topic} -> {msg}")
                return True
            self.logger.error(f"[RPC] Failed to publish response: {res.rc}")
            return False
        except Exception as e:
            self.logger.error(f"[RPC] Error sending response: {e}")
            return False

    # ------------- Connect/Telemetry -------------
    def connect(self):
        """Connect to ThingsBoard"""
        try:
            with self.connection_lock:
                if not self.connected:
                    self.logger.debug(f"Connecting to {self.host}:{self.port}")
                    self.client.connect(self.host, self.port, 60)
                    self.client.loop_start()
                    # Wait for connection with timeout
                    timeout = 10
                    start_time = time.time()
                    while not self.connected and (time.time() - start_time) < timeout:
                        time.sleep(0.1)
                    if self.connected:
                        self.logger.debug("Successfully connected to ThingsBoard")
                        return True
                    else:
                        self.logger.error("Connection timeout")
                        return False
                else:
                    return True
        except Exception as e:
            self.logger.error(f"Connection error: {e}")
            return False

    def send_telemetry(self, telemetry_data: dict, topic: str = None):
        """
        Send telemetry data to ThingsBoard
        telemetry_data: dict payload
        topic: optional target topic (if None, uses self.topic or v1/gateway/telemetry)
        """
        try:
            # Auto-reconnect if needed
            if not self.connected:
                self.logger.debug("Not connected, attempting to reconnect...")
                if not self.connect():
                    self.logger.error("Failed to reconnect")
                    return False

            message = json.dumps(telemetry_data)
            
            publish_topic = topic if topic else getattr(self, 'topic', "v1/gateway/telemetry")
            
            result = self.client.publish(publish_topic, message, qos=1)

            if result.rc == mqtt.MQTT_ERR_SUCCESS:
                self.logger.debug(f"Telemetry sent successfully to {publish_topic}")
                self.logger.debug(f"Data: {telemetry_data}")
                return True
            else:
                self.logger.error(f"Failed to send telemetry: {result.rc}")
                return False
        except Exception as e:
            self.logger.error(f"Error sending telemetry: {e}")
            return False

    def close(self):
        """Close connection"""
        try:
            if self.client:
                self.client.loop_stop()
                self.client.disconnect()
            self.connected = False
            self.logger.debug("ThingsBoard connection closed")
        except Exception as e:
            self.logger.error(f"Error closing connection: {e}")


# ---------------- Example usage ----------------
if __name__ == "__main__":
    # Config
    THINGSBOARD_HOST = "thingsboard.weaverbase.com"
    THINGSBOARD_PORT = 1883
    ACCESS_TOKEN = "obqbBrUX2SfxylzlHF0m"

    tb = ThingsBoardSender(THINGSBOARD_HOST, THINGSBOARD_PORT, ACCESS_TOKEN)


    def rpc_reset_remote(method, params):
        ok = params.get("param") is True
        if not ok:
            return {"success": False, "message": "param must be true"}
        return {"success": True, "message": "reset_remote initiated", "timestamp": int(time.time()*1000)}

    def rpc_reboot(method, params):
        ok = params.get("param") is True
        if not ok:
            return {"success": False, "message": "param must be true"}
        return {"success": True, "message": "reboot scheduled", "timestamp": int(time.time()*1000)}

    tb.register_rpc_method("reset_remote", rpc_reset_remote, {
        "required": ["param"], "types": {"param": "bool"}
    })
    tb.register_rpc_method("reboot", rpc_reboot, {
        "required": ["param"], "types": {"param": "bool"}
    })

    if tb.connect():
        tb.start_rpc_handler()
        sample = {
            "DeviceA": [{
                "ts": int(time.time()*1000),
                "values": {"data_value": 25.3, "status": "ok"}
            }]
        }
        tb.send_telemetry(sample)

        try:
            while True:
                time.sleep(1)
        except KeyboardInterrupt:
            pass
        finally:
            tb.close()
    else:
        print("Failed to connect")
