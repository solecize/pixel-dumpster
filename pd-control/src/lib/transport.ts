import { bleIsConnected, getDeviceStatus, wizardIsConnected } from "./api";
import type { DiscoveredDevice } from "./types";

export type TransportKind = "bluetooth" | "usb" | "wifi";

export interface TransportLinks {
  bluetooth: boolean;
  usb: boolean;
  wifi: boolean;
}

export const EMPTY_TRANSPORT: TransportLinks = {
  bluetooth: false,
  usb: false,
  wifi: false,
};

/** Poll active sessions (BLE/USB wizard) plus WiFi HTTP reachability. */
export async function pollTransportLinks(
  device: DiscoveredDevice | null
): Promise<TransportLinks> {
  const [bluetooth, usb, wifi] = await Promise.all([
    bleIsConnected().catch(() => false),
    wizardIsConnected().catch(() => false),
    device
      ? getDeviceStatus(device.ip, device.port)
          .then(() => true)
          .catch(() => false)
      : Promise.resolve(false),
  ]);
  return { bluetooth, usb, wifi };
}
