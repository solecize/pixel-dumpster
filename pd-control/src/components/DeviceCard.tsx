import type { DiscoveredDevice } from "../lib/types";
import type { TransportKind, TransportLinks } from "../lib/transport";
import { EMPTY_TRANSPORT } from "../lib/transport";
import { TransportDock } from "./TransportDock";

interface DeviceCardProps {
  device: DiscoveredDevice;
  links?: TransportLinks;
  onSelect: (device: DiscoveredDevice) => void;
  onConfigureTransport: (device: DiscoveredDevice, kind: TransportKind) => void;
}

export function DeviceCard({
  device,
  links = EMPTY_TRANSPORT,
  onSelect,
  onConfigureTransport,
}: DeviceCardProps) {
  return (
    <div
      role="button"
      tabIndex={0}
      onClick={() => onSelect(device)}
      onKeyDown={(e) => {
        if (e.key === "Enter" || e.key === " ") {
          e.preventDefault();
          onSelect(device);
        }
      }}
      className="w-full min-w-0 bg-pd-panel border border-pd-border rounded-lg p-4 text-left hover:border-pd-accent/50 transition cursor-pointer focus:outline-none focus-visible:ring-1 focus-visible:ring-pd-accent"
    >
      {/*
        Keep name + dock on one row when space allows. The dock wrapper MUST
        stay shrink-0 / width:auto — a full-width toolbar row previously ate
        clicks in the empty gap beside the icons.
      */}
      <div className="flex items-center gap-3 min-w-0">
        <div className="min-w-0 flex-1">
          <div className="flex items-center gap-2 min-w-0">
            <span
              className={`w-2 h-2 rounded-full shrink-0 ${
                links.wifi ? "bg-pd-green" : "bg-gray-600"
              }`}
            />
            <span className="font-medium truncate">{device.name}</span>
          </div>
          <div className="text-sm text-gray-500 mt-1 truncate">
            {device.ip}:{device.port}
            {device.txt.width && (
              <span className="ml-2">
                {device.txt.width}x{device.txt.height}
              </span>
            )}
          </div>
        </div>
        <div
          className="shrink-0"
          onClick={(e) => e.stopPropagation()}
          onKeyDown={(e) => e.stopPropagation()}
        >
          <TransportDock
            links={links}
            size="sm"
            stopPropagation
            onConfigure={(kind) => onConfigureTransport(device, kind)}
          />
        </div>
      </div>
    </div>
  );
}
