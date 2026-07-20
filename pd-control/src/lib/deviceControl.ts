/**
 * Content/control API over WiFi HTTP, BLE NDJSON, or USB wizard.
 * Honors an explicit user preference when that link is connected; otherwise
 * falls back WiFi → Bluetooth → USB.
 *
 * Every control call emits correlated events into `eventLog` when tracing
 * is enabled (intent → transport → request/response → optional state).
 */
import type { ContentList, DeviceStatus, DiscoveredDevice } from "./types";
import type { TransportKind, TransportLinks } from "./transport";
import {
  blePlay,
  blePoll,
  bleSend,
  bleStatus,
  bleStop,
  deviceDeleteContent,
  devicePlay,
  deviceRenameContent,
  deviceSetContentMeta,
  deviceStop,
  getDeviceConfig,
  getDeviceContent,
  getDeviceLog,
  getDeviceStatus,
  setDeviceConfig,
  uploadContentToDevice,
  uploadLocalFileToDevice,
  uploadLocalSequenceToDevice,
  wizardPoll,
  wizardSend,
} from "./api";
import {
  loadTransportPrefs,
  prefsKeyForDevice,
  type ControlViaPref,
} from "./transportPrefs";
import { emitTrace, tracedOp } from "./eventLog";

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
  command: string,
  action = "ndjson"
): Promise<Record<string, unknown>[]> {
  emitTrace({
    level: "info",
    phase: "request",
    action,
    transport,
    summary: `${transport} send`,
    detail: { command },
  });
  const lines =
    transport === "bluetooth"
      ? await bleSend(command)
      : await wizardSend(command);
  const objs = parseJsonLines(lines);
  emitTrace({
    level: "info",
    phase: "response",
    action,
    transport,
    summary: `${transport} recv ${objs.length} object(s)`,
    detail: { lines, objects: objs },
  });
  return objs;
}

async function ndjsonSendExpectType(
  transport: "bluetooth" | "usb",
  command: string,
  type: string,
  waitRounds = 8,
  action = "ndjson"
): Promise<Record<string, unknown>> {
  let objs = await ndjsonSend(transport, command, action);
  for (let i = 0; i < waitRounds; i++) {
    const hit = findByType(objs, type);
    if (hit) return hit;
    await new Promise((r) => setTimeout(r, 250 + i * 150));
    const more =
      transport === "bluetooth" ? await blePoll() : await wizardPoll();
    const parsed = parseJsonLines(more);
    if (parsed.length) {
      emitTrace({
        level: "debug",
        phase: "response",
        action,
        transport,
        summary: `${transport} poll +${parsed.length}`,
        detail: { objects: parsed },
      });
    }
    objs = [...objs, ...parsed];
  }
  throw new Error(`${transport}: no ${type} response for ${command}`);
}

function noteTransport(
  action: string,
  via: TransportKind,
  detail?: unknown
): void {
  emitTrace({
    level: "info",
    phase: "transport",
    action,
    transport: via,
    summary: `via ${via}`,
    detail,
  });
}

async function snapshotState(
  action: string,
  device: DiscoveredDevice,
  links: TransportLinks,
  via: TransportKind,
  opts?: { expectPlaying?: boolean }
): Promise<void> {
  const expectPlaying = Boolean(opts?.expectPlaying);
  const deadline = Date.now() + 1000;
  let st: DeviceStatus | null = null;
  try {
    for (;;) {
      st = await controlStatusRaw(device, links, via);
      if (!expectPlaying || st.playing || Date.now() >= deadline) break;
      await new Promise((r) => setTimeout(r, 120));
    }
    emitTrace({
      level: "info",
      phase: "state",
      action,
      transport: via,
      summary: st.playing
        ? `playing ${st.path ?? "?"} target ${st.fps ?? "—"}fps` +
          (st.achieved_fps != null
            ? ` achieved ~${Number(st.achieved_fps).toFixed(1)}fps`
            : "") +
          (st.cache ? ` cache=${st.cache}` : "")
        : "idle",
      detail: st,
    });
  } catch (err) {
    emitTrace({
      level: "warn",
      phase: "state",
      action,
      transport: via,
      summary: `state snapshot failed: ${String(err)}`,
    });
  }
}

