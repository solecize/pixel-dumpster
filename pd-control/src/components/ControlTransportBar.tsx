import { useEffect, useRef, useState } from "react";
import { Bluetooth, ChevronDown, Usb, Wifi } from "lucide-react";
import type { TransportKind, TransportLinks } from "../lib/transport";

const OPTIONS: {
  kind: TransportKind;
  label: string;
  Icon: typeof Wifi;
}[] = [
  { kind: "wifi", label: "WiFi", Icon: Wifi },
  { kind: "bluetooth", label: "Bluetooth", Icon: Bluetooth },
  { kind: "usb", label: "USB", Icon: Usb },
];

interface ControlTransportBarProps {
  links: TransportLinks;
  value: TransportKind | null;
  onSelectControl: (kind: TransportKind) => void;
  onOpenSettings: (kind: TransportKind) => void;
  className?: string;
}

/**
 * Combined control-via tabs + transport status.
 * Order: WiFi | Bluetooth | USB. Chevron expands Use / Settings actions.
 */
export function ControlTransportBar({
  links,
  value,
  onSelectControl,
  onOpenSettings,
  className = "",
}: ControlTransportBarProps) {
  const [openKind, setOpenKind] = useState<TransportKind | null>(null);
  const rootRef = useRef<HTMLDivElement>(null);

  useEffect(() => {
    if (!openKind) return;
    const onDoc = (e: MouseEvent) => {
      if (!rootRef.current?.contains(e.target as Node)) {
        setOpenKind(null);
      }
    };
    document.addEventListener("mousedown", onDoc);
    return () => document.removeEventListener("mousedown", onDoc);
  }, [openKind]);

  const anyUp = links.wifi || links.bluetooth || links.usb;
  if (!anyUp) return null;

  return (
    <div
      ref={rootRef}
      className={`inline-flex items-stretch gap-0.5 p-0.5 rounded-lg bg-pd-bg border border-pd-border ${className}`}
      role="group"
      aria-label="Control transport"
    >
      {OPTIONS.map(({ kind, label, Icon }) => {
        const up = links[kind];
        const active = value === kind;
        const menuOpen = openKind === kind;

        return (
          <div key={kind} className="relative flex">
            <button
              type="button"
              disabled={!up}
              title={
                up
                  ? active
                    ? `${label} — active control path`
                    : `Use ${label} for content control`
                  : `${label} not connected — open Settings`
              }
              onClick={() => {
                if (up) {
                  onSelectControl(kind);
                } else {
                  onOpenSettings(kind);
                }
              }}
              className={`flex items-center gap-1.5 pl-2.5 pr-1.5 py-1 text-xs rounded-l-md transition disabled:opacity-35 ${
                active
                  ? "bg-pd-accent text-white"
                  : up
                    ? "text-gray-200 hover:bg-pd-border/40"
                    : "text-gray-500 hover:bg-pd-border/30"
              }`}
            >
              <Icon className="w-3.5 h-3.5" strokeWidth={2} />
              <span>{label}</span>
              {up && (
                <span
                  className={`w-1.5 h-1.5 rounded-full ${
                    active ? "bg-white" : "bg-pd-green"
                  }`}
                  aria-hidden
                />
              )}
            </button>
            <button
              type="button"
              title={`${label} options`}
              aria-expanded={menuOpen}
              aria-haspopup="menu"
              onClick={(e) => {
                e.stopPropagation();
                setOpenKind(menuOpen ? null : kind);
              }}
              className={`px-1.5 py-1 rounded-r-md transition ${
                active
                  ? "bg-pd-accent/90 text-white hover:bg-indigo-500"
                  : "text-gray-400 hover:text-gray-200 hover:bg-pd-border/40"
              }`}
            >
              <ChevronDown
                className={`w-3.5 h-3.5 transition-transform ${
                  menuOpen ? "rotate-180" : ""
                }`}
              />
            </button>

            {menuOpen && (
              <div
                role="menu"
                className="absolute right-0 top-full mt-1 z-30 min-w-[10.5rem] rounded-md border border-pd-border bg-pd-panel shadow-lg py-1 text-xs"
              >
                <button
                  type="button"
                  role="menuitem"
                  disabled={!up}
                  className="w-full px-3 py-1.5 text-left hover:bg-pd-border/50 disabled:opacity-40 disabled:cursor-not-allowed"
                  onClick={() => {
                    if (up) onSelectControl(kind);
                    setOpenKind(null);
                  }}
                >
                  Use for control
                </button>
                <button
                  type="button"
                  role="menuitem"
                  className="w-full px-3 py-1.5 text-left hover:bg-pd-border/50"
                  onClick={() => {
                    onOpenSettings(kind);
                    setOpenKind(null);
                  }}
                >
                  {up ? "Edit connection…" : "Connect…"}
                </button>
              </div>
            )}
          </div>
        );
      })}
    </div>
  );
}
