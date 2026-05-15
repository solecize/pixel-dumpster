use crate::daemon_api::DaemonApi;
use crate::device_api::DeviceApi;
use crate::discovery::{DiscoveredDevice, DiscoveryState};
use crate::flasher::{self, FlashConfig, FlashProgress, SerialPortInfo};
use crate::pi_installer::{self, PiInstallConfig, PiInstallProgress, SshConfig};
use crate::serial_wizard;
use std::sync::Mutex;
use tauri::{Emitter, State};

pub struct AppState {
    pub discovery: Mutex<DiscoveryState>,
    pub wizard: serial_wizard::SharedWizardState,
}

impl Default for AppState {
    fn default() -> Self {
        Self {
            discovery: Mutex::new(DiscoveryState::new()),
            wizard: serial_wizard::new_shared_state(),
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

// --- Content Upload ---

#[tauri::command]
pub async fn upload_content_to_device(
    device_ip: String,
    device_port: u16,
    content_path: String,
) -> Result<(), String> {
    // Find the project content directory (go up from pd-control to project root)
    let mut content_dir = std::env::current_dir().map_err(|e| e.to_string())?;
    content_dir.push("..");
    content_dir.push("content");
    
    // Canonicalize to resolve .. and symlinks
    let content_dir = content_dir.canonicalize()
        .map_err(|e| format!("Failed to resolve content directory: {}", e))?;
    
    // Build full path to content file/directory
    let full_path = content_dir.join(&content_path);
    
    if !full_path.exists() {
        return Err(format!("Content not found: {} (looked in: {})", content_path, full_path.display()));
    }
    
    let client = reqwest::Client::new();
    
    // If it's a directory (sequence), upload all files
    if full_path.is_dir() {
        let entries = std::fs::read_dir(&full_path)
            .map_err(|e| format!("Failed to read directory: {}", e))?;
        
        for entry in entries {
            let entry = entry.map_err(|e| e.to_string())?;
            let path = entry.path();
            
            if path.is_file() {
                let file_name = path.file_name()
                    .and_then(|n| n.to_str())
                    .ok_or("Invalid file name")?;
                
                let remote_path = format!("{}/{}", content_path, file_name);
                let data = std::fs::read(&path)
                    .map_err(|e| format!("Failed to read file: {}", e))?;
                
                let url = format!("http://{}:{}/api/upload?path={}", device_ip, device_port, remote_path);
                client.post(&url)
                    .body(data)
                    .send()
                    .await
                    .map_err(|e| format!("Upload failed: {}", e))?;
            }
        }
    } else {
        // Single file upload
        let data = std::fs::read(&full_path)
            .map_err(|e| format!("Failed to read file: {}", e))?;
        
        let url = format!("http://{}:{}/api/upload?path={}", device_ip, device_port, content_path);
        client.post(&url)
            .body(data)
            .send()
            .await
            .map_err(|e| format!("Upload failed: {}", e))?;
    }
    
    Ok(())
}
