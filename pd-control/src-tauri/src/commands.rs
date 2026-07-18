use crate::ble_link::{self, BleDeviceInfo};
use crate::daemon_api::DaemonApi;
use crate::device_api::DeviceApi;
use crate::discovery::{DiscoveredDevice, DiscoveryState};
use crate::flasher::{self, FlashConfig, FlashProgress, SerialPortInfo};
use crate::pi_installer::{self, PiInstallConfig, PiInstallProgress, SshConfig};
use crate::serial_wizard;
use std::sync::Mutex;
use tauri::{Emitter, Manager, State};

pub struct AppState {
    pub discovery: Mutex<DiscoveryState>,
    pub wizard: serial_wizard::SharedWizardState,
    pub ble: ble_link::SharedBleState,
}

impl Default for AppState {
    fn default() -> Self {
        Self {
            discovery: Mutex::new(DiscoveryState::new()),
            wizard: serial_wizard::new_shared_state(),
            ble: ble_link::new_shared_state(),
        }
    }
}

// --- Discovery commands ---

#[tauri::command]
pub async fn discover_devices(
    state: State<'_, AppState>,
) -> Result<Vec<DiscoveredDevice>, String> {
    log::info!("discover_devices: starting scan");
    {
        let mut discovery = state.discovery.lock().map_err(|e| e.to_string())?;
        discovery.start();
    }
    // Give mDNS time to discover — 5s for multicast queries + responses
    tokio::time::sleep(std::time::Duration::from_secs(5)).await;
    let discovery = state.discovery.lock().map_err(|e| e.to_string())?;
    let devices = discovery.get_devices();
    log::info!("discover_devices: scan complete, found {} device(s)", devices.len());
    Ok(devices)
}

#[tauri::command]
pub async fn stop_discovery(state: State<'_, AppState>) -> Result<(), String> {
    let mut discovery = state.discovery.lock().map_err(|e| e.to_string())?;
    discovery.stop();
    Ok(())
}

#[tauri::command]
pub async fn add_manual_device(
    state: State<'_, AppState>,
    ip: String,
    port: u16,
    device_type: String,
) -> Result<DiscoveredDevice, String> {
    let discovery = state.discovery.lock().map_err(|e| e.to_string())?;
    Ok(discovery.add_manual(ip, port, device_type))
}

// --- Device (ESP32) commands ---

#[tauri::command]
pub async fn device_status(ip: String, port: u16) -> Result<serde_json::Value, String> {
    let api = DeviceApi::new(&ip, port);
    let status = api.status().await?;
    serde_json::to_value(status).map_err(|e| e.to_string())
}

#[tauri::command]
pub async fn device_play(
    ip: String,
    port: u16,
    path: String,
    transition: Option<String>,
    duration_ms: Option<u32>,
) -> Result<serde_json::Value, String> {
    let api = DeviceApi::new(&ip, port);
    api.play(&path, transition.as_deref(), duration_ms).await
}

#[tauri::command]
pub async fn device_stop(ip: String, port: u16) -> Result<serde_json::Value, String> {
    let api = DeviceApi::new(&ip, port);
    api.stop().await
}

#[tauri::command]
pub async fn device_list_content(ip: String, port: u16) -> Result<serde_json::Value, String> {
    let api = DeviceApi::new(&ip, port);
    let list = api.list_content().await?;
    serde_json::to_value(list).map_err(|e| e.to_string())
}

#[tauri::command]
pub async fn device_config(ip: String, port: u16) -> Result<serde_json::Value, String> {
    let api = DeviceApi::new(&ip, port);
    api.config().await
}

#[tauri::command]
pub async fn device_set_config(
    ip: String,
    port: u16,
    config: serde_json::Value,
) -> Result<serde_json::Value, String> {
    let api = DeviceApi::new(&ip, port);
    api.set_config(config).await
}

#[tauri::command]
pub async fn device_wizard_config(ip: String, port: u16) -> Result<serde_json::Value, String> {
    let api = DeviceApi::new(&ip, port);
    api.wizard_config().await
}

#[tauri::command]
pub async fn device_set_wizard_config(
    ip: String,
    port: u16,
    config: serde_json::Value,
) -> Result<serde_json::Value, String> {
    let api = DeviceApi::new(&ip, port);
    api.wizard_set_config(config).await
}

#[tauri::command]
pub async fn device_layout(ip: String, port: u16) -> Result<serde_json::Value, String> {
    let api = DeviceApi::new(&ip, port);
    api.layout().await
}

