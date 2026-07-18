/**
 * Content/control API over WiFi HTTP, BLE NDJSON, or USB wizard.
 * Honors an explicit user preference when that link is connected; otherwise
 * falls back WiFi → Bluetooth → USB.
 */
import type { ContentList, DeviceStatus, DiscoveredDevice } from "./types";
import type { TransportKind, TransportLinks } from "./transport";
import {
  blePlay,
  blePoll,
  bleSend,
  bleStatus,
  bleStop,
  devicePlay,
  deviceStop,
  getDeviceConfig,
  getDeviceContent,
  getDeviceStatus,
  setDeviceConfig,
  uploadContentToDevice,
  uploadLocalFileToDevice,
  wizardPoll,
  wizardSend,
} from "./api";
import {
  loadTransportPrefs,
  prefsKeyForDevice,
  type ControlViaPref,
} from "./transportPrefs";

export function preferredControlTransport(
  links: TransportLinks,
  prefer?: ControlViaPref | null
): TransportKind | null {
  if (prefer && links[prefer]) return prefer;
  if (links.wifi) return "wifi";
  if (links.bluetooth) return "bluetooth";
  if (links.usb) return "usb";
  return null;
}

/** Load persisted control preference for a device key / default. */
export function loadControlViaPref(prefsKey = "default"): ControlViaPref | null {
  const v = loadTransportPrefs(prefsKey).controlVia;
  return v === "wifi" || v === "bluetooth" || v === "usb" ? v : null;
}

function viaFor(
  device: DiscoveredDevice,
  links: TransportLinks
): TransportKind | null {
  return preferredControlTransport(
    links,
    loadControlViaPref(prefsKeyForDevice(device))
  );
}

function parseJsonLines(lines: string[]): Record<string, unknown>[] {
  const out: Record<string, unknown>[] = [];
  for (const line of lines) {
    try {
      out.push(JSON.parse(line) as Record<string, unknown>);
    } catch {
      /* ignore non-JSON */
    }
  }
  return out;
}

function findByType(
  objs: Record<string, unknown>[],
  type: string
): Record<string, unknown> | null {
  return objs.find((o) => o.type === type) ?? null;
}

function assertAck(
  objs: Record<string, unknown>[],
  cmd: string,
  transport: string
): void {
  const ack = objs.find((o) => o.type === "ack" && o.cmd === cmd);
  if (!ack) {
    throw new Error(`${transport}: no ack for ${cmd}`);
  }
  if (ack.ok !== true) {
    throw new Error(
      `${transport} ${cmd}: ${typeof ack.error === "string" ? ack.error : "failed"}`
    );
  }
}

async function ndjsonSend(
  transport: "bluetooth" | "usb",
  command: string
): Promise<Record<string, unknown>[]> {
  const lines =
    transport === "bluetooth"
      ? await bleSend(command)
      : await wizardSend(command);
  return parseJsonLines(lines);
}

async function ndjsonSendExpectType(
  transport: "bluetooth" | "usb",
  command: string,
  type: string,
  waitRounds = 8
): Promise<Record<string, unknown>> {
  let objs = await ndjsonSend(transport, command);
  for (let i = 0; i < waitRounds; i++) {
    const hit = findByType(objs, type);
    if (hit) return hit;
    /* list/status can arrive late after deferred FS work — poll, don't re-send */
    await new Promise((r) => setTimeout(r, 250 + i * 150));
    const more =
      transport === "bluetooth" ? await blePoll() : await wizardPoll();
    objs = [...objs, ...parseJsonLines(more)];
  }
  throw new Error(`${transport}: no ${type} response for ${command}`);
}

export async function controlStatus(
  device: DiscoveredDevice,
  links: TransportLinks
): Promise<DeviceStatus> {
  const via = viaFor(device, links);
  if (!via) throw new Error("No control transport connected");

  if (via === "wifi") {
    return getDeviceStatus(device.ip, device.port);
  }

  if (via === "bluetooth") {
    const objs = parseJsonLines(await bleStatus());
    const st = findByType(objs, "status");
    if (!st) throw new Error("bluetooth: no status response");
    return st as DeviceStatus;
  }

  return (await ndjsonSendExpectType(
    "usb",
    '{"cmd":"status"}',
    "status"
  )) as DeviceStatus;
}

