import { useState, useEffect, useCallback } from "react";
import type { DiscoveredDevice } from "../lib/types";
import {
  getDeviceLayout,
  setDeviceLayout,
  previewDeviceLayout,
  addManualDevice,
  getDeviceWizardConfig,
  setDeviceWizardConfig,
  startTestPattern,
  stopTestPattern,
} from "../lib/api";
import {
  controlGetAutoQuantize,
  controlSetAutoQuantize,
} from "../lib/deviceControl";
import {
  loadAutoQuantizePref,
  saveAutoQuantizePref,
} from "../lib/deviceSession";
import { prefsKeyForDevice } from "../lib/transportPrefs";
import {
  SETTINGS_CARDS,
  scrollToSettingsCard,
  type SettingsCardId,
} from "../lib/settingsCards";
import { PanelLayoutSVG } from "./PanelLayoutSVG";
import { DeviceCard } from "./DeviceCard";
import { TransportDock } from "./TransportDock";
import { UsbConnectionCard } from "./UsbConnectionCard";
import { BluetoothConnectionCard } from "./BluetoothConnectionCard";
import { WifiConnectionCard } from "./WifiConnectionCard";
import type { TransportKind, TransportLinks } from "../lib/transport";
import { EMPTY_TRANSPORT } from "../lib/transport";

const SETUP_TEST_PATTERNS = [
  { id: "color_test", label: "Color Test", desc: "R/G/B bands" },
  {
    id: "numbered_panels",
    label: "Numbered Panels",
    desc: "Panel number on each module",
  },
  { id: "checkerboard", label: "Checkerboard", desc: "Checker across all panels" },
  { id: "rgb_sweep", label: "RGB Sweep", desc: "Full-canvas colour sweep" },
  {
    id: "bouncing_ball",
    label: "Bouncing Ball",
    desc: "Ball crossing panel boundaries",
  },
];

interface DeviceSetupPanelProps {
  selected: DiscoveredDevice | null;
  onSelect: (device: DiscoveredDevice) => void;
  devices: DiscoveredDevice[];
  links?: TransportLinks;
  onScan: () => void;
  scanning: boolean;
  onDevicesChange: (devices: DiscoveredDevice[]) => void;
  onConfigureTransport: (
    device: DiscoveredDevice | null,
    kind: TransportKind
  ) => void;
  /** Guided tour over Settings cards (not the firmware WizardPanel). */
  wizardMode?: boolean;
  onWizardModeChange?: (active: boolean) => void;
  /** Focus / highlight a card (from TransportDock). */
  focusCard?: SettingsCardId | null;
  onFocusCardHandled?: () => void;
  /** Open firmware-driven panel layout wizard over USB/BLE. */
  onOpenHardwareWizard?: (kind?: "usb" | "ble") => void;
  /** Refresh TransportLinks after connect/disconnect. */
  onLinksChanged?: (kind?: "usb" | "bluetooth") => void;
}

function cardClass(
  id: SettingsCardId,
  highlight: SettingsCardId | null
): string {
  const base =
    "bg-pd-panel rounded-lg p-4 border transition scroll-mt-4 border-pd-border";
  return highlight === id
    ? `${base} border-pd-accent ring-2 ring-pd-accent/40`
    : base;
}

