import { useCallback, useEffect, useRef, useState } from "react";
import { listSerialPorts, wizardConnect, wizardDisconnect } from "../lib/api";
import { loadTransportPrefs, saveTransportPrefs } from "../lib/transportPrefs";

interface SerialPort {
  port_name: string;
  description: string;
  is_esp32: boolean;
}

interface UsbConnectionCardProps {
  prefsKey?: string;
  /** From TransportLinks.usb */
  linked?: boolean;
  highlighted?: boolean;
  className?: string;
  onConnected?: () => void;
  onDisconnected?: () => void;
}

export function UsbConnectionCard({
  prefsKey = "default",
  linked = false,
  highlighted = false,
  className = "",
  onConnected,
  onDisconnected,
}: UsbConnectionCardProps) {
  const prefsKeyRef = useRef(prefsKey);
  prefsKeyRef.current = prefsKey;

  const [ports, setPorts] = useState<SerialPort[]>([]);
  const [selectedPort, setSelectedPort] = useState(
    () => loadTransportPrefs(prefsKey).serialPort || ""
  );
  const [connecting, setConnecting] = useState(false);
  const [error, setError] = useState<string | null>(null);

  const refreshPorts = useCallback(async () => {
    try {
      const p = await listSerialPorts();
      setPorts(p);
      setSelectedPort((prev) => {
        if (prev && p.some((port) => port.port_name === prev)) return prev;
        const remembered = loadTransportPrefs(prefsKeyRef.current).serialPort;
        if (remembered && p.some((port) => port.port_name === remembered)) {
          return remembered;
        }
        const esp = p.find((port) => port.is_esp32);
        if (esp) return esp.port_name;
        return p[0]?.port_name || "";
      });
    } catch (err) {
      console.error("Failed to list ports:", err);
    }
  }, []);

  useEffect(() => {
    refreshPorts();
    const interval = setInterval(refreshPorts, 3000);
    return () => clearInterval(interval);
  }, [refreshPorts]);

  useEffect(() => {
    if (selectedPort) {
      saveTransportPrefs(prefsKeyRef.current, {
        linkKind: "usb",
        serialPort: selectedPort,
      });
    }
  }, [selectedPort]);

  const handleConnect = async () => {
    if (!selectedPort) return;
    setConnecting(true);
    setError(null);
    try {
      await wizardConnect(selectedPort);
      saveTransportPrefs(prefsKeyRef.current, {
        linkKind: "usb",
        serialPort: selectedPort,
      });
      onConnected?.();
    } catch (err) {
      setError(String(err));
    } finally {
      setConnecting(false);
    }
  };

  const handleDisconnect = async () => {
    setError(null);
    try {
      await wizardDisconnect();
    } catch {
      /* ignore */
    }
    onDisconnected?.();
  };

  return (
    <div
      id="pd-card-usb"
      className={`bg-pd-panel rounded-lg p-4 border transition scroll-mt-4 ${
        highlighted
          ? "border-pd-accent ring-2 ring-pd-accent/40"
          : "border-pd-border"
      } ${className}`}
    >
      <div className="flex items-center justify-between mb-3">
        <h3 className="font-semibold">USB Serial</h3>
        <span
          className={`text-xs px-2 py-0.5 rounded ${
            linked
              ? "bg-pd-green/15 text-pd-green"
              : "bg-pd-bg text-gray-500 border border-pd-border"
          }`}
        >
          {linked ? "Connected" : "Not connected"}
        </span>
      </div>

      <div className="flex items-center gap-2 flex-wrap">
        <select
          value={selectedPort}
          onChange={(e) => setSelectedPort(e.target.value)}
          className="flex-1 min-w-[12rem] bg-pd-bg border border-pd-border rounded px-3 py-1.5 text-sm"
          disabled={linked}
        >
          <option value="">Select port...</option>
          {ports.map((p) => (
            <option key={p.port_name} value={p.port_name}>
              {p.port_name}
              {p.is_esp32 ? " (ESP32)" : ""}
              {p.description ? ` — ${p.description}` : ""}
            </option>
          ))}
        </select>
        {!linked ? (
          <button
            type="button"
            onClick={handleConnect}
            disabled={connecting || !selectedPort}
            className="px-4 py-1.5 bg-pd-accent hover:bg-indigo-600 text-white text-sm rounded transition disabled:opacity-50"
          >
            {connecting ? "Connecting…" : "Connect"}
          </button>
        ) : (
          <button
            type="button"
            onClick={handleDisconnect}
            className="px-4 py-1.5 bg-red-600 hover:bg-red-500 text-white text-sm rounded transition"
          >
            Disconnect
          </button>
        )}
      </div>

      {linked && selectedPort && (
        <p className="mt-2 text-xs text-pd-green">Connected via {selectedPort}</p>
      )}
      {error && (
        <p className="mt-2 text-xs text-pd-red">{error}</p>
      )}
    </div>
  );
}