/** Untraced status used by polls and internal snapshots. */
async function controlStatusRaw(
  device: DiscoveredDevice,
  _links: TransportLinks,
  via: TransportKind
): Promise<DeviceStatus> {
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
    "status",
    8,
    "status"
  )) as DeviceStatus;
}

export type ControlOpts = {
  /** Background poll — suppressed in the event log unless "include polls". */
  poll?: boolean;
};

export async function controlStatus(
  device: DiscoveredDevice,
  links: TransportLinks,
  opts?: ControlOpts
): Promise<DeviceStatus> {
  const action = opts?.poll ? "poll_status" : "status";
  return tracedOp(action, async () => {
    const via = viaFor(device, links);
    if (!via) throw new Error("No control transport connected");
    noteTransport(action, via);
    const st = await controlStatusRaw(device, links, via);
    emitTrace({
      level: "info",
      phase: "response",
      action,
      transport: via,
      summary: st.playing
        ? `status playing ${st.path ?? "?"} @ ${st.fps ?? "—"}fps`
        : "status idle",
      detail: st,
    });
    return st;
  });
}

function normalizeContentEntry(raw: Record<string, unknown>): ContentList["items"][number] {
  const path = String(raw.path ?? "");
  const name = String(raw.name ?? path);
  const isSeq = Boolean(
    raw.is_sequence ?? raw.sequence ?? ((raw.frames as number) || 0) > 1
  );
  const frameCount = Number(raw.frame_count ?? raw.frames ?? 0) || undefined;
  const fps = Number(raw.fps ?? 0) || undefined;
  return {
    path,
    name,
    is_sequence: isSeq,
    frame_count: frameCount,
    fps,
  };
}

export async function controlListContent(
  device: DiscoveredDevice,
  links: TransportLinks,
  opts?: ControlOpts
): Promise<ContentList> {
  const action = opts?.poll ? "poll_list" : "list";
  return tracedOp(action, async () => {
    const via = viaFor(device, links);
    if (!via) throw new Error("No control transport connected");
    noteTransport(action, via);

    let list: ContentList;
    if (via === "wifi") {
      const raw = await getDeviceContent(device.ip, device.port);
      list = {
        items: (raw.items || []).map((item) =>
          normalizeContentEntry(item as unknown as Record<string, unknown>)
        ),
      };
    } else {
      const transport = via === "bluetooth" ? "bluetooth" : "usb";
      const raw = await ndjsonSendExpectType(
        transport,
        '{"cmd":"list"}',
        "list",
        5,
        action
      );
      const items = Array.isArray(raw.items) ? raw.items : [];
      list = {
        items: items.map((item) =>
          normalizeContentEntry(item as Record<string, unknown>)
        ),
      };
    }
    emitTrace({
      level: "info",
      phase: "response",
      action,
      transport: via,
      summary: `list ${list.items.length} item(s)`,
      detail: {
        items: list.items.map((i) => ({
          path: i.path,
          fps: i.fps,
          frames: i.frame_count,
          sequence: i.is_sequence,
        })),
      },
    });
    return list;
  });
}

export async function controlPlay(
  device: DiscoveredDevice,
  links: TransportLinks,
  path: string,
  transition = "fade",
  durationMs = 800
): Promise<void> {
  return tracedOp("play", async () => {
    const via = viaFor(device, links);
    if (!via) throw new Error("No control transport connected");
    noteTransport("play", via, { path, transition, durationMs });
    emitTrace({
      level: "info",
      phase: "intent",
      action: "play",
      transport: via,
      summary: `play ${path}`,
      detail: { path, transition, durationMs },
    });

    if (via === "wifi") {
      await devicePlay(device.ip, device.port, path, transition, durationMs);
    } else if (via === "bluetooth") {
      const objs = parseJsonLines(
        await blePlay(path, transition, durationMs)
      );
      emitTrace({
        level: "info",
        phase: "response",
        action: "play",
        transport: via,
        summary: "BLE play ack",
        detail: objs,
      });
      assertAck(objs, "play", "BLE");
    } else {
      const cmd = JSON.stringify({
        cmd: "play",
        path,
        transition,
        duration_ms: durationMs,
      });
      const objs = await ndjsonSend("usb", cmd, "play");
      assertAck(objs, "play", "USB");
    }
    await snapshotState("play", device, links, via, { expectPlaying: true });
  });
}

