import { useState, useEffect, useCallback, useRef } from "react";
import { listen } from "@tauri-apps/api/event";
import type { SerialPortInfo, FlashProgress } from "../lib/types";
import {
  listSerialPorts,
  checkFlashTool,
  checkIdfInstalled,
  flashDevice,
} from "../lib/api";

export function FlashPanel() {
  const [ports, setPorts] = useState<SerialPortInfo[]>([]);
  const [selectedPort, setSelectedPort] = useState<string>("");
  const [flashTool, setFlashTool] = useState<string | null>(null);
  const [flashToolError, setFlashToolError] = useState<string | null>(null);
  const [idfInstalled, setIdfInstalled] = useState(false);
  const [buildFromSource, setBuildFromSource] = useState(false);
  const [firmwareDir, setFirmwareDir] = useState("");
  const [projectDir, setProjectDir] = useState("");
  const [flashing, setFlashing] = useState(false);
  const [log, setLog] = useState<string[]>([]);
  const [progress, setProgress] = useState<number | null>(null);
  const [error, setError] = useState<string | null>(null);
  const [success, setSuccess] = useState(false);
  const logEndRef = useRef<HTMLDivElement>(null);

  const refreshPorts = useCallback(async () => {
    try {
      const p = await listSerialPorts();
      setPorts(p);
      // Auto-select first ESP32 port, or re-select if current port vanished
      const currentStillExists = p.some((port) => port.port_name === selectedPort);
      if (!selectedPort || !currentStillExists) {
        const esp = p.find((port) => port.is_esp32);
        if (esp) setSelectedPort(esp.port_name);
        else if (p.length > 0) setSelectedPort(p[0].port_name);
        else setSelectedPort("");
      }
    } catch (err) {
      console.error("Failed to list ports:", err);
    }
  }, [selectedPort]);

  const checkTools = useCallback(async () => {
    try {
      const tool = await checkFlashTool();
      setFlashTool(tool);
      setFlashToolError(null);
    } catch (err) {
      setFlashTool(null);
      setFlashToolError(String(err));
    }

    try {
      const idf = await checkIdfInstalled();
      setIdfInstalled(idf);
    } catch {
      setIdfInstalled(false);
    }
  }, []);

  useEffect(() => {
    refreshPorts();
    checkTools();
    const interval = setInterval(refreshPorts, 3000);
    return () => clearInterval(interval);
  }, [refreshPorts, checkTools]);

  // Listen for flash progress events
  useEffect(() => {
    const unlisten = listen<FlashProgress>("flash-progress", (event) => {
      const p = event.payload;
      setLog((prev) => [...prev, p.message]);
      if (p.percent !== null) setProgress(p.percent);
      if (p.done) {
        setFlashing(false);
        if (p.error) {
          setError(p.error);
        } else {
          setSuccess(true);
        }
      }
    });

    return () => {
      unlisten.then((fn) => fn());
    };
  }, []);

  // Auto-scroll log
  useEffect(() => {
    logEndRef.current?.scrollIntoView({ behavior: "smooth" });
  }, [log]);

  const handleFlash = async () => {
    if (!selectedPort) return;

    setFlashing(true);
    setLog([]);
    setProgress(0);
    setError(null);
    setSuccess(false);

    // Refresh ports and pick the right one
    let portToFlash = selectedPort;
    try {
      const freshPorts = await listSerialPorts();
      setPorts(freshPorts);
      if (!freshPorts.some((p) => p.port_name === portToFlash)) {
        const esp = freshPorts.find((p) => p.is_esp32);
        const fallback = esp?.port_name || freshPorts[0]?.port_name;
        if (fallback) {
          setLog((prev) => [...prev, `Port ${portToFlash} gone, using ${fallback}`]);
          portToFlash = fallback;
          setSelectedPort(fallback);
        }
      }
    } catch {
      // Port refresh failed, try with current selection
    }

    try {
      const lines = await flashDevice(
        portToFlash,
        buildFromSource ? undefined : firmwareDir || undefined,
        buildFromSource,
        buildFromSource ? projectDir || undefined : undefined
      );
      // Invoke resolved — flash succeeded. Merge any lines we didn't get via events.
      if (lines && lines.length > 0) {
        setLog((prev) => {
          // Only add lines not already in the log (events may have added them)
          const existing = new Set(prev);
          const newLines = lines.filter((l) => !existing.has(l));
          return newLines.length > 0 ? [...prev, ...newLines] : prev;
        });
      }
      setProgress(100);
      setSuccess(true);
    } catch (err) {
      const msg = String(err);
      setError(msg);
      setLog((prev) => [...prev, `ERROR: ${msg}`]);
    } finally {
      setFlashing(false);
    }
  };

  const espPorts = ports.filter((p) => p.is_esp32);
  const otherPorts = ports.filter((p) => !p.is_esp32);

  return (
    <div>
      <div className="flex items-center justify-between mb-6">
        <div>
          <h2 className="text-xl font-bold">Flash Firmware</h2>
          <p className="text-sm text-gray-500">
            Flash pixel-dumpster firmware to an ESP32-S3 via USB
          </p>
        </div>
        <span className="px-2 py-1 text-xs rounded bg-blue-500/20 text-blue-400">
          ESP32-S3
        </span>
      </div>

      {/* Tool status */}
      <div className="bg-pd-panel rounded-lg p-4 border border-pd-border mb-4">
        <h3 className="font-semibold mb-3 text-sm">Prerequisites</h3>
        <div className="space-y-2 text-sm">
          <div className="flex items-center gap-2">
            <span
              className={`w-2 h-2 rounded-full ${
                flashTool ? "bg-pd-green" : "bg-pd-red"
              }`}
            />
            <span className="text-gray-400">Flash Tool:</span>
            {flashTool ? (
              <span className="text-gray-200">{flashTool}</span>
            ) : (
              <span className="text-pd-red">{flashToolError || "Not found"}</span>
            )}
          </div>
          <div className="flex items-center gap-2">
            <span
              className={`w-2 h-2 rounded-full ${
                idfInstalled ? "bg-pd-green" : "bg-gray-600"
              }`}
            />
            <span className="text-gray-400">ESP-IDF:</span>
            <span className={idfInstalled ? "text-gray-200" : "text-gray-600"}>
              {idfInstalled ? "Available" : "Not found (optional, for build-from-source)"}
            </span>
          </div>
        </div>
      </div>

      {/* Port selection */}
      <div className="bg-pd-panel rounded-lg p-4 border border-pd-border mb-4">
        <div className="flex items-center justify-between mb-3">
          <h3 className="font-semibold text-sm">Serial Port</h3>
          <button
            onClick={refreshPorts}
            className="px-3 py-1 text-xs bg-pd-border hover:bg-gray-600 rounded transition"
          >
            Refresh
          </button>
        </div>

        {ports.length === 0 ? (
          <p className="text-sm text-gray-500">
            No serial ports detected. Connect your ESP32-S3 via USB.
          </p>
        ) : (
          <select
            value={selectedPort}
            onChange={(e) => setSelectedPort(e.target.value)}
            className="w-full px-3 py-2 text-sm bg-pd-dark border border-pd-border rounded"
          >
            {espPorts.length > 0 && (
              <optgroup label="ESP32-S3 Devices">
                {espPorts.map((p) => (
                  <option key={p.port_name} value={p.port_name}>
                    {p.port_name} — {p.description}
                    {p.serial_number ? ` (${p.serial_number})` : ""}
                  </option>
                ))}
              </optgroup>
            )}
            {otherPorts.length > 0 && (
              <optgroup label="Other Ports">
                {otherPorts.map((p) => (
                  <option key={p.port_name} value={p.port_name}>
                    {p.port_name} — {p.description}
                  </option>
                ))}
              </optgroup>
            )}
          </select>
        )}
      </div>

      {/* Firmware source */}
      <div className="bg-pd-panel rounded-lg p-4 border border-pd-border mb-4">
        <h3 className="font-semibold text-sm mb-3">Firmware Source</h3>

        <div className="space-y-3">
          <label className="flex items-center gap-3 cursor-pointer">
            <input
              type="radio"
              checked={!buildFromSource}
              onChange={() => setBuildFromSource(false)}
              className="accent-pd-accent"
            />
            <div>
              <div className="text-sm">Pre-built firmware</div>
              <div className="text-xs text-gray-500">
                Flash from a directory containing built binaries
              </div>
            </div>
          </label>

          {!buildFromSource && (
            <div className="ml-6">
              <input
                type="text"
                value={firmwareDir}
                onChange={(e) => setFirmwareDir(e.target.value)}
                placeholder="Path to build/ directory (e.g. /path/to/project/build)"
                className="w-full px-3 py-2 text-sm bg-pd-dark border border-pd-border rounded"
              />
              <p className="text-xs text-gray-600 mt-1">
                Should contain bootloader/bootloader.bin, partition_table/partition-table.bin, pixel-dumpster.bin
              </p>
            </div>
          )}

          <label
            className={`flex items-center gap-3 ${
              idfInstalled ? "cursor-pointer" : "opacity-50 cursor-not-allowed"
            }`}
          >
            <input
              type="radio"
              checked={buildFromSource}
              onChange={() => setBuildFromSource(true)}
              disabled={!idfInstalled}
              className="accent-pd-accent"
            />
            <div>
              <div className="text-sm">Build from source</div>
              <div className="text-xs text-gray-500">
                {idfInstalled
                  ? "Compile firmware with ESP-IDF and flash"
                  : "Requires ESP-IDF to be installed"}
              </div>
            </div>
          </label>

          {buildFromSource && (
            <div className="ml-6">
              <input
                type="text"
                value={projectDir}
                onChange={(e) => setProjectDir(e.target.value)}
                placeholder="Path to project root (contains CMakeLists.txt)"
                className="w-full px-3 py-2 text-sm bg-pd-dark border border-pd-border rounded"
              />
            </div>
          )}
        </div>
      </div>

      {/* Flash button */}
      <div className="mb-4">
        <button
          onClick={handleFlash}
          disabled={flashing || !selectedPort || !flashTool}
          className="w-full px-4 py-3 text-sm font-semibold bg-pd-accent hover:bg-indigo-600 rounded transition disabled:opacity-50 disabled:cursor-not-allowed"
        >
          {flashing
            ? "Flashing..."
            : buildFromSource
            ? "Build & Flash"
            : "Flash Firmware"}
        </button>
      </div>

      {/* Progress bar */}
      {(flashing || success || error) && (
        <div className="mb-4">
          <div className="h-2 bg-pd-border rounded-full overflow-hidden">
            <div
              className={`h-full transition-all duration-300 ${
                error
                  ? "bg-pd-red"
                  : success
                  ? "bg-pd-green"
                  : "bg-pd-accent"
              }`}
              style={{ width: `${progress ?? 0}%` }}
            />
          </div>
          {error && (
            <p className="text-sm text-pd-red mt-2">{error}</p>
          )}
          {success && (
            <p className="text-sm text-pd-green mt-2">
              Flash complete! Device will restart automatically.
            </p>
          )}
        </div>
      )}

      {/* Log output */}
      {(flashing || log.length > 0 || error || success) && (
        <div className="bg-pd-panel rounded-lg border border-pd-border">
          <div className="flex items-center justify-between p-3 border-b border-pd-border">
            <h3 className="font-semibold text-sm">Output</h3>
            <button
              onClick={() => setLog([])}
              className="px-3 py-1 text-xs bg-pd-border hover:bg-gray-600 rounded transition"
            >
              Clear
            </button>
          </div>
          <div className="p-3 max-h-[300px] overflow-y-auto font-mono text-xs leading-relaxed">
            {log.map((line, i) => (
              <div
                key={i}
                className={`py-0.5 ${
                  line.includes("ERROR") || line.includes("failed")
                    ? "text-pd-red"
                    : line.includes("complete") || line.includes("Complete")
                    ? "text-pd-green"
                    : "text-gray-300"
                }`}
              >
                {line}
              </div>
            ))}
            <div ref={logEndRef} />
          </div>
        </div>
      )}
    </div>
  );
}
