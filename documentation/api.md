# API Reference

This document provides a comprehensive reference for the pixel-dumpster HTTP API and UDP notification system.

## Overview

The pixel-dumpster API provides a simple RESTful interface for managing LED matrix display content and receiving notifications for state changes.

### Base URL

```
http://pixel-dumpster.local:8088
```

### UDP Port

```
9876
```

## HTTP API Endpoints

### POST /reload

Triggers a reload of the `now.json` file to update the display state.

**Request:**
```http
POST /reload
Content-Type: application/json
```

**Response:**
```http
HTTP/1.1 204 No Content
```

**Example:**
```bash
curl -X POST http://pixel-dumpster.local:8088/reload
```

### GET /state

Returns the current display state.

**Request:**
```http
GET /state
```

**Response:**
```http
HTTP/1.1 200 OK
Content-Type: application/json

{
  "mode": "idle|system|game|custom",
  "system": "mame",
  "game": "pacman",
  "asset": "assets/custom.png",
  "updated_at": 1768109057
}
```

**Example:**
```bash
curl http://pixel-dumpster.local:8088/state
```

### POST /upload

Uploads a PNG asset to the device storage.

**Request:**
```http
POST /upload
Content-Type: multipart/form-data

file: <binary PNG data>
```

**Response:**
```http
HTTP/1.1 200 OK
Content-Type: text/plain

File uploaded
```

**Example:**
```bash
curl -X POST -F "file=@logo.png" http://pixel-dumpster.local:8088/upload
```

### GET /list

Returns a list of all available assets on the device.

**Request:**
```http
GET /list
```

**Response:**
```http
HTTP/1.1 200 OK
Content-Type: application/json

{
  "files": [
    {
      "path": "/pd/default.png",
      "name": "default.png",
      "type": 3,
      "size": 1024,
      "created_at": 1768109057,
      "updated_at": 1768109057
    },
    {
      "path": "/pd/system/mame.png",
      "name": "mame.png",
      "type": 0,
      "size": 2048,
      "created_at": 1768109058,
      "updated_at": 1768109058
    }
  ]
}
```

**Artifact Types:**
- `0`: SYSTEM
- `1`: GAME
- `2`: CUSTOM
- `3`: DEFAULT

**Example:**
```bash
curl http://pixel-dumpster.local:8088/list
```

### GET /status

Returns system status and statistics.

**Request:**
```http
GET /status
```

**Response:**
```http
HTTP/1.1 200 OK
Content-Type: application/json

{
  "uptime": 12345678,
  "notification_count": 42,
  "last_notification": 12345670,
  "udp_enabled": true,
  "http_enabled": true,
  "polling_enabled": true,
  "wifi_connected": true,
  "wifi_ssid": "network-name",
  "local_ip": "192.168.1.100",
  "recent_events": [
    {
      "type": 0,
      "source": "192.168.1.50",
      "timestamp": 12345670,
      "data": "refresh"
    }
  ]
}
```

**Notification Types:**
- `0`: UDP_DOORBELL
- `1`: HTTP_POST
- `2`: POLLING

**Example:**
```bash
curl http://pixel-dumpster.local:8088/status
```

## UDP Notification System

### Overview

The UDP notification system provides a lightweight way to trigger display updates without HTTP overhead.

### Port

```
9876
```

### Protocol

Any UDP packet received on port 9876 triggers a reload of the `now.json` file and updates the display.

### Message Format

The message content is currently ignored - any packet triggers the same action. Future versions may support different message types.

### Examples

**Python:**
```python
import socket

sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sock.sendto(b"refresh", ("pixel-dumpster.local", 9876))
sock.close()
```

**Node.js:**
```javascript
const dgram = require('dgram');
const client = dgram.createSocket('udp4');

client.send('refresh', 9876, 'pixel-dumpster.local', (err) => {
    client.close();
});
```

**Bash:**
```bash
echo "refresh" | nc -u -w0 pixel-dumpster.local 9876
```

## File System Structure

### Artifact Storage

Artifacts are stored in the `/pd/` directory with the following structure:

