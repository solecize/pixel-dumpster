# pixel-dumpster Client Examples

This directory contains example client implementations for interacting with pixel-dumpster devices.

## Python Client

The `python-client.py` script provides a complete command-line interface for pixel-dumpster devices.

### Installation

```bash
# Install required dependencies
pip install requests
```

### Usage

```bash
# Get current display state
python python-client.py state

# Upload an asset
python python-client.py upload logo.png my-logo.png

# List all assets
python python-client.py list

# Get system status
python python-client.py status

# Send UDP notification
python python-client.py notify refresh

# Set display mode
python python-client.py mode game mame pacman
```

### Python Notes

The Python example is CLI-first to keep filenames kebab-case. For SDK-style usage, copy the client class into your own module (with your preferred naming) and import it there.

## Node.js Client

The `node-client.js` script provides the same functionality for Node.js environments.

### Installation

```bash
# Install dependencies
npm install
```

### Usage

```bash
# Get current display state
node node-client.js state

# Upload an asset
node node-client.js upload logo.png my-logo.png

# List all assets
node node-client.js list

# Get system status
node node-client.js status

# Send UDP notification
node node-client.js notify refresh

# Set display mode
node node-client.js mode game mame pacman
```

### Node.js Notes

The Node.js example is CLI-first to keep filenames kebab-case. For SDK-style usage, copy the client class into your own module and import it there.

## Integration Examples

For integrations, use the HTTP API directly (see `../documentation/api.md`) or wrap the CLI scripts with your automation tooling.

## Troubleshooting

### Connection Issues

1. **Host Resolution**: Ensure `pixel-dumpster.local` resolves to the correct IP address
2. **Network Connectivity**: Check that the device is on the same network
3. **Firewall**: Verify UDP port 9876 and HTTP port 8088 are not blocked

### UDP Notifications

If UDP notifications don't work:

1. Check network connectivity
2. Verify the device is listening on port 9876
3. Try using the HTTP `/reload` endpoint as an alternative

### File Uploads

If file uploads fail:

1. Ensure files are in PNG format
2. Check file size (ESP32 has limited storage)
3. Verify the device has sufficient free space

### API Errors

Common HTTP status codes:

- `200 OK`: Request successful
- `204 No Content`: Request successful, no content returned
- `400 Bad Request`: Invalid request parameters
- `404 Not Found`: Endpoint or resource not found
- `500 Internal Server Error`: Device error

## Advanced Usage

### Custom Host Configuration

Use custom host and ports by editing the scripts or wrapping them with environment variables in your automation tooling.

### Error Handling

For robust error handling, wrap the CLI scripts in your automation tooling (bash, Python subprocess, Make) and implement retries there.

### Batch Operations

Batch operations can be handled by iterating over files and invoking the CLI scripts, or by using the HTTP endpoints directly (see `../documentation/api.md`).
