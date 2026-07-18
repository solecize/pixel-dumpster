import { useEffect, useRef, useState } from "react";
import {
  bleConnect,
  bleDisconnect,
  bleIsConnected,
  bleScan,
  type BleDeviceInfo,
} from "../lib/api";
import { loadTransportPrefs, saveTransportPrefs } from "../lib/transportPrefs";

interface BluetoothConnectionCardProps {
  prefsKey?: string;
  /** From TransportLinks.bluetooth */
  linked?: boolean;
  highlighted?: boolean;
  className?: string;
  /** Attempt to reconnect the last BLE device on mount. */
  autoReconnect?: boolean;
  onConnected?: () => void;
  onDisconnected?: () => void;
}

export function BluetoothConnectionCard({
  prefsKey = "default",
  linked = false,
  highlighted = false,
  className = "",
  autoReconnect = false,
  onConnected,
  onDisconnected,
}: BluetoothConnectionCardProps) {
  const prefsKeyRef = useRef(prefsKey);
  prefsKeyRef.current = prefsKey;
  const autoTriedRef = useRef(false);

  const [bleDevices, setBleDevices] = useState<BleDeviceInfo[]>(() => {
    const saved = loadTransportPrefs(prefsKey);
    return saved.bleId
      ? [{ id: saved.bleId, name: saved.bleName || "pixel-dumpster", rssi: null }]
      : [];
  });
  const [selectedBleId, setSelectedBleId] = useState(
    () => loadTransportPrefs(prefsKey).bleId || ""
  );
  const [scanning, setScanning] = useState(false);
  const [connecting, setConnecting] = useState(false);
  const [error, setError] = useState<string | null>(null);

  useEffect(() => {
    if (!selectedBleId) return;
    const name =
      bleDevices.find((d) => d.id === selectedBleId)?.name ||
      loadTransportPrefs(prefsKeyRef.current).bleName ||
      "pixel-dumpster";
    saveTransportPrefs(prefsKeyRef.current, {
      linkKind: "ble",
      bleId: selectedBleId,
      bleName: name,
    });
  }, [selectedBleId, bleDevices]);

  useEffect(() => {
    if (autoTriedRef.current) return;
    if (!autoReconnect) return;
    autoTriedRef.current = true;
    let cancelled = false;

    (async () => {
      try {
        if (await bleIsConnected()) {
          if (!cancelled) onConnected?.();
          return;
        }
      } catch {
        /* ignore */
      }

      const remembered = loadTransportPrefs(prefsKeyRef.current);
      const id = selectedBleId || remembered.bleId;
      if (!id) return;

      if (!cancelled) {
        setSelectedBleId(id);
        setBleDevices((prev) =>
          prev.some((d) => d.id === id)
            ? prev
            : [
                {
                  id,
                  name: remembered.bleName || "pixel-dumpster",
                  rssi: null,
                },
                ...prev,
              ]
        );
        setConnecting(true);
        setError(null);
      }
      try {
        await bleConnect(id);
        if (cancelled) return;
        saveTransportPrefs(prefsKeyRef.current, {
          linkKind: "ble",
          bleId: id,
          bleName: remembered.bleName || "pixel-dumpster",
        });
        onConnected?.();
      } catch (err) {
        if (!cancelled) setError(`Auto-reconnect failed: ${String(err)}`);
      } finally {
        if (!cancelled) setConnecting(false);
      }
    })();

    return () => {
      cancelled = true;
    };
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [autoReconnect]);

  const handleScan = async () => {
    setScanning(true);
    setError(null);
    try {
      const found = await bleScan("pixel-dumpster");
      const remembered = loadTransportPrefs(prefsKeyRef.current);
      const merged = [...found];
      if (
        remembered.bleId &&
        !merged.some((d) => d.id === remembered.bleId)
      ) {
        merged.unshift({
          id: remembered.bleId,
          name: remembered.bleName || "pixel-dumpster",
          rssi: null,
        });
      }
      setBleDevices(merged);
      if (remembered.bleId && merged.some((d) => d.id === remembered.bleId)) {
        setSelectedBleId(remembered.bleId);
      } else if (found.length > 0 && !selectedBleId) {
        setSelectedBleId(found[0].id);
      }
    } catch (err) {
      setError(String(err));
    } finally {
      setScanning(false);
    }
  };

  const handleConnect = async () => {
    if (!selectedBleId) return;
    setConnecting(true);
    setError(null);
    try {
      const name =
        bleDevices.find((d) => d.id === selectedBleId)?.name ?? selectedBleId;
      await bleConnect(selectedBleId);
      saveTransportPrefs(prefsKeyRef.current, {
        linkKind: "ble",
        bleId: selectedBleId,
        bleName: name,
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
      await bleDisconnect();
    } catch {
      /* ignore */
    }
    onDisconnected?.();
  };

  const savedId = loadTransportPrefs(prefsKey).bleId;

  return (
    <div
      id="pd-card-bluetooth"
      className={`bg-pd-panel rounded-lg p-4 border transition scroll-mt-4 ${
        highlighted
          ? "border-pd-accent ring-2 ring-pd-accent/40"
          : "border-pd-border"
      } ${className}`}
    >
      <div className="flex items-center justify-between mb-3">
        <h3 className="font-semibold">Bluetooth</h3>
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
          value={selectedBleId}
          onChange={(e) => setSelectedBleId(e.target.value)}
          className="flex-1 min-w-[12rem] bg-pd-bg border border-pd-border rounded px-3 py-1.5 text-sm"
          disabled={linked}
        >
          <option value="">Select BLE device...</option>
          {bleDevices.map((d) => {
            const suffix =
              d.rssi != null
                ? ` (${d.rssi} dBm)`
                : d.id === savedId
                  ? " (saved)"
                  : "";
            return (
              <option key={d.id} value={d.id}>
                {d.name}
                {suffix}
              </option>
            );
          })}
        </select>
        {!linked && (
          <button
            type="button"
            onClick={handleScan}
            disabled={scanning}
            className="px-3 py-1.5 bg-pd-bg border border-pd-border text-sm rounded hover:border-pd-accent disabled:opacity-50"
          >
            {scanning ? "Scanning…" : "Scan"}
          </button>
        )}
        {!linked ? (
          <button
            type="button"
            onClick={handleConnect}
            disabled={connecting || !selectedBleId}
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

      {error && (
        <p className="mt-2 text-xs text-pd-red">{error}</p>
      )}
    </div>
  );
}
