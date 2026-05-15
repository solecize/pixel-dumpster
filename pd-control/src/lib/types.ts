export interface DiscoveredDevice {
  name: string;
  host: string;
  ip: string;
  port: number;
  device_type: "device" | "daemon";
  txt: Record<string, string>;
}

export interface DeviceStatus {
  playing?: boolean;
  path?: string;
  is_sequence?: boolean;
  current_frame?: number;
  total_frames?: number;
  fps?: number;
}

export interface ContentEntry {
  path: string;
  name: string;
  is_sequence?: boolean;
  frame_count?: number;
  fps?: number;
}

export interface ContentList {
  items: ContentEntry[];
}

export interface DaemonStatus {
  running?: boolean;
  current_system?: string;
  last_event?: string;
  last_event_detail?: string;
  transport?: string;
  device_host?: string;
  device_port?: number;
  serial_device?: string;
  uptime_seconds?: number;
  verbose?: boolean;
  dry_run?: boolean;
  events?: {
    game_select: boolean;
    game_launch: boolean;
    game_end: boolean;
    system_select: boolean;
  };
}

export interface DaemonLog {
  lines: string[];
  count: number;
}

// --- Flash types ---

export interface SerialPortInfo {
  port_name: string;
  description: string;
  is_esp32: boolean;
  vid: number | null;
  pid: number | null;
  manufacturer: string | null;
  serial_number: string | null;
}

export interface FlashProgress {
  stage: string;
  message: string;
  percent: number | null;
  done: boolean;
  error: string | null;
}

// --- Pi installer types ---

export interface PiDaemonStatus {
  installed: boolean;
  running: boolean;
  version: string | null;
  config_exists: boolean;
}

export interface PiInstallProgress {
  step: string;
  message: string;
  step_number: number;
  total_steps: number;
  done: boolean;
  error: string | null;
}
