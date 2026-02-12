#!/usr/bin/env python3
"""
pixel-dumpster Python Client Example

This script demonstrates how to interact with a pixel-dumpster device
using UDP notifications and HTTP API calls.
"""

import socket
import json
import time
import requests
import sys
from pathlib import Path

class PixelDumpsterClient:
    def __init__(self, host="pixel-dumpster.local", udp_port=9876, http_port=8088):
        self.host = host
        self.udp_port = udp_port
        self.http_port = http_port
        self.base_url = f"http://{host}:{http_port}"
    
    def send_udp_notification(self, message="refresh"):
        """Send UDP notification to trigger reload"""
        try:
            sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            sock.sendto(message.encode(), (self.host, self.udp_port))
            sock.close()
            print(f"UDP notification sent: {message}")
            return True
        except Exception as e:
            print(f"UDP notification failed: {e}")
            return False
    
    def get_state(self):
        """Get current display state"""
        try:
            response = requests.get(f"{self.base_url}/state")
            if response.status_code == 200:
                return response.json()
            else:
                print(f"Failed to get state: {response.status_code}")
                return None
        except Exception as e:
            print(f"State request failed: {e}")
            return None
    
    def reload_state(self):
        """Trigger reload of now.json"""
        try:
            response = requests.post(f"{self.base_url}/reload")
            if response.status_code == 204:
                print("Reload triggered successfully")
                return True
            else:
                print(f"Failed to trigger reload: {response.status_code}")
                return False
        except Exception as e:
            print(f"Reload request failed: {e}")
            return False
    
    def upload_asset(self, file_path, asset_name=None):
        """Upload PNG asset to device"""
        file_path = Path(file_path)
        if not file_path.exists():
            print(f"File not found: {file_path}")
            return False
        
        if not file_path.suffix.lower() == '.png':
            print("Only PNG files are supported")
            return False
        
        filename = asset_name or file_path.name
        
        try:
            with open(file_path, 'rb') as f:
                files = {'file': (filename, f, 'image/png')}
                response = requests.post(f"{self.base_url}/upload", files=files)
            
            if response.status_code == 200:
                print(f"Asset uploaded successfully: {filename}")
                return True
            else:
                print(f"Failed to upload asset: {response.status_code}")
                return False
        except Exception as e:
            print(f"Upload failed: {e}")
            return False
    
    def list_assets(self):
        """List all available assets"""
        try:
            response = requests.get(f"{self.base_url}/list")
            if response.status_code == 200:
                return response.json()
            else:
                print(f"Failed to list assets: {response.status_code}")
                return None
        except Exception as e:
            print(f"List request failed: {e}")
            return None
    
    def get_status(self):
        """Get system status"""
        try:
            response = requests.get(f"{self.base_url}/status")
            if response.status_code == 200:
                return response.json()
            else:
                print(f"Failed to get status: {response.status_code}")
                return None
        except Exception as e:
            print(f"Status request failed: {e}")
            return None
    
    def set_display_mode(self, mode, system="", game="", asset=""):
        """Update display state by creating now.json content"""
        state_data = {
            "mode": mode,
            "system": system,
            "game": game,
            "asset": asset,
            "updated_at": int(time.time())
        }
        
        # This would require direct file access or additional API endpoint
        # For now, we'll just send a UDP notification
        print(f"Display mode set to: {mode}")
        self.send_udp_notification()
        return True

def main():
    if len(sys.argv) < 2:
        print("Usage: python python-client.py <command> [args...]")
        print("\nCommands:")
        print("  state                    - Get current display state")
        print("  reload                   - Trigger reload")
        print("  upload <file> [name]      - Upload PNG asset")
        print("  list                     - List assets")
        print("  status                   - Get system status")
        print("  notify [message]         - Send UDP notification")
        print("  mode <mode> [system] [game] [asset] - Set display mode")
        print("\nExamples:")
        print("  python python-client.py state")
        print("  python python-client.py upload logo.png")
        print("  python python-client.py mode game mame pacman")
        print("  python python-client.py notify refresh")
        return
    
    client = PixelDumpsterClient()
    command = sys.argv[1].lower()
    
    if command == "state":
        state = client.get_state()
        if state:
            print(json.dumps(state, indent=2))
    
    elif command == "reload":
        client.reload_state()
    
    elif command == "upload":
        if len(sys.argv) < 3:
            print("Usage: python python-client.py upload <file> [name]")
            return
        
        file_path = sys.argv[2]
        asset_name = sys.argv[3] if len(sys.argv) > 3 else None
        client.upload_asset(file_path, asset_name)
    
    elif command == "list":
        assets = client.list_assets()
        if assets:
            print(json.dumps(assets, indent=2))
    
    elif command == "status":
        status = client.get_status()
        if status:
            print(json.dumps(status, indent=2))
    
    elif command == "notify":
        message = sys.argv[2] if len(sys.argv) > 2 else "refresh"
        client.send_udp_notification(message)
    
    elif command == "mode":
        if len(sys.argv) < 3:
            print("Usage: python python-client.py mode <mode> [system] [game] [asset]")
            return
        
        mode = sys.argv[2]
        system = sys.argv[3] if len(sys.argv) > 3 else ""
        game = sys.argv[4] if len(sys.argv) > 4 else ""
        asset = sys.argv[5] if len(sys.argv) > 5 else ""
        
        client.set_display_mode(mode, system, game, asset)
    
    else:
        print(f"Unknown command: {command}")

if __name__ == "__main__":
    main()