export async function controlListContent(
  device: DiscoveredDevice,
  links: TransportLinks
): Promise<ContentList> {
  const via = viaFor(device, links);
  if (!via) throw new Error("No control transport connected");

  if (via === "wifi") {
    return getDeviceContent(device.ip, device.port);
  }

  const transport = via === "bluetooth" ? "bluetooth" : "usb";
  const list = await ndjsonSendExpectType(
    transport,
    '{"cmd":"list"}',
    "list",
    5
  );
  const items = Array.isArray(list.items) ? list.items : [];
  return { items: items as ContentList["items"] };
}

export async function controlPlay(
  device: DiscoveredDevice,
  links: TransportLinks,
  path: string,
  transition = "fade",
  durationMs = 800
): Promise<void> {
  const via = viaFor(device, links);
  if (!via) throw new Error("No control transport connected");

  if (via === "wifi") {
    await devicePlay(device.ip, device.port, path, transition, durationMs);
    return;
  }

  if (via === "bluetooth") {
    const objs = parseJsonLines(
      await blePlay(path, transition, durationMs)
    );
    assertAck(objs, "play", "BLE");
    return;
  }

  const cmd = JSON.stringify({
    cmd: "play",
    path,
    transition,
    duration_ms: durationMs,
  });
  const objs = await ndjsonSend("usb", cmd);
  assertAck(objs, "play", "USB");
}

export async function controlStop(
  device: DiscoveredDevice,
  links: TransportLinks
): Promise<void> {
  const via = viaFor(device, links);
  if (!via) throw new Error("No control transport connected");

  if (via === "wifi") {
    await deviceStop(device.ip, device.port);
    return;
  }

  if (via === "bluetooth") {
    const objs = parseJsonLines(await bleStop());
    assertAck(objs, "stop", "BLE");
    return;
  }

  const objs = await ndjsonSend("usb", '{"cmd":"stop"}');
  assertAck(objs, "stop", "USB");
}

export async function controlUpload(
  device: DiscoveredDevice,
  links: TransportLinks,
  contentPath: string
): Promise<void> {
  const via = viaFor(device, links);
  if (!via) throw new Error("No control transport connected");
  await uploadContentToDevice(device.ip, device.port, contentPath, via);
}

/** Upload a user-picked local PNG into `images/<filename>` on the device. */
export async function controlUploadLocal(
  device: DiscoveredDevice,
  links: TransportLinks,
  localPath: string,
  remotePath?: string
): Promise<string> {
  const via = viaFor(device, links);
  if (!via) throw new Error("No control transport connected");
  return uploadLocalFileToDevice(
    device.ip,
    device.port,
    localPath,
    remotePath,
    via
  );
}

/**
 * Auto-quantize toggle: full config over WiFi; over BLE/USB probe with
 * `set_playback` (no field) which acks the current value without changing it.
 */
export async function controlGetAutoQuantize(
  device: DiscoveredDevice,
  links: TransportLinks
): Promise<boolean | null> {
  const via = viaFor(device, links);
  if (!via) return null;

  if (via === "wifi") {
    try {
      const cfg = (await getDeviceConfig(device.ip, device.port)) as {
        display?: { auto_quantize_palette?: boolean };
      };
      return Boolean(cfg?.display?.auto_quantize_palette);
    } catch {
      return null;
    }
  }

  const transport = via === "bluetooth" ? "bluetooth" : "usb";
  try {
    const objs = await ndjsonSend(transport, '{"cmd":"set_playback"}');
    const ack = objs.find((o) => o.type === "ack" && o.cmd === "set_playback");
    if (ack && typeof ack.auto_quantize_palette === "boolean") {
      return ack.auto_quantize_palette;
    }
  } catch {
    /* fall through */
  }
  return null;
}

export async function controlSetAutoQuantize(
  device: DiscoveredDevice,
  links: TransportLinks,
  enabled: boolean
): Promise<void> {
  const via = viaFor(device, links);
  if (!via) throw new Error("No control transport connected");

  if (via === "wifi") {
    await setDeviceConfig(device.ip, device.port, {
      display: { auto_quantize_palette: enabled },
      save: true,
    });
    return;
  }

  const transport = via === "bluetooth" ? "bluetooth" : "usb";
  const objs = await ndjsonSend(
    transport,
    JSON.stringify({
      cmd: "set_playback",
      auto_quantize_palette: enabled,
      save: true,
    })
  );
  assertAck(objs, "set_playback", transport === "bluetooth" ? "BLE" : "USB");
}
