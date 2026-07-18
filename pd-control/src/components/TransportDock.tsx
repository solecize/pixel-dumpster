import { Bluetooth, Usb, Wifi } from "lucide-react";
import type { TransportKind, TransportLinks } from "../lib/transport";

interface TransportDockProps {
  links: TransportLinks;
  onConfigure: (kind: TransportKind) => void;
  /** Currently preferred control transport (ring highlight). */
  activeControl?: TransportKind | null;
  /** compact = sidebar; default = device cards / headers */
  size?: "sm" | "md";
  className?: string;
  stopPropagation?: boolean;
}

const ITEMS: {
  kind: TransportKind;
  label: string;
  Icon: typeof Bluetooth;
}[] = [
  { kind: "bluetooth", label: "Bluetooth", Icon: Bluetooth },
  { kind: "usb", label: "USB serial", Icon: Usb },
  { kind: "wifi", label: "WiFi", Icon: Wifi },
];

export function TransportDock({
  links,
  onConfigure,
  activeControl = null,
  size = "md",
  className = "",
  stopPropagation = false,
}: TransportDockProps) {
  const iconClass = size === "sm" ? "w-3.5 h-3.5" : "w-4 h-4";
  const btnClass =
    size === "sm"
      ? "w-7 h-7 rounded-md"
      : "w-8 h-8 rounded-lg";

  return (
    <div
      className={`inline-flex items-center gap-1 p-0.5 rounded-xl bg-pd-dark/80 border border-pd-border/80 ${className}`}
      role="toolbar"
      aria-label="Device transports"
    >
      {ITEMS.map(({ kind, label, Icon }) => {
        const on = links[kind];
        return (
          <button
            key={kind}
            type="button"
            title={
              kind === "bluetooth"
                ? on
                  ? "Bluetooth connected — click to use for content control"
                  : "Bluetooth: not connected — click to scan & connect"
                : kind === "wifi"
                  ? on
                    ? "WiFi reachable — click to use for content control"
                    : "WiFi: not discovered yet — click to configure (or Add by IP)"
                  : on
                    ? "USB connected — click to use for content control"
                    : "USB serial: not connected — click to connect"
            }
            aria-label={`${label}, ${on ? "connected" : "not connected"}`}
            aria-pressed={on}
            onClick={(e) => {
              if (stopPropagation) {
                e.stopPropagation();
                e.preventDefault();
              }
              onConfigure(kind);
            }}
            className={`${btnClass} flex items-center justify-center transition
              hover:bg-pd-border/50 focus:outline-none focus-visible:ring-1 focus-visible:ring-pd-accent
              ${on ? "text-white" : "text-gray-600"}
              ${activeControl === kind ? "ring-1 ring-pd-accent bg-pd-accent/20" : ""}`}
          >
            <Icon className={iconClass} strokeWidth={2} />
          </button>
        );
      })}
    </div>
  );
}
