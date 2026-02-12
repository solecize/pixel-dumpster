# pixel-dumpster

## Your infinite bin of pixel-punk trash

A network-addressable display app for ESP32 devices connected to LED matrix displays.

This repository is treated as a living specification: you define project needs and goals, and the firmware is implemented to match those requirements.

Naming: this project uses kebab-style naming for files and folders (see `documentation/kebab-style-naming.md`).

### Features

- **ESP32 Support**: Optimized for ESP32 Matrix Portal and similar devices
- **LED Matrix Driver**: HUB75 and PxMatrix support with configurable resolution
- **Setup Wizard**: First-run configuration with USB keyboard input
- **Artifact Storage**: Local pixel art library with organized directory structure
- **Push Notifications**: UDP doorbell and HTTP API for remote updates
- **Web Interface**: RESTful API for asset management and control
- **Robust Design**: Deterministic fallback and error handling

### Hardware Requirements

- ESP32 development board (Matrix Portal recommended)
- HUB75 LED matrix panel (224x64 default, configurable)
- USB keyboard for setup (optional after configuration)
- Power supply for LED matrix

### Software Requirements

- ESP-IDF (native toolchain)
- ESP32 toolchain + `idf.py`
- USB drivers for flashing (platform dependent)

### Quick Start (ESP-IDF)

1. **Install ESP-IDF** using Espressif's installer
2. **Set up your environment** (`. $IDF_PATH/export.sh`)
3. **Build** the firmware
   ```bash
   idf.py build
   ```
4. **Flash** the firmware
   ```bash
   idf.py flash
   ```
5. **Monitor** logs
   ```bash
   idf.py monitor
   ```
6. **Connect USB keyboard** for first-time setup
7. **Follow the wizard** to configure:
   - Matrix resolution (e.g., 224x64)
   - Matrix orientation (0/90/180/270 degrees)
   - WiFi credentials
   - Device name

### Configuration

The device stores configuration in `/pd/config.json`:

```json
{
  "matrix_width": 224,
  "matrix_height": 64,
  "orientation_deg": 0,
  "wifi_ssid": "your-network",
  "wifi_password": "your-password",
  "device_name": "pixel-dumpster",
  "setup_complete": true,
  "config_version": 1
}
```

### Artifact Storage

Artifacts are stored in the `/pd/` directory:

```
/pd/
├── now.json          # Current display state
├── default.png       # Default/fallback image
├── system/           # System-specific art
├── game/             # Game-specific art
│   └── mame/         # Per-system subdirs
└── assets/           # Custom assets
```

### Display State

The current display state is maintained in `/pd/now.json`:

```json
{
  "mode": "idle|system|game|custom",
  "system": "mame",
  "game": "pacman",
  "asset": "assets/custom.png",
  "updated_at": 1768109057
}
```

### API Endpoints

#### POST `/reload`
Triggers reload of `now.json`
- Response: 204 No Content

#### GET `/state`
Returns current display state
- Response: JSON with current state

#### POST `/upload`
Upload PNG asset to specified path
- Form data: file upload
- Response: 200 OK

#### GET `/list`
Returns asset inventory
- Response: JSON array of available assets

#### GET `/status`
Returns system status and statistics
- Response: JSON with system information

### Push Notifications

#### UDP Doorbell (Primary)
- Port: 9876
- Any packet received triggers reload of `now.json`

#### Polling Backstop
- Interval: ~1 second
- Checks for changes in `now.json`

### Integration Examples

#### Python Client
```python
import socket
import json

# Send UDP notification
sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sock.sendto(b"refresh", ("pixel-dumpster.local", 9876))

# Update display state
import requests
state = {
    "mode": "game",
    "system": "mame", 
    "game": "pacman",
    "updated_at": int(time.time())
}
requests.post("http://pixel-dumpster.local:8088/upload", files={"file": open("pacman.png", "rb")})
```

#### Node.js Client
```javascript
const dgram = require('dgram');
const client = dgram.createSocket('udp4');

// Send UDP notification
client.send('refresh', 9876, 'pixel-dumpster.local', (err) => {
    client.close();
});
```

### Development

#### Building
```bash
idf.py build
```

#### Flashing
```bash
idf.py flash
```

#### Monitoring
```bash
idf.py monitor
```

#### Configuration
```bash
idf.py menuconfig
```

### Troubleshooting

#### Setup Issues
- Ensure USB keyboard is connected before power on
- Check serial monitor for setup wizard prompts
- Verify matrix panel connections

#### WiFi Issues
- Check SSID and password in configuration
- Verify network availability
- Monitor serial output for connection status

#### Display Issues
- Verify matrix resolution matches physical panel
- Check orientation settings
- Ensure adequate power supply

### Architecture

The system follows these design principles:

- **Artifacts are truth**: Display state derived from stored files
- **Push is advisory**: Notifications suggest changes, files confirm
- **Controller-agnostic**: Works with any upstream system
- **Frontend-agnostic**: No specific UI requirements
- **Deterministic fallback**: Always shows something meaningful
- **Minimal API surface**: Simple, focused endpoints

### License

This project is open source. See LICENSE file for details.

### Contributing

1. Fork the repository
2. Create a feature branch
3. Make your changes
4. Test thoroughly
5. Submit a pull request

### Support

For issues and questions:
- Check the troubleshooting section
- Review serial monitor output
- Open an issue on the project repository
