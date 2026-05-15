import { useState, useEffect, useCallback, useRef } from "react";
import {
  listSerialPorts,
  wizardConnect,
  wizardDisconnect,
  wizardSend,
  wizardReboot,
  wizardPoll,
} from "../lib/api";

interface SerialPort {
  port_name: string;
  description: string;
  is_esp32: boolean;
}

interface WizardNav {
  back: boolean;
  next: boolean;
}

interface WizardState {
  type: "state";
  step: string;
  step_index: number;
  step_count: number;
  title: string;
  mode: "menu" | "text";
  options?: string[];
  selected?: number;
  value?: string;
  mask?: boolean;
  nav: WizardNav;
}

interface WizardComplete {
  type: "complete";
  config: Record<string, unknown>;
}

interface WizardError {
  type: "error";
  message: string;
}

interface WifiScan {
  type: "wifi_scan";
  scanning: boolean;
  ssids: string[];
}

interface WifiTest {
  type: "wifi_test";
  testing: boolean;
  success?: boolean;
  ip?: string;
}

interface ReztestStatus {
  type: "reztest_status";
  index: number;
  total: number;
  label: string;
  width: number;
  height: number;
  scan_wiring: number;
}

interface PanelLayoutTest {
  type: "panel_layout_test";
  running: boolean;
}

type DeviceMessage =
  | WizardState
  | WizardComplete
  | WizardError
  | WifiScan
  | WifiTest
  | ReztestStatus
  | PanelLayoutTest
  | { type: string; [key: string]: unknown };

