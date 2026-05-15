import { useState, useEffect, useRef } from "react";
import { listen } from "@tauri-apps/api/event";
import type { PiDaemonStatus, PiInstallProgress } from "../lib/types";
import {
  piTestConnection,
  piCheckDaemon,
  piInstallDaemon,
  piUninstallDaemon,
} from "../lib/api";

export function PiSetupPanel() {
  // SSH connection
  const [host, setHost] = useState("retropie.local");
  const [sshPort, setSshPort] = useState("22");
  const [username, setUsername] = useState("pi");
  const [password, setPassword] = useState("");
  const [keyPath, setKeyPath] = useState("");
  const [useKey, setUseKey] = useState(false);

  // Connection status
  const [connected, setConnected] = useState(false);
  const [connectionInfo, setConnectionInfo] = useState<string | null>(null);
  const [connectionError, setConnectionError] = useState<string | null>(null);
  const [connecting, setConnecting] = useState(false);

  // Daemon status
  const [daemonStatus, setDaemonStatus] = useState<PiDaemonStatus | null>(null);

  // Install config
  const [deviceHost, setDeviceHost] = useState("192.168.1.154");
  const [devicePort, setDevicePort] = useState("8088");
  const [transport, setTransport] = useState("wifi");
  const [serialDevice, setSerialDevice] = useState("/dev/ttyACM0");
  const [toolsDir, setToolsDir] = useState("");

  // Install progress
  const [installing, setInstalling] = useState(false);
  const [log, setLog] = useState<string[]>([]);
  const [error, setError] = useState<string | null>(null);
  const [success, setSuccess] = useState(false);
  const logEndRef = useRef<HTMLDivElement>(null);

  // Listen for install progress events
  useEffect(() => {
    const unlisten = listen<PiInstallProgress>("pi-install-progress", (event) => {
      const p = event.payload;
      setLog((prev) => [...prev, `[${p.step_number}/${p.total_steps}] ${p.message}`]);
      if (p.done) {
        setInstalling(false);
        if (p.error) {
          setError(p.error);
        } else {
          setSuccess(true);
          refreshDaemonStatus();
        }
      }
    });
    return () => {
      unlisten.then((fn) => fn());
    };
  }, []);

  useEffect(() => {
    logEndRef.current?.scrollIntoView({ behavior: "smooth" });
  }, [log]);

  const handleTestConnection = async () => {
    setConnecting(true);
    setConnectionError(null);
    setConnectionInfo(null);
    setConnected(false);
    setDaemonStatus(null);

    try {
      const info = await piTestConnection(
        host,
        parseInt(sshPort),
        username,
        useKey ? undefined : password,
        useKey ? keyPath : undefined
      );
      setConnected(true);
      setConnectionInfo(info);
      refreshDaemonStatus();
    } catch (err) {
      setConnectionError(String(err));
    } finally {
      setConnecting(false);
    }
  };

  const refreshDaemonStatus = async () => {
    try {
      const status = await piCheckDaemon(
        host,
        parseInt(sshPort),
        username,
        useKey ? undefined : password,
        useKey ? keyPath : undefined
      );
      setDaemonStatus(status);
    } catch {
      // Ignore — connection might be closed
    }
  };

  const handleInstall = async () => {
    setInstalling(true);
    setLog([]);
    setError(null);
    setSuccess(false);

    try {
      await piInstallDaemon(
        host,
        parseInt(sshPort),
        username,
        useKey ? undefined : password,
        useKey ? keyPath : undefined,
        deviceHost,
        parseInt(devicePort),
        transport,
        transport === "serial" ? serialDevice : undefined,
        toolsDir
      );
    } catch (err) {
      setError(String(err));
      setInstalling(false);
    }
  };

  const handleUninstall = async () => {
    setInstalling(true);
    setLog([]);
    setError(null);
    setSuccess(false);

    try {
      await piUninstallDaemon(
        host,
        parseInt(sshPort),
        username,
        useKey ? undefined : password,
        useKey ? keyPath : undefined
      );
    } catch (err) {
      setError(String(err));
      setInstalling(false);
    }
  };

  return (
    <div>
      <div className="flex items-center justify-between mb-6">
        <div>
          <h2 className="text-xl font-bold">Raspberry Pi Setup</h2>
          <p className="text-sm text-gray-500">
            Install dumpster-diver daemon on a RetroPie system via SSH
          </p>
        </div>
        <span className="px-2 py-1 text-xs rounded bg-pd-green/20 text-pd-green">
          RetroPie
        </span>
      </div>

      {/* SSH Connection */}
      <div className="bg-pd-panel rounded-lg p-4 border border-pd-border mb-4">
        <h3 className="font-semibold text-sm mb-3">SSH Connection</h3>
        <div className="space-y-3">
          <div className="grid grid-cols-3 gap-3">
            <div className="col-span-2">
              <label className="block text-xs text-gray-500 mb-1">Host</label>
              <input
                type="text"
                value={host}
                onChange={(e) => setHost(e.target.value)}
                placeholder="retropie.local or IP"
                className="w-full px-3 py-2 text-sm bg-pd-dark border border-pd-border rounded"
              />
            </div>
            <div>
              <label className="block text-xs text-gray-500 mb-1">Port</label>
              <input
                type="text"
                value={sshPort}
                onChange={(e) => setSshPort(e.target.value)}
                className="w-full px-3 py-2 text-sm bg-pd-dark border border-pd-border rounded"
              />
            </div>
          </div>

          <div>
            <label className="block text-xs text-gray-500 mb-1">Username</label>
            <input
              type="text"
              value={username}
              onChange={(e) => setUsername(e.target.value)}
              className="w-full px-3 py-2 text-sm bg-pd-dark border border-pd-border rounded"
            />
          </div>

          <div className="flex items-center gap-3 text-sm">
            <label className="flex items-center gap-2 cursor-pointer">
              <input
                type="radio"
                checked={!useKey}
                onChange={() => setUseKey(false)}
                className="accent-pd-accent"
              />
              Password
            </label>
            <label className="flex items-center gap-2 cursor-pointer">
              <input
                type="radio"
                checked={useKey}
                onChange={() => setUseKey(true)}
                className="accent-pd-accent"
              />
              SSH Key
            </label>
          </div>

          {!useKey ? (
            <div>
              <label className="block text-xs text-gray-500 mb-1">Password</label>
              <input
                type="password"
                value={password}
                onChange={(e) => setPassword(e.target.value)}
                className="w-full px-3 py-2 text-sm bg-pd-dark border border-pd-border rounded"
              />
            </div>
          ) : (
            <div>
              <label className="block text-xs text-gray-500 mb-1">Key Path</label>
              <input
                type="text"
                value={keyPath}
                onChange={(e) => setKeyPath(e.target.value)}
                placeholder="~/.ssh/id_rsa"
                className="w-full px-3 py-2 text-sm bg-pd-dark border border-pd-border rounded"
              />
            </div>
          )}

          <button
            onClick={handleTestConnection}
            disabled={connecting || !host || !username}
            className="px-4 py-2 text-sm bg-pd-accent hover:bg-indigo-600 rounded transition disabled:opacity-50"
          >
            {connecting ? "Connecting..." : "Test Connection"}
          </button>

          {connectionInfo && (
            <div className="p-3 bg-pd-green/10 border border-pd-green/30 rounded text-sm text-pd-green">
              Connected: {connectionInfo}
            </div>
          )}
          {connectionError && (
            <div className="p-3 bg-pd-red/10 border border-pd-red/30 rounded text-sm text-pd-red">
              {connectionError}
            </div>
          )}
        </div>
      </div>

      {/* Daemon Status (shown after connection) */}
      {connected && daemonStatus && (
        <div className="bg-pd-panel rounded-lg p-4 border border-pd-border mb-4">
          <h3 className="font-semibold text-sm mb-3">Daemon Status</h3>
          <div className="grid grid-cols-2 gap-3 text-sm">
            <div className="flex justify-between">
              <span className="text-gray-500">Installed</span>
              <span className={daemonStatus.installed ? "text-pd-green" : "text-gray-500"}>
                {daemonStatus.installed ? "Yes" : "No"}
              </span>
            </div>
            <div className="flex justify-between">
              <span className="text-gray-500">Running</span>
              <span className={daemonStatus.running ? "text-pd-green" : "text-gray-500"}>
                {daemonStatus.running ? "Yes" : "No"}
              </span>
            </div>
            <div className="flex justify-between">
              <span className="text-gray-500">Config</span>
              <span>{daemonStatus.config_exists ? "Exists" : "Not found"}</span>
            </div>
          </div>
        </div>
      )}

      {/* Install Configuration (shown after connection) */}
      {connected && (
        <div className="bg-pd-panel rounded-lg p-4 border border-pd-border mb-4">
          <h3 className="font-semibold text-sm mb-3">Device Configuration</h3>
          <div className="space-y-3">
            <div className="grid grid-cols-2 gap-3">
              <div>
                <label className="block text-xs text-gray-500 mb-1">
                  Pixel Dumpster IP
                </label>
                <input
                  type="text"
                  value={deviceHost}
                  onChange={(e) => setDeviceHost(e.target.value)}
                  className="w-full px-3 py-2 text-sm bg-pd-dark border border-pd-border rounded"
                />
              </div>
              <div>
                <label className="block text-xs text-gray-500 mb-1">Port</label>
                <input
                  type="text"
                  value={devicePort}
                  onChange={(e) => setDevicePort(e.target.value)}
                  className="w-full px-3 py-2 text-sm bg-pd-dark border border-pd-border rounded"
                />
              </div>
            </div>

            <div>
              <label className="block text-xs text-gray-500 mb-1">Transport</label>
              <select
                value={transport}
                onChange={(e) => setTransport(e.target.value)}
                className="w-full px-3 py-2 text-sm bg-pd-dark border border-pd-border rounded"
              >
                <option value="wifi">WiFi</option>
                <option value="serial">Serial</option>
              </select>
            </div>

            {transport === "serial" && (
              <div>
                <label className="block text-xs text-gray-500 mb-1">
                  Serial Device
                </label>
                <input
                  type="text"
                  value={serialDevice}
                  onChange={(e) => setSerialDevice(e.target.value)}
                  className="w-full px-3 py-2 text-sm bg-pd-dark border border-pd-border rounded"
                />
              </div>
            )}

            <div>
              <label className="block text-xs text-gray-500 mb-1">
                Tools Directory (local path to tools/)
              </label>
              <input
                type="text"
                value={toolsDir}
                onChange={(e) => setToolsDir(e.target.value)}
                placeholder="Path containing dumpster-diver binary and es-scripts/"
                className="w-full px-3 py-2 text-sm bg-pd-dark border border-pd-border rounded"
              />
              <p className="text-xs text-gray-600 mt-1">
                Must contain the compiled dumpster-diver binary and es-scripts/ directory
              </p>
            </div>
          </div>
        </div>
      )}

      {/* Action buttons */}
      {connected && (
        <div className="flex gap-3 mb-4">
          <button
            onClick={handleInstall}
            disabled={installing || !toolsDir}
            className="flex-1 px-4 py-3 text-sm font-semibold bg-pd-accent hover:bg-indigo-600 rounded transition disabled:opacity-50 disabled:cursor-not-allowed"
          >
            {installing ? "Installing..." : "Install Daemon"}
          </button>
          {daemonStatus?.installed && (
            <button
              onClick={handleUninstall}
              disabled={installing}
              className="px-4 py-3 text-sm font-semibold bg-pd-red/20 hover:bg-pd-red/30 text-pd-red rounded transition disabled:opacity-50"
            >
              Uninstall
            </button>
          )}
        </div>
      )}

      {/* Error / Success */}
      {error && (
        <div className="mb-4 p-3 bg-pd-red/10 border border-pd-red/30 rounded text-sm text-pd-red">
          {error}
        </div>
      )}
      {success && (
        <div className="mb-4 p-3 bg-pd-green/10 border border-pd-green/30 rounded text-sm text-pd-green">
          Operation complete! Reboot the Pi or start the daemon manually.
        </div>
      )}

      {/* Log output */}
      {log.length > 0 && (
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
                  line.includes("complete") || line.includes("Complete")
                    ? "text-pd-green"
                    : line.includes("ERROR") || line.includes("failed")
                    ? "text-pd-red"
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