export async function controlStop(
  device: DiscoveredDevice,
  links: TransportLinks
): Promise<void> {
  return tracedOp("stop", async () => {
    const via = viaFor(device, links);
    if (!via) throw new Error("No control transport connected");
    noteTransport("stop", via);

    if (via === "wifi") {
      await deviceStop(device.ip, device.port);
    } else if (via === "bluetooth") {
      const objs = parseJsonLines(await bleStop());
      emitTrace({
        level: "info",
        phase: "response",
        action: "stop",
        transport: via,
        summary: "BLE stop ack",
        detail: objs,
      });
      assertAck(objs, "stop", "BLE");
    } else {
      const objs = await ndjsonSend("usb", '{"cmd":"stop"}', "stop");
      assertAck(objs, "stop", "USB");
    }
    await snapshotState("stop", device, links, via);
  });
}

export async function controlUpload(
  device: DiscoveredDevice,
  links: TransportLinks,
  contentPath: string
): Promise<void> {
  return tracedOp("upload_content", async () => {
    const via = viaFor(device, links);
    if (!via) throw new Error("No control transport connected");
    noteTransport("upload_content", via, { contentPath });
    await uploadContentToDevice(device.ip, device.port, contentPath, via);
  });
}

/** Upload a user-picked local PNG into `images/<filename>` on the device. */
export async function controlUploadLocal(
  device: DiscoveredDevice,
  links: TransportLinks,
  localPath: string,
  remotePath?: string
): Promise<string> {
  return tracedOp("upload_file", async () => {
    const via = viaFor(device, links);
    if (!via) throw new Error("No control transport connected");
    noteTransport("upload_file", via, { localPath, remotePath });
    const remote = await uploadLocalFileToDevice(
      device.ip,
      device.port,
      localPath,
      remotePath,
      via
    );
    emitTrace({
      level: "info",
      phase: "response",
      action: "upload_file",
      transport: via,
      summary: `uploaded → ${remote}`,
      detail: { remote },
    });
    return remote;
  });
}

/** Upload a user-picked folder of numbered PNGs as `images/<name>/`. */
export async function controlUploadLocalSequence(
  device: DiscoveredDevice,
  links: TransportLinks,
  localDir: string,
  fps: number,
  remoteName?: string
): Promise<string> {
  return tracedOp("upload_sequence", async () => {
    const via = viaFor(device, links);
    if (!via) throw new Error("No control transport connected");
    noteTransport("upload_sequence", via, { localDir, fps, remoteName });
    const remote = await uploadLocalSequenceToDevice(
      device.ip,
      device.port,
      localDir,
      fps,
      remoteName,
      via
    );
    emitTrace({
      level: "info",
      phase: "response",
      action: "upload_sequence",
      transport: via,
      summary: `sequence → ${remote} @ ${fps}fps`,
      detail: { remote, fps },
    });
    await snapshotState("upload_sequence", device, links, via);
    return remote;
  });
}

/**
 * Auto-quantize toggle: full config over WiFi; over BLE/USB probe with
 * `set_playback` (no field) which acks the current value without changing it.
 */
