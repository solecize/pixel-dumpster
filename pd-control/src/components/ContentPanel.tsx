import { useState, useEffect, useCallback, useRef } from "react";
import { open } from "@tauri-apps/plugin-dialog";
import type { DiscoveredDevice, DeviceStatus, ContentEntry } from "../lib/types";
import { addManualDevice } from "../lib/api";
import {
  controlDeleteContent,
  controlGetAutoQuantize,
  controlGetShowFpsCounter,
  controlListContent,
  controlPlay,
  controlRenameContent,
  controlSetAutoQuantize,
  controlSetSequenceFps,
  controlSetShowFpsCounter,
  controlStatus,
  controlStop,
  controlUploadLocal,
  controlUploadLocalSequence,
  preferredControlTransport,
} from "../lib/deviceControl";
import type { TransportKind, TransportLinks } from "../lib/transport";
import { EMPTY_TRANSPORT } from "../lib/transport";
import {
  loadAutoQuantizePref,
  loadShowFpsCounterPref,
  saveAutoQuantizePref,
  saveShowFpsCounterPref,
} from "../lib/deviceSession";
import { prefsKeyForDevice } from "../lib/transportPrefs";
import type { ControlViaPref } from "../lib/transportPrefs";
import {
  catalogForMode,
  durationForTransition,
  migrateContentPlayPref,
  removeContentPlayPref,
  resolveContentPlayPref,
  setContentPlayMode,
  setContentPlayTransition,
  transitionLabel,
  type ContentPlayMode,
  type TransitionId,
} from "../lib/transitions";
import { DeviceCard } from "./DeviceCard";
import { TransportDock } from "./TransportDock";
import { ControlTransportBar } from "./ControlTransportBar";
import { ContentItemMenu } from "./ContentItemMenu";
import { ContentActionDialog } from "./ContentActionDialog";
import { EventLogPanel } from "./EventLogPanel";

interface ContentPanelProps {
  devices: DiscoveredDevice[];
  selected: DiscoveredDevice | null;
  links?: TransportLinks;
  controlViaPref?: ControlViaPref | null;
  onControlViaChange?: (kind: ControlViaPref) => void;
  onSelect: (device: DiscoveredDevice) => void;
  onScan: () => void;
  scanning: boolean;
  onDevicesChange: (devices: DiscoveredDevice[]) => void;
  onConfigureTransport: (
    device: DiscoveredDevice | null,
    kind: TransportKind,
    opts?: { forceSettings?: boolean }
  ) => void;
}

function basenameOfPath(path: string): string {
  const parts = path.replace(/\\/g, "/").split("/");
  return parts[parts.length - 1] || path;
}

function dirnameOfPath(path: string): string {
  const normalized = path.replace(/\\/g, "/");
  const idx = normalized.lastIndexOf("/");
  return idx >= 0 ? normalized.slice(0, idx) : "";
}

function joinContentPath(dir: string, name: string): string {
  return dir ? `${dir}/${name}` : name;
}

function entryIsSequence(item: ContentEntry): boolean {
  return Boolean(item.is_sequence) || (item.frame_count ?? 0) > 1;
}

type PlayPrefMap = Record<
  string,
  { mode: ContentPlayMode; transition: TransitionId }
>;

function buildPlayPrefMap(
  prefsKey: string,
  items: ContentEntry[]
): PlayPrefMap {
  const map: PlayPrefMap = {};
  for (const item of items) {
    map[item.path] = resolveContentPlayPref(
      prefsKey,
      item.path,
      entryIsSequence(item)
    );
  }
  return map;
}