#[tauri::command]
pub async fn device_set_layout(
    ip: String,
    port: u16,
    layout: serde_json::Value,
) -> Result<serde_json::Value, String> {
    let api = DeviceApi::new(&ip, port);
    api.set_layout(layout).await
}

#[tauri::command]
pub async fn device_preview_layout(
    ip: String,
    port: u16,
    layout: serde_json::Value,
) -> Result<serde_json::Value, String> {
    let api = DeviceApi::new(&ip, port);
    api.preview_layout(layout).await
}

#[tauri::command]
pub async fn device_test_start(
    ip: String,
    port: u16,
    pattern: String,
    brightness: Option<i32>,
) -> Result<serde_json::Value, String> {
    let api = DeviceApi::new(&ip, port);
    api.test_start(&pattern, brightness).await
}

#[tauri::command]
pub async fn device_test_stop(ip: String, port: u16) -> Result<serde_json::Value, String> {
    let api = DeviceApi::new(&ip, port);
    api.test_stop().await
}

#[tauri::command]
pub async fn device_panel_select(
    ip: String,
    port: u16,
    panel_index: i32,
) -> Result<serde_json::Value, String> {
    let api = DeviceApi::new(&ip, port);
    api.panel_select(panel_index).await
}

// --- Daemon commands ---

#[tauri::command]
pub async fn daemon_status(ip: String, port: u16) -> Result<serde_json::Value, String> {
    let api = DaemonApi::new(&ip, port);
    let status = api.status().await?;
    serde_json::to_value(status).map_err(|e| e.to_string())
}

#[tauri::command]
pub async fn daemon_config(ip: String, port: u16) -> Result<serde_json::Value, String> {
    let api = DaemonApi::new(&ip, port);
    api.config().await
}

#[tauri::command]
pub async fn daemon_reload(ip: String, port: u16) -> Result<serde_json::Value, String> {
    let api = DaemonApi::new(&ip, port);
    api.reload().await
}

#[tauri::command]
pub async fn daemon_inject_event(
    ip: String,
    port: u16,
    event_type: String,
    system: Option<String>,
    game: Option<String>,
    rom_path: Option<String>,
) -> Result<serde_json::Value, String> {
    let api = DaemonApi::new(&ip, port);
    api.inject_event(
        &event_type,
        system.as_deref(),
        game.as_deref(),
        rom_path.as_deref(),
    )
    .await
}

#[tauri::command]
pub async fn daemon_log(ip: String, port: u16) -> Result<serde_json::Value, String> {
    let api = DaemonApi::new(&ip, port);
    let log = api.log().await?;
    serde_json::to_value(log).map_err(|e| e.to_string())
}

// --- Flash commands ---

#[tauri::command]
pub fn list_serial_ports() -> Vec<SerialPortInfo> {
    flasher::list_serial_ports()
}

#[tauri::command]
pub fn check_flash_tool() -> Result<String, String> {
    // Check ~/.cargo/bin/espflash directly (Tauri app doesn't inherit shell PATH)
    if let Some(home) = std::env::var_os("HOME") {
        let espflash = std::path::PathBuf::from(home).join(".cargo/bin/espflash");
        if espflash.exists() {
            return Ok(format!("espflash ({})", espflash.display()));
        }
    }
    // Check PATH
    if std::process::Command::new("espflash")
        .arg("--version")
        .output()
        .map(|o| o.status.success())
        .unwrap_or(false)
    {
        return Ok("espflash".to_string());
    }
    if std::process::Command::new("esptool.py")
        .arg("version")
        .output()
        .map(|o| o.status.success())
        .unwrap_or(false)
    {
        return Ok("esptool.py".to_string());
    }
    if let Ok(idf_path) = std::env::var("IDF_PATH") {
        let esptool = std::path::PathBuf::from(&idf_path)
            .join("components/esptool_py/esptool/esptool.py");
        if esptool.exists() {
            return Ok(format!("esptool.py ({})", esptool.display()));
        }
    }
    Err("No flash tool found. Install espflash (cargo install espflash) or esptool.py.".to_string())
}

#[tauri::command]
pub fn check_idf_installed() -> bool {
    std::env::var("IDF_PATH").is_ok()
        || std::process::Command::new("idf.py")
            .arg("--version")
            .output()
            .map(|o| o.status.success())
            .unwrap_or(false)
}