export default function WizardPanel() {
  const [ports, setPorts] = useState<SerialPort[]>([]);
  const [selectedPort, setSelectedPort] = useState("");
  const [connected, setConnected] = useState(false);
  const [connecting, setConnecting] = useState(false);
  const [wizardState, setWizardState] = useState<WizardState | null>(null);
  const [complete, setComplete] = useState(false);
  const [completeConfig, setCompleteConfig] = useState<Record<string, unknown> | null>(null);
  const [error, setError] = useState<string | null>(null);
  const [log, setLog] = useState<string[]>([]);
  const [textInput, setTextInput] = useState("");
  const [wifiScanning, setWifiScanning] = useState(false);
  const [wifiTesting, setWifiTesting] = useState(false);
  const [wifiResult, setWifiResult] = useState<{ success: boolean; ip?: string } | null>(null);
  const [reztest, setReztest] = useState<ReztestStatus | null>(null);
  const [panelLayoutTest, setPanelLayoutTest] = useState<PanelLayoutTest | null>(null);
  const [layoutSelectedPanel, setLayoutSelectedPanel] = useState(0);
  const [busy, setBusy] = useState(false);
  const logEndRef = useRef<HTMLDivElement>(null);
  const pollRef = useRef<ReturnType<typeof setInterval> | null>(null);

  const addLog = useCallback((msg: string) => {
    setLog((prev) => [...prev, msg]);
  }, []);

  // Refresh ports
  const refreshPorts = useCallback(async () => {
    try {
      const p = await listSerialPorts();
      setPorts(p);
      if (!selectedPort) {
        const esp = p.find((port) => port.is_esp32);
        if (esp) setSelectedPort(esp.port_name);
        else if (p.length > 0) setSelectedPort(p[0].port_name);
      }
    } catch (err) {
      console.error("Failed to list ports:", err);
    }
  }, [selectedPort]);

  useEffect(() => {
    refreshPorts();
    const interval = setInterval(refreshPorts, 3000);
    return () => clearInterval(interval);
  }, [refreshPorts]);

  // Auto-scroll log
  useEffect(() => {
    logEndRef.current?.scrollIntoView({ behavior: "smooth" });
  }, [log]);

  // Process messages from device
  const processMessages = useCallback(
    (lines: string[]) => {
      for (const line of lines) {
        try {
          const msg: DeviceMessage = JSON.parse(line);
          addLog(`<< ${line}`);
          switch (msg.type) {
            case "state":
              setWizardState(msg as WizardState);
              setTextInput((msg as WizardState).value || "");
              setError(null);
              setReztest(null);
              setBusy(false);
              break;
            case "complete":
              setComplete(true);
              setCompleteConfig((msg as WizardComplete).config);
              setWizardState(null);
              setBusy(false);
              break;
            case "error":
              setError((msg as WizardError).message);
              setBusy(false);
              break;
            case "wifi_scan": {
              const scan = msg as WifiScan;
              setWifiScanning(scan.scanning);
              break;
            }
            case "wifi_test": {
              const test = msg as WifiTest;
              setWifiTesting(test.testing);
              if (!test.testing) {
                setWifiResult({
                  success: test.success ?? false,
                  ip: test.ip,
                });
              }
              break;
            }
            case "reztest_status":
              setReztest(msg as ReztestStatus);
              setWizardState(null);
              break;
            case "reztest_starting":
              addLog("Device entering reztest mode — will reboot...");
              break;
            case "reztest_locked":
              addLog(`Locked in: ${(msg as Record<string, unknown>).label}`);
              break;
            case "reztest_done":
              addLog("Reztest complete — device rebooting...");
              break;
            case "reztest_next":
              addLog(`Skipping to combo #${(msg as Record<string, unknown>).index}`);
              break;
            case "panel_layout_test":
              setPanelLayoutTest(msg as PanelLayoutTest);
              setWizardState(null);
              break;
            case "test_pattern_ack":
              addLog(`Test pattern: ${(msg as Record<string, unknown>).pattern}`);
              break;
          }
        } catch {
          if (line.trim()) addLog(`<< ${line}`);
        }
      }
    },
    [addLog]
  );

  // Poll for messages
  useEffect(() => {
    if (!connected) return;
    pollRef.current = setInterval(async () => {
      try {
        const lines = await wizardPoll();
        if (lines.length > 0) processMessages(lines);
      } catch {
        // ignore poll errors
      }
    }, 500);
    return () => {
      if (pollRef.current) clearInterval(pollRef.current);
    };
  }, [connected, processMessages]);

  const handleConnect = async () => {
    if (!selectedPort) return;
    setConnecting(true);
    setError(null);
    setLog([]);
    setComplete(false);
    setCompleteConfig(null);
    setWizardState(null);
    setReztest(null);
    try {
      addLog(`Connecting to ${selectedPort}...`);
      const lines = await wizardConnect(selectedPort);
      setConnected(true);
      addLog("Connected!");
      processMessages(lines);
    } catch (err) {
      setError(String(err));
      addLog(`ERROR: ${err}`);
    } finally {
      setConnecting(false);
    }
  };

  const handleDisconnect = async () => {
    try {
      await wizardDisconnect();
    } catch {
      // ignore
    }
    setConnected(false);
    setWizardState(null);
    setComplete(false);
    setReztest(null);
    addLog("Disconnected");
  };

  const sendCmd = async (cmd: Record<string, unknown>) => {
    setBusy(true);
    try {
      const json = JSON.stringify(cmd);
      addLog(`>> ${json}`);
      const lines = await wizardSend(json);
      processMessages(lines);
    } catch (err) {
      setError(String(err));
      addLog(`ERROR: ${err}`);
    } finally {
      setBusy(false);
    }
  };

  const handleMenuSelect = (index: number) => {
    sendCmd({ cmd: "select", index });
  };

  const handleTextSubmit = () => {
    if (busy) return;
    setBusy(true);
    sendCmd({ cmd: "input", value: textInput });
    // busy will be cleared when we receive the next state or complete message
  };

  const handleBack = () => {
    sendCmd({ cmd: "nav", dir: "back" });
  };

  const handleNext = () => {
    if (busy) return;
    // On text mode steps, submit the text value (Next behaves like Submit)
    if (wizardState?.mode === "text") {
      setBusy(true);
      sendCmd({ cmd: "input", value: textInput });
    } else {
      sendCmd({ cmd: "nav", dir: "next" });
    }
  };

  const handleScanWifi = () => {
    sendCmd({ cmd: "scan_wifi" });
  };

  const handleReztestKeep = () => {
    sendCmd({ cmd: "reztest_keep" });
  };

  const handleReztestSkip = () => {
    sendCmd({ cmd: "reztest_skip" });
  };

  const handlePanelLayoutConfirm = () => {
    sendCmd({ cmd: "panel_layout_confirm" });
    setPanelLayoutTest(null);
  };

  const handlePanelSelect = (delta: number) => {
    const next = layoutSelectedPanel + delta;
    setLayoutSelectedPanel(next);
    sendCmd({ cmd: "panel_select", index: next });
  };

  return (
    <div>
      <div className="flex items-center justify-between mb-6">
        <div>
          <h2 className="text-xl font-bold">Device Setup Wizard</h2>
          <p className="text-sm text-gray-500">
            Configure your ESP32 device via USB serial
          </p>
        </div>
        <span className="px-2 py-1 text-xs rounded bg-purple-500/20 text-purple-400">
          Serial Wizard
        </span>
      </div>

      {/* Connection */}
      <div className="bg-pd-panel rounded-lg p-4 border border-pd-border mb-4">
        <h3 className="font-semibold text-sm mb-3">Serial Connection</h3>
        <div className="flex items-center gap-2">
          <select
            value={selectedPort}
            onChange={(e) => setSelectedPort(e.target.value)}
            className="flex-1 bg-pd-bg border border-pd-border rounded px-3 py-1.5 text-sm"
            disabled={connected}
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
          {!connected ? (
            <button
              onClick={handleConnect}
              disabled={!selectedPort || connecting}
              className="px-4 py-1.5 bg-purple-600 text-white text-sm rounded hover:bg-purple-500 disabled:opacity-50 disabled:cursor-not-allowed"
            >
              {connecting ? "Connecting..." : "Connect"}
            </button>
          ) : (
            <button
              onClick={handleDisconnect}
              className="px-4 py-1.5 bg-red-600 text-white text-sm rounded hover:bg-red-500"
            >
              Disconnect
            </button>
          )}
        </div>
        {connected && (
          <div className="mt-2 flex items-center gap-2 text-xs text-pd-green">
            <span className="w-2 h-2 rounded-full bg-pd-green" />
            Connected to {selectedPort}
          </div>
        )}
      </div>

      {/* Wizard Step */}
      {wizardState && (
        <div className="bg-pd-panel rounded-lg p-4 border border-pd-border mb-4">
          <div className="flex items-center justify-between mb-3">
            <h3 className="font-semibold text-sm">
              Step {wizardState.step_index + 1} of {wizardState.step_count}
            </h3>
            <span className="text-xs text-gray-500">{wizardState.step}</span>
          </div>

          {/* Progress dots */}
          <div className="flex gap-1 mb-4">
            {Array.from({ length: wizardState.step_count }).map((_, i) => (
              <div
                key={i}
                className={`h-1.5 flex-1 rounded-full ${
                  i < wizardState.step_index
                    ? "bg-purple-600"
                    : i === wizardState.step_index
                    ? "bg-purple-400"
                    : "bg-gray-700"
                }`}
              />
            ))}
          </div>

          <h4 className="text-lg font-medium mb-4">{wizardState.title}</h4>

          {/* WiFi scanning indicator */}
          {wifiScanning && (
            <div className="mb-3 text-sm text-yellow-400 animate-pulse">
              Scanning for WiFi networks...
            </div>
          )}

          {/* WiFi test indicator */}
          {wifiTesting && (
            <div className="mb-3 text-sm text-yellow-400 animate-pulse">
              Testing WiFi connection...
            </div>
          )}

          {/* WiFi test result */}
          {wifiResult && !wifiTesting && (
            <div
              className={`mb-3 text-sm ${
                wifiResult.success ? "text-pd-green" : "text-pd-red"
              }`}
            >
              {wifiResult.success
                ? `WiFi connected! IP: ${wifiResult.ip}`
                : "WiFi connection failed. Please try again."}
            </div>
          )}

          {/* Menu mode */}
          {wizardState.mode === "menu" && wizardState.options && (
            <div className="space-y-1 mb-4">
              {wizardState.options.map((option, i) => (
                <button
                  key={i}
                  onClick={() => handleMenuSelect(i)}
                  disabled={busy}
                  className={`w-full text-left px-3 py-2 rounded text-sm transition-colors ${
                    i === wizardState.selected
                      ? "bg-purple-600/30 border border-purple-500 text-white"
                      : "bg-pd-bg border border-pd-border text-gray-300 hover:bg-pd-bg/80 hover:border-gray-600"
                  } disabled:opacity-50`}
                >
                  <span className="text-gray-500 mr-2">{i + 1}.</span>
                  {option}
                </button>
              ))}
              {wizardState.step === "wifi_ssid" && (
                <button
                  onClick={handleScanWifi}
                  disabled={busy || wifiScanning}
                  className="mt-2 px-3 py-1 text-xs bg-gray-700 rounded hover:bg-gray-600 disabled:opacity-50"
                >
                  Re-scan WiFi
                </button>
              )}
            </div>
          )}

          {/* Text mode — numeric stepper for row/col steps */}
          {wizardState.mode === "text" &&
            (wizardState.step === "panel_rows" || wizardState.step === "panel_cols" ||
             wizardState.step === "panel_res_custom_w" || wizardState.step === "panel_res_custom_h") ? (
            <div className="mb-4">
              <div className="flex items-center gap-3">
                <button
                  onClick={() => setTextInput(v => String(Math.max(1, parseInt(v||"1") - 1)))}
                  disabled={busy}
                  className="w-9 h-9 rounded bg-pd-bg border border-pd-border text-lg hover:bg-gray-700 disabled:opacity-50"
                >
                  −
                </button>
                <input
                  type="number"
                  min={1}
                  max={wizardState.step === "panel_rows" || wizardState.step === "panel_cols" ? 8 : 512}
                  value={textInput}
                  onChange={(e) => setTextInput(e.target.value)}
                  onKeyDown={(e) => { if (e.key === "Enter") handleTextSubmit(); }}
                  disabled={busy}
                  className="w-20 text-center bg-pd-bg border border-pd-border rounded px-2 py-2 text-sm focus:border-purple-500 focus:outline-none disabled:opacity-50"
                  autoFocus
                />
                <button
                  onClick={() => setTextInput(v => String(parseInt(v||"0") + 1))}
                  disabled={busy}
                  className="w-9 h-9 rounded bg-pd-bg border border-pd-border text-lg hover:bg-gray-700 disabled:opacity-50"
                >
                  +
                </button>
                <button
                  onClick={handleTextSubmit}
                  disabled={busy}
                  className="flex-1 px-4 py-2 bg-purple-600 text-white text-sm rounded hover:bg-purple-500 disabled:opacity-50"
                >
                  OK
                </button>
              </div>
            </div>
          ) : wizardState.mode === "text" ? (
            <div className="mb-4">
              <div className="flex gap-2">
                <input
                  type={wizardState.mask ? "password" : "text"}
                  value={textInput}
                  onChange={(e) => setTextInput(e.target.value)}
                  onKeyDown={(e) => {
                    if (e.key === "Enter") handleTextSubmit();
                  }}
                  placeholder={wizardState.title}
                  disabled={busy}
                  className="flex-1 bg-pd-bg border border-pd-border rounded px-3 py-2 text-sm focus:border-purple-500 focus:outline-none disabled:opacity-50"
                  autoFocus
                />
                <button
                  onClick={handleTextSubmit}
                  disabled={busy}
                  className="px-4 py-2 bg-purple-600 text-white text-sm rounded hover:bg-purple-500 disabled:opacity-50"
                >
                  Submit
                </button>
              </div>
              {wizardState.step === "static_ip" && (
                <p className="text-xs text-gray-500 mt-1">
                  Leave blank for DHCP
                </p>
              )}
            </div>
          ) : null}

          {/* Error */}
          {error && (
            <div className="mb-3 px-3 py-2 bg-red-500/10 border border-red-500/30 rounded text-sm text-pd-red">
              {error}
            </div>
          )}

          {/* Navigation */}
          <div className="flex justify-between">
            <button
              onClick={handleBack}
              disabled={!wizardState.nav.back || busy}
              className="px-4 py-1.5 text-sm bg-gray-700 text-gray-300 rounded hover:bg-gray-600 disabled:opacity-30 disabled:cursor-not-allowed"
            >
              Back
            </button>
            <button
              onClick={handleNext}
              disabled={busy}
              className="px-4 py-1.5 text-sm bg-purple-600 text-white rounded hover:bg-purple-500 disabled:opacity-50"
            >
              {wizardState.mode === "text" ? "Submit" : "Next"}
            </button>
          </div>
        </div>
      )}

      {/* Reztest mode */}
      {reztest && (
        <div className="bg-pd-panel rounded-lg p-4 border border-pd-border mb-4">
          <h3 className="font-semibold text-sm mb-3">Resolution Test Mode</h3>
          <p className="text-sm text-gray-400 mb-3">
            Testing combo {reztest.index + 1} of {reztest.total}
          </p>
          <div className="bg-pd-bg rounded p-3 mb-4 text-sm">
            <div className="font-medium text-white mb-1">{reztest.label}</div>
            <div className="text-gray-400">
              {reztest.width}x{reztest.height}, scan wiring: {reztest.scan_wiring}
            </div>
          </div>
          <p className="text-sm text-gray-400 mb-4">
            Does the display look correct?
          </p>
          <div className="flex gap-3">
            <button
              onClick={handleReztestKeep}
              disabled={busy}
              className="flex-1 px-4 py-2 bg-pd-green text-black text-sm font-medium rounded hover:bg-green-400 disabled:opacity-50"
            >
              Yes — Keep This
            </button>
            <button
              onClick={handleReztestSkip}
              disabled={busy}
              className="flex-1 px-4 py-2 bg-gray-600 text-white text-sm rounded hover:bg-gray-500 disabled:opacity-50"
            >
              No — Try Next
            </button>
          </div>
        </div>
      )}

      {/* Panel layout test mode */}
      {panelLayoutTest && (
        <div className="bg-pd-panel rounded-lg p-4 border border-pd-border mb-4">
          <h3 className="font-semibold text-sm mb-3">Panel Layout Test</h3>
          <p className="text-sm text-gray-400 mb-4">
            Each panel shows its number with R/G/B reference squares.
            Use the arrows to highlight each panel and verify wiring.
          </p>
          <div className="flex items-center gap-2 mb-4 text-sm text-yellow-400 animate-pulse">
            <span className="w-2 h-2 rounded-full bg-yellow-400" />
            Panel-layout pattern running…
          </div>
          <div className="flex items-center justify-center gap-4 mb-4">
            <button
              onClick={() => handlePanelSelect(-1)}
              disabled={busy}
              className="w-12 h-12 rounded-lg bg-pd-bg border border-pd-border text-xl hover:bg-gray-700 disabled:opacity-50 flex items-center justify-center"
            >
              ←
            </button>
            <div className="text-center">
              <div className="text-lg font-bold">Panel {layoutSelectedPanel + 1}</div>
              <div className="text-xs text-gray-500">selected</div>
            </div>
            <button
              onClick={() => handlePanelSelect(1)}
              disabled={busy}
              className="w-12 h-12 rounded-lg bg-pd-bg border border-pd-border text-xl hover:bg-gray-700 disabled:opacity-50 flex items-center justify-center"
            >
              →
            </button>
          </div>
          <div className="flex gap-3">
            <button
              onClick={handlePanelLayoutConfirm}
              disabled={busy}
              className="flex-1 px-4 py-2 bg-pd-green text-black text-sm font-medium rounded hover:bg-green-400 disabled:opacity-50"
            >
              Looks Correct — Confirm
            </button>
            <button
              onClick={() => sendCmd({ cmd: "nav", dir: "back" })}
              disabled={busy}
              className="flex-1 px-4 py-2 bg-gray-600 text-white text-sm rounded hover:bg-gray-500 disabled:opacity-50"
            >
              Back — Re-configure
            </button>
          </div>
        </div>
      )}

      {/* Complete */}
      {complete && completeConfig && (
        <div className="bg-pd-panel rounded-lg p-4 border border-pd-border mb-4">
          <div className="flex items-center gap-2 mb-3">
            <span className="w-3 h-3 rounded-full bg-pd-green" />
            <h3 className="font-semibold text-sm text-pd-green">
              Device Already Configured
            </h3>
          </div>
          <p className="text-sm text-gray-400 mb-3">
            This device has a saved configuration. It should connect to WiFi
            automatically. Try <strong>Scan</strong> in the sidebar to find it on
            the network.
          </p>
          <div className="mb-3 px-3 py-2 bg-purple-600/10 border border-purple-500/30 rounded text-sm text-purple-300">
            <strong>Multi-panel setup:</strong> Once discovered on the network, open the device →
            <strong> Diagnostics</strong> tab → <strong>Panel Layout → Edit</strong> to configure rows, cols and panel size.
            Or use <strong>Re-run Setup</strong> below to reconfigure everything from scratch via USB.
          </div>
          <div className="bg-pd-bg rounded p-3 text-sm space-y-1.5">
            <div className="flex justify-between">
              <span className="text-gray-500">Canvas</span>
              <span className="text-gray-200">
                {String(completeConfig.matrix_width)}×{String(completeConfig.matrix_height)}
              </span>
            </div>
            {completeConfig.panel_rows && Number(completeConfig.panel_rows) > 1 || completeConfig.panel_cols && Number(completeConfig.panel_cols) > 1 ? (
              <div className="flex justify-between">
                <span className="text-gray-500">Panels</span>
                <span className="text-gray-200">
                  {String(completeConfig.panel_cols)}×{String(completeConfig.panel_rows)} ({String(completeConfig.panel_width)}×{String(completeConfig.panel_height)} each)
                </span>
              </div>
            ) : null}
            <div className="flex justify-between">
              <span className="text-gray-500">WiFi</span>
              <span className="text-gray-200">{String(completeConfig.wifi_ssid || "—")}</span>
            </div>
            <div className="flex justify-between">
              <span className="text-gray-500">Name</span>
              <span className="text-gray-200">{String(completeConfig.device_name || "—")}</span>
            </div>
            <div className="flex justify-between">
              <span className="text-gray-500">Hostname</span>
              <span className="text-gray-200">{String(completeConfig.hostname || "—")}</span>
            </div>
            <div className="flex justify-between">
              <span className="text-gray-500">IP</span>
              <span className="text-gray-200">
                {completeConfig.static_ip ? String(completeConfig.static_ip) : "DHCP (auto)"}
              </span>
            </div>
          </div>
          <div className="mt-4 flex gap-2">
            <button
              onClick={async () => {
                setBusy(true);
                addLog(">> Rebooting device via hardware reset...");
                try {
                  await wizardReboot();
                } catch {
                  // device disconnects on reboot, that's expected
                }
                setBusy(false);
                addLog("Device is rebooting. Wait ~10s then click Scan in the sidebar.");
                setConnected(false);
                setComplete(false);
                setCompleteConfig(null);
              }}
              disabled={busy}
              className="flex-1 px-4 py-2 text-sm bg-pd-green text-black font-medium rounded hover:bg-green-400 disabled:opacity-50"
            >
              Reboot Device
            </button>
            <button
              onClick={() => {
                setComplete(false);
                setCompleteConfig(null);
                sendCmd({ cmd: "hello", force: true });
              }}
              disabled={busy}
              className="flex-1 px-4 py-2 text-sm bg-purple-600 text-white rounded hover:bg-purple-500 disabled:opacity-50"
            >
              Re-run Setup
            </button>
          </div>
        </div>
      )}

      {/* Log */}
      {log.length > 0 && (
        <div className="bg-pd-panel rounded-lg border border-pd-border">
          <div className="flex items-center justify-between px-4 py-2 border-b border-pd-border">
            <h3 className="font-semibold text-sm">Serial Log</h3>
            <button
              onClick={() => setLog([])}
              className="text-xs text-gray-500 hover:text-gray-300"
            >
              Clear
            </button>
          </div>
          <div className="p-3 max-h-48 overflow-y-auto font-mono text-xs space-y-0.5">
            {log.map((line, i) => (
              <div
                key={i}
                className={
                  line.startsWith(">>")
                    ? "text-blue-400"
                    : line.startsWith("ERROR")
                    ? "text-pd-red"
                    : "text-gray-400"
                }
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
