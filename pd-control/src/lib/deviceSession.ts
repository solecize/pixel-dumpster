import type { DiscoveredDevice } from "./types";
import { loadTransportPrefs } from "./transportPrefs";

const STORAGE_KEY = "pd.lastDevice.v1";

/** Persist the last selected device so Content works without a fresh mDNS scan. */
export function saveLastDevice(device: DiscoveredDevice | null): void {
  try {
    if (!device) {
      localStorage.removeItem(STORAGE_KEY);
      return;
    }
    localStorage.setItem(STORAGE_KEY, JSON.stringify(device));
  } catch {
    /* ignore */
  }
}

export function loadLastDevice(): DiscoveredDevice | null {
  try {
    const raw = localStorage.getItem(STORAGE_KEY);
    if (!raw) return null;
    const d = JSON.parse(raw) as DiscoveredDevice;
    if (!d || typeof d !== "object" || !d.name) return null;
    return {
      name: String(d.name),
      host: String(d.host || d.name),
      ip: String(d.ip || "0.0.0.0"),
      port: Number(d.port) || 8088,
      device_type: d.device_type === "daemon" ? "daemon" : "device",
      txt: d.txt && typeof d.txt === "object" ? d.txt : {},
    };
  } catch {
    return null;
  }
}

/**
 * Build a selectable device for BLE/USB-only sessions when WiFi discovery
 * hasn't produced one yet.
 */
export function offlineControlDevice(
  links: { bluetooth: boolean; usb: boolean }
): DiscoveredDevice | null {
  if (!links.bluetooth && !links.usb) return null;

  const remembered = loadLastDevice();
  if (remembered) return remembered;

  const prefs = loadTransportPrefs("default");
  const name =
    (links.bluetooth && prefs.bleName) ||
    "pixel-dumpster";

  return {
    name,
    host: name,
    ip: "0.0.0.0",
    port: 8088,
    device_type: "device",
    txt: {
      transport: links.bluetooth ? "bluetooth" : "usb",
      offline: "1",
    },
  };
}

/** Prefer a live discovery hit that matches a remembered/offline device. */
export function mergeSelectedWithDiscovery(
  selected: DiscoveredDevice | null,
  discovered: DiscoveredDevice[]
): DiscoveredDevice | null {
  if (!selected) return null;
  const byName = discovered.find(
    (d) => d.name.trim().toLowerCase() === selected.name.trim().toLowerCase()
  );
  if (byName) return byName;
  const byIp =
    selected.ip && selected.ip !== "0.0.0.0"
      ? discovered.find((d) => d.ip === selected.ip && d.port === selected.port)
      : undefined;
  return byIp || selected;
}

const AQ_KEY = "pd.autoQuantize.v1";

function aqMap(): Record<string, boolean> {
  try {
    const raw = localStorage.getItem(AQ_KEY);
    if (!raw) return {};
    const parsed = JSON.parse(raw) as Record<string, boolean>;
    return parsed && typeof parsed === "object" ? parsed : {};
  } catch {
    return {};
  }
}

function aqKeyFor(device: { name?: string; ip?: string } | null): string {
  if (!device) return "default";
  const name = (device.name || "").trim().toLowerCase();
  if (name) return `name:${name}`;
  if (device.ip) return `ip:${device.ip}`;
  return "default";
}

/** Last known auto-quantize for offline UI before BLE/WiFi can confirm. */
export function loadAutoQuantizePref(
  device: { name?: string; ip?: string } | null
): boolean | null {
  const map = aqMap();
  const key = aqKeyFor(device);
  if (typeof map[key] === "boolean") return map[key];
  if (typeof map.default === "boolean") return map.default;
  return null;
}

export function saveAutoQuantizePref(
  device: { name?: string; ip?: string } | null,
  enabled: boolean
): void {
  try {
    const map = aqMap();
    const key = aqKeyFor(device);
    map[key] = enabled;
    map.default = enabled;
    localStorage.setItem(AQ_KEY, JSON.stringify(map));
  } catch {
    /* ignore */
  }
}

const FPS_HUD_KEY = "pd.showFpsCounter";

function fpsHudMap(): Record<string, boolean> {
  try {
    const raw = localStorage.getItem(FPS_HUD_KEY);
    if (!raw) return {};
    const parsed = JSON.parse(raw) as Record<string, boolean>;
    return parsed && typeof parsed === "object" ? parsed : {};
  } catch {
    return {};
  }
}

export function loadShowFpsCounterPref(
  device: { name?: string; ip?: string } | null
): boolean | null {
  const map = fpsHudMap();
  const key = aqKeyFor(device);
  if (typeof map[key] === "boolean") return map[key];
  if (typeof map.default === "boolean") return map.default;
  return null;
}

export function saveShowFpsCounterPref(
  device: { name?: string; ip?: string } | null,
  enabled: boolean
): void {
  try {
    const map = fpsHudMap();
    const key = aqKeyFor(device);
    map[key] = enabled;
    map.default = enabled;
    localStorage.setItem(FPS_HUD_KEY, JSON.stringify(map));
  } catch {
    /* ignore */
  }
}