```
/pd/
├── now.json          # Current display state
├── default.png       # Default/fallback image
├── system/           # System-specific art
│   ├── mame.png
│   ├── nes.png
│   └── snes.png
├── game/             # Game-specific art
│   ├── mame/
│   │   ├── pacman.png
│   │   └── donkeykong.png
│   └── nes/
│       ├── mario.png
│       └── zelda.png
└── assets/           # Custom assets
    ├── logo.png
    └── custom.png
```

### Configuration

Device configuration is stored in `/pd/config.json`:

```json
{
  "matrix_width": 224,
  "matrix_height": 64,
  "orientation_deg": 0,
  "wifi_ssid": "network-name",
  "wifi_password": "password",
  "device_name": "pixel-dumpster",
  "setup_complete": true,
  "config_version": 1
}
```

### Display State

Current display state is maintained in `/pd/now.json`:

```json
{
  "mode": "idle|system|game|custom",
  "system": "mame",
  "game": "pacman",
  "asset": "assets/custom.png",
  "updated_at": 1768109057
}
```

## Display Modes

### idle

Shows the default image or device name.

**State:**
```json
{
  "mode": "idle",
  "system": "",
  "game": "",
  "asset": "",
  "updated_at": 1768109057
}
```

### system

Shows system-specific artwork.

**State:**
```json
{
  "mode": "system",
  "system": "mame",
  "game": "",
  "asset": "",
  "updated_at": 1768109057
}
```

**Display:** `/pd/system/mame.png`

### game

Shows game-specific artwork.

**State:**
```json
{
  "mode": "game",
  "system": "mame",
  "game": "pacman",
  "asset": "",
  "updated_at": 1768109057
}
```

**Display:** `/pd/game/mame/pacman.png`

### custom

Shows custom asset.

**State:**
```json
{
  "mode": "custom",
  "system": "",
  "game": "",
  "asset": "assets/custom.png",
  "updated_at": 1768109057
}
```

**Display:** `/pd/assets/custom.png`

## Error Handling

### HTTP Status Codes

- `200 OK`: Request successful
- `204 No Content`: Request successful, no content returned
- `400 Bad Request`: Invalid request parameters
- `404 Not Found`: Endpoint or resource not found
- `500 Internal Server Error`: Device error

### Error Response Format

```json
{
  "error": "Error message",
  "code": 400,
  "timestamp": 1768109057
}
```

### Common Errors

**Invalid file type:**
```json
{
  "error": "Only PNG files are supported",
  "code": 400,
  "timestamp": 1768109057
}
```

**File not found:**
```json
{
  "error": "Artifact not found: /pd/missing.png",
  "code": 404,
  "timestamp": 1768109057
}
```

**Storage full:**
```json
{
  "error": "Insufficient storage space",
  "code": 500,
  "timestamp": 1768109057
}
```

## Rate Limiting

To prevent abuse, the API implements rate limiting:

- **UDP notifications**: 10 per second
- **HTTP requests**: 60 per minute
- **File uploads**: 5 per minute

Exceeding limits results in HTTP 429 responses.

## Authentication

Currently, the API does not require authentication. Future versions may support:

- **API keys** for client identification
- **Basic authentication** for admin functions
- **TLS/SSL** for encrypted communication

## CORS Support

The API includes CORS headers for web client compatibility:

```http
Access-Control-Allow-Origin: *
Access-Control-Allow-Methods: GET, POST, OPTIONS
Access-Control-Allow-Headers: Content-Type
```

## WebSocket Support

Future versions may support WebSocket connections for real-time updates:

```javascript
const ws = new WebSocket('ws://pixel-dumpster.local:8089/ws');

ws.onmessage = function(event) {
    const data = JSON.parse(event.data);
    console.log('Display state changed:', data);
};
```

## SDK Examples

### Python SDK

```python
from pixel_dumpster import PixelDumpsterClient

client = PixelDumpsterClient("pixel-dumpster.local")

# Get current state
state = client.get_state()
print(f"Current mode: {state['mode']}")

# Upload asset
client.upload_asset("logo.png")

# Set display mode
client.set_mode("game", "mame", "pacman")

# Send notification
client.notify("refresh")
```

### Node.js SDK

