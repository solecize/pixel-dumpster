# pixel-dumpster

## your infinite bin of pixel-punk trash

------------------------------------------------------------------------

> Please update this living document as requirements change or when additional features and documents are added. Please include Related Documentation in the following list:

#### Related Documentation:

[readme.md](../readme.md) | [development.md](development.md) | [api.md](api.md) | [kebab-style-naming.md](kebab-style-naming.md)

# 1. Project Overview

**pixel-dumpster** is a network-addressable display app for esp32 devices connected to one or more LED matrix that:

-   Runs on ESP32 (Matrix Portal or similar) using the ESP-IDF native workflow
-   Drives an arbitrary LED matrix (HUB75, or similar)
-   Maintains a local pixel art library
-   Accepts push notifications over Wi-Fi
-   Displays art based on upstream context (system/game/custom)
-   Is controller-agnostic and frontend-agnostic
-   Uses an artifact + push notification software model

The software is designed to be:

-   Robust
-   Decoupled
-   Deterministic
-   Work with many ecosystems as a companion destination (automated pixel art, automatically updated arcade marquee, etc.)

This repository is a living specification: you define project needs and goals, and the firmware is implemented to match those requirements.

------------------------------------------------------------------------

# 2. MVP: First-Run Setup Wizard

## 2.1 Goal

On first boot (or reset), run a setup flow that works with any connected
LED matrix, resulting in saved configuration for:

-   Matrix resolution
-   Orientation
-   Wi-Fi credentials
-   Device/project name

## 2.2 Boot-Safe Display Requirement

On boot without configuration:

-   Render the word `keyboard`
-   Displayed in the upper-left corner
-   Using an LED-friendly bitmap font (5x7 or similar)

## 2.3 USB Keyboard Detection

-   Detect USB keyboard on Matrix Portal USB-C (USB host mode)
-   Display waiting indicator until detected
-   Accept ASCII, Enter, Backspace

## 2.4 Wizard Flow

1.  Confirm keyboard works
2.  Panel resolution (WIDTHxHEIGHT)
3.  Panel orientation (0/90/180/270)
4.  Wi-Fi setup (scan → select → password)
5.  Device/project name

## 2.5 Persisted Configuration (Example)

``` json
{
  "matrix_width": 224,
  "matrix_height": 64,
  "orientation_deg": 0,
  "wifi_ssid": "network",
  "wifi_password": "password",
  "device_name": "pixel-dumpster",
  "setup_complete": true,
  "config_version": 1
}
```

------------------------------------------------------------------------

# 3. Artifact Storage Contract

Root directory:

    /pd/

Required:

    /pd/now.json
    /pd/default.png

Optional:

    /pd/system/<system>.png
    /pd/game/<system>/<game>.png
    /pd/assets/<custom>.png

## 3.1 now.json Schema

``` json
{
  "mode": "idle|system|game|custom",
  "system": "mame",
  "game": "pacman",
  "asset": "assets/custom.png",
  "updated_at": 1768109057
}
```

------------------------------------------------------------------------

# 4. Push Notification Model

## 4.1 UDP Doorbell (Primary)

-   Consumer listens on UDP port (example: 9876)
-   Any packet received triggers reload of now.json

## 4.2 Polling Backstop

-   Consumer polls now.json every \~1 second

------------------------------------------------------------------------

# 5. Minimal On-Device APIs

HTTP runs on port (example: 8088)

## POST /reload

Triggers re-read of now.json\
Response: 204 No Content

## GET /state

Returns current internal display state

## POST /upload

Upload PNG asset to specified path

## GET /list

Returns asset inventory

------------------------------------------------------------------------

# 6. Design Principles

-   Artifacts are truth
-   Push is advisory
-   Controller-agnostic
-   Frontend-agnostic
-   Deterministic fallback
-   Minimal API surface
-   Human-readable and friendly
-   Configuration-friendly device
-   kebab-style-naming