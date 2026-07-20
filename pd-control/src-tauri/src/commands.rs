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

// --- Event tracing ---

#[tauri::command]
pub fn set_http_trace_enabled(enabled: bool) -> Result<(), String> {
    crate::http_trace::set_enabled(enabled);
    log::info!("HTTP wire tracing {}", if enabled { "enabled" } else { "disabled" });
    Ok(())
}

#[tauri::command]
pub fn get_http_trace_enabled() -> Result<bool, String> {
    Ok(crate::http_trace::is_enabled())
}

#[tauri::command]
pub fn append_trace_event(event: serde_json::Value) -> Result<(), String> {
    crate::trace_file::append_event(event)
}

#[tauri::command]
pub fn reveal_control_event_log() -> Result<String, String> {
    crate::trace_file::reveal_in_finder()
}

#[tauri::command]
pub fn control_event_log_path() -> Result<String, String> {
    crate::trace_file::log_path().map(|p| p.to_string_lossy().to_string())
}

#[tauri::command]
pub async fn device_log(ip: String, port: u16) -> Result<serde_json::Value, String> {
    let api = DeviceApi::new(&ip, port);
    api.device_log().await
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
pub async fn device_delete_content(
    ip: String,
    port: u16,
    path: String,
) -> Result<serde_json::Value, String> {
    let api = DeviceApi::new(&ip, port);
    api.delete_content(&path).await
}

#[tauri::command]
pub async fn device_rename_content(
    ip: String,
    port: u16,
    from: String,
    to: String,
) -> Result<serde_json::Value, String> {
    let api = DeviceApi::new(&ip, port);
    api.rename_content(&from, &to).await
}

#[tauri::command]
pub async fn device_set_content_meta(
    ip: String,
    port: u16,
    path: String,
    fps: u32,
) -> Result<serde_json::Value, String> {
    let api = DeviceApi::new(&ip, port);
    api.set_content_meta(&path, fps).await
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
    /* Keep '/' unencoded in the query — reqwest's .query() turns it into %2F
     * which ESP httpd does not decode (upload/meta would land on a wrong path). */
    let url = format!("http://{}:{}/api/upload?path={}", ip, port, remote);
    let resp = client
        .post(&url)
        .body(data)
        .send()
        .await
        .map_err(|e| format!("Upload failed: {e}"))?;
    if !resp.status().is_success() {
        let status = resp.status();
        let body = resp.text().await.unwrap_or_default();
        return Err(format!(
            "Upload failed for '{remote}': HTTP {status}{}",
            if body.is_empty() {
                String::new()
            } else {
                format!(" ({body})")
            }
        ));
    }
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

fn sanitize_content_name(name: &str) -> String {
    let mut out = String::with_capacity(name.len());
    for ch in name.chars() {
        if ch.is_ascii_alphanumeric() || ch == '-' || ch == '_' {
            out.push(ch);
        } else if ch == ' ' {
            out.push('-');
        }
    }
    while out.starts_with('.') {
        out.remove(0);
    }
    if out.is_empty() {
        "sequence".into()
    } else {
        out
    }
}

/// Detect `prefix%0Nd.png` + start index from sorted frame file names.
fn detect_frame_pattern(file_names: &[String]) -> Result<(String, i32, usize), String> {
    if file_names.is_empty() {
        return Err("No PNG frames found in folder".into());
    }

    let mut parsed: Vec<(String, usize, i32)> = Vec::new();
    for name in file_names {
        let stem = name
            .strip_suffix(".png")
            .or_else(|| name.strip_suffix(".PNG"))
            .unwrap_or(name);
        let digit_count = stem.chars().rev().take_while(|c| c.is_ascii_digit()).count();
        if digit_count == 0 {
            return Err(format!(
                "Frame '{name}' has no trailing frame number (expected e.g. frame0001.png)"
            ));
        }
        let split = stem.len() - digit_count;
        let prefix = stem[..split].to_string();
        let num: i32 = stem[split..]
            .parse()
            .map_err(|_| format!("Invalid frame number in '{name}'"))?;
        parsed.push((prefix, digit_count, num));
    }

    let (prefix0, width0, start) = &parsed[0];
    for (i, (prefix, width, num)) in parsed.iter().enumerate() {
        if prefix != prefix0 || width != width0 {
            return Err(
                "Frames must share one naming pattern (same prefix and digit width)".into(),
            );
        }
        if *num != start + i as i32 {
            return Err(format!(
                "Frame numbers must be contiguous (gap before frame {})",
                start + i as i32
            ));
        }
    }

    let pattern = format!("{prefix0}%0{width0}d.png");
    Ok((pattern, *start, parsed.len()))
}

fn collect_top_level_pngs(dir: &std::path::Path) -> Result<Vec<std::path::PathBuf>, String> {
    let mut frames = Vec::new();
    let entries = std::fs::read_dir(dir).map_err(|e| format!("Failed to read folder: {e}"))?;
    for entry in entries {
        let entry = entry.map_err(|e| e.to_string())?;
        let path = entry.path();
        if !path.is_file() {
            continue;
        }
        let name = path
            .file_name()
            .and_then(|n| n.to_str())
            .unwrap_or("")
            .to_string();
        if name.eq_ignore_ascii_case("meta.json") {
            continue;
        }
        if name.to_ascii_lowercase().ends_with(".png") {
            frames.push(path);
        }
    }
    frames.sort_by(|a, b| {
        a.file_name()
            .unwrap_or_default()
            .cmp(b.file_name().unwrap_or_default())
    });
    Ok(frames)
}

async fn upload_bytes_one(
    state: &AppState,
    mode: &str,
    device_ip: &str,
    device_port: u16,
    remote: &str,
    data: Vec<u8>,
) -> Result<(), String> {
    match mode {
        "bluetooth" => ble_link::upload_bytes(&state.ble, remote, &data).await,
        "usb" => {
            let wizard_state = state.wizard.clone();
            let remote_c = remote.to_string();
            tokio::task::spawn_blocking(move || {
                serial_wizard::upload_bytes(&wizard_state, &remote_c, &data)
            })
            .await
            .map_err(|e| format!("Task error: {e}"))?
        }
        _ => http_upload(device_ip, device_port, remote, data).await,
    }
}

/// Upload a local folder of numbered PNGs as `images/<name>/` with generated meta.json.
///
/// Optional conventions in the source folder:
/// - `overlay/` — PNG sequence uploaded to `overlays/<name>` and linked in meta
/// - `background.png` — uploaded to `backgrounds/<name>-bg.png` and linked in meta
#[tauri::command]
pub async fn upload_local_sequence_to_device(
    state: State<'_, AppState>,
    device_ip: String,
    device_port: u16,
    local_dir: String,
    fps: u32,
    remote_name: Option<String>,
    via: Option<String>,
) -> Result<String, String> {
    if !(1..=120).contains(&fps) {
        return Err("FPS must be between 1 and 120".into());
    }

    let dir = std::path::PathBuf::from(&local_dir);
    if !dir.is_dir() {
        return Err(format!("Not a folder: {local_dir}"));
    }

    let folder_name = dir
        .file_name()
        .and_then(|n| n.to_str())
        .unwrap_or("sequence");
    let seq_name = sanitize_content_name(
        remote_name
            .as_deref()
            .map(str::trim)
            .filter(|s| !s.is_empty())
            .unwrap_or(folder_name),
    );

    let frames = collect_top_level_pngs(&dir)?;
    if frames.is_empty() {
        return Err("Folder has no PNG frames (put frames in the folder root)".into());
    }

    let names: Vec<String> = frames
        .iter()
        .map(|p| {
            p.file_name()
                .and_then(|n| n.to_str())
                .unwrap_or("")
                .to_string()
        })
        .collect();
    let (pattern, start, frame_count) = detect_frame_pattern(&names)?;

    for path in &frames {
        let meta = std::fs::metadata(path).map_err(|e| format!("stat failed: {e}"))?;
        if meta.len() > UPLOAD_MAX_BYTES {
            return Err(format!(
                "Frame {} is too large ({} bytes); max is {} bytes",
                path.display(),
                meta.len(),
                UPLOAD_MAX_BYTES
            ));
        }
    }

    let mode = resolve_upload_mode(&state, &device_ip, via.as_deref()).await?;
    let remote_root = format!("images/{seq_name}");

    for path in &frames {
        let file_name = path
            .file_name()
            .and_then(|n| n.to_str())
            .ok_or("Invalid frame name")?;
        let remote = format!("{remote_root}/{file_name}");
        upload_one(
            &state,
            mode,
            &device_ip,
            device_port,
            path,
            &remote,
        )
        .await?;
    }

    let mut background: Option<String> = None;
    let bg_path = dir.join("background.png");
    if bg_path.is_file() {
        let remote_bg = format!("backgrounds/{seq_name}-bg.png");
        upload_one(
            &state,
            mode,
            &device_ip,
            device_port,
            &bg_path,
            &remote_bg,
        )
        .await?;
        background = Some(remote_bg);
    }

    let mut overlay: Option<String> = None;
    let overlay_dir = dir.join("overlay");
    if overlay_dir.is_dir() {
        let ov_frames = collect_top_level_pngs(&overlay_dir)?;
        if !ov_frames.is_empty() {
            let ov_names: Vec<String> = ov_frames
                .iter()
                .map(|p| {
                    p.file_name()
                        .and_then(|n| n.to_str())
                        .unwrap_or("")
                        .to_string()
                })
                .collect();
            let (ov_pattern, ov_start, ov_count) = detect_frame_pattern(&ov_names)?;
            let ov_remote_root = format!("overlays/{seq_name}");
            for path in &ov_frames {
                let file_name = path
                    .file_name()
                    .and_then(|n| n.to_str())
                    .ok_or("Invalid overlay frame name")?;
                let remote = format!("{ov_remote_root}/{file_name}");
                upload_one(
                    &state,
                    mode,
                    &device_ip,
                    device_port,
                    path,
                    &remote,
                )
                .await?;
            }
            let ov_meta = serde_json::json!({
                "name": format!("{seq_name} overlay"),
                "fps": fps,
                "loop": true,
                "frames": ov_count,
                "pattern": ov_pattern,
                "start": ov_start,
            });
            let ov_meta_bytes = serde_json::to_vec(&ov_meta)
                .map_err(|e| format!("Failed to encode overlay meta: {e}"))?;
            upload_bytes_one(
                &state,
                mode,
                &device_ip,
                device_port,
                &format!("{ov_remote_root}/meta.json"),
                ov_meta_bytes,
            )
            .await?;
            overlay = Some(ov_remote_root);
        }
    }

    let mut meta = serde_json::json!({
        "name": seq_name,
        "fps": fps,
        "loop": true,
        "frames": frame_count,
        "pattern": pattern,
        "start": start,
    });
    if let Some(bg) = background {
        meta.as_object_mut()
            .unwrap()
            .insert("background".into(), serde_json::Value::String(bg));
    }
    if let Some(ov) = overlay {
        meta.as_object_mut()
            .unwrap()
            .insert("overlay".into(), serde_json::Value::String(ov));
    }

    let meta_bytes =
        serde_json::to_vec(&meta).map_err(|e| format!("Failed to encode meta.json: {e}"))?;
    upload_bytes_one(
        &state,
        mode,
        &device_ip,
        device_port,
        &format!("{remote_root}/meta.json"),
        meta_bytes,
    )
    .await?;

    Ok(remote_root)
}
