import { useState, useEffect, useCallback } from "react";
import type { DiscoveredDevice, DeviceStatus, ContentEntry } from "../lib/types";
import {
  getDeviceStatus,
  getDeviceContent,
  getDeviceConfig,
  setDeviceConfig,
  devicePlay,
  deviceStop,
  addManualDevice,
  uploadContentToDevice,
} from "../lib/api";
import { testContent } from "../lib/testContent";

interface ContentPanelProps {
  devices: DiscoveredDevice[];
  selected: DiscoveredDevice | null;
  onSelect: (device: DiscoveredDevice) => void;
  onScan: () => void;
  scanning: boolean;
  onDevicesChange: (devices: DiscoveredDevice[]) => void;
}

export function ContentPanel({
  devices,
  selected,
  onSelect,
  onScan,
  scanning,
  onDevicesChange,
}: ContentPanelProps) {
  const [status, setStatus] = useState<DeviceStatus | null>(null);
  const [content, setContent] = useState<ContentEntry[]>([]);
  const [error, setError] = useState<string | null>(null);
  const [showManual, setShowManual] = useState(false);
  const [manualIp, setManualIp] = useState("");
  const [manualPort, setManualPort] = useState("8088");
  const [uploadingTest, setUploadingTest] = useState<string | null>(null);
  const [autoQuantizePalette, setAutoQuantizePalette] = useState(false);
  const [autoQuantizeSaving, setAutoQuantizeSaving] = useState(false);
  const [autoQuantizeSaved, setAutoQuantizeSaved] = useState<string | null>(null);

  const device = selected;

  const refreshStatus = useCallback(async () => {
    if (!device) return;
    try {
      const s = await getDeviceStatus(device.ip, device.port);
      setStatus(s);
      setError(null);
    } catch (err) {
      setError(String(err));
    }
  }, [device]);

  const refreshContent = useCallback(async () => {
    if (!device) return;
    try {
      const c = await getDeviceContent(device.ip, device.port);
      setContent(c.items || []);
    } catch (err) {
      setError(String(err));
    }
  }, [device]);

  const refreshPlaybackConfig = useCallback(async () => {
    if (!device) return;
    try {
      const cfg = (await getDeviceConfig(device.ip, device.port)) as {
        display?: { auto_quantize_palette?: boolean };
      };
      setAutoQuantizePalette(Boolean(cfg?.display?.auto_quantize_palette));
      setAutoQuantizeSaved(null);
    } catch (err) {
      /* Non-fatal — older firmware may not expose the field yet. */
      console.warn("Playback config load error:", err);
    }
  }, [device]);

  useEffect(() => {
    refreshStatus();
    refreshContent();
    refreshPlaybackConfig();
    const interval = setInterval(refreshStatus, 3000);
    return () => clearInterval(interval);
  }, [refreshStatus, refreshContent, refreshPlaybackConfig]);

  const handleAutoQuantizeChange = async (next: boolean) => {
    if (!device) return;
    setAutoQuantizePalette(next);
    setAutoQuantizeSaving(true);
    setAutoQuantizeSaved(null);
    try {
      await setDeviceConfig(device.ip, device.port, {
        display: { auto_quantize_palette: next },
        save: true,
      });
      setAutoQuantizeSaved(
        next
          ? "On — sequences will use a 64-color PSRAM cache on next play."
          : "Off — full-color decoding (slower)."
      );
    } catch (err) {
      setAutoQuantizePalette(!next);
      setAutoQuantizeSaved(`Error: ${String(err)}`);
    } finally {
      setAutoQuantizeSaving(false);
    }
  };

  const handlePlay = async (path: string) => {
    if (!device) return;
    try {
      await devicePlay(device.ip, device.port, path, "fade", 800);
      setTimeout(refreshStatus, 500);
    } catch (err) {
      setError(String(err));
    }
  };

  const handleStop = async () => {
    if (!device) return;
    try {
      await deviceStop(device.ip, device.port);
      setTimeout(refreshStatus, 500);
    } catch (err) {
      setError(String(err));
    }
  };

  const handlePlayTest = async (testPath: string) => {
    if (!device) return;
    setUploadingTest(testPath);
    try {
      if (testPath.startsWith("system/")) {
        await devicePlay(device.ip, device.port, testPath, "fade", 800);
      } else {
        // First try to play - if the device doesn't have it yet, upload then play
        try {
          await devicePlay(device.ip, device.port, testPath, "fade", 800);
        } catch {
          await uploadContentToDevice(device.ip, device.port, testPath);
          await new Promise((resolve) => setTimeout(resolve, 500));
          await devicePlay(device.ip, device.port, testPath, "fade", 800);
        }
      }
      setTimeout(refreshStatus, 500);
      setError(null);
    } catch (err) {
      setError(String(err));
    } finally {
      setUploadingTest(null);
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
    return (
      <div className="space-y-6">
        <div className="flex items-center justify-between">
          <h2 className="text-xl font-bold">Content</h2>
          <button
            onClick={onScan}
            disabled={scanning}
            className="px-4 py-1.5 text-sm bg-pd-accent hover:bg-indigo-600 text-white rounded transition disabled:opacity-50"
          >
            {scanning ? "Scanning..." : "Scan Network"}
          </button>
        </div>

        {scanning ? (
          <div className="flex items-center justify-center h-64">
            <div className="text-center">
              <div className="inline-block w-8 h-8 border-2 border-gray-600 border-t-purple-500 rounded-full animate-spin mb-4" />
              <p className="text-gray-400">Scanning for devices...</p>
            </div>
          </div>
        ) : devices.length > 0 ? (
          <div className="grid grid-cols-1 sm:grid-cols-2 lg:grid-cols-3 gap-3">
            {devices.map((d, i) => (
              <button
                key={i}
                onClick={() => onSelect(d)}
                className="bg-pd-panel border border-pd-border rounded-lg p-4 text-left hover:border-pd-accent/50 transition"
              >
                <div className="flex items-center gap-2 mb-2">
                  <span className="w-2 h-2 rounded-full bg-pd-green" />
                  <span className="font-medium">{d.name}</span>
                </div>
                <div className="text-sm text-gray-500">
                  {d.ip}:{d.port}
                  {d.txt.width && (
                    <span className="ml-2">
                      {d.txt.width}x{d.txt.height}
                    </span>
                  )}
                </div>
              </button>
            ))}
          </div>
        ) : (
          <div className="bg-pd-panel rounded-lg p-8 border border-pd-border text-center">
            <p className="text-gray-400 mb-4">No devices found on the network.</p>
            <p className="text-sm text-gray-600 mb-6">
              Make sure your ESP32 is powered on and connected to WiFi.
            </p>
            <div className="flex gap-2 justify-center">
              <button
                onClick={() => setShowManual(!showManual)}
                className="px-4 py-2 text-sm bg-pd-border hover:bg-gray-600 text-white rounded transition"
              >
                Add Manually
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
      <div className="flex items-center justify-between">
        <div>
          <h2 className="text-xl font-bold">{device.name}</h2>
          <p className="text-sm text-gray-500">
            {device.ip}:{device.port}
            {device.txt.width && (
              <span className="ml-2">{device.txt.width}x{device.txt.height}</span>
            )}
          </p>
        </div>
        <div className="flex items-center gap-2">
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
        <div className="flex items-center justify-between mb-3">
          <h3 className="font-semibold">Content Library ({content.length})</h3>
          <button
            onClick={refreshContent}
            className="px-3 py-1 text-sm bg-pd-border hover:bg-gray-600 rounded transition"
          >
            Refresh
          </button>
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
            No content found on device.
          </p>
        )}
      </div>

      {/* Test Content — built-in samples for trying out animations/transitions */}
      <div>
        <div className="flex items-center justify-between mb-3">
          <h3 className="font-semibold">Test Content</h3>
          <span className="text-xs text-gray-600">Built-in samples</span>
        </div>
        <div className="grid grid-cols-1 sm:grid-cols-2 lg:grid-cols-3 gap-3">
          {testContent.map((item) => (
            <button
              key={item.path}
              onClick={() => handlePlayTest(item.path)}
              disabled={uploadingTest === item.path}
              className="bg-pd-panel/50 border border-pd-border/50 rounded-lg p-3 text-left hover:border-purple-500/50 transition disabled:opacity-50 disabled:cursor-wait"
            >
              <div className="flex items-start justify-between mb-1">
                <div className="text-sm font-medium truncate flex-1">
                  {item.name}
                </div>
                <span className="text-xs px-1.5 py-0.5 rounded bg-purple-600/20 text-purple-400 ml-2 flex-shrink-0">
                  {item.category}
                </span>
              </div>
              <div className="text-xs text-gray-500 mb-1">
                {item.description}
              </div>
              <div className="text-xs text-gray-600">
                {item.path}
              </div>
              {uploadingTest === item.path && (
                <div className="text-xs text-purple-400 mt-1">
                  Uploading...
                </div>
              )}
            </button>
          ))}
        </div>
      </div>
    </div>
  );
}