```javascript
const { PixelDumpsterClient } = require('pixel-dumpster');

const client = new PixelDumpsterClient('pixel-dumpster.local');

// Get current state
const state = await client.getState();
console.log(`Current mode: ${state.mode}`);

// Upload asset
await client.uploadAsset('logo.png');

// Set display mode
await client.setMode('game', 'mame', 'pacman');

// Send notification
await client.notify('refresh');
```

## Integration Patterns

### Game Integration

```python
# Python example for game integration
import time
from pixel_dumpster import PixelDumpsterClient

client = PixelDumpsterClient()

def on_game_change(system, game):
    """Handle game change event"""
    # Upload marquee if needed
    client.upload_asset(f"marquees/{game}.png", f"game/{system}/{game}.png")
    
    # Update display state
    client.set_mode("game", system, game)
    
    # Send notification
    client.notify("game_change")

# Example usage
on_game_change("mame", "pacman")
```

### System Monitoring

```python
# Python example for system monitoring
import asyncio
from pixel_dumpster import PixelDumpsterClient

client = PixelDumpsterClient()

async def monitor_system():
    """Monitor pixel-dumpster system status"""
    while True:
        status = await client.get_status()
        print(f"Uptime: {status['uptime']}ms")
        print(f"Notifications: {status['notification_count']}")
        
        await asyncio.sleep(30)

asyncio.run(monitor_system())
```

### Asset Management

```python
# Python example for asset management
from pixel_dumpster import PixelDumpsterClient
from pathlib import Path

client = PixelDumpsterClient()

def sync_assets(local_dir, remote_type="custom"):
    """Sync local assets with device"""
    local_dir = Path(local_dir)
    
    for png_file in local_dir.glob("*.png"):
        if remote_type == "system":
            remote_path = f"system/{png_file.stem}.png"
        elif remote_type == "game":
            remote_path = f"game/{png_file.parent.name}/{png_file.stem}.png"
        else:
            remote_path = f"assets/{png_file.name}"
        
        client.upload_asset(str(png_file), remote_path)
        print(f"Uploaded: {png_file.name} -> {remote_path}")

# Example usage
sync_assets("./assets/system", "system")
sync_assets("./assets/game", "game")
sync_assets("./assets/custom", "custom")
```

## Testing

### API Testing

```bash
# Test basic connectivity
curl http://pixel-dumpster.local:8088/status

# Test state endpoint
curl http://pixel-dumpster.local:8088/state

# Test file upload
curl -X POST -F "file=@test.png" http://pixel-dumpster.local:8088/upload

# Test UDP notification
echo "test" | nc -u -w0 pixel-dumpster.local 9876
```

### Load Testing

```python
# Python load testing example
import asyncio
import aiohttp
from concurrent.futures import ThreadPoolExecutor

async def test_endpoint(session, url, method='GET', data=None):
    try:
        if method == 'POST':
            async with session.post(url, data=data) as response:
                return response.status
        else:
            async with session.get(url) as response:
                return response.status
    except Exception as e:
        print(f"Request failed: {e}")
        return 0

async def load_test(base_url, concurrent_requests=10):
    async with aiohttp.ClientSession() as session:
        tasks = []
        for i in range(concurrent_requests):
            task = test_endpoint(session, f"{base_url}/status")
            tasks.append(task)
        
        results = await asyncio.gather(*tasks)
        success_count = sum(1 for r in results if r == 200)
        print(f"Successful requests: {success_count}/{concurrent_requests}")

# Run load test
asyncio.run(load_test("http://pixel-dumpster.local:8088"))
```

## Troubleshooting

### Connection Issues

1. **Host Resolution**
   ```bash
   ping pixel-dumpster.local
   nslookup pixel-dumpster.local
   ```

2. **Port Availability**
   ```bash
   nmap -p 8088,9876 pixel-dumpster.local
   ```

3. **Network Connectivity**
   ```bash
   traceroute pixel-dumpster.local
   ```

### Debug Headers

Add debug headers to requests for additional information:

```bash
curl -H "X-Debug: true" http://pixel-dumpster.local:8088/status
```

### Log Analysis

Monitor device logs for API activity:

```bash
# Connect to device serial monitor
idf.py monitor

# Look for API-related messages
# API: GET /status - 200
# UDP: Packet received from 192.168.1.50
# Storage: Uploaded file: logo.png (1024 bytes)
```
