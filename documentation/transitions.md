# Transitions & Display Content System

## Transition Catalog

24 transition types are available for switching between content items.

### Wipes (8)

Reveal new content by sweeping a hard edge across the display. Old content stays in place, new content appears behind the edge.

| Name | Description |
|------|-------------|
| `wipe-left` | Edge sweeps left→right |
| `wipe-right` | Edge sweeps right→left |
| `wipe-up` | Edge sweeps top→bottom |
| `wipe-down` | Edge sweeps bottom→top |
| `wipe-diag-tl` | Diagonal wipe from top-left corner |
| `wipe-diag-tr` | Diagonal wipe from top-right corner |
| `wipe-diag-bl` | Diagonal wipe from bottom-left corner |
| `wipe-diag-br` | Diagonal wipe from bottom-right corner |

### Slides (4)

Old content pushes off-screen, new content enters from the opposite side. Both move together.

| Name | Description |
|------|-------------|
| `slide-left` | Old exits left, new enters right |
| `slide-right` | Old exits right, new enters left |
| `slide-up` | Old exits top, new enters bottom |
| `slide-down` | Old exits bottom, new enters top |

### Rolls (2)

Old content scrolls off one edge, new content is revealed in the vacated space.

| Name | Description |
|------|-------------|
| `roll-up` | Old scrolls up, new revealed from bottom |
| `roll-down` | Old scrolls down, new revealed from top |

### Splits (3)

Old content splits apart, revealing new content underneath.

| Name | Description |
|------|-------------|
| `split-h` | Top half slides up, bottom half slides down |
| `split-v` | Left half slides left, right half slides right |
| `split-diag` | Diagonal split — triangles slide to opposite corners |

### Zooms (2)

Two-phase transitions with a "pass through the camera" effect and bounce easing.

| Name | Description |
|------|-------------|
| `zoom-in` | A zooms past the camera (100%→300%), disappears. B appears small (15%) and grows to 100% with bounce |
| `zoom-out` | A shrinks to tiny (100%→15%), disappears. B appears huge (300%) and shrinks to 100% with bounce |

### Flips (2)

Card-flip effect — old content squeezes to a line, then new content expands from the line.

| Name | Description |
|------|-------------|
| `flip-h` | Horizontal flip (squeeze/expand along vertical axis) |
| `flip-v` | Vertical flip (squeeze/expand along horizontal axis) |

### Blends (1)

| Name | Description |
|------|-------------|
| `fade` | Per-pixel crossfade between old and new content |

### Builds (2)

New content appears incrementally in random order over the old content.

| Name | Description |
|------|-------------|
| `block-build` | Random 4×4 pixel blocks reveal new content |
| `pixel-build` | Random individual pixels reveal new content |

---

## Configurable Transition Parameters

Some transitions accept optional parameters that override built-in defaults. These can be specified in metadata via a `params` object.

### Zoom Parameters

| Parameter | Default | Description |
|-----------|---------|-------------|
| `zoom_vanish` | 3.0 | Scale at which image has "passed the camera" and vanishes |
| `zoom_crossover` | 0.45 | Fraction of duration where A→B handoff occurs (0.0–1.0) |
| `bounce_overshoot` | 0.10 | How much B overshoots past 100% before settling (0.0–1.0) |
| `bounce_settle` | 0.20 | Fraction of phase 2 spent settling back from overshoot |

### Build Parameters

| Parameter | Default | Description |
|-----------|---------|-------------|
| `block_size` | 4 | Pixel size of blocks for `block-build` |

---

## Content Types

### Static Image
A single PNG file (e.g., `images/frogger.png`).

### Sequence
A directory of numbered PNGs (`0001.png`, `0002.png`, ...) with an optional `meta.json`:

```json
{
  "fps": 24,
  "loop": true
}
```

### Playlist (planned)
An ordered list of content items with transitions between them. Stored as `playlist.json` in `/content/playlists/`.

---

## Metadata Schema

### Per-Item `meta.json`

Lives inside a content directory (e.g., `images/pac-ghost/meta.json`).

```json
{
  "fps": 24,
  "loop": true,
  "enter": {
    "transition": "zoom-in",
    "duration_ms": 1000,
    "params": {
      "zoom_vanish": 4.0,
      "bounce_overshoot": 0.15
    }
  },
  "exit": {
    "transition": "fade",
    "duration_ms": 300
  },
  "hold_ms": 5000
}
```

| Field | Type | Description |
|-------|------|-------------|
| `fps` | int | Frame rate for sequences (default: 12) |
| `loop` | bool | Whether sequence loops (default: true) |
| `enter` | object | Transition used when this item starts playing |
| `exit` | object | Transition used when this item finishes |
| `hold_ms` | int | How long to display before auto-advancing. 0 or absent = hold forever |
| `enter.transition` | string | Transition type name |
| `enter.duration_ms` | int | Transition duration in milliseconds (default: 500) |
| `enter.params` | object | Optional parameter overrides for the transition |

---

## Global Configuration: `config.json`

Lives at `/content/config.json`. Controls default behavior for transitions, display, and attract mode.

```json
{
  "transition": {
    "mode": "random",
    "baseline": "fade",
    "duration_ms": 800,
    "params": {}
  },
  "display": {
    "hold_ms": 5000,
    "loop_sequences": true
  },
  "attract": {
    "enabled": false,
    "path": "images/",
    "shuffle": true,
    "idle_timeout_ms": 0
  }
}
```

