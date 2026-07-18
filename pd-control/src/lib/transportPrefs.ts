export type LinkKind = "usb" | "ble";

/** Which connected transport Content/control should use. */
export type ControlViaPref = "wifi" | "bluetooth" | "usb";

export interface DeviceTransportPrefs {
  linkKind?: LinkKind;
  /** Explicit control-path choice (honored when that link is up). */
  controlVia?: ControlViaPref;
  serialPort?: string;
  bleId?: string;
  bleName?: string;
}

const STORAGE_KEY = "pd.transportPrefs.v1";

type PrefsMap = Record<string, DeviceTransportPrefs>;

function readAll(): PrefsMap {
  try {
    const raw = localStorage.getItem(STORAGE_KEY);
    if (!raw) return {};
    const parsed = JSON.parse(raw) as PrefsMap;
    return parsed && typeof parsed === "object" ? parsed : {};
  } catch {
    return {};
  }
}

function writeAll(map: PrefsMap): void {
  try {
    localStorage.setItem(STORAGE_KEY, JSON.stringify(map));
  } catch {
    /* quota / private mode — ignore */
  }
}

/** Stable key for a discovered device, or "default" when none selected. */
export function prefsKeyForDevice(device: {
  name?: string;
  ip?: string;
} | null): string {
  if (!device) return "default";
  const name = (device.name || "").trim().toLowerCase();
  if (name) return `name:${name}`;
  if (device.ip) return `ip:${device.ip}`;
  return "default";
}

export function loadTransportPrefs(key: string): DeviceTransportPrefs {
  const all = readAll();
  const specific = all[key] || {};
  if (key === "default") return { ...specific };
  // Fall back to default for any missing fields (e.g. last BLE id).
  return { ...(all.default || {}), ...specific };
}

/** Merge prefs for a device key. Also mirrors onto "default" for cold start. */
export function saveTransportPrefs(
  key: string,
  patch: DeviceTransportPrefs
): void {
  const all = readAll();
  const next: DeviceTransportPrefs = { ...(all[key] || {}), ...patch };
  for (const k of Object.keys(next) as (keyof DeviceTransportPrefs)[]) {
    if (next[k] === "" || next[k] == null) delete next[k];
  }
  all[key] = next;
  if (key !== "default") {
    all.default = { ...(all.default || {}), ...patch };
    for (const k of Object.keys(all.default) as (keyof DeviceTransportPrefs)[]) {
      if (all.default[k] === "" || all.default[k] == null) delete all.default[k];
    }
  }
  writeAll(all);
}
