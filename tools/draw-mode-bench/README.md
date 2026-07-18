# draw-mode-bench

Standalone ESP-IDF app that A/B tests **full-matrix** vs **bounds-limited** vs **sliding dirty-rect** panel updates using the same `pd-display` stack as Pixel Dumpster.

Does **not** modify the Control app. Production firmware should be reflashed after running.

## Build & flash

```bash
source ../../esp-idf/export.sh   # or your IDF export
cd tools/draw-mode-bench
cp ../../sdkconfig sdkconfig     # pin/layout parity with production
idf.py set-target esp32s3
idf.py -p /dev/cu.usbmodem101 flash monitor
```

Uses the existing `pd` LittleFS partition (does not erase content).

## What it does

1. Inits display from saved `pd-config`.
2. Loads `images/pac-ghost` PNG frames into SPIRAM as RGB (decode once), or synthesizes a sparse sprite if missing.
3. ~10s **full** mode — blit into full canvas FB + `pd_display_render_framebuf`.
4. Clear once, then ~10s **bounds** mode — centered `pd_display_render_rgb` content rect only.
5. ~10s **slide** mode — bounce horizontally with erase of `old∖new` + `pd_display_render_rgb_at` (no trails).
6. Logs `bench: mode=… fps=…` and a final `RESULT` line.

## Measured result (this hardware)

On the user’s matrix with 115 pac-ghost frames (64×64) from LittleFS:

| Mode | Achieved FPS |
|------|----------------|
| full (entire canvas push) | **27.44** |
| bounds (64×64 rect only) | **113.14** |
| slide (dirty erase + render_at) | **108.74** |
| bounds/full | **~4.1×** |
| slide/full | **~4.0×** |

So for sparse content, panel update size dominates — bounds-limited draws are a clear win, and sliding with erase of `old∖new` stays nearly as fast.

## Restore production firmware

```bash
cd ../..
idf.py -p /dev/cu.usbmodem101 flash
```