export async function controlGetAutoQuantize(
  device: DiscoveredDevice,
  links: TransportLinks
): Promise<boolean | null> {
  return tracedOp("get_auto_quantize", async () => {
    const via = viaFor(device, links);
    if (!via) return null;
    noteTransport("get_auto_quantize", via);

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
      const objs = await ndjsonSend(
        transport,
        '{"cmd":"set_playback"}',
        "get_auto_quantize"
      );
      const ack = objs.find(
        (o) => o.type === "ack" && o.cmd === "set_playback"
      );
      if (ack && typeof ack.auto_quantize_palette === "boolean") {
        return ack.auto_quantize_palette;
      }
    } catch {
      /* fall through */
    }
    return null;
  });
}

export async function controlSetAutoQuantize(
  device: DiscoveredDevice,
  links: TransportLinks,
  enabled: boolean
): Promise<void> {
  return tracedOp("set_auto_quantize", async () => {
    const via = viaFor(device, links);
    if (!via) throw new Error("No control transport connected");
    noteTransport("set_auto_quantize", via, { enabled });

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
      }),
      "set_auto_quantize"
    );
    assertAck(objs, "set_playback", transport === "bluetooth" ? "BLE" : "USB");
  });
}

export async function controlGetShowFpsCounter(
  device: DiscoveredDevice,
  links: TransportLinks
): Promise<boolean | null> {
  return tracedOp("get_show_fps_counter", async () => {
    const via = viaFor(device, links);
    if (!via) return null;
    noteTransport("get_show_fps_counter", via);

    if (via === "wifi") {
      try {
        const cfg = (await getDeviceConfig(device.ip, device.port)) as {
          display?: { show_fps_counter?: boolean };
        };
        return Boolean(cfg?.display?.show_fps_counter);
      } catch {
        return null;
      }
    }

    const transport = via === "bluetooth" ? "bluetooth" : "usb";
    try {
      const objs = await ndjsonSend(
        transport,
        '{"cmd":"set_playback"}',
        "get_show_fps_counter"
      );
      const ack = objs.find(
        (o) => o.type === "ack" && o.cmd === "set_playback"
      );
      if (ack && typeof ack.show_fps_counter === "boolean") {
        return ack.show_fps_counter;
      }
    } catch {
      /* fall through */
    }
    return null;
  });
}

export async function controlSetShowFpsCounter(
  device: DiscoveredDevice,
  links: TransportLinks,
  enabled: boolean
): Promise<void> {
  return tracedOp("set_show_fps_counter", async () => {
    const via = viaFor(device, links);
    if (!via) throw new Error("No control transport connected");
    noteTransport("set_show_fps_counter", via, { enabled });

    if (via === "wifi") {
      await setDeviceConfig(device.ip, device.port, {
        display: { show_fps_counter: enabled },
        save: true,
      });
      return;
    }

    const transport = via === "bluetooth" ? "bluetooth" : "usb";
    const objs = await ndjsonSend(
      transport,
      JSON.stringify({
        cmd: "set_playback",
        show_fps_counter: enabled,
        save: true,
      }),
      "set_show_fps_counter"
    );
    assertAck(objs, "set_playback", transport === "bluetooth" ? "BLE" : "USB");
  });
}

/** Pull firmware diagnostic ring buffer into the Event Log (WiFi only). */
export async function controlFetchDeviceLog(
  device: DiscoveredDevice,
  links: TransportLinks
): Promise<string[]> {
  return tracedOp("fetch_device_log", async () => {
    const via = viaFor(device, links);
    if (via !== "wifi") {
      throw new Error("Device log requires WiFi");
    }
    noteTransport("fetch_device_log", via);
    const payload = await getDeviceLog(device.ip, device.port);
    const lines = Array.isArray(payload.lines) ? payload.lines : [];
    emitTrace({
      level: "info",
      phase: "state",
      action: "fetch_device_log",
      transport: "wifi",
      summary: `device log ${lines.length} line(s)` +
        (payload.cache_reason ? ` cache_reason=${payload.cache_reason}` : ""),
      detail: payload,
    });
    for (const line of lines) {
      emitTrace({
        level: "info",
        phase: "state",
        action: "device_log",
        transport: "wifi",
        summary: String(line),
      });
    }
    return lines.map(String);
  });
}

