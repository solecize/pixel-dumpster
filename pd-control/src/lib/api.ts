import { invoke } from "@tauri-apps/api/core";
import type {
  DiscoveredDevice,
  DeviceStatus,
  ContentList,
  DaemonStatus,
  DaemonLog,
  SerialPortInfo,
  PiDaemonStatus,
} from "./types";

// --- Discovery ---

export async function discoverDevices(): Promise<DiscoveredDevice[]> {
  return invoke("discover_devices");
}

export async function stopDiscovery(): Promise<void> {
  return invoke("stop_discovery");
}

export async function addManualDevice(
  ip: string,
  port: number,
  deviceType: string
): Promise<DiscoveredDevice> {
  return invoke("add_manual_device", { ip, port, deviceType });
}

// --- Device (ESP32) ---

export async function getDeviceStatus(
  ip: string,
  port: number
): Promise<DeviceStatus> {
  return invoke("device_status", { ip, port });
}

export async function devicePlay(
  ip: string,
  port: number,
  path: string,
  transition?: string,
  durationMs?: number
): Promise<unknown> {
  return invoke("device_play", { ip, port, path, transition, durationMs });
}

export async function deviceStop(
  ip: string,
  port: number
): Promise<unknown> {
  return invoke("device_stop", { ip, port });
}

export async function getDeviceContent(
  ip: string,
  port: number
): Promise<ContentList> {
  return invoke("device_list_content", { ip, port });
}

export async function getDeviceConfig(
  ip: string,
  port: number
): Promise<unknown> {
  return invoke("device_config", { ip, port });
}

export async function setDeviceConfig(
  ip: string,
  port: number,
  config: Record<string, unknown>
): Promise<unknown> {
  return invoke("device_set_config", { ip, port, config });
}

export async function getDeviceLayout(
  ip: string,
  port: number
): Promise<unknown> {
  return invoke("device_layout", { ip, port });
}

export async function setDeviceLayout(
  ip: string,
  port: number,
  layout: Record<string, unknown>
): Promise<unknown> {
  return invoke("device_set_layout", { ip, port, layout });
}

export async function previewDeviceLayout(
  ip: string,
  port: number,
  layout: Record<string, unknown>
): Promise<unknown> {
  return invoke("device_preview_layout", { ip, port, layout });
}

export async function startTestPattern(
  ip: string,
  port: number,
  pattern: string,
  brightness?: number
): Promise<unknown> {
  return invoke("device_test_start", { ip, port, pattern, brightness });
}

export async function stopTestPattern(
  ip: string,
  port: number
): Promise<unknown> {
  return invoke("device_test_stop", { ip, port });
}

export async function panelSelect(
  ip: string,
  port: number,
  panelIndex: number
): Promise<unknown> {
  return invoke("device_panel_select", { ip, port, panelIndex });
}

// --- Daemon ---

export async function getDaemonStatus(
  ip: string,
  port: number
): Promise<DaemonStatus> {
  return invoke("daemon_status", { ip, port });
}

export async function getDaemonConfig(
  ip: string,
  port: number
): Promise<unknown> {
  return invoke("daemon_config", { ip, port });
}

export async function daemonReload(
  ip: string,
  port: number
): Promise<unknown> {
  return invoke("daemon_reload", { ip, port });
}

export async function daemonInjectEvent(
  ip: string,
  port: number,
  eventType: string,
  system?: string,
  game?: string,
  romPath?: string
): Promise<unknown> {
  return invoke("daemon_inject_event", {
    ip,
    port,
    eventType,
    system,
    game,
    romPath,
  });
}

export async function getDaemonLog(
  ip: string,
  port: number
): Promise<DaemonLog> {
  return invoke("daemon_log", { ip, port });
}

// --- Flash ---

export async function listSerialPorts(): Promise<SerialPortInfo[]> {
  return invoke("list_serial_ports");
}

export async function checkFlashTool(): Promise<string> {
  return invoke("check_flash_tool");
}

export async function checkIdfInstalled(): Promise<boolean> {
  return invoke("check_idf_installed");
}

export async function flashDevice(
  port: string,
  firmwareDir?: string,
  buildFromSource?: boolean,
  projectDir?: string
): Promise<string[]> {
  return invoke("flash_device", {
    port,
    firmwareDir: firmwareDir ?? null,
    buildFromSource: buildFromSource ?? false,
    projectDir: projectDir ?? null,
  });
}

// --- Pi Installer ---

export async function piTestConnection(
  host: string,
  port: number,
  username: string,
  password?: string,
  keyPath?: string
): Promise<string> {
  return invoke("pi_test_connection", {
    host,
    port,
    username,
    password: password ?? null,
    keyPath: keyPath ?? null,
  });
}

export async function piCheckDaemon(
  host: string,
  port: number,
  username: string,
  password?: string,
  keyPath?: string
): Promise<PiDaemonStatus> {
  return invoke("pi_check_daemon", {
    host,
    port,
    username,
    password: password ?? null,
    keyPath: keyPath ?? null,
  });
}

export async function piInstallDaemon(
  host: string,
  sshPort: number,
  username: string,
  password: string | undefined,
  keyPath: string | undefined,
  deviceHost: string,
  devicePort: number,
  transport: string,
  serialDevice: string | undefined,
  toolsDir: string
): Promise<void> {
  return invoke("pi_install_daemon", {
    host,
    sshPort,
    username,
    password: password ?? null,
    keyPath: keyPath ?? null,
    deviceHost,
    devicePort,
    transport,
    serialDevice: serialDevice ?? null,
    toolsDir,
  });
}

export async function piUninstallDaemon(
  host: string,
  port: number,
  username: string,
  password?: string,
  keyPath?: string
): Promise<void> {
  return invoke("pi_uninstall_daemon", {
    host,
    port,
    username,
    password: password ?? null,
    keyPath: keyPath ?? null,
  });
}

// --- Serial Wizard ---

export async function wizardConnect(port: string): Promise<string[]> {
  return invoke("wizard_connect", { port });
}

export async function wizardDisconnect(): Promise<void> {
  return invoke("wizard_disconnect");
}

export async function wizardSend(command: string): Promise<string[]> {
  return invoke("wizard_send", { command });
}

export async function wizardReboot(): Promise<void> {
  return invoke("wizard_reboot");
}

export async function wizardPoll(): Promise<string[]> {
  return invoke("wizard_poll");
}

// --- Content Upload ---

export async function uploadContentToDevice(
  deviceIp: string,
  devicePort: number,
  contentPath: string
): Promise<void> {
  return invoke("upload_content_to_device", {
    deviceIp,
    devicePort,
    contentPath,
  });
}
