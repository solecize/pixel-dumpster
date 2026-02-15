# Pixel Dumpster — Content System Spec

## Overview

The content system manages visual assets displayed on the LED matrix. Content is
authored on a host machine, synced to the device over WiFi, and played back on
demand via a CLI tool or future web UI.

---

## Folder Structure

Both the host (`content/`) and device (`/content/`) share the same layout:

```
content/
├── images/
│   ├── pacman.png              # single static image
│   ├── fire/                   # PNG sequence (animated)
│   │   ├── meta.json           # sequence metadata
│   │   ├── 0001.png
│   │   ├── 0002.png
│   │   └── ...
│   └── rainbow/
│       ├── meta.json
│       ├── 0001.png
│       └── ...
├── text/
│   ├── welcome.json            # text content definition
│   └── scores.json
├── fonts/
│   ├── tiny5.bdf               # bitmap fonts (BDF format)
│   └── pixel8.bdf
└── glyphs/
    ├── weather/
    │   ├── meta.json           # glyph mapping definition
    │   ├── sun.png
    │   ├── cloud.png
    │   └── rain.png
    └── emoji/
        ├── meta.json
        ├── smile.png
        └── heart.png
```

---

## Image Formats

### Static Images
- **Format**: PNG (24-bit RGB or 8-bit indexed)
- **Size**: Should match or be smaller than the matrix resolution
- **Scaling**: Images smaller than the matrix are centered; larger are cropped

### PNG Sequences (Animated)
Each sequence lives in its own subdirectory under `images/`. Frames are numbered
PNG files. A `meta.json` file defines playback parameters.

**Frame naming**: `NNNN.png` — zero-padded 4-digit frame number, starting at 0001.

**meta.json**:
```json
{
  "name": "Pac-Man",
  "fps": 12,
  "loop": true,
  "hold_last": false,
  "width": 64,
  "height": 64
}
```

| Field       | Type   | Required | Description                                    |
|-------------|--------|----------|------------------------------------------------|
| name        | string | yes      | Display name for the sequence                  |
| fps         | number | yes      | Frames per second (1–60)                       |
| loop        | bool   | no       | Loop playback (default: true)                  |
| hold_last   | bool   | no       | Hold last frame when not looping (default: false) |
| width       | number | no       | Expected frame width (informational)           |
| height      | number | no       | Expected frame height (informational)          |

---

## Text Content

Text content files define scrolling or static text to display using bitmap fonts.

**text/welcome.json**:
```json
{
  "name": "Welcome",
  "font": "tiny5",
  "text": "Hello World!",
  "scroll": true,
  "scroll_speed": 30,
  "color": "#FF8800",
  "glyphs": {
    ":sun:": "glyphs/weather/sun.png",
    ":heart:": "glyphs/emoji/heart.png"
  }
}
```

Glyph tokens (e.g. `:sun:`) in the text string are replaced with the
corresponding glyph image inline with the text.

---

## Fonts

Bitmap fonts in **BDF format**. BDF is simple to parse, widely available for
pixel fonts, and maps directly to LED matrix rendering.

Stored in `content/fonts/`. Referenced by name (filename without extension) in
text content files.

---

## Glyphs

Small PNG images that can be embedded inline in text content. Organized in
themed subdirectories. Each subdirectory has a `meta.json` mapping glyph names
to files:

**glyphs/weather/meta.json**:
```json
{
  "name": "Weather Icons",
  "size": 8,
  "glyphs": {
    "sun": "sun.png",
    "cloud": "cloud.png",
    "rain": "rain.png"
  }
}
```

---

## Content Sync (CLI → Device)

### Protocol
Content is synced from the host CLI to the device over HTTP (WiFi).

### Endpoints

| Method | Path              | Description                          |
|--------|-------------------|--------------------------------------|
| GET    | /api/content      | List all content on device (JSON)    |
| GET    | /api/content/hash | Get content manifest with file hashes|
| POST   | /api/upload       | Upload a file (multipart/form-data)  |
| DELETE | /api/content/:path| Delete a file from device            |
| POST   | /api/play         | Play content `{"path":"images/fire"}`|
| POST   | /api/stop         | Stop playback, return to idle        |
| GET    | /api/status       | Current playback status              |

### Sync Flow
1. CLI scans local `content/` directory, computes file hashes
2. CLI fetches device manifest (`GET /api/content/hash`)
3. CLI diffs: new files, changed files, deleted files
4. CLI uploads changed/new files with progress feedback (hash marks / percentage)
5. CLI deletes removed files
6. CLI confirms sync complete

### Progress Feedback
```
Syncing content to device (192.168.1.42)...
[████████████░░░░░░░░] 60%  uploading images/fire/0012.png
```

---

## Playback

### Image Playback
1. Device receives `POST /api/play {"path":"images/fire"}`
2. Device checks if path is a directory (sequence) or file (static image)
3. **Static image**: decode PNG, render to display buffer, hold
4. **Sequence**: load `meta.json`, decode frames one at a time at the
   specified FPS, loop or hold as configured
5. `POST /api/stop` returns to idle screen

### Memory Strategy
- Decode one PNG frame at a time into a display-sized RGB buffer
- For sequences: decode next frame while current frame is displayed (double buffer)
- Maximum frame size: matrix resolution × 3 bytes (RGB)
- 64×64 matrix = 12KB per frame, double-buffered = 24KB — well within ESP32-S3 RAM

---

## Device Storage

### Current Partition Layout
```
pd (LittleFS) — 256KB at 0x3B0000
```

### Recommended Expansion
With 8MB flash and two 1.8MB OTA slots, there is room to expand content storage.
Proposed revised layout:

```
nvs        data  nvs       0x9000   0x5000    (20KB — settings)
otadata    data  ota       0xe000   0x2000    (8KB)
app0       app   ota_0     0x10000  0x1D0000  (1.8MB)
app1       app   ota_1     0x1E0000 0x1D0000  (1.8MB)
pd_config  data  littlefs  0x3B0000 0x10000   (64KB — config only)
pd_content data  littlefs  0x3C0000 0x40000   (256KB — content)
```

Or if OTA is not needed initially, reclaim one OTA slot for more content space.

### Future: Mass Storage
When USB mass storage is supported, the content directory moves to the external
drive. The device config specifies the content source:
- `"content_source": "flash"` — use internal LittleFS
- `"content_source": "usb"` — use mounted USB drive at `/usb/content/`

---

## Implementation Phases

### Phase 1 — MVP (Current)
- [ ] PNG decoder on device (lodepng — single C file)
- [ ] Static PNG display via HTTP command
- [ ] Content CLI: list images, select, play over WiFi
- [ ] Basic file upload (single file at a time)

### Phase 2 — Sequences
- [ ] PNG sequence playback with meta.json
- [ ] Double-buffered frame decoding
- [ ] Content sync with hash-based diffing
- [ ] Progress feedback in CLI

### Phase 3 — Text & Glyphs
- [ ] BDF font parser
- [ ] Text rendering on matrix
- [ ] Glyph inline substitution
- [ ] Scrolling text support

### Phase 4 — Mass Storage
- [ ] USB mass storage detection
- [ ] Content source switching (flash vs USB)
- [ ] CLI device configuration for storage path

---

## CLI Tool: `content-cli`

```
Usage: content-cli [options]
  --host <ip>       Device IP address (or hostname.local)
  --sync            Sync local content/ to device
  --list            List content on device
  --play <path>     Play content by path
  --stop            Stop playback

Interactive mode (no args): menu-driven content browser
```
