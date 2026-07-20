import type { TransportKind, TransportLinks } from "../lib/transport";

const OPTIONS: { kind: TransportKind; label: string }[] = [
  { kind: "wifi", label: "WiFi" },
  { kind: "bluetooth", label: "Bluetooth" },
  { kind: "usb", label: "USB" },
];

interface ControlViaPickerProps {
  links: TransportLinks;
  value: TransportKind | null;
  onChange: (kind: TransportKind) => void;
  className?: string;
}

/**
 * Up-front choice of which connected transport Content should use.
 * Disabled options are not currently linked.
 */
export function ControlViaPicker({
  links,
  value,
  onChange,
  className = "",
}: ControlViaPickerProps) {
  const anyUp = links.wifi || links.bluetooth || links.usb;
  if (!anyUp) return null;

  return (
    <div
      className={`inline-flex gap-1 p-0.5 rounded-lg bg-pd-bg border border-pd-border ${className}`}
      role="group"
      aria-label="Control over"
    >
      {OPTIONS.map(({ kind, label }) => {
        const up = links[kind];
        const active = value === kind;
        return (
          <button
            key={kind}
            type="button"
            disabled={!up}
            title={
              up
                ? `Use ${label} for content control`
                : `${label} not connected`
            }
            onClick={() => onChange(kind)}
            className={`px-2.5 py-1 text-xs rounded-md transition disabled:opacity-30 disabled:cursor-not-allowed ${
              active
                ? "bg-pd-accent text-white"
                : "text-gray-400 hover:text-gray-200 hover:bg-pd-border/40"
            }`}
          >
            {label}
          </button>
        );
      })}
    </div>
  );
}
