import { useState, useEffect, useCallback } from "react";
import { Sidebar } from "./components/Sidebar";
import { ContentPanel } from "./components/ContentPanel";
import { DeviceSetupPanel } from "./components/DeviceSetupPanel";
import { FlashPanel } from "./components/FlashPanel";
import { PiSetupPanel } from "./components/PiSetupPanel";
import WizardPanel from "./components/WizardPanel";
import type { DiscoveredDevice } from "./lib/types";
import { discoverDevices } from "./lib/api";
import {
  EMPTY_TRANSPORT,
  pollTransportLinks,
  type TransportKind,
  type TransportLinks,
} from "./lib/transport";
import {
  prefsKeyForDevice,
  saveTransportPrefs,
  type ControlViaPref,
} from "./lib/transportPrefs";
import { loadControlViaPref } from "./lib/deviceControl";
import {
  cardIdForTransport,
  type SettingsCardId,
} from "./lib/settingsCards";
import {
  loadLastDevice,
  mergeSelectedWithDiscovery,
  offlineControlDevice,
  saveLastDevice,
} from "./lib/deviceSession";

type View = "content" | "device-setup" | "flash" | "pi-setup";

export default function App() {
  const [devices, setDevices] = useState<DiscoveredDevice[]>([]);
  const [selected, setSelected] = useState<DiscoveredDevice | null>(() =>
    loadLastDevice()
  );
  const [scanning, setScanning] = useState(false);
  const [view, setView] = useState<View>("content");
  const [links, setLinks] = useState<TransportLinks>(EMPTY_TRANSPORT);
  const [settingsWizardMode, setSettingsWizardMode] = useState(false);
  const [hardwareWizardOpen, setHardwareWizardOpen] = useState(false);
  const [hardwareWizardLink, setHardwareWizardLink] = useState<"usb" | "ble">(
    "usb"
  );
  const [focusCard, setFocusCard] = useState<SettingsCardId | null>(null);
  const [linksTick, setLinksTick] = useState(0);
  const [controlViaPref, setControlViaPref] = useState<ControlViaPref | null>(
    () => loadControlViaPref(prefsKeyForDevice(loadLastDevice()))
  );

  const handleScan = useCallback(async () => {
    setScanning(true);
    try {
      const found = await discoverDevices();
      setDevices(found);
    } catch (err) {
      console.error("Discovery failed:", err);
    } finally {
      setScanning(false);
    }
  }, []);

  useEffect(() => {
    handleScan();
  }, [handleScan]);

  useEffect(() => {
    let cancelled = false;
    const tick = async () => {
      const next = await pollTransportLinks(selected);
      if (!cancelled) setLinks(next);
    };
    tick();
    const id = setInterval(tick, 2500);
    return () => {
      cancelled = true;
      clearInterval(id);
    };
  }, [selected, linksTick]);

  // When discovery returns, upgrade a remembered/offline selection to a live hit.
  useEffect(() => {
    setSelected((prev) => {
      const next = mergeSelectedWithDiscovery(prev, devices);
      if (next && next !== prev) saveLastDevice(next);
      return next;
    });
  }, [devices]);

  // BLE/USB connected but nothing selected (common with WiFi offline) →
  // synthesize/restore a device so Content can list/play.
  useEffect(() => {
    if (selected) return;
    if (!links.bluetooth && !links.usb) return;
    const offline = offlineControlDevice(links);
    if (!offline) return;
    setSelected(offline);
    saveLastDevice(offline);
  }, [selected, links]);

  // Keep control-via pref in sync when the selected device changes.
  useEffect(() => {
    setControlViaPref(loadControlViaPref(prefsKeyForDevice(selected)));
  }, [selected?.name, selected?.ip]);

  const setControlVia = (kind: ControlViaPref) => {
    setControlViaPref(kind);
    saveTransportPrefs(prefsKeyForDevice(selected), { controlVia: kind });
  };

  const handleSelectDevice = (device: DiscoveredDevice) => {
    setSelected(device);
    saveLastDevice(device);
  };

  const handleSetView = (v: View) => {
    setView(v);
    if (v !== "device-setup") {
      setSettingsWizardMode(false);
      setHardwareWizardOpen(false);
    }
  };

  const openHardwareWizard = (kind: "usb" | "ble") => {
    setHardwareWizardLink(kind);
    setHardwareWizardOpen(true);
    setSettingsWizardMode(false);
    setView("device-setup");
  };

  const handleConfigureTransport = (
    device: DiscoveredDevice | null,
    kind: TransportKind,
    opts?: { forceSettings?: boolean }
  ) => {
    if (device) {
      setSelected(device);
      saveLastDevice(device);
    }

    // Already connected: choose it as the control path (don't force Settings),
    // unless the caller explicitly wants the Settings card (chevron → Edit).
    if (links[kind] && !opts?.forceSettings) {
      setControlVia(kind);
      return;
    }

    setHardwareWizardOpen(false);
    setView("device-setup");
    setFocusCard(cardIdForTransport(kind));
  };

  const espDevices = devices.filter((d) => d.device_type === "device");
  // Keep the selected offline/BLE device visible even when mDNS is empty.
  const contentDevices =
    selected &&
    selected.device_type === "device" &&
    !espDevices.some(
      (d) =>
        d.name.trim().toLowerCase() === selected.name.trim().toLowerCase() ||
        (d.ip === selected.ip && d.port === selected.port)
    )
      ? [selected, ...espDevices]
      : espDevices;

  return (
    <div className="flex h-screen bg-pd-dark text-gray-100">
      <Sidebar
        devices={devices}
        selected={selected}
        links={links}
        onSelect={handleSelectDevice}
        onSetView={handleSetView}
        activeView={view}
        onConfigureTransport={(kind) =>
          handleConfigureTransport(selected, kind)
        }
      />

      <main className="flex-1 overflow-y-auto p-6">
        {hardwareWizardOpen ? (
          <div className="space-y-4">
            <div className="flex items-center justify-between">
              <div>
                <h2 className="text-xl font-bold">Hardware layout setup</h2>
                <p className="text-sm text-gray-500">
                  Firmware wizard for reztest, chain, and matrix steps over
                  USB/BLE
                </p>
              </div>
              <button
                onClick={() => setHardwareWizardOpen(false)}
                className="px-3 py-1.5 text-sm bg-pd-border hover:bg-gray-600 rounded transition"
              >
                Back to Settings
              </button>
            </div>
            <WizardPanel
              initialLinkKind={hardwareWizardLink}
              prefsKey={prefsKeyForDevice(selected)}
              hideTitle
              autoReconnectBle={hardwareWizardLink === "ble"}
            />
          </div>
        ) : view === "content" ? (
          <ContentPanel
            devices={contentDevices}
            selected={selected}
            links={links}
            controlViaPref={controlViaPref}
            onControlViaChange={setControlVia}
            onSelect={handleSelectDevice}
            onScan={handleScan}
            scanning={scanning}
            onDevicesChange={setDevices}
            onConfigureTransport={handleConfigureTransport}
          />
        ) : view === "device-setup" ? (
          <DeviceSetupPanel
            selected={selected}
            onSelect={handleSelectDevice}
            devices={contentDevices}
            links={links}
            onScan={handleScan}
            scanning={scanning}
            onDevicesChange={setDevices}
            onConfigureTransport={handleConfigureTransport}
            wizardMode={settingsWizardMode}
            onWizardModeChange={(active) => {
              setHardwareWizardOpen(false);
              setSettingsWizardMode(active);
              if (active) setFocusCard("usb");
            }}
            focusCard={focusCard}
            onFocusCardHandled={() => setFocusCard(null)}
            onOpenHardwareWizard={(kind) =>
              openHardwareWizard(kind ?? "usb")
            }
            onLinksChanged={(kind) => {
              setLinksTick((n) => n + 1);
              // Choosing a link in Settings should make it the active control path.
              if (kind === "bluetooth" || kind === "usb") {
                setControlVia(kind);
              }
            }}
          />
        ) : view === "flash" ? (
          <FlashPanel />
        ) : (
          <PiSetupPanel />
        )}
      </main>
    </div>
  );
}
