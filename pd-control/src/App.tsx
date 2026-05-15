import { useState, useEffect, useCallback } from "react";
import { Sidebar } from "./components/Sidebar";
import { ContentPanel } from "./components/ContentPanel";
import { DeviceSetupPanel } from "./components/DeviceSetupPanel";
import { FlashPanel } from "./components/FlashPanel";
import { PiSetupPanel } from "./components/PiSetupPanel";
import type { DiscoveredDevice } from "./lib/types";
import { discoverDevices } from "./lib/api";

type View = "content" | "device-setup" | "flash" | "pi-setup";

export default function App() {
  const [devices, setDevices] = useState<DiscoveredDevice[]>([]);
  const [selected, setSelected] = useState<DiscoveredDevice | null>(null);
  const [scanning, setScanning] = useState(false);
  const [view, setView] = useState<View>("content");

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

  const handleSelectDevice = (device: DiscoveredDevice) => {
    setSelected(device);
  };

  const handleSetView = (v: View) => {
    setView(v);
  };

  const espDevices = devices.filter((d) => d.device_type === "device");

  return (
    <div className="flex h-screen bg-pd-dark text-gray-100">
      <Sidebar
        devices={devices}
        selected={selected}
        onSelect={handleSelectDevice}
        onSetView={handleSetView}
        activeView={view}
      />

      <main className="flex-1 overflow-y-auto p-6">
        {view === "content" ? (
          <ContentPanel
            devices={espDevices}
            selected={selected}
            onSelect={handleSelectDevice}
            onScan={handleScan}
            scanning={scanning}
            onDevicesChange={setDevices}
          />
        ) : view === "device-setup" ? (
          <DeviceSetupPanel
            selected={selected}
            onSelect={handleSelectDevice}
            devices={espDevices}
            onScan={handleScan}
            scanning={scanning}
            onDevicesChange={setDevices}
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