export function ContentPanel({
  devices,
  selected,
  links = EMPTY_TRANSPORT,
  controlViaPref = null,
  onControlViaChange,
  onSelect,
  onScan,
  scanning,
  onDevicesChange,
  onConfigureTransport,
}: ContentPanelProps) {
  const [status, setStatus] = useState<DeviceStatus | null>(null);
  const [content, setContent] = useState<ContentEntry[]>([]);
  const [error, setError] = useState<string | null>(null);
  const [notice, setNotice] = useState<string | null>(null);
  const [showManual, setShowManual] = useState(false);
  const [manualIp, setManualIp] = useState("");
  const [manualPort, setManualPort] = useState("8088");
  const [uploading, setUploading] = useState(false);
  const [autoQuantizePalette, setAutoQuantizePalette] = useState(() =>
    Boolean(loadAutoQuantizePref(selected))
  );
  const [autoQuantizeSaving, setAutoQuantizeSaving] = useState(false);
  const [autoQuantizeSaved, setAutoQuantizeSaved] = useState<string | null>(null);
  const [showFpsCounter, setShowFpsCounter] = useState(() =>
    Boolean(loadShowFpsCounterPref(selected))
  );
  const [showFpsSaving, setShowFpsSaving] = useState(false);
  const [playPrefs, setPlayPrefs] = useState<PlayPrefMap>({});
  const [actionDialog, setActionDialog] = useState<
    | { kind: "rename"; item: ContentEntry }
    | { kind: "fps"; item: ContentEntry }
    | { kind: "delete"; item: ContentEntry }
    | { kind: "seq-upload"; folderPath: string; folderName: string }
    | null
  >(null);
  const [actionBusy, setActionBusy] = useState(false);

  const device = selected;
  const prefsKey = prefsKeyForDevice(device);
  const controlVia = preferredControlTransport(links, controlViaPref);
  const linksRef = useRef(links);
  linksRef.current = links;
  const preferRef = useRef(controlViaPref);
  preferRef.current = controlViaPref;
  /** Skip background status polls while play/list/upload owns the link. */
  const busyRef = useRef(false);

  useEffect(() => {
    const remembered = loadAutoQuantizePref(device);
    if (remembered !== null) setAutoQuantizePalette(remembered);
    const fpsHud = loadShowFpsCounterPref(device);
    if (fpsHud !== null) setShowFpsCounter(fpsHud);
  }, [device?.name, device?.ip]);

  useEffect(() => {
    setPlayPrefs(buildPlayPrefMap(prefsKey, content));
  }, [prefsKey, content]);

  const refreshStatus = useCallback(async () => {
    if (!device || busyRef.current) return;
    const l = linksRef.current;
    if (!preferredControlTransport(l, preferRef.current)) return;
    try {
      const s = await controlStatus(device, l, { poll: true });
      setStatus(s);
      /* Clear only connectivity-style banners; keep play/action errors until
       * the next explicit action clears them. */
      setError((prev) =>
        prev &&
        /timed out|could not connect|error sending request|No control transport/i.test(
          prev
        )
          ? null
          : prev
      );
    } catch (err) {
      /* Status is polled every few seconds — don't flash a red banner when
       * the device is briefly busy (e.g. building a palette cache). */
      console.warn("status poll failed:", err);
    }
  }, [device]);

  const refreshContent = useCallback(async (opts?: { poll?: boolean }) => {
    if (!device) return;
    const l = linksRef.current;
    if (!preferredControlTransport(l, preferRef.current)) return;
    busyRef.current = true;
    try {
      const c = await controlListContent(device, l, {
        poll: opts?.poll ?? true,
      });
      setContent(c.items || []);
    } catch (err) {
      console.warn("content list failed:", err);
    } finally {
      busyRef.current = false;
    }
  }, [device]);

  const refreshPlaybackConfig = useCallback(async () => {
    if (!device) return;
    const remembered = loadAutoQuantizePref(device);
    if (remembered !== null) setAutoQuantizePalette(remembered);
    const fpsHudPref = loadShowFpsCounterPref(device);
    if (fpsHudPref !== null) setShowFpsCounter(fpsHudPref);
    try {
      const aq = await controlGetAutoQuantize(device, linksRef.current);
      if (aq !== null) {
        setAutoQuantizePalette(aq);
        saveAutoQuantizePref(device, aq);
        setAutoQuantizeSaved(null);
      }
      const sfc = await controlGetShowFpsCounter(device, linksRef.current);
      if (sfc !== null) {
        setShowFpsCounter(sfc);
        saveShowFpsCounterPref(device, sfc);
      }
    } catch (err) {
      /* Non-fatal — older firmware may not expose the field yet. */
      console.warn("Playback config load error:", err);
    }
  }, [device]);

  useEffect(() => {
    refreshStatus();
    refreshContent();
    refreshPlaybackConfig();
    /* BLE/USB status is expensive and used to steal play/list bandwidth.
     * Poll much less often than WiFi HTTP. */
    const ms =
      controlVia === "bluetooth" || controlVia === "usb" ? 15000 : 3000;
    const interval = setInterval(refreshStatus, ms);
    return () => clearInterval(interval);
  }, [
    controlVia,
    controlViaPref,
    refreshStatus,
    refreshContent,
    refreshPlaybackConfig,
  ]);

  const handleAutoQuantizeChange = async (next: boolean) => {
    if (!device) return;
    setAutoQuantizePalette(next);
    saveAutoQuantizePref(device, next);
    setAutoQuantizeSaving(true);
    setAutoQuantizeSaved(null);
    try {
      await controlSetAutoQuantize(device, links, next);
      setAutoQuantizeSaved(
        next
          ? "On — sequences will use a 64-color PSRAM cache on next play."
          : "Off — full-color decoding (slower)."
      );
    } catch (err) {
      setAutoQuantizePalette(!next);
      saveAutoQuantizePref(device, !next);
      setAutoQuantizeSaved(`Error: ${String(err)}`);
    } finally {
      setAutoQuantizeSaving(false);
    }
  };

  const handleShowFpsCounterChange = async (next: boolean) => {
    if (!device) return;
    setShowFpsCounter(next);
    saveShowFpsCounterPref(device, next);
    setShowFpsSaving(true);
    try {
      await controlSetShowFpsCounter(device, links, next);
    } catch (err) {
      setShowFpsCounter(!next);
      saveShowFpsCounterPref(device, !next);
      setError(`Show FPS on panel failed: ${String(err)}`);
    } finally {
      setShowFpsSaving(false);
    }
  };

  const playPath = async (path: string, isSequence?: boolean) => {
    if (!device) return;
    const seq =
      isSequence ??
      entryIsSequence(
        content.find((c) => c.path === path) ?? {
          path,
          name: basenameOfPath(path),
          is_sequence: !path.toLowerCase().endsWith(".png"),
          frame_count: 0,
          fps: 0,
        }
      );
    const pref =
      playPrefs[path] ?? resolveContentPlayPref(prefsKey, path, seq);
    const t = pref.transition;
    await controlPlay(device, links, path, t, durationForTransition(t));
    setTimeout(refreshStatus, 400);
  };

  const handlePlay = async (item: ContentEntry) => {
    if (!device) return;
    setError(null);
    busyRef.current = true;
    try {
      await playPath(item.path, entryIsSequence(item));
    } catch (err) {
      setError(String(err));
    } finally {
      busyRef.current = false;
    }
  };

  const handleCardPlayMode = (item: ContentEntry, mode: ContentPlayMode) => {
    const next = setContentPlayMode(prefsKey, item.path, mode);
    setPlayPrefs((prev) => ({
      ...prev,
      [item.path]: {
        mode: next.mode ?? mode,
        transition: next.transition,
      },
    }));
  };

  const handleCardTransition = (
    item: ContentEntry,
    transition: TransitionId
  ) => {
    const next = setContentPlayTransition(
      prefsKey,
      item.path,
      transition,
      entryIsSequence(item)
    );
    setPlayPrefs((prev) => ({
      ...prev,
      [item.path]: {
        mode: next.mode ?? "animation",
        transition: next.transition,
      },
    }));
  };

  const handleStop = async () => {
    if (!device) return;
    busyRef.current = true;
    try {
      await controlStop(device, links);
      setTimeout(refreshStatus, 500);
    } catch (err) {
      setError(String(err));
    } finally {
      busyRef.current = false;
    }
  };

  const runRename = async (item: ContentEntry, nextName: string) => {
    if (!device) return;
    let name = nextName.trim();
    if (!name) return;
    const currentName = basenameOfPath(item.path);
    /* Keep .png on static files if the user omitted the extension. */
    if (
      !item.is_sequence &&
      currentName.toLowerCase().endsWith(".png") &&
      !name.toLowerCase().endsWith(".png")
    ) {
      name = `${name}.png`;
    }
    if (name === currentName) {
      setActionDialog(null);
      return;
    }
    if (/[\\/]/.test(name) || name.includes("..")) {
      setError("Name cannot contain path separators.");
      return;
    }
    const to = joinContentPath(dirnameOfPath(item.path), name);
    setActionBusy(true);
    busyRef.current = true;
    setError(null);
    try {
      await controlRenameContent(device, links, item.path, to);
      migrateContentPlayPref(prefsKey, item.path, to);
      setPlayPrefs((prev) => {
        const next = { ...prev };
        if (next[item.path]) {
          next[to] = next[item.path];
          delete next[item.path];
        }
        return next;
      });
      setNotice(`Renamed to ${to}`);
      setActionDialog(null);
      await refreshContent();
    } catch (err) {
      setError(`Rename failed: ${String(err)}`);
    } finally {
      setActionBusy(false);
      busyRef.current = false;
    }
  };

  const runDelete = async (item: ContentEntry) => {
    if (!device) return;
    setActionBusy(true);
    busyRef.current = true;
    setError(null);
    try {
      await controlDeleteContent(device, links, item.path);
      removeContentPlayPref(prefsKey, item.path);
      setPlayPrefs((prev) => {
        const next = { ...prev };
        delete next[item.path];
        return next;
      });
      setNotice(`Deleted ${item.path}`);
      setActionDialog(null);
      await refreshContent();
      setTimeout(refreshStatus, 300);
    } catch (err) {
      setError(`Delete failed: ${String(err)}`);
    } finally {
      setActionBusy(false);
      busyRef.current = false;
    }
  };

  const runChangeFps = async (item: ContentEntry, raw: string) => {
    if (!device) return;
    const fps = Number.parseInt(raw.trim(), 10);
    if (!Number.isFinite(fps) || fps < 1 || fps > 120) {
      setError("FPS must be an integer from 1 to 120.");
      return;
    }
    setActionBusy(true);
    busyRef.current = true;
    setError(null);
    try {
      await controlSetSequenceFps(device, links, item.path, fps);
      setActionDialog(null);
      await refreshContent();
      /* Re-play so timing reloads from meta (and status FPS updates). */
      const playingThis = Boolean(status?.playing && status.path === item.path);
      if (playingThis) {
        try {
          await playPath(item.path, entryIsSequence(item));
        } catch {
          /* meta was saved; play refresh is best-effort */
        }
      }
      await refreshStatus();
      setNotice(`Set ${item.name} target to ${fps} fps`);
    } catch (err) {
      setError(`Change FPS failed: ${String(err)}`);
    } finally {
      setActionBusy(false);
      busyRef.current = false;
    }
  };

  const handleUpload = async () => {
    if (!device) return;
    if (!preferredControlTransport(links, controlViaPref)) {
      setError("Connect WiFi, Bluetooth, or USB before uploading.");
      return;
    }
    try {
      const selectedPath = await open({
        multiple: false,
        filters: [{ name: "PNG image", extensions: ["png"] }],
      });
      if (!selectedPath || Array.isArray(selectedPath)) return;

      setUploading(true);
      busyRef.current = true;
      setError(null);
      setNotice(null);
      const via = preferredControlTransport(links, controlViaPref);
      const remote = await controlUploadLocal(device, links, selectedPath);
      await refreshContent();
      setNotice(`Uploaded ${remote} via ${via}.`);
      try {
        await playPath(remote, false);
      } catch (playErr) {
        setNotice(
          `Uploaded ${remote} via ${via}, but play failed: ${String(playErr)}`
        );
      }
    } catch (err) {
      const via = preferredControlTransport(links, controlViaPref);
      setError(
        via
          ? `Upload failed (${via}): ${String(err)}`
          : String(err)
      );
    } finally {
      busyRef.current = false;
      setUploading(false);
    }
  };

  const handleUploadSequence = async () => {
    if (!device) return;
    if (!preferredControlTransport(links, controlViaPref)) {
      setError("Connect WiFi, Bluetooth, or USB before uploading.");
      return;
    }
    try {
      const selectedPath = await open({
        directory: true,
        multiple: false,
      });
      if (!selectedPath || Array.isArray(selectedPath)) return;
      const folderName = basenameOfPath(selectedPath);
      setActionDialog({
        kind: "seq-upload",
        folderPath: selectedPath,
        folderName,
      });
    } catch (err) {
      setError(`Could not open folder picker: ${String(err)}`);
    }
  };

  const runSequenceUpload = async (folderPath: string, rawFps: string) => {
    if (!device) return;
    const fps = Number.parseInt(rawFps.trim(), 10);
    if (!Number.isFinite(fps) || fps < 1 || fps > 120) {
      setError("FPS must be an integer from 1 to 120.");
      return;
    }
    setActionBusy(true);
    setUploading(true);
    busyRef.current = true;
    setError(null);
    setNotice(null);
    const via = preferredControlTransport(links, controlViaPref);
    try {
      const remote = await controlUploadLocalSequence(
        device,
        links,
        folderPath,
        fps
      );
      setActionDialog(null);
      await refreshContent();
      try {
        await playPath(remote, true);
        await refreshStatus();
        setNotice(
          `Uploaded sequence ${remote} @ ${fps}fps target via ${via}.`
        );
      } catch (playErr) {
        setNotice(
          `Uploaded ${remote} via ${via}, but play failed: ${String(playErr)}`
        );
      }
    } catch (err) {
      setError(
        via
          ? `Sequence upload failed (${via}): ${String(err)}`
          : String(err)
      );
    } finally {
      busyRef.current = false;
      setUploading(false);
      setActionBusy(false);
    }
  };

  const handleAddManual = async () => {
    if (!manualIp) return;
    try {
      const d = await addManualDevice(manualIp, parseInt(manualPort), "device");
      onDevicesChange([...devices, d]);
      onSelect(d);
      setShowManual(false);
      setManualIp("");
    } catch (err) {
      setError(`Failed to add device: ${String(err)}`);
    }
  };

  // No device selected
  if (!device) {
    const hasOfflineLink = links.bluetooth || links.usb;
    return (
      <div className="space-y-6">
        <div className="flex items-center justify-between gap-2 flex-wrap">
          <h2 className="text-xl font-bold">Content</h2>
          <div className="flex items-center gap-2">
            <TransportDock
              links={links}
              onConfigure={(kind) => onConfigureTransport(null, kind)}
            />
            <button
              onClick={onScan}
              disabled={scanning}
              className="px-4 py-1.5 text-sm bg-pd-accent hover:bg-indigo-600 text-white rounded transition disabled:opacity-50"
            >
              {scanning ? "Scanning..." : "Scan Network"}
            </button>
          </div>
        </div>

        {scanning ? (
          <div className="flex items-center justify-center h-64">
            <div className="text-center">
              <div className="inline-block w-8 h-8 border-2 border-gray-600 border-t-pd-accent rounded-full animate-spin mb-4" />
              <p className="text-gray-400">Scanning for devices...</p>
            </div>
          </div>
        ) : devices.length > 0 ? (
          <div className="grid grid-cols-1 sm:grid-cols-2 lg:grid-cols-3 gap-3">
            {devices.map((d) => (
              <DeviceCard
                key={`${d.ip}:${d.port}`}
                device={d}
                links={{
                  bluetooth: links.bluetooth,
                  usb: links.usb,
                  /* Discovered over mDNS ⇒ reachable on WiFi for listing purposes */
                  wifi: d.ip !== "0.0.0.0",
                }}
                onSelect={onSelect}
                onConfigureTransport={onConfigureTransport}
              />
            ))}
          </div>
        ) : hasOfflineLink ? (
          <div className="bg-pd-panel rounded-lg p-8 border border-pd-border text-center">
            <p className="text-gray-300 mb-2">
              {links.bluetooth ? "Bluetooth" : "USB"} is connected.
            </p>
            <p className="text-sm text-gray-500">
              Preparing content over{" "}
              {links.bluetooth ? "Bluetooth" : "USB"}…
            </p>
          </div>
        ) : (
          <div className="bg-pd-panel rounded-lg p-8 border border-pd-border text-center">
            <p className="text-gray-400 mb-4">No device selected.</p>
            <p className="text-sm text-gray-600 mb-6">
              Connect Bluetooth or USB in Settings, or scan the network when the
              device is on WiFi.
            </p>
            <div className="flex gap-2 justify-center flex-wrap">
              <button
                type="button"
                onClick={() => onConfigureTransport(null, "bluetooth")}
                className="px-4 py-2 text-sm bg-pd-accent hover:bg-indigo-600 text-white rounded transition"
              >
                Connect Bluetooth
              </button>
              <button
                type="button"
                onClick={() => onConfigureTransport(null, "usb")}
                className="px-4 py-2 text-sm bg-pd-border hover:bg-gray-600 text-white rounded transition"
              >
                Connect USB
              </button>
              <button
                onClick={() => setShowManual(!showManual)}
                className="px-4 py-2 text-sm bg-pd-border hover:bg-gray-600 text-white rounded transition"
              >
                Add WiFi Manually
              </button>
            </div>
            {showManual && (
              <div className="mt-4 max-w-sm mx-auto space-y-2">
                <input
                  type="text"
                  placeholder="IP address"
                  value={manualIp}
                  onChange={(e) => setManualIp(e.target.value)}
                  className="w-full px-3 py-2 text-sm bg-pd-dark border border-pd-border rounded"
                />
                <div className="flex gap-2">
                  <input
                    type="text"
                    placeholder="Port"
                    value={manualPort}
                    onChange={(e) => setManualPort(e.target.value)}
                    className="flex-1 px-3 py-2 text-sm bg-pd-dark border border-pd-border rounded"
                  />
                  <button
                    onClick={handleAddManual}
                    className="px-4 py-2 text-sm bg-pd-green hover:bg-green-600 text-black font-medium rounded transition"
                  >
                    Add
                  </button>
                </div>
              </div>
            )}
          </div>
        )}
      </div>
    );
  }

  const playing = Boolean(status?.playing);
  const pathDisplay = status?.path?.trim() || "—";
  const frameDisplay = (() => {
    if (!status?.is_sequence || !status.playing) return "—";
    const target = status.fps ?? "—";
    const achieved =
      typeof status.achieved_fps === "number" && status.achieved_fps > 0
        ? status.achieved_fps.toFixed(1)
        : null;
    const cacheNote =
      status.cache && status.cache !== "off" && status.cache !== "live"
        ? ` · cache ${status.cache}`
        : "";
    const rate = achieved
      ? `target ${target}fps · ~${achieved}fps${cacheNote}`
      : `${target}fps${cacheNote}`;
    return `${status.current_frame ?? 0}/${status.total_frames ?? 0} @ ${rate}`;
  })();

  // Device selected — show Now Playing + Content
  return (
    <div className="space-y-6">
      {/* Header with device selector */}
      <div className="flex items-center justify-between gap-3 flex-wrap">
        <div>
          <h2 className="text-xl font-bold">{device.name}</h2>
          <p className="text-sm text-gray-500">
            {controlVia === "bluetooth"
              ? "via Bluetooth"
              : controlVia === "usb"
                ? "via USB"
                : device.ip !== "0.0.0.0"
                  ? `${device.ip}:${device.port}`
                  : "not reachable over WiFi"}
            {device.txt.width && (
              <span className="ml-2">
                {device.txt.width}x{device.txt.height}
              </span>
            )}
          </p>
        </div>
        <div className="flex items-center gap-2 flex-wrap">
          <ControlTransportBar
            links={links}
            value={controlVia}
            onSelectControl={(kind) => onControlViaChange?.(kind)}
            onOpenSettings={(kind) =>
              onConfigureTransport(device, kind, { forceSettings: true })
            }
          />
          <select
            value={`${device.ip}:${device.port}`}
            onChange={(e) => {
              const [ip, port] = e.target.value.split(":");
              const d = devices.find((x) => x.ip === ip && x.port === parseInt(port));
              if (d) onSelect(d);
            }}
            className="bg-pd-bg border border-pd-border rounded px-3 py-1.5 text-sm"
          >
            {devices.map((d) => (
              <option key={`${d.ip}:${d.port}`} value={`${d.ip}:${d.port}`}>
                {d.name}
              </option>
            ))}
          </select>
          <button
            onClick={onScan}
            disabled={scanning}
            className="px-3 py-1.5 text-sm bg-pd-border hover:bg-gray-600 rounded transition disabled:opacity-50"
          >
            {scanning ? "..." : "Scan"}
          </button>
        </div>
      </div>

      {error && (
        <div className="p-3 bg-pd-red/10 border border-pd-red/30 rounded text-sm text-pd-red">
          {error}
        </div>
      )}
      {notice && (
        <div className="p-3 bg-pd-green/10 border border-pd-green/30 rounded text-sm text-pd-green">
          {notice}
        </div>
      )}

      {/* Now Playing — fixed three rows so height does not jump */}
      <div className="bg-pd-panel rounded-lg p-4 border border-pd-border">
        <div className="flex items-center justify-between mb-3">
          <h3 className="font-semibold">Now Playing</h3>
          <button
            onClick={handleStop}
            className="px-3 py-1 text-sm bg-pd-red/20 text-pd-red hover:bg-pd-red/30 rounded transition"
          >
            Stop
          </button>
        </div>
        <div className="space-y-2 text-sm min-h-[4.5rem]">
          <div className="flex justify-between gap-3">
            <span className="text-gray-500 shrink-0">Status</span>
            <span className="text-right truncate">
              {status ? (playing ? "Playing" : "Stopped") : "…"}
            </span>
          </div>
          <div className="flex justify-between gap-3">
            <span className="text-gray-500 shrink-0">Path</span>
            <span className="text-right truncate max-w-xs" title={pathDisplay}>
              {status ? pathDisplay : "…"}
            </span>
          </div>
          <div className="flex justify-between gap-3">
            <span className="text-gray-500 shrink-0">Frame</span>
            <span className="text-right truncate">
              {status ? frameDisplay : "…"}
            </span>
          </div>
        </div>
        <label className="flex items-start gap-3 cursor-pointer mt-3 pt-3 border-t border-pd-border">
          <input
            type="checkbox"
            className="mt-1"
            checked={showFpsCounter}
            disabled={showFpsSaving}
            onChange={(e) => {
              void handleShowFpsCounterChange(e.target.checked);
            }}
          />
          <span>
            <span className="text-sm text-gray-200">Show FPS on panel</span>
            <span className="block text-xs text-gray-500 mt-0.5">
              Draws target→achieved (e.g. 24&gt;6.2) on the LED panel while a
              sequence plays.
            </span>
          </span>
        </label>
      </div>

      <EventLogPanel device={device} links={links} />

      {/* Playback */}
      <div className="bg-pd-panel rounded-lg p-4 border border-pd-border">
        <h3 className="font-semibold mb-3">Playback</h3>
        <label className="flex items-start gap-3 cursor-pointer">
          <input
            type="checkbox"
            className="mt-1"
            checked={autoQuantizePalette}
            disabled={autoQuantizeSaving}
            onChange={(e) => {
              void handleAutoQuantizeChange(e.target.checked);
            }}
          />
          <span>
            <span className="text-sm text-gray-200">
              Auto-quantize animations to 64 colors (faster playback)
            </span>
            <span className="block text-xs text-gray-500 mt-1">
              When on, each sequence is converted to a shared 64-color palette and
              cached in PSRAM for smoother playback. Stop and play again after
              changing this setting so the cache rebuilds.
            </span>
            {autoQuantizeSaved && (
              <span className="block text-xs text-pd-amber mt-1">
                {autoQuantizeSaved}
              </span>
            )}
          </span>
        </label>
      </div>

      {/* Content Library */}
      <div>
        <div className="flex items-center justify-between mb-3 gap-2 flex-wrap">
          <h3 className="font-semibold">Content Library ({content.length})</h3>
          <div className="flex items-center gap-2 flex-wrap">
            <button
              onClick={handleUpload}
              disabled={
                uploading || !preferredControlTransport(links, controlViaPref)
              }
              className="px-3 py-1 text-sm bg-pd-accent hover:bg-indigo-600 text-white rounded transition disabled:opacity-50"
            >
              {uploading ? "Uploading…" : "Upload PNG"}
            </button>
            <button
              onClick={() => {
                void handleUploadSequence();
              }}
              disabled={
                uploading || !preferredControlTransport(links, controlViaPref)
              }
              className="px-3 py-1 text-sm bg-pd-border hover:bg-gray-600 rounded transition disabled:opacity-50"
              title="Pick a folder of numbered PNGs"
            >
              Upload Sequence
            </button>
            <button
              onClick={() => {
                void refreshContent({ poll: false });
              }}
              className="px-3 py-1 text-sm bg-pd-border hover:bg-gray-600 rounded transition"
            >
              Refresh
            </button>
          </div>
        </div>
        <div className="grid grid-cols-1 sm:grid-cols-2 lg:grid-cols-3 gap-2">
          {content.map((item) => {
            const seq = entryIsSequence(item);
            const pref =
              playPrefs[item.path] ??
              resolveContentPlayPref(prefsKey, item.path, seq);
            const mode = seq ? pref.mode : "animation";
            const catalog = catalogForMode(mode);
            return (
              <div
                key={item.path}
                role="button"
                tabIndex={0}
                onClick={() => void handlePlay(item)}
                onKeyDown={(e) => {
                  if (e.key === "Enter" || e.key === " ") {
                    e.preventDefault();
                    void handlePlay(item);
                  }
                }}
                className="bg-pd-panel border border-pd-border rounded-lg px-3 py-2.5 text-left hover:border-pd-accent/50 transition flex items-center gap-2 w-full cursor-pointer"
              >
                <div className="flex-1 min-w-0">
                  <div className="text-sm font-medium truncate">{item.name}</div>
                  {seq && (
                    <div className="text-xs text-pd-amber mt-0.5 truncate">
                      {item.frame_count ?? "?"} frames
                      {item.fps ? ` @ ${item.fps}fps` : ""}
                      <span className="ml-1.5 text-gray-500">
                        · {mode === "sprite" ? "Sprites" : "Animation"}
                      </span>
                    </div>
                  )}
                  <label
                    className="mt-1.5 flex items-center gap-1.5 text-[11px] text-gray-400"
                    onClick={(e) => e.stopPropagation()}
                    onKeyDown={(e) => e.stopPropagation()}
                  >
                    <span className="shrink-0">Transition</span>
                    <select
                      value={pref.transition}
                      onChange={(e) =>
                        handleCardTransition(
                          item,
                          e.target.value as TransitionId
                        )
                      }
                      className="bg-pd-bg border border-pd-border rounded px-1.5 py-0.5 text-[11px] text-gray-200 max-w-full min-w-0"
                      title="Transition when this item is played"
                    >
                      {catalog.map((id) => (
                        <option key={id} value={id}>
                          {transitionLabel(id)}
                        </option>
                      ))}
                    </select>
                  </label>
                </div>
                <ContentItemMenu
                  item={item}
                  playMode={mode}
                  onPlayModeChange={handleCardPlayMode}
                  onRename={(it) =>
                    setActionDialog({ kind: "rename", item: it })
                  }
                  onDelete={(it) =>
                    setActionDialog({ kind: "delete", item: it })
                  }
                  onChangeFps={(it) =>
                    setActionDialog({ kind: "fps", item: it })
                  }
                />
              </div>
            );
          })}
        </div>
        {content.length === 0 && (
          <p className="text-gray-500 text-sm text-center py-8">
            No content on device. Upload a PNG to get started.
          </p>
        )}
      </div>

      {actionDialog?.kind === "rename" && (
        <ContentActionDialog
          mode={{
            kind: "rename",
            itemName: actionDialog.item.name,
            initial: basenameOfPath(actionDialog.item.path),
          }}
          busy={actionBusy}
          onCancel={() => !actionBusy && setActionDialog(null)}
          onSubmit={(value) => {
            void runRename(actionDialog.item, value);
          }}
        />
      )}
      {actionDialog?.kind === "fps" && (
        <ContentActionDialog
          mode={{
            kind: "fps",
            itemName: actionDialog.item.name,
            initial: actionDialog.item.fps ?? 12,
          }}
          busy={actionBusy}
          onCancel={() => !actionBusy && setActionDialog(null)}
          onSubmit={(value) => {
            void runChangeFps(actionDialog.item, value);
          }}
        />
      )}
      {actionDialog?.kind === "delete" && (
        <ContentActionDialog
          mode={{ kind: "delete", itemName: actionDialog.item.name }}
          busy={actionBusy}
          onCancel={() => !actionBusy && setActionDialog(null)}
          onSubmit={() => {
            void runDelete(actionDialog.item);
          }}
        />
      )}
      {actionDialog?.kind === "seq-upload" && (
        <ContentActionDialog
          mode={{
            kind: "seq-upload",
            folderName: actionDialog.folderName,
            initialFps: 12,
          }}
          busy={actionBusy}
          onCancel={() => !actionBusy && setActionDialog(null)}
          onSubmit={(value) => {
            void runSequenceUpload(actionDialog.folderPath, value);
          }}
        />
      )}
    </div>
  );
}
