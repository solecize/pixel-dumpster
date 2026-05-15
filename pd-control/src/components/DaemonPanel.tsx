import { useState, useEffect, useCallback } from "react";
import type { DiscoveredDevice, DaemonStatus } from "../lib/types";
import {
  getDaemonStatus,
  getDaemonLog,
  daemonReload,
  daemonInjectEvent,
} from "../lib/api";

interface DaemonPanelProps {
  device: DiscoveredDevice;
}

export function DaemonPanel({ device }: DaemonPanelProps) {
  const [tab, setTab] = useState<"status" | "log" | "test" | "config">(
    "status"
  );
  const [status, setStatus] = useState<DaemonStatus | null>(null);
  const [logLines, setLogLines] = useState<string[]>([]);
  const [error, setError] = useState<string | null>(null);

  // Test event form state
  const [eventType, setEventType] = useState("system-select");
  const [eventSystem, setEventSystem] = useState("arcade");
  const [eventGame, setEventGame] = useState("");
  const [eventRom, setEventRom] = useState("");

  const refreshStatus = useCallback(async () => {
    try {
      const s = await getDaemonStatus(device.ip, device.port);
      setStatus(s);
      setError(null);
    } catch (err) {
      setError(String(err));
    }
  }, [device.ip, device.port]);

  const refreshLog = useCallback(async () => {
    try {
      const log = await getDaemonLog(device.ip, device.port);
      setLogLines(log.lines || []);
    } catch (err) {
      setError(String(err));
    }
  }, [device.ip, device.port]);

  useEffect(() => {
    refreshStatus();
    const interval = setInterval(refreshStatus, 3000);
    return () => clearInterval(interval);
  }, [refreshStatus]);

  useEffect(() => {
    if (tab === "log") {
      refreshLog();
      const interval = setInterval(refreshLog, 2000);
      return () => clearInterval(interval);
    }
  }, [tab, refreshLog]);

  const handleReload = async () => {
    try {
      await daemonReload(device.ip, device.port);
      setTimeout(refreshStatus, 500);
    } catch (err) {
      setError(String(err));
    }
  };

  const handleInjectEvent = async () => {
    try {
      await daemonInjectEvent(
        device.ip,
        device.port,
        eventType,
        eventSystem || undefined,
        eventGame || undefined,
        eventRom || undefined
      );
      setTimeout(refreshStatus, 500);
      if (tab === "log") setTimeout(refreshLog, 500);
    } catch (err) {
      setError(String(err));
    }
  };

  const formatUptime = (seconds?: number) => {
    if (!seconds) return "unknown";
    const h = Math.floor(seconds / 3600);
    const m = Math.floor((seconds % 3600) / 60);
    const s = Math.floor(seconds % 60);
    if (h > 0) return `${h}h ${m}m ${s}s`;
    if (m > 0) return `${m}m ${s}s`;
    return `${s}s`;
  };

  const tabs = [
    { id: "status" as const, label: "Status" },
    { id: "log" as const, label: "Log" },
    { id: "test" as const, label: "Test Events" },
    { id: "config" as const, label: "Config" },
  ];

  return (
    <div>
      <div className="flex items-center justify-between mb-6">
        <div>
          <h2 className="text-xl font-bold">{device.name}</h2>
          <p className="text-sm text-gray-500">
            {device.ip}:{device.port}
          </p>
        </div>
        <div className="flex items-center gap-2">
          <button
            onClick={handleReload}
            className="px-3 py-1 text-sm bg-pd-border hover:bg-gray-600 rounded transition"
          >
            Reload Config
          </button>
          <span className="px-2 py-1 text-xs rounded bg-pd-amber/20 text-pd-amber">
            Daemon
          </span>
        </div>
      </div>

      {error && (
        <div className="mb-4 p-3 bg-pd-red/10 border border-pd-red/30 rounded text-sm text-pd-red">
          {error}
        </div>
      )}

      <div className="flex gap-1 mb-6 border-b border-pd-border">
        {tabs.map((t) => (
          <button
            key={t.id}
            onClick={() => setTab(t.id)}
            className={`px-4 py-2 text-sm transition ${
              tab === t.id
                ? "border-b-2 border-pd-accent text-white"
                : "text-gray-500 hover:text-gray-300"
            }`}
          >
            {t.label}
          </button>
        ))}
      </div>

      {tab === "status" && status && (
        <div className="space-y-4">
          <div className="bg-pd-panel rounded-lg p-4 border border-pd-border">
            <h3 className="font-semibold mb-3">Daemon State</h3>
            <div className="grid grid-cols-2 gap-3 text-sm">
              <StatusRow label="Running" value={status.running ? "Yes" : "No"} />
              <StatusRow label="Uptime" value={formatUptime(status.uptime_seconds)} />
              <StatusRow label="Transport" value={status.transport || "unknown"} />
              <StatusRow
                label="Target"
                value={
                  status.transport === "serial"
                    ? status.serial_device || "none"
                    : `${status.device_host}:${status.device_port}`
                }
              />
              <StatusRow label="Current System" value={status.current_system || "none"} />
              <StatusRow label="Last Event" value={status.last_event || "none"} />
              <StatusRow label="Event Detail" value={status.last_event_detail || "none"} />
              <StatusRow label="Verbose" value={status.verbose ? "On" : "Off"} />
              <StatusRow label="Dry Run" value={status.dry_run ? "On" : "Off"} />
            </div>
          </div>

          {status.events && (
            <div className="bg-pd-panel rounded-lg p-4 border border-pd-border">
              <h3 className="font-semibold mb-3">Event Enables</h3>
              <div className="grid grid-cols-2 gap-3 text-sm">
                <StatusRow label="Game Select" value={status.events.game_select ? "On" : "Off"} />
                <StatusRow label="Game Launch" value={status.events.game_launch ? "On" : "Off"} />
                <StatusRow label="Game End" value={status.events.game_end ? "On" : "Off"} />
                <StatusRow label="System Select" value={status.events.system_select ? "On" : "Off"} />
              </div>
            </div>
          )}
        </div>
      )}

      {tab === "status" && !status && (
        <p className="text-gray-500 text-sm">Loading...</p>
      )}

      {tab === "log" && (
        <div className="bg-pd-panel rounded-lg border border-pd-border">
          <div className="flex items-center justify-between p-3 border-b border-pd-border">
            <h3 className="font-semibold text-sm">
              Event Log ({logLines.length} lines)
            </h3>
            <button
              onClick={refreshLog}
              className="px-3 py-1 text-xs bg-pd-border hover:bg-gray-600 rounded transition"
            >
              Refresh
            </button>
          </div>
          <div className="p-3 max-h-[600px] overflow-y-auto font-mono text-xs leading-relaxed">
            {logLines.length > 0 ? (
              logLines.map((line, i) => (
                <div
                  key={i}
                  className={`py-0.5 ${
                    line.includes("ERROR")
                      ? "text-pd-red"
                      : line.includes("[V]")
                      ? "text-gray-600"
                      : "text-gray-300"
                  }`}
                >
                  {line}
                </div>
              ))
            ) : (
              <p className="text-gray-600">No log entries.</p>
            )}
          </div>
        </div>
      )}

      {tab === "test" && (
        <div className="bg-pd-panel rounded-lg p-4 border border-pd-border space-y-4">
          <h3 className="font-semibold">Inject Test Event</h3>
          <div className="space-y-3">
            <div>
              <label className="block text-xs text-gray-500 mb-1">
                Event Type
              </label>
              <select
                value={eventType}
                onChange={(e) => setEventType(e.target.value)}
                className="w-full px-3 py-2 text-sm bg-pd-dark border border-pd-border rounded"
              >
                <option value="system-select">system-select</option>
                <option value="game-select">game-select</option>
                <option value="game-start">game-start</option>
                <option value="game-end">game-end</option>
              </select>
            </div>
            <div>
              <label className="block text-xs text-gray-500 mb-1">System</label>
              <input
                type="text"
                value={eventSystem}
                onChange={(e) => setEventSystem(e.target.value)}
                placeholder="e.g. arcade, snes, nes"
                className="w-full px-3 py-2 text-sm bg-pd-dark border border-pd-border rounded"
              />
            </div>
            <div>
              <label className="block text-xs text-gray-500 mb-1">
                Game Name
              </label>
              <input
                type="text"
                value={eventGame}
                onChange={(e) => setEventGame(e.target.value)}
                placeholder="e.g. Pac-Man"
                className="w-full px-3 py-2 text-sm bg-pd-dark border border-pd-border rounded"
              />
            </div>
            <div>
              <label className="block text-xs text-gray-500 mb-1">
                ROM Path
              </label>
              <input
                type="text"
                value={eventRom}
                onChange={(e) => setEventRom(e.target.value)}
                placeholder="e.g. /home/pi/RetroPie/roms/arcade/pacman.zip"
                className="w-full px-3 py-2 text-sm bg-pd-dark border border-pd-border rounded"
              />
            </div>
            <button
              onClick={handleInjectEvent}
              className="px-4 py-2 text-sm bg-pd-accent hover:bg-indigo-600 rounded transition"
            >
              Send Event
            </button>
          </div>
        </div>
      )}

      {tab === "config" && (
        <div className="bg-pd-panel rounded-lg p-4 border border-pd-border">
          <h3 className="font-semibold mb-3">Daemon Configuration</h3>
          <p className="text-gray-500 text-sm">
            Configuration editor coming soon.
          </p>
        </div>
      )}
    </div>
  );
}

function StatusRow({ label, value }: { label: string; value: string }) {
  return (
    <div className="flex justify-between">
      <span className="text-gray-500">{label}</span>
      <span className="text-right truncate max-w-[200px]">{value}</span>
    </div>
  );
}