#[tauri::command]
pub async fn flash_device(
    app: tauri::AppHandle,
    port: String,
    firmware_dir: Option<String>,
    build_from_source: bool,
    project_dir: Option<String>,
) -> Result<Vec<String>, String> {
    let config = FlashConfig {
        port,
        firmware_dir,
        build_from_source,
        project_dir,
    };

    log::info!("flash_device called: port={}, firmware_dir={:?}, build_from_source={}", 
        config.port, config.firmware_dir, config.build_from_source);

    let app_handle = app.clone();
    let result = tokio::task::spawn_blocking(move || {
        flasher::flash_firmware_sync(&config, |progress: FlashProgress| {
            log::info!("flash progress: {}", progress.message);
            let _ = app_handle.emit("flash-progress", &progress);
        })
    })
    .await
    .map_err(|e| format!("Task error: {}", e))?;

    log::info!("flash_device done, success={}", result.is_ok());
    result
}

// --- Pi installer commands ---

#[tauri::command]
pub async fn pi_test_connection(
    host: String,
    port: u16,
    username: String,
    password: Option<String>,
    key_path: Option<String>,
) -> Result<String, String> {
    let config = SshConfig {
        host,
        port,
        username,
        password,
        key_path,
    };
    tokio::task::spawn_blocking(move || pi_installer::test_connection(&config))
        .await
        .map_err(|e| e.to_string())?
}

#[tauri::command]
pub async fn pi_check_daemon(
    host: String,
    port: u16,
    username: String,
    password: Option<String>,
    key_path: Option<String>,
) -> Result<serde_json::Value, String> {
    let config = SshConfig {
        host,
        port,
        username,
        password,
        key_path,
    };
    let status = tokio::task::spawn_blocking(move || pi_installer::check_daemon_status(&config))
        .await
        .map_err(|e| e.to_string())??;
    serde_json::to_value(status).map_err(|e| e.to_string())
}

#[tauri::command]
pub async fn pi_install_daemon(
    app: tauri::AppHandle,
    host: String,
    ssh_port: u16,
    username: String,
    password: Option<String>,
    key_path: Option<String>,
    device_host: String,
    device_port: u16,
    transport: String,
    serial_device: Option<String>,
    tools_dir: String,
) -> Result<(), String> {
    let ssh_config = SshConfig {
        host,
        port: ssh_port,
        username,
        password,
        key_path,
    };
    let install_config = PiInstallConfig {
        device_host,
        device_port,
        transport,
        serial_device,
    };

    let app_handle = app.clone();
    tokio::task::spawn_blocking(move || {
        pi_installer::install_daemon(&ssh_config, &install_config, &tools_dir, |progress: PiInstallProgress| {
            let _ = app_handle.emit("pi-install-progress", &progress);
        })
    })
    .await
    .map_err(|e| e.to_string())?
}

#[tauri::command]
pub async fn pi_uninstall_daemon(
    app: tauri::AppHandle,
    host: String,
    port: u16,
    username: String,
    password: Option<String>,
    key_path: Option<String>,
) -> Result<(), String> {
    let config = SshConfig {
        host,
        port,
        username,
        password,
        key_path,
    };

    let app_handle = app.clone();
    tokio::task::spawn_blocking(move || {
        pi_installer::uninstall_daemon(&config, |progress: PiInstallProgress| {
            let _ = app_handle.emit("pi-install-progress", &progress);
        })
    })
    .await
    .map_err(|e| e.to_string())?
}

// --- Serial Wizard commands ---

#[tauri::command]
pub async fn wizard_connect(
    state: State<'_, AppState>,
    port: String,
) -> Result<Vec<String>, String> {
    let wizard_state = state.wizard.clone();
    tokio::task::spawn_blocking(move || {
        serial_wizard::connect(&wizard_state, &port, 115200)
    })
    .await
    .map_err(|e| format!("Task error: {}", e))?
}

#[tauri::command]
pub async fn wizard_disconnect(
    state: State<'_, AppState>,
) -> Result<(), String> {
    let wizard_state = state.wizard.clone();
    tokio::task::spawn_blocking(move || {
        serial_wizard::disconnect(&wizard_state)
    })
    .await
    .map_err(|e| format!("Task error: {}", e))?
}

#[tauri::command]
pub async fn wizard_send(
    state: State<'_, AppState>,
    command: String,
) -> Result<Vec<String>, String> {
    let wizard_state = state.wizard.clone();
    tokio::task::spawn_blocking(move || {
        serial_wizard::send_command(&wizard_state, &command)
    })
    .await
    .map_err(|e| format!("Task error: {}", e))?
}