export async function controlDeleteContent(
  device: DiscoveredDevice,
  links: TransportLinks,
  path: string
): Promise<void> {
  return tracedOp("delete", async () => {
    const via = viaFor(device, links);
    if (!via) throw new Error("No control transport connected");
    noteTransport("delete", via, { path });
    emitTrace({
      level: "info",
      phase: "intent",
      action: "delete",
      transport: via,
      summary: `delete ${path}`,
      detail: { path },
    });

    if (via === "wifi") {
      await deviceDeleteContent(device.ip, device.port, path);
    } else {
      const transport = via === "bluetooth" ? "bluetooth" : "usb";
      const objs = await ndjsonSend(
        transport,
        JSON.stringify({ cmd: "delete", path }),
        "delete"
      );
      assertAck(objs, "delete", transport === "bluetooth" ? "BLE" : "USB");
    }
    await snapshotState("delete", device, links, via);
  });
}

export async function controlRenameContent(
  device: DiscoveredDevice,
  links: TransportLinks,
  from: string,
  to: string
): Promise<void> {
  return tracedOp("rename", async () => {
    const via = viaFor(device, links);
    if (!via) throw new Error("No control transport connected");
    noteTransport("rename", via, { from, to });

    if (via === "wifi") {
      await deviceRenameContent(device.ip, device.port, from, to);
    } else {
      const transport = via === "bluetooth" ? "bluetooth" : "usb";
      const objs = await ndjsonSend(
        transport,
        JSON.stringify({ cmd: "rename", from, to }),
        "rename"
      );
      assertAck(objs, "rename", transport === "bluetooth" ? "BLE" : "USB");
    }
  });
}

export async function controlSetSequenceFps(
  device: DiscoveredDevice,
  links: TransportLinks,
  path: string,
  fps: number
): Promise<void> {
  return tracedOp("set_meta", async () => {
    const via = viaFor(device, links);
    if (!via) throw new Error("No control transport connected");
    noteTransport("set_meta", via, { path, fps });
    emitTrace({
      level: "info",
      phase: "intent",
      action: "set_meta",
      transport: via,
      summary: `set_meta ${path} fps=${fps}`,
      detail: { path, fps },
    });

    if (via === "wifi") {
      await deviceSetContentMeta(device.ip, device.port, path, fps);
    } else {
      const transport = via === "bluetooth" ? "bluetooth" : "usb";
      const objs = await ndjsonSend(
        transport,
        JSON.stringify({ cmd: "set_meta", path, fps }),
        "set_meta"
      );
      assertAck(objs, "set_meta", transport === "bluetooth" ? "BLE" : "USB");
    }

    /* Verify: list entry + live status (raw calls — keep same opId). */
    try {
      let items: ContentList["items"] = [];
      if (via === "wifi") {
        const raw = await getDeviceContent(device.ip, device.port);
        items = (raw.items || []).map((item) =>
          normalizeContentEntry(item as unknown as Record<string, unknown>)
        );
      } else {
        const transport = via === "bluetooth" ? "bluetooth" : "usb";
        const raw = await ndjsonSendExpectType(
          transport,
          '{"cmd":"list"}',
          "list",
          5,
          "set_meta"
        );
        items = (Array.isArray(raw.items) ? raw.items : []).map((item) =>
          normalizeContentEntry(item as Record<string, unknown>)
        );
      }
      const entry = items.find((i) => i.path === path);
      emitTrace({
        level: entry?.fps === fps ? "info" : "warn",
        phase: "state",
        action: "set_meta",
        transport: via,
        summary: entry
          ? `list after set_meta: ${entry.path} fps=${entry.fps ?? "—"} (wanted ${fps})`
          : `list after set_meta: path ${path} not found`,
        detail: { entry, wantedFps: fps },
      });
    } catch (err) {
      emitTrace({
        level: "warn",
        phase: "state",
        action: "set_meta",
        transport: via,
        summary: `list verify failed: ${String(err)}`,
      });
    }
    await snapshotState("set_meta", device, links, via);
  });
}