### `transition` — How content switches

| Field | Type | Description |
|-------|------|-------------|
| `mode` | string | `random`, `baseline`, or `per-item` |
| `baseline` | string | Transition name used as default/fallback |
| `duration_ms` | int | Default transition duration (ms) |
| `params` | object | Default parameter overrides (zoom_vanish, bounce, block_size, etc.) |

**Modes:**
- `random` — pick a random transition from the catalog each time
- `baseline` — always use `transition.baseline`
- `per-item` — use item's `meta.json` `enter` transition if present, otherwise fall back to `baseline`

### `display` — How content is shown

| Field | Type | Description |
|-------|------|-------------|
| `hold_ms` | int | Default hold time for static images in attract mode (0 = hold forever) |
| `loop_sequences` | bool | Whether sequences loop by default (can be overridden per-item) |

### `attract` — Auto-cycle / screensaver mode

| Field | Type | Description |
|-------|------|-------------|
| `enabled` | bool | Start attract mode on boot |
| `path` | string | Directory to cycle through (e.g., `images/`) |
| `shuffle` | bool | Randomize order vs. alphabetical |
| `idle_timeout_ms` | int | Resume attract mode after this much idle time (0 = never auto-resume) |

Attract mode is interrupted by any `/api/play` command. If `idle_timeout_ms > 0`, it resumes after that duration of no commands.

### Priority Chain

When determining which transition to use (highest wins):

1. **HTTP request** explicit `transition` field
2. **Per-item** `meta.json` `enter` (if mode is `per-item`)
3. **Mode logic** (`random` picks one, `baseline` uses baseline)
4. **`baseline`** — always the final fallback

---

### Playlist `playlist.json`

```json
{
  "name": "Arcade Classics",
  "loop": true,
  "items": [
    {
      "path": "images/frogger.png",
      "hold_ms": 5000,
      "transition": "wipe-left",
      "duration_ms": 800
    },
    {
      "path": "images/pac-ghost",
      "transition": "block-build",
      "duration_ms": 1500
    },
    {
      "path": "images/zaxxon.png",
      "hold_ms": 4000,
      "transition": "zoom-in",
      "duration_ms": 1500,
      "params": {
        "zoom_vanish": 4.0
      }
    }
  ]
}
```

| Field | Type | Description |
|-------|------|-------------|
| `name` | string | Display name for the playlist |
| `loop` | bool | Restart from first item after last (default: true) |
| `items[].path` | string | Content path relative to `/content/` |
| `items[].hold_ms` | int | Override item's own hold_ms |
| `items[].transition` | string | Transition INTO this item |
| `items[].duration_ms` | int | Transition duration (default: 500) |
| `items[].params` | object | Optional transition parameter overrides |

---

## Playlist Rules

1. **`transition` on a playlist item = the transition INTO that item** from whatever was showing before
2. **Priority**: playlist item `transition` > item's own `meta.json` `enter` > `none`
3. **`hold_ms` in playlist** overrides the item's own `hold_ms`
4. **Sequences** play all frames, then hold for `hold_ms` (if set), then advance. If `loop: true` in the sequence's own meta, it loops until `hold_ms` expires
5. **Static images** display for `hold_ms`, then advance
6. **`hold_ms: 0`** on a static image = transition immediately after enter transition completes
7. **No playlist** = single item play (current behavior), no auto-advance
8. **`loop: true`** on playlist = restart from first item after last
9. **Default `duration_ms`** = 500ms if not specified

---

## Architecture

### Framebuffer System

- `pd_framebuf_t`: RGB888 buffer matching display dimensions (e.g., 64×64 = 12,288 bytes)
- `content_fb`: tracks current display state, updated every frame
- Transition engine uses three framebuffers: `from` (old content), `to` (new content), `out` (blended result)
- Total memory: ~36KB for a 64×64 display (3 framebuffers × 12KB)

### Transition Engine (`pd-transition`)

- Progress-based rendering: each transition takes `from`, `to`, and a progress float (0.0–1.0)
- Driven by `pd_content_tick()` which is called from the main loop
- Timer-based: uses `esp_timer_get_time()` for frame-independent animation
- Dispatch table maps transition type enum to render function pointer

### Content Pipeline

```
PNG file → lodepng decode → RGB888 buffer → framebuf_blit → display_render_framebuf → HUB75 panel
                                                ↓
                                        transition engine
                                        (from + to → out)
```

---

## HTTP API

### Play with transition

```
POST /api/play
Content-Type: application/json

{
  "path": "images/zaxxon.png",
  "transition": "zoom-in",
  "duration_ms": 1500,
  "params": {
    "zoom_vanish": 4.0
  }
}
```

- `path` (required): content path relative to `/content/`
- `transition` (optional): transition type name. Omit to use config mode logic
- `duration_ms` (optional): transition duration, default from config
- `params` (optional): transition parameter overrides

### Attract Mode Control

```
POST /api/attract/start
POST /api/attract/stop
GET /api/attract/status
```

### Configuration

```
GET /api/config
POST /api/config
Content-Type: application/json

{
  "transition": {
    "mode": "random"
  }
}
```

Partial updates are merged with existing config.

---

## Future Work

- **Text content**: bitmap font rendering with scroll/zoom animations
- **Fonts/glyphs**: `/fonts/` and `/glyphs/` directories for user-uploadable assets
- **Image effects**: color-build (dark→light reveal), pulse, blink
- **USB storage**: content on thumb drive instead of LittleFS
