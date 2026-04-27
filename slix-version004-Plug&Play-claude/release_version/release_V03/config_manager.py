import json
import os

class ConfigManager:
    def __init__(self, config_file="config.json"):
        self.config_file = config_file
        self.config_data = {}
        self.load_config()

    def load_config(self):
        if not os.path.exists(self.config_file):
            raise FileNotFoundError(f"Error: Could not find configuration file at {self.config_file}")
            
        try:
            with open(self.config_file, 'r', encoding='utf-8') as file:
                self.config_data = json.load(file)
                print(f"Configuration loaded successfully from {self.config_file}")
        except json.JSONDecodeError as e:
            raise ValueError(f"Error parsing JSON in {self.config_file}: {e}")

    def get_system_config(self):
        return self.config_data.get("system", {})

    def get_thingsboard_config(self):
        return self.config_data.get("thingsboard", {})

    def get_active_sensors(self):
        sensors = self.config_data.get("sensors", {})
        active_sensors = {}
        
        for port, config in sensors.items():
            if config.get("enabled", False):
                active_sensors[int(port)] = config
                
        return active_sensors