#[tauri::command]
pub async fn wizard_reboot(
    state: State<'_, AppState>,
) -> Result<(), String> {
    let wizard_state = state.wizard.clone();
    tokio::task::spawn_blocking(move || {
        serial_wizard::reboot_device(&wizard_state)
    })
    .await
    .map_err(|e| format!("Task error: {}", e))?
}

#[tauri::command]
pub async fn wizard_poll(
    state: State<'_, AppState>,
) -> Result<Vec<String>, String> {
    let wizard_state = state.wizard.clone();
    tokio::task::spawn_blocking(move || {
        serial_wizard::poll(&wizard_state)
    })
    .await
    .map_err(|e| format!("Task error: {}", e))?
}

#[tauri::command]
pub async fn wizard_is_connected(state: State<'_, AppState>) -> Result<bool, String> {
    let wizard_state = state.wizard.clone();
    Ok(tokio::task::spawn_blocking(move || serial_wizard::is_connected(&wizard_state))
        .await
        .map_err(|e| format!("Task error: {e}"))?)
}

// --- BLE secondary transport (Nordic UART NDJSON) ---

#[tauri::command]
pub async fn ble_scan(
    name_filter: Option<String>,
) -> Result<Vec<BleDeviceInfo>, String> {
    ble_link::scan(name_filter, 6000).await
}

#[tauri::command]
pub async fn ble_connect(
    state: State<'_, AppState>,
    id: String,
) -> Result<Vec<String>, String> {
    ble_link::connect(&state.ble, &id).await
}

#[tauri::command]
pub async fn ble_disconnect(state: State<'_, AppState>) -> Result<(), String> {
    ble_link::disconnect(&state.ble).await
}

#[tauri::command]
pub async fn ble_send(
    state: State<'_, AppState>,
    command: String,
) -> Result<Vec<String>, String> {
    ble_link::send_command(&state.ble, &command).await
}

#[tauri::command]
pub async fn ble_poll(state: State<'_, AppState>) -> Result<Vec<String>, String> {
    ble_link::poll(&state.ble).await
}

#[tauri::command]
pub async fn ble_play(
    state: State<'_, AppState>,
    path: String,
    transition: Option<String>,
    duration_ms: Option<i32>,
) -> Result<Vec<String>, String> {
    ble_link::play(
        &state.ble,
        &path,
        transition.as_deref(),
        duration_ms,
    )
    .await
}

#[tauri::command]
pub async fn ble_stop(state: State<'_, AppState>) -> Result<Vec<String>, String> {
    ble_link::stop(&state.ble).await
}

#[tauri::command]
pub async fn ble_status(state: State<'_, AppState>) -> Result<Vec<String>, String> {
    ble_link::status(&state.ble).await
}

#[tauri::command]
pub async fn ble_is_connected(state: State<'_, AppState>) -> Result<bool, String> {
    Ok(ble_link::is_connected(&state.ble).await)
}

// --- Content Upload ---

/// Locate the directory that holds the bundled sample/test content.
///
/// In a packaged build this is the `content/` resource bundled alongside
/// the app (see `bundle.resources` in tauri.conf.json) — the app's current
/// working directory at runtime is *not* the project checkout, so a path
/// relative to it (as this used to assume) never resolves once installed.
/// In dev mode (`npm run tauri dev`), fall back to the repo-relative
/// `../content` folder next to `pd-control/`, since no bundled resource
/// exists yet.
fn resolve_content_dir(app: &tauri::AppHandle) -> Result<std::path::PathBuf, String> {
    if let Ok(resource_path) = app.path().resolve("content", tauri::path::BaseDirectory::Resource) {
        if resource_path.exists() {
            return Ok(resource_path);
        }
    }

    let mut dev_path = std::env::current_dir().map_err(|e| e.to_string())?;
    dev_path.push("..");
    dev_path.push("content");
    dev_path
        .canonicalize()
        .map_err(|e| format!("Failed to resolve content directory (checked bundled resources and dev path): {}", e))
}

const UPLOAD_MAX_BYTES: u64 = 2 * 1024 * 1024;

