import { useEffect, useState } from "react";
import type { DiscoveredDevice } from "../lib/types";
import { getDeviceWizardConfig, setDeviceWizardConfig } from "../lib/api";
import type { TransportLinks } from "../lib/transport";

interface WifiConnectionCardProps {
  device: DiscoveredDevice | null;
  links: TransportLinks;
  highlighted?: boolean;
  className?: string;
  /** Open edit mode when focusing this card from the dock. */
  startEditing?: boolean;
  onStartEditingHandled?: () => void;
}

export function WifiConnectionCard({
  device,
  links,
  highlighted = false,
  className = "",
  startEditing = false,
  onStartEditingHandled,
}: WifiConnectionCardProps) {
  const [wizardConfig, setWizardConfig] = useState<Record<string, unknown> | null>(
    null
  );
  const [editing, setEditing] = useState(false);
  const [saving, setSaving] = useState(false);
  const [saved, setSaved] = useState<string | null>(null);
  const [form, setForm] = useState({
    wifi_ssid: "",
    wifi_password: "",
    static_ip: "",
    static_gateway: "",
    static_netmask: "",
  });

  useEffect(() => {
    if (!device || !links.wifi) {
      setWizardConfig(null);
      return;
    }
    let cancelled = false;
    (async () => {
      try {
        const w = (await getDeviceWizardConfig(device.ip, device.port)) as Record<
          string,
          unknown
        >;
        if (!cancelled) setWizardConfig(w);
      } catch {
        if (!cancelled) setWizardConfig(null);
      }
    })();
    return () => {
      cancelled = true;
    };
  }, [device, links.wifi]);

  useEffect(() => {
    if (!startEditing) return;
    if (!device || !links.wifi || !wizardConfig) {
      onStartEditingHandled?.();
      return;
    }
    const w = wizardConfig;
    setForm({
      wifi_ssid: String(w.wifi_ssid || ""),
      wifi_password: "",
      static_ip: String(w.static_ip || ""),
      static_gateway: String(w.static_gateway || ""),
      static_netmask: String(w.static_netmask || ""),
    });
    setEditing(true);
    setSaved(null);
    onStartEditingHandled?.();
  }, [
    startEditing,
    device,
    links.wifi,
    wizardConfig,
    onStartEditingHandled,
  ]);

  const ssid = wizardConfig
    ? String((wizardConfig as Record<string, string>).wifi_ssid || "—")
    : links.wifi
      ? "…"
      : "—";

  const canEditHttp = Boolean(device && links.wifi);

  return (
    <div
      id="pd-card-wifi"
      className={`bg-pd-panel rounded-lg p-4 border transition scroll-mt-4 ${
        highlighted
          ? "border-pd-accent ring-2 ring-pd-accent/40"
          : "border-pd-border"
      } ${className}`}
    >
      <div className="flex items-center justify-between mb-3">
        <h3 className="font-semibold">WiFi</h3>
        <div className="flex items-center gap-2">
          <span
            className={`text-xs px-2 py-0.5 rounded ${
              links.wifi
                ? "bg-pd-green/15 text-pd-green"
                : "bg-pd-bg text-gray-500 border border-pd-border"
            }`}
          >
            {links.wifi ? "Online" : "Not reachable"}
          </span>
          {canEditHttp && (
            <button
              type="button"
              onClick={() => {
                setEditing(!editing);
                setSaved(null);
                if (!editing && wizardConfig) {
                  const w = wizardConfig as Record<string, string>;
                  setForm({
                    wifi_ssid: String(w.wifi_ssid || ""),
                    wifi_password: "",
                    static_ip: String(w.static_ip || ""),
                    static_gateway: String(w.static_gateway || ""),
                    static_netmask: String(w.static_netmask || ""),
                  });
                }
              }}
              className="text-xs px-2 py-1 rounded bg-pd-bg border border-pd-border hover:border-gray-500"
            >
              {editing ? "Cancel" : "Edit"}
            </button>
          )}
        </div>
      </div>

      {!device && (
        <p className="text-sm text-gray-500">
          Select a device to view or edit WiFi settings over HTTP.
        </p>
      )}

      {device && !links.wifi && (
        <div className="space-y-2 text-sm">
          <p className="text-gray-400">
            Device is not reachable over WiFi HTTP ({device.ip}:{device.port}).
          </p>
          <p className="text-xs text-gray-600">
            Connect USB or Bluetooth above for control while offline. SSID is
            stored on the device — edit over HTTP when the device is online, or
            use hardware layout setup over USB/BLE for first-time provisioning.
          </p>
        </div>
      )}

      {device && links.wifi && !editing && (
        <div className="grid grid-cols-2 gap-x-6 gap-y-1.5 text-sm">
          <div className="text-gray-500">SSID</div>
          <div>{ssid}</div>
          <div className="text-gray-500">IP address</div>
          <div>
            {wizardConfig
              ? String((wizardConfig as Record<string, string>).static_ip || "") ||
                "DHCP (auto)"
              : `${device.ip} (current)`}
          </div>
          <div className="text-gray-500">Reachable at</div>
          <div>
            {device.ip}:{device.port}
          </div>
        </div>
      )}

      {editing && canEditHttp && (
        <div className="space-y-3">
          <div>
            <label className="text-xs text-gray-500 block mb-1">WiFi SSID</label>
            <input
              type="text"
              value={form.wifi_ssid}
              onChange={(e) =>
                setForm((f) => ({ ...f, wifi_ssid: e.target.value }))
              }
              className="w-full bg-pd-bg border border-pd-border rounded px-2 py-1.5 text-sm text-white focus:border-pd-accent focus:outline-none"
            />
          </div>
          <div>
            <label className="text-xs text-gray-500 block mb-1">
              WiFi password
            </label>
            <input
              type="password"
              value={form.wifi_password}
              onChange={(e) =>
                setForm((f) => ({ ...f, wifi_password: e.target.value }))
              }
              className="w-full bg-pd-bg border border-pd-border rounded px-2 py-1.5 text-sm text-white focus:border-pd-accent focus:outline-none"
              placeholder="Leave blank to keep the current password"
            />
          </div>
          <div>
            <label className="text-xs text-gray-500 block mb-1">
              Static IP{" "}
              <span className="text-gray-600">(blank = DHCP)</span>
            </label>
            <input
              type="text"
              value={form.static_ip}
              onChange={(e) =>
                setForm((f) => ({
                  ...f,
                  static_ip: e.target.value,
                  ...(e.target.value === ""
                    ? { static_gateway: "", static_netmask: "" }
                    : {}),
                }))
              }
              placeholder="192.168.1.50"
              className="w-full bg-pd-bg border border-pd-border rounded px-2 py-1.5 text-sm text-white focus:border-pd-accent focus:outline-none"
            />
          </div>
          {form.static_ip !== "" && (
            <div className="grid grid-cols-2 gap-3">
              <div>
                <label className="text-xs text-gray-500 block mb-1">
                  Gateway
                </label>
                <input
                  type="text"
                  value={form.static_gateway}
                  onChange={(e) =>
                    setForm((f) => ({ ...f, static_gateway: e.target.value }))
                  }
                  className="w-full bg-pd-bg border border-pd-border rounded px-2 py-1.5 text-sm text-white focus:border-pd-accent focus:outline-none"
                />
              </div>
              <div>
                <label className="text-xs text-gray-500 block mb-1">
                  Netmask
                </label>
                <input
                  type="text"
                  value={form.static_netmask}
                  onChange={(e) =>
                    setForm((f) => ({ ...f, static_netmask: e.target.value }))
                  }
                  className="w-full bg-pd-bg border border-pd-border rounded px-2 py-1.5 text-sm text-white focus:border-pd-accent focus:outline-none"
                />
              </div>
            </div>
          )}
          {saved && <div className="text-xs text-pd-green">{saved}</div>}
          <button
            type="button"
            disabled={saving || !device}
            onClick={async () => {
              if (!device) return;
              setSaving(true);
              setSaved(null);
              try {
                const payload: Record<string, unknown> = {
                  wifi_ssid: form.wifi_ssid,
                  static_ip: form.static_ip,
                  static_gateway: form.static_gateway,
                  static_netmask: form.static_netmask,
                };
                if (form.wifi_password !== "") {
                  payload.wifi_password = form.wifi_password;
                }
                await setDeviceWizardConfig(device.ip, device.port, payload);
                setSaved(
                  "Saved. Reboot the device to apply WiFi/network changes."
                );
                setEditing(false);
                const w = (await getDeviceWizardConfig(
                  device.ip,
                  device.port
                )) as Record<string, unknown>;
                setWizardConfig(w);
              } catch (err) {
                setSaved(`Error: ${String(err)}`);
              } finally {
                setSaving(false);
              }
            }}
            className="w-full px-4 py-2 bg-pd-accent hover:bg-indigo-600 text-white text-sm rounded transition disabled:opacity-50"
          >
            {saving ? "Saving…" : "Save"}
          </button>
        </div>
      )}
    </div>
  );
}