export function DeviceSetupPanel({
  selected,
  onSelect,
  devices,
  links = EMPTY_TRANSPORT,
  onScan,
  scanning,
  onDevicesChange,
  onConfigureTransport,
  wizardMode = false,
  onWizardModeChange,
  focusCard = null,
  onFocusCardHandled,
  onOpenHardwareWizard,
  onLinksChanged,
}: DeviceSetupPanelProps) {
  const [layout, setLayout] = useState<Record<string, unknown> | null>(null);
  const [editLayout, setEditLayout] = useState(false);
  const [layoutForm, setLayoutForm] = useState({
    panel_width: 64,
    panel_height: 32,
    panel_rows: 1,
    panel_cols: 1,
    chain_pattern: 0,
    panel_rotation_deg: 0,
    color_order: 0,
  });
  const [layoutSaving, setLayoutSaving] = useState(false);
  const [layoutSaved, setLayoutSaved] = useState<string | null>(null);
  const [brightness, setBrightness] = useState(50);
  const [testPattern, setTestPattern] = useState<string | null>(null);
  const [testBusy, setTestBusy] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const [showManual, setShowManual] = useState(false);
  const [wizardConfig, setWizardConfig] = useState<Record<string, unknown> | null>(
    null
  );
  const [editDeviceInfo, setEditDeviceInfo] = useState(false);
  const [deviceInfoForm, setDeviceInfoForm] = useState({
    device_name: "",
    hostname: "",
    timezone: "",
  });
  const [deviceInfoSaving, setDeviceInfoSaving] = useState(false);
  const [deviceInfoSaved, setDeviceInfoSaved] = useState<string | null>(null);
  const [manualIp, setManualIp] = useState("");
  const [manualPort, setManualPort] = useState("8088");
  const [autoQuantizePalette, setAutoQuantizePalette] = useState(false);
  const [autoQuantizeSaving, setAutoQuantizeSaving] = useState(false);
  const [autoQuantizeSaved, setAutoQuantizeSaved] = useState<string | null>(null);
  const [wizardStep, setWizardStep] = useState(0);
  const [wifiStartEditing, setWifiStartEditing] = useState(false);
  const [highlight, setHighlight] = useState<SettingsCardId | null>(null);

  const device = selected;
  const prefsKey = prefsKeyForDevice(device);

  const handleTestStart = async (patternId: string) => {
    if (!device) return;
    setTestBusy(true);
    try {
      await startTestPattern(device.ip, device.port, patternId, brightness);
      setTestPattern(patternId);
    } catch {
      /* non-fatal */
    } finally {
      setTestBusy(false);
    }
  };

  const handleTestStop = async () => {
    if (!device) return;
    setTestBusy(true);
    try {
      await stopTestPattern(device.ip, device.port);
      setTestPattern(null);
    } catch {
      /* non-fatal */
    } finally {
      setTestBusy(false);
    }
  };

  const refreshLayout = useCallback(async () => {
    if (!device) return;
    try {
      const l = (await getDeviceLayout(device.ip, device.port)) as Record<
        string,
        unknown
      >;
      setLayout(l);
      setLayoutForm({
        panel_width: Number(l.panel_width) || 64,
        panel_height: Number(l.panel_height) || 32,
        panel_rows: Number(l.panel_rows) || 1,
        panel_cols: Number(l.panel_cols) || 1,
        chain_pattern: Number(l.chain_pattern) || 0,
        panel_rotation_deg: Number(l.panel_rotation_deg) || 0,
        color_order: Number(l.color_order) || 0,
      });
    } catch (err) {
      setError(`Layout load error: ${String(err)}`);
    }
  }, [device]);

  const refreshWizardConfig = useCallback(async () => {
    if (!device) return;
    try {
      const w = (await getDeviceWizardConfig(device.ip, device.port)) as Record<
        string,
        unknown
      >;
      setWizardConfig(w);
    } catch (err) {
      setError(`Device settings load error: ${String(err)}`);
    }
  }, [device]);

  const refreshContentConfig = useCallback(async () => {
    if (!device) return;
    const remembered = loadAutoQuantizePref(device);
    if (remembered !== null) setAutoQuantizePalette(remembered);
    try {
      const aq = await controlGetAutoQuantize(device, links);
      if (aq !== null) {
        setAutoQuantizePalette(aq);
        saveAutoQuantizePref(device, aq);
        setAutoQuantizeSaved(null);
      }
    } catch (err) {
      console.warn("Content config load error:", err);
    }
  }, [device, links]);

  useEffect(() => {
    if (!device) {
      setLayout(null);
      setWizardConfig(null);
      return;
    }
    refreshLayout();
    refreshWizardConfig();
    refreshContentConfig();
  }, [device, refreshLayout, refreshWizardConfig, refreshContentConfig]);

  useEffect(() => {
    if (!focusCard) return;
    setHighlight(focusCard);
    scrollToSettingsCard(focusCard);
    if (focusCard === "wifi") setWifiStartEditing(true);
    const step = SETTINGS_CARDS.indexOf(focusCard);
    if (step >= 0 && wizardMode) setWizardStep(step);
    onFocusCardHandled?.();
  }, [focusCard, wizardMode, onFocusCardHandled]);

  useEffect(() => {
    if (!wizardMode) {
      setWizardStep(0);
      return;
    }
    const id = SETTINGS_CARDS[wizardStep];
    setHighlight(id);
    scrollToSettingsCard(id);
  }, [wizardMode, wizardStep]);

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

  const connectionCards = (
    <>
      <UsbConnectionCard
        prefsKey={prefsKey}
        linked={links.usb}
        highlighted={highlight === "usb"}
        onConnected={() => onLinksChanged?.("usb")}
        onDisconnected={() => onLinksChanged?.()}
      />
      <BluetoothConnectionCard
        prefsKey={prefsKey}
        linked={links.bluetooth}
        highlighted={highlight === "bluetooth"}
        autoReconnect
        onConnected={() => onLinksChanged?.("bluetooth")}
        onDisconnected={() => onLinksChanged?.()}
      />
      <WifiConnectionCard
        device={device}
        links={links}
        highlighted={highlight === "wifi"}
        startEditing={wifiStartEditing}
        onStartEditingHandled={() => setWifiStartEditing(false)}
      />
    </>
  );

  const wizardBar = wizardMode ? (
    <div className="sticky top-0 z-10 -mx-1 px-1 py-3 mb-2 bg-pd-dark/95 backdrop-blur border-b border-pd-border">
      <div className="flex items-center justify-between gap-3 flex-wrap">
        <div>
          <div className="text-sm font-semibold">Setup Wizard</div>
          <div className="text-xs text-gray-500">
            Step {wizardStep + 1} of {SETTINGS_CARDS.length} —{" "}
            {SETTINGS_CARDS[wizardStep]}
          </div>
        </div>
        <div className="flex items-center gap-2">
          <button
            type="button"
            disabled={wizardStep <= 0}
            onClick={() => setWizardStep((s) => Math.max(0, s - 1))}
            className="px-3 py-1.5 text-sm bg-pd-border hover:bg-gray-600 rounded transition disabled:opacity-40"
          >
            Back
          </button>
          {wizardStep < SETTINGS_CARDS.length - 1 ? (
            <button
              type="button"
              onClick={() =>
                setWizardStep((s) =>
                  Math.min(SETTINGS_CARDS.length - 1, s + 1)
                )
              }
              className="px-3 py-1.5 text-sm bg-pd-accent hover:bg-indigo-600 text-white rounded transition"
            >
              Next
            </button>
          ) : (
            <button
              type="button"
              onClick={() => {
                onWizardModeChange?.(false);
                setHighlight(null);
              }}
              className="px-3 py-1.5 text-sm bg-pd-green hover:bg-green-600 text-black font-medium rounded transition"
            >
              Done
            </button>
          )}
          <button
            type="button"
            onClick={() => {
              onWizardModeChange?.(false);
              setHighlight(null);
            }}
            className="px-3 py-1.5 text-sm text-gray-400 hover:text-white"
          >
            Exit
          </button>
        </div>
      </div>
    </div>
  ) : null;

  if (!device) {
    return (
      <div className="space-y-6">
        {wizardBar}
        <div className="flex items-center justify-between flex-wrap gap-2">
          <h2 className="text-xl font-bold">Settings</h2>
          <div className="flex items-center gap-2">
            {!wizardMode && (
              <button
                type="button"
                onClick={() => onWizardModeChange?.(true)}
                className="px-3 py-1.5 text-sm bg-pd-accent hover:bg-indigo-600 text-white rounded transition"
              >
                Launch Wizard
              </button>
            )}
            <button
              onClick={onScan}
              disabled={scanning}
              className="px-4 py-1.5 text-sm bg-pd-border hover:bg-gray-600 text-white rounded transition disabled:opacity-50"
            >
              {scanning ? "Scanning..." : "Scan Network"}
            </button>
          </div>
        </div>

        <p className="text-sm text-gray-500">
          Connect USB or Bluetooth below without a network device, or pick a
          device when WiFi discovery finds one.
        </p>

        {connectionCards}

        {error && (
          <div className="p-3 bg-pd-red/10 border border-pd-red/30 rounded text-sm text-pd-red">
            {error}
          </div>
        )}

        {scanning ? (
          <div className="flex items-center justify-center h-40">
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
                  wifi: true,
                }}
                onSelect={onSelect}
                onConfigureTransport={onConfigureTransport}
              />
            ))}
          </div>
        ) : (
          <div className="bg-pd-panel rounded-lg p-6 border border-pd-border text-center">
            <p className="text-gray-400 mb-4">No WiFi devices found.</p>
            <button
              onClick={() => setShowManual(!showManual)}
              className="px-4 py-2 text-sm bg-pd-border hover:bg-gray-600 text-white rounded transition"
            >
              Add Manually
            </button>
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

  return (
    <div className="space-y-6">
      {wizardBar}

      <div className="flex items-center justify-between gap-3 flex-wrap">
        <div>
          <h2 className="text-xl font-bold">Settings</h2>
          <p className="text-sm text-gray-500">
            {device.name} · {device.ip}:{device.port}
            {device.txt.width && (
              <span className="ml-2">
                {device.txt.width}x{device.txt.height}
              </span>
            )}
          </p>
        </div>
        <div className="flex items-center gap-2 flex-wrap">
          <TransportDock
            links={links}
            onConfigure={(kind) => onConfigureTransport(device, kind)}
          />
          <select
            value={`${device.ip}:${device.port}`}
            onChange={(e) => {
              const [ip, port] = e.target.value.split(":");
              const d = devices.find(
                (x) => x.ip === ip && x.port === parseInt(port)
              );
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
          {!wizardMode && (
            <button
              type="button"
              onClick={() => onWizardModeChange?.(true)}
              className="px-3 py-1.5 text-sm bg-pd-accent hover:bg-indigo-600 text-white rounded transition"
            >
              Launch Wizard
            </button>
          )}
        </div>
      </div>

      {error && (
        <div className="p-3 bg-pd-red/10 border border-pd-red/30 rounded text-sm text-pd-red">
          {error}
        </div>
      )}

      {connectionCards}

      {/* Device Info */}
      <div id="pd-card-device" className={cardClass("device", highlight)}>
        <div className="flex items-center justify-between mb-3">
          <h3 className="font-semibold">Device Info</h3>
          <button
            onClick={() => {
              setEditDeviceInfo(!editDeviceInfo);
              setDeviceInfoSaved(null);
              if (!editDeviceInfo) {
                const w = (wizardConfig || {}) as Record<string, string>;
                setDeviceInfoForm({
                  device_name: String(w.device_name || device.name),
                  hostname: String(w.hostname || ""),
                  timezone: String(w.timezone || ""),
                });
              }
            }}
            className="text-xs px-2 py-1 rounded bg-pd-bg border border-pd-border hover:border-gray-500"
          >
            {editDeviceInfo ? "Cancel" : "Edit"}
          </button>
        </div>

        {!editDeviceInfo && (
          <div className="grid grid-cols-2 gap-x-6 gap-y-1.5 text-sm">
            <div className="text-gray-500">Name</div>
            <div>{device.name}</div>
            <div className="text-gray-500">Address</div>
            <div>
              {device.ip}:{device.port}
            </div>
            {layout && (
              <>
                <div className="text-gray-500">Canvas</div>
                <div>
                  {String(layout.matrix_width)}x{String(layout.matrix_height)}
                </div>
                <div className="text-gray-500">Panels</div>
                <div>
                  {String(layout.panel_cols)}x{String(layout.panel_rows)} (
                  {String(layout.panel_width)}x{String(layout.panel_height)} each)
                </div>
              </>
            )}
            {wizardConfig && (
              <>
                <div className="text-gray-500">Hostname</div>
                <div>
                  {String(
                    (wizardConfig as Record<string, string>).hostname || "—"
                  )}
                </div>
                <div className="text-gray-500">Timezone</div>
                <div>
                  {String(
                    (wizardConfig as Record<string, string>).timezone || "—"
                  )}
                </div>
              </>
            )}
          </div>
        )}

        {editDeviceInfo && (
          <div className="space-y-3">
            <div>
              <label className="text-xs text-gray-500 block mb-1">
                Device name
              </label>
              <input
                type="text"
                value={deviceInfoForm.device_name}
                onChange={(e) =>
                  setDeviceInfoForm((f) => ({
                    ...f,
                    device_name: e.target.value,
                  }))
                }
                className="w-full bg-pd-bg border border-pd-border rounded px-2 py-1.5 text-sm text-white focus:border-pd-accent focus:outline-none"
              />
            </div>
            <div className="grid grid-cols-2 gap-3">
              <div>
                <label className="text-xs text-gray-500 block mb-1">
                  Hostname
                </label>
                <input
                  type="text"
                  value={deviceInfoForm.hostname}
                  onChange={(e) =>
                    setDeviceInfoForm((f) => ({
                      ...f,
                      hostname: e.target.value,
                    }))
                  }
                  className="w-full bg-pd-bg border border-pd-border rounded px-2 py-1.5 text-sm text-white focus:border-pd-accent focus:outline-none"
                />
              </div>
              <div>
                <label className="text-xs text-gray-500 block mb-1">
                  Timezone
                </label>
                <input
                  type="text"
                  value={deviceInfoForm.timezone}
                  onChange={(e) =>
                    setDeviceInfoForm((f) => ({
                      ...f,
                      timezone: e.target.value,
                    }))
                  }
                  placeholder="e.g. CST6CDT"
                  className="w-full bg-pd-bg border border-pd-border rounded px-2 py-1.5 text-sm text-white focus:border-pd-accent focus:outline-none"
                />
              </div>
            </div>
            {deviceInfoSaved && (
              <div className="text-xs text-pd-green">{deviceInfoSaved}</div>
            )}
            <button
              onClick={async () => {
                setDeviceInfoSaved(null);
                if (!device) return;
                setDeviceInfoSaving(true);
                try {
                  await setDeviceWizardConfig(device.ip, device.port, {
                    device_name: deviceInfoForm.device_name,
                    hostname: deviceInfoForm.hostname,
                    timezone: deviceInfoForm.timezone,
                  });
                  setDeviceInfoSaved("Saved.");
                  setEditDeviceInfo(false);
                  setTimeout(refreshWizardConfig, 500);
                } catch (err) {
                  setDeviceInfoSaved(`Error: ${String(err)}`);
                } finally {
                  setDeviceInfoSaving(false);
                }
              }}
              disabled={deviceInfoSaving}
              className="w-full px-4 py-2 bg-pd-accent hover:bg-indigo-600 text-white text-sm rounded transition disabled:opacity-50"
            >
              {deviceInfoSaving ? "Saving..." : "Save"}
            </button>
          </div>
        )}
      </div>

      {/* Panel Layout */}
      <div id="pd-card-layout" className={cardClass("layout", highlight)}>
        <div className="flex items-center justify-between mb-3 gap-2 flex-wrap">
          <h3 className="font-semibold">Panel Layout</h3>
          <div className="flex items-center gap-2">
            <button
              type="button"
              onClick={() =>
                onOpenHardwareWizard?.(links.bluetooth ? "ble" : "usb")
              }
              className="text-xs px-2 py-1 rounded bg-pd-bg border border-pd-border hover:border-pd-accent"
            >
              Configure panels over USB/BLE
            </button>
            <button
              onClick={() => {
                setEditLayout(!editLayout);
                setLayoutSaved(null);
              }}
              className="text-xs px-2 py-1 rounded bg-pd-bg border border-pd-border hover:border-gray-500"
            >
              {editLayout ? "Cancel" : "Edit"}
            </button>
          </div>
        </div>

        {!editLayout && layout && (
          <>
            <div className="grid grid-cols-2 gap-x-6 gap-y-1.5 text-sm mb-4">
              <div className="text-gray-500">Panel size</div>
              <div>
                {String(layout.panel_width)}x{String(layout.panel_height)}
              </div>
              <div className="text-gray-500">Grid</div>
              <div>
                {String(layout.panel_rows)} row x {String(layout.panel_cols)} col
              </div>
              <div className="text-gray-500">Chain pattern</div>
              <div>
                {[
                  "Linear",
                  "Serpentine TL",
                  "Serpentine TR",
                  "Serpentine BL",
                  "Serpentine BR",
                  "Zigzag TL",
                  "Zigzag TR",
                  "Zigzag BL",
                  "Zigzag BR",
                ][Number(layout.chain_pattern)] ?? String(layout.chain_pattern)}
              </div>
              <div className="text-gray-500">Mount rotation</div>
              <div>{String(layout.panel_rotation_deg)}</div>
              <div className="text-gray-500">Color order</div>
              <div>
                {["RGB", "BGR", "GRB", "BRG"][Number(layout.color_order)] ??
                  String(layout.color_order)}
              </div>
            </div>
            <PanelLayoutSVG
              panelWidth={Number(layout.panel_width) || 64}
              panelHeight={Number(layout.panel_height) || 32}
              panelRows={Number(layout.panel_rows) || 1}
              panelCols={Number(layout.panel_cols) || 1}
              chainPattern={Number(layout.chain_pattern) || 0}
              panelRotationDeg={Number(layout.panel_rotation_deg) || 0}
              testPattern={testPattern}
            />
            <div className="mt-2 flex items-center gap-2">
              <select
                value={testPattern ?? ""}
                onChange={(e) =>
                  e.target.value
                    ? handleTestStart(e.target.value)
                    : handleTestStop()
                }
                disabled={testBusy || !device}
                className="flex-1 bg-pd-bg border border-pd-border rounded px-2 py-1.5 text-sm text-white focus:border-pd-accent focus:outline-none disabled:opacity-40"
              >
                <option value="">— No test pattern —</option>
                {SETUP_TEST_PATTERNS.map((p) => (
                  <option key={p.id} value={p.id}>
                    {p.label} — {p.desc}
                  </option>
                ))}
              </select>
              {testPattern && (
                <button
                  onClick={handleTestStop}
                  disabled={testBusy}
                  className="px-3 py-1.5 text-xs bg-pd-red/20 text-pd-red rounded border border-pd-red/30 hover:bg-pd-red/30 disabled:opacity-50"
                >
                  Stop
                </button>
              )}
            </div>
          </>
        )}
        {!editLayout && !layout && (
          <p className="text-gray-500 text-sm">
            {links.wifi
              ? "Loading…"
              : "Connect WiFi HTTP to load layout, or configure panels over USB/BLE."}
          </p>
        )}

        {editLayout && (
          <div className="space-y-3">
            <PanelLayoutSVG
              panelWidth={layoutForm.panel_width}
              panelHeight={layoutForm.panel_height}
              panelRows={layoutForm.panel_rows}
              panelCols={layoutForm.panel_cols}
              chainPattern={layoutForm.chain_pattern}
              panelRotationDeg={layoutForm.panel_rotation_deg}
              testPattern={testPattern}
            />
            <div className="mb-1 flex items-center gap-2">
              <select
                value={testPattern ?? ""}
                onChange={(e) =>
                  e.target.value
                    ? handleTestStart(e.target.value)
                    : handleTestStop()
                }
                disabled={testBusy || !device}
                className="flex-1 bg-pd-bg border border-pd-border rounded px-2 py-1.5 text-sm text-white focus:border-pd-accent focus:outline-none disabled:opacity-40"
              >
                <option value="">— No test pattern —</option>
                {SETUP_TEST_PATTERNS.map((p) => (
                  <option key={p.id} value={p.id}>
                    {p.label} — {p.desc}
                  </option>
                ))}
              </select>
              {testPattern && (
                <button
                  onClick={handleTestStop}
                  disabled={testBusy}
                  className="px-3 py-1.5 text-xs bg-pd-red/20 text-pd-red rounded border border-pd-red/30 hover:bg-pd-red/30 disabled:opacity-50"
                >
                  Stop
                </button>
              )}
            </div>
            <div className="grid grid-cols-2 gap-3">
              <div>
                <label className="text-xs text-gray-500 block mb-1">
                  Panel width (px)
                </label>
                <input
                  type="number"
                  min={8}
                  max={512}
                  value={layoutForm.panel_width}
                  onChange={(e) =>
                    setLayoutForm((f) => ({
                      ...f,
                      panel_width: Number(e.target.value),
                    }))
                  }
                  className="w-full bg-pd-bg border border-pd-border rounded px-2 py-1.5 text-sm text-white focus:border-pd-accent focus:outline-none"
                />
              </div>
              <div>
                <label className="text-xs text-gray-500 block mb-1">
                  Panel height (px)
                </label>
                <input
                  type="number"
                  min={8}
                  max={512}
                  value={layoutForm.panel_height}
                  onChange={(e) =>
                    setLayoutForm((f) => ({
                      ...f,
                      panel_height: Number(e.target.value),
                    }))
                  }
                  className="w-full bg-pd-bg border border-pd-border rounded px-2 py-1.5 text-sm text-white focus:border-pd-accent focus:outline-none"
                />
              </div>
              <div>
                <label className="text-xs text-gray-500 block mb-1">Rows</label>
                <input
                  type="number"
                  min={1}
                  max={8}
                  value={layoutForm.panel_rows}
                  onChange={(e) =>
                    setLayoutForm((f) => ({
                      ...f,
                      panel_rows: Number(e.target.value),
                    }))
                  }
                  className="w-full bg-pd-bg border border-pd-border rounded px-2 py-1.5 text-sm text-white focus:border-pd-accent focus:outline-none"
                />
              </div>
              <div>
                <label className="text-xs text-gray-500 block mb-1">Cols</label>
                <input
                  type="number"
                  min={1}
                  max={8}
                  value={layoutForm.panel_cols}
                  onChange={(e) =>
                    setLayoutForm((f) => ({
                      ...f,
                      panel_cols: Number(e.target.value),
                    }))
                  }
                  className="w-full bg-pd-bg border border-pd-border rounded px-2 py-1.5 text-sm text-white focus:border-pd-accent focus:outline-none"
                />
              </div>
            </div>
            <div>
              <label className="text-xs text-gray-500 block mb-1">
                Chain pattern
              </label>
              <select
                value={layoutForm.chain_pattern}
                onChange={(e) =>
                  setLayoutForm((f) => ({
                    ...f,
                    chain_pattern: Number(e.target.value),
                  }))
                }
                className="w-full bg-pd-bg border border-pd-border rounded px-2 py-1.5 text-sm text-white focus:border-pd-accent focus:outline-none"
              >
                <option value={0}>Linear (row-by-row sequential)</option>
                <option value={1}>Serpentine top-left, snake down</option>
                <option value={2}>Serpentine top-right, snake down</option>
                <option value={3}>Serpentine bottom-left, snake up</option>
                <option value={4}>Serpentine bottom-right, snake up</option>
                <option value={5}>Zigzag top-left, down</option>
                <option value={6}>Zigzag top-right, down</option>
                <option value={7}>Zigzag bottom-left, up</option>
                <option value={8}>Zigzag bottom-right, up</option>
              </select>
            </div>
            <div>
              <label className="text-xs text-gray-500 block mb-1">
                Panel mount rotation (per physical module)
              </label>
              <select
                value={layoutForm.panel_rotation_deg}
                onChange={(e) =>
                  setLayoutForm((f) => ({
                    ...f,
                    panel_rotation_deg: Number(e.target.value),
                  }))
                }
                className="w-full bg-pd-bg border border-pd-border rounded px-2 py-1.5 text-sm text-white focus:border-pd-accent focus:outline-none"
              >
                <option value={0}>0 (normal)</option>
                <option value={90}>90 CW</option>
                <option value={180}>180</option>
                <option value={270}>270 CW</option>
              </select>
            </div>
            <div>
              <label className="text-xs text-gray-500 block mb-1">
                Color order
              </label>
              <select
                value={layoutForm.color_order}
                onChange={(e) =>
                  setLayoutForm((f) => ({
                    ...f,
                    color_order: Number(e.target.value),
                  }))
                }
                className="w-full bg-pd-bg border border-pd-border rounded px-2 py-1.5 text-sm text-white focus:border-pd-accent focus:outline-none"
              >
                <option value={0}>RGB</option>
                <option value={1}>BGR</option>
                <option value={2}>GRB</option>
                <option value={3}>BRG</option>
              </select>
            </div>
            <div className="text-xs text-gray-500 pt-1">
              {(() => {
                const rot = layoutForm.panel_rotation_deg;
                const cw =
                  rot === 90 || rot === 270
                    ? layoutForm.panel_height * layoutForm.panel_rows
                    : layoutForm.panel_width * layoutForm.panel_cols;
                const ch =
                  rot === 90 || rot === 270
                    ? layoutForm.panel_width * layoutForm.panel_cols
                    : layoutForm.panel_height * layoutForm.panel_rows;
                return `Canvas will be ${cw}x${ch} px`;
              })()}
            </div>
            {layoutSaved && (
              <div className="text-xs text-pd-green">{layoutSaved}</div>
            )}
            <div className="flex gap-2">
              <button
                onClick={async () => {
                  setLayoutSaved(null);
                  if (!device) return;
                  try {
                    const res = (await previewDeviceLayout(
                      device.ip,
                      device.port,
                      layoutForm
                    )) as { ok?: boolean; error?: string };
                    if (res && res.ok === false && res.error) {
                      setLayoutSaved(res.error);
                    } else {
                      setLayoutSaved("Preview applied not saved yet");
                    }
                  } catch (err) {
                    setLayoutSaved(`Preview error: ${String(err)}`);
                  }
                }}
                className="flex-1 px-4 py-2 bg-blue-600 text-white text-sm rounded hover:bg-blue-500"
              >
                Preview
              </button>
              <button
                onClick={async () => {
                  setLayoutSaved(null);
                  if (!device || !layout) return;
                  setLayoutForm({
                    panel_width: Number(layout.panel_width) || 64,
                    panel_height: Number(layout.panel_height) || 32,
                    panel_rows: Number(layout.panel_rows) || 1,
                    panel_cols: Number(layout.panel_cols) || 1,
                    chain_pattern: Number(layout.chain_pattern) || 0,
                    panel_rotation_deg: Number(layout.panel_rotation_deg) || 0,
                    color_order: Number(layout.color_order) || 0,
                  });
                  try {
                    const res = (await previewDeviceLayout(
                      device.ip,
                      device.port,
                      {
                        panel_width: Number(layout.panel_width) || 64,
                        panel_height: Number(layout.panel_height) || 32,
                        panel_rows: Number(layout.panel_rows) || 1,
                        panel_cols: Number(layout.panel_cols) || 1,
                        chain_pattern: Number(layout.chain_pattern) || 0,
                        panel_rotation_deg:
                          Number(layout.panel_rotation_deg) || 0,
                        color_order: Number(layout.color_order) || 0,
                      }
                    )) as { ok?: boolean; error?: string };
                    if (res && res.ok === false && res.error) {
                      setLayoutSaved(res.error);
                    } else {
                      setLayoutSaved("Reverted to saved config");
                    }
                  } catch (err) {
                    setLayoutSaved(`Revert error: ${String(err)}`);
                  }
                }}
                className="px-4 py-2 bg-pd-bg border border-pd-border text-white text-sm rounded hover:border-gray-500"
              >
                Revert
              </button>
            </div>
            <button
              onClick={async () => {
                if (!device) return;
                setLayoutSaving(true);
                setLayoutSaved(null);
                try {
                  await setDeviceLayout(device.ip, device.port, layoutForm);
                  setLayoutSaved("Saved device is rebooting...");
                  setEditLayout(false);
                  setTimeout(refreshLayout, 8000);
                } catch (err) {
                  setLayoutSaved(`Error: ${String(err)}`);
                } finally {
                  setLayoutSaving(false);
                }
              }}
              disabled={layoutSaving}
              className="w-full px-4 py-2 bg-pd-accent hover:bg-indigo-600 text-white text-sm rounded transition disabled:opacity-50"
            >
              {layoutSaving ? "Saving..." : "Save & Reboot"}
            </button>
          </div>
        )}
      </div>

      {/* Playback */}
      <div id="pd-card-playback" className={cardClass("playback", highlight)}>
        <h3 className="font-semibold mb-3">Playback</h3>
        <label className="flex items-start gap-3 cursor-pointer">
          <input
            type="checkbox"
            className="mt-1"
            checked={autoQuantizePalette}
            disabled={autoQuantizeSaving}
            onChange={async (e) => {
              if (!device) return;
              const next = e.target.checked;
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
            }}
          />
          <span>
            <span className="text-sm text-gray-200">
              Auto-quantize animations to 64 colors (faster playback)
            </span>
            <span className="block text-xs text-gray-500 mt-1">
              When on, each sequence is converted to a shared 64-color palette and
              cached in PSRAM for smoother playback. When off, full-color decoding
              is used (slower) with no palette overhead.
            </span>
          </span>
        </label>
        {autoQuantizeSaved && (
          <div className="text-xs text-pd-green mt-2">{autoQuantizeSaved}</div>
        )}
      </div>

      {/* Brightness */}
      <div
        id="pd-card-brightness"
        className={cardClass("brightness", highlight)}
      >
        <h3 className="font-semibold mb-3">Brightness</h3>
        <div className="flex items-center gap-3">
          <input
            type="range"
            min={5}
            max={100}
            step={5}
            value={brightness}
            onChange={(e) => setBrightness(Number(e.target.value))}
            className="flex-1 accent-pd-accent"
          />
          <span className="text-sm w-10 text-right">{brightness}%</span>
        </div>
      </div>
    </div>
  );
}