async fn resolve_upload_mode(
    state: &AppState,
    device_ip: &str,
    via: Option<&str>,
) -> Result<&'static str, String> {
    let ble_up = ble_link::is_connected(&state.ble).await;
    let wizard_state = state.wizard.clone();
    let usb_up = tokio::task::spawn_blocking(move || serial_wizard::is_connected(&wizard_state))
        .await
        .unwrap_or(false);

    match via {
        Some("wifi") => Ok("wifi"),
        Some("bluetooth") => {
            if !ble_up {
                return Err("Bluetooth not connected".into());
            }
            Ok("bluetooth")
        }
        Some("usb") => {
            if !usb_up {
                return Err("USB wizard not connected".into());
            }
            Ok("usb")
        }
        _ => {
            if !device_ip.is_empty() {
                Ok("wifi")
            } else if ble_up {
                Ok("bluetooth")
            } else if usb_up {
                Ok("usb")
            } else {
                Err("No upload transport available".into())
            }
        }
    }
}

async fn http_upload(ip: &str, port: u16, remote: &str, data: Vec<u8>) -> Result<(), String> {
    let client = reqwest::Client::new();
    let url = format!("http://{}:{}/api/upload?path={}", ip, port, remote);
    client
        .post(&url)
        .body(data)
        .send()
        .await
        .map_err(|e| format!("Upload failed: {e}"))?;
    Ok(())
}

async fn upload_one(
    state: &AppState,
    mode: &str,
    device_ip: &str,
    device_port: u16,
    local: &std::path::Path,
    remote: &str,
) -> Result<(), String> {
    match mode {
        "bluetooth" => ble_link::upload_path(&state.ble, local, remote).await,
        "usb" => {
            let wizard_state = state.wizard.clone();
            let path_c = local.to_path_buf();
            let remote_c = remote.to_string();
            tokio::task::spawn_blocking(move || {
                serial_wizard::upload_path(&wizard_state, &path_c, &remote_c)
            })
            .await
            .map_err(|e| format!("Task error: {e}"))?
        }
        _ => {
            let data = std::fs::read(local).map_err(|e| format!("Failed to read file: {e}"))?;
            http_upload(device_ip, device_port, remote, data).await
        }
    }
}

#[tauri::command]
pub async fn upload_content_to_device(
    app: tauri::AppHandle,
    state: State<'_, AppState>,
    device_ip: String,
    device_port: u16,
    content_path: String,
    via: Option<String>,
) -> Result<(), String> {
    let content_dir = resolve_content_dir(&app)?;

    // Build full path to content file/directory
    let full_path = content_dir.join(&content_path);

    if !full_path.exists() {
        return Err(format!(
            "Content not found: {} (looked in: {})",
            content_path,
            full_path.display()
        ));
    }

    let mode = resolve_upload_mode(&state, &device_ip, via.as_deref()).await?;

    if full_path.is_dir() {
        let entries =
            std::fs::read_dir(&full_path).map_err(|e| format!("Failed to read directory: {}", e))?;

        for entry in entries {
            let entry = entry.map_err(|e| e.to_string())?;
            let path = entry.path();

            if path.is_file() {
                let file_name = path
                    .file_name()
                    .and_then(|n| n.to_str())
                    .ok_or("Invalid file name")?;

                let remote_path = format!("{}/{}", content_path, file_name);
                upload_one(
                    &state,
                    mode,
                    &device_ip,
                    device_port,
                    &path,
                    &remote_path,
                )
                .await?;
            }
        }
    } else {
        upload_one(
            &state,
            mode,
            &device_ip,
            device_port,
            &full_path,
            &content_path,
        )
        .await?;
    }

    Ok(())
}

/// Upload an arbitrary local PNG into the device content library.
#[tauri::command]
pub async fn upload_local_file_to_device(
    state: State<'_, AppState>,
    device_ip: String,
    device_port: u16,
    local_path: String,
    remote_path: Option<String>,
    via: Option<String>,
) -> Result<String, String> {
    let local = std::path::PathBuf::from(&local_path);
    if !local.is_file() {
        return Err(format!("Not a file: {local_path}"));
    }

    let file_name = local
        .file_name()
        .and_then(|n| n.to_str())
        .ok_or("Invalid file name")?;
    if !file_name.to_ascii_lowercase().ends_with(".png") {
        return Err("Only PNG files are supported".into());
    }

    let meta = std::fs::metadata(&local).map_err(|e| format!("stat failed: {e}"))?;
    if meta.len() > UPLOAD_MAX_BYTES {
        return Err(format!(
            "File too large ({} bytes); max is {} bytes",
            meta.len(),
            UPLOAD_MAX_BYTES
        ));
    }

    let remote = remote_path.unwrap_or_else(|| format!("images/{file_name}"));
    let mode = resolve_upload_mode(&state, &device_ip, via.as_deref()).await?;
    upload_one(&state, mode, &device_ip, device_port, &local, &remote).await?;
    Ok(remote)
}
