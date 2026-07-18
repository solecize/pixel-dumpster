import { useState, useEffect, useCallback, useRef } from "react";
import { open } from "@tauri-apps/plugin-dialog";
import type { DiscoveredDevice, DeviceStatus, ContentEntry } from "../lib/types";
import { addManualDevice } from "../lib/api";
import {
  controlGetAutoQuantize,
  controlListContent,
  controlPlay,
  controlSetAutoQuantize,
  controlStatus,
  controlStop,
  controlUploadLocal,
  preferredControlTransport,
} from "../lib/deviceControl";
import type { TransportKind, TransportLinks } from "../lib/transport";
import { EMPTY_TRANSPORT } from "../lib/transport";
import {
  loadAutoQuantizePref,
  saveAutoQuantizePref,
} from "../lib/deviceSession";
import type { ControlViaPref } from "../lib/transportPrefs";
import { DeviceCard } from "./DeviceCard";
import { TransportDock } from "./TransportDock";
import { ControlViaPicker } from "./ControlViaPicker";

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
    kind: TransportKind
  ) => void;
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

  const device = selected;
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
  }, [device?.name, device?.ip]);

  const refreshStatus = useCallback(async () => {
    if (!device || busyRef.current) return;
    const l = linksRef.current;
    if (!preferredControlTransport(l, preferRef.current)) return;
    try {
      const s = await controlStatus(device, l);
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

  const refreshContent = useCallback(async () => {
    if (!device) return;
    const l = linksRef.current;
    if (!preferredControlTransport(l, preferRef.current)) return;
    busyRef.current = true;
    try {
      const c = await controlListContent(device, l);
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
    try {
      const aq = await controlGetAutoQuantize(device, linksRef.current);
      if (aq !== null) {
        setAutoQuantizePalette(aq);
        saveAutoQuantizePref(device, aq);
        setAutoQuantizeSaved(null);
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

  const handlePlay = async (path: string) => {
    if (!device) return;
    busyRef.current = true;
    try {
      /* Instant cut — fade 800ms was a large chunk of BLE "lag" for stills. */
      await controlPlay(device, links, path, "none", 0);
      setTimeout(refreshStatus, 400);
    } catch (err) {
      setError(String(err));
    } finally {
      busyRef.current = false;
    }
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
        await controlPlay(device, links, remote, "none", 0);
        setTimeout(refreshStatus, 400);
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
          <ControlViaPicker
            links={links}
            value={controlVia}
            onChange={(kind) => onControlViaChange?.(kind)}
          />
          <TransportDock
            links={links}
            activeControl={controlVia}
            onConfigure={(kind) => onConfigureTransport(device, kind)}
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

      {/* Now Playing */}
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
        {status ? (
          <div className="space-y-2 text-sm">
            <div className="flex justify-between">
              <span className="text-gray-500">Status</span>
              <span>{status.playing ? "Playing" : "Stopped"}</span>
            </div>
            {status.path && (
              <div className="flex justify-between">
                <span className="text-gray-500">Path</span>
                <span className="text-right truncate max-w-xs">{status.path}</span>
              </div>
            )}
            {status.is_sequence && (
              <div className="flex justify-between">
                <span className="text-gray-500">Frame</span>
                <span>{status.current_frame}/{status.total_frames} @ {status.fps}fps</span>
              </div>
            )}
          </div>
        ) : (
          <p className="text-gray-500 text-sm">Loading...</p>
        )}
      </div>

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
        <div className="flex items-center justify-between mb-3 gap-2">
          <h3 className="font-semibold">Content Library ({content.length})</h3>
          <div className="flex items-center gap-2">
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
              onClick={refreshContent}
              className="px-3 py-1 text-sm bg-pd-border hover:bg-gray-600 rounded transition"
            >
              Refresh
            </button>
          </div>
        </div>
        <div className="grid grid-cols-1 sm:grid-cols-2 lg:grid-cols-3 gap-3">
          {content.map((item) => (
            <button
              key={item.path}
              onClick={() => handlePlay(item.path)}
              className="bg-pd-panel border border-pd-border rounded-lg p-3 text-left hover:border-pd-accent/50 transition"
            >
              <div className="text-sm font-medium truncate">{item.name}</div>
              <div className="text-xs text-gray-500 mt-1 truncate">{item.path}</div>
              {item.is_sequence && (
                <div className="text-xs text-pd-amber mt-1">
                  {item.frame_count} frames @ {item.fps}fps
                </div>
              )}
            </button>
          ))}
        </div>
        {content.length === 0 && (
          <p className="text-gray-500 text-sm text-center py-8">
            No content on device. Upload a PNG to get started.
          </p>
        )}
      </div>
    </div>
  );
}
