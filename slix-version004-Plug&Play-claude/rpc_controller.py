import os
import time
import threading
import logging

class RPCHandler:
    
    def _is_param_true(self, params):
        if params is True: return True
        if isinstance(params, dict) and params.get("param") is True: return True
        return False

    def handle_reset_remote(self, method, params):
        if not self._is_param_true(params):
            return {"success": False, "message": "params must be true"}
            
        logging.info("[RPC] Executing: reset_remote via nohup")
        os.system("nohup sh /etc/init.d/S99sshtunnel restart > /dev/null 2>&1 &")
        
        return {
            "success": True, 
            "message": "SSH tunnel restart initiated. Please wait 15-20 seconds.", 
            "timestamp": int(time.time() * 1000)
        }

    def handle_reboot(self, method, params):
        if not self._is_param_true(params):
            return {"success": False, "message": "params must be true"}
            
        logging.info("[RPC] Executing: reboot via nohup")
        
        def delayed_reboot():
            time.sleep(2)
            os.system("nohup reboot > /dev/null 2>&1 &")
            
        threading.Thread(target=delayed_reboot, daemon=True).start()
        
        return {
            "success": True, 
            "message": "Reboot command sent", 
            "timestamp": int(time.time() * 1000)
        }

    def handle_restart_process(self, method, params):
        if not self._is_param_true(params):
            return {"success": False, "message": "params must be true"}
            
        logging.info("[RPC] Executing: restart_process via nohup")
        
        def delayed_restart():
            time.sleep(2)
            os.system("nohup sh /etc/init.d/S99main_service restart > /dev/null 2>&1 &")
            
        threading.Thread(target=delayed_restart, daemon=True).start()
        
        return {
            "success": True, 
            "message": "Main service will restart in 2 seconds", 
            "timestamp": int(time.time() * 1000)
        }