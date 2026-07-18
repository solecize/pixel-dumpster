/** Stable Settings card anchors / wizard tour order. */
export const SETTINGS_CARDS = [
  "usb",
  "bluetooth",
  "wifi",
  "device",
  "layout",
  "playback",
  "brightness",
] as const;

export type SettingsCardId = (typeof SETTINGS_CARDS)[number];

export function settingsCardAnchor(id: SettingsCardId): string {
  return `pd-card-${id}`;
}

export function scrollToSettingsCard(id: SettingsCardId): void {
  requestAnimationFrame(() => {
    document.getElementById(settingsCardAnchor(id))?.scrollIntoView({
      behavior: "smooth",
      block: "start",
    });
  });
}

/** Map TransportDock kind → Settings card id. */
export function cardIdForTransport(
  kind: "bluetooth" | "usb" | "wifi"
): SettingsCardId {
  if (kind === "bluetooth") return "bluetooth";
  if (kind === "usb") return "usb";
  return "wifi";
}
