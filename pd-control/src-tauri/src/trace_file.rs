//! Durable NDJSON control-event log under the app data directory.

use std::fs::{self, OpenOptions};
use std::io::Write;
use std::path::PathBuf;
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::{Mutex, OnceLock};

use serde_json::Value;
use tauri::{AppHandle, Manager};

static APP: OnceLock<AppHandle> = OnceLock::new();
static SEQ: AtomicU64 = AtomicU64::new(1);
static WRITE_LOCK: Mutex<()> = Mutex::new(());

const MAX_BYTES: u64 = 5 * 1024 * 1024;

pub fn init(app: AppHandle) {
    let _ = APP.set(app);
}

fn logs_dir() -> Result<PathBuf, String> {
    let app = APP.get().ok_or_else(|| "trace_file not initialized".to_string())?;
    let dir = app
        .path()
        .app_data_dir()
        .map_err(|e| format!("app_data_dir: {e}"))?
        .join("logs");
    fs::create_dir_all(&dir).map_err(|e| format!("create logs dir: {e}"))?;
    Ok(dir)
}

pub fn log_path() -> Result<PathBuf, String> {
    Ok(logs_dir()?.join("control-events.log"))
}

fn rotate_if_needed(path: &PathBuf) -> Result<(), String> {
    let meta = match fs::metadata(path) {
        Ok(m) => m,
        Err(_) => return Ok(()),
    };
    if meta.len() < MAX_BYTES {
        return Ok(());
    }
    let rotated = path.with_extension("1.log");
    let _ = fs::remove_file(&rotated);
    fs::rename(path, &rotated).map_err(|e| format!("rotate log: {e}"))?;
    Ok(())
}

/// Append one JSON object as a single NDJSON line. Adds monotonic `seq` + `ts`.
pub fn append_event(mut payload: Value) -> Result<(), String> {
    let _guard = WRITE_LOCK
        .lock()
        .map_err(|_| "trace file lock poisoned".to_string())?;
    let path = log_path()?;
    rotate_if_needed(&path)?;

    if let Some(obj) = payload.as_object_mut() {
        obj.insert(
            "seq".to_string(),
            Value::from(SEQ.fetch_add(1, Ordering::Relaxed)),
        );
        if !obj.contains_key("ts") {
            let ms = std::time::SystemTime::now()
                .duration_since(std::time::UNIX_EPOCH)
                .map(|d| d.as_millis() as u64)
                .unwrap_or(0);
            obj.insert("ts".to_string(), Value::from(ms));
        }
    }

    let line = serde_json::to_string(&payload).map_err(|e| e.to_string())?;
    let mut f = OpenOptions::new()
        .create(true)
        .append(true)
        .open(&path)
        .map_err(|e| format!("open log: {e}"))?;
    writeln!(f, "{line}").map_err(|e| format!("write log: {e}"))?;
    Ok(())
}

pub fn reveal_in_finder() -> Result<String, String> {
    let path = log_path()?;
    if !path.exists() {
        // Touch empty file so Reveal always has a target.
        append_event(serde_json::json!({
            "phase": "state",
            "action": "log_init",
            "summary": "control event log created",
        }))?;
    }
    let path_str = path.to_string_lossy().to_string();
    #[cfg(target_os = "macos")]
    {
        std::process::Command::new("open")
            .args(["-R", &path_str])
            .spawn()
            .map_err(|e| format!("open -R: {e}"))?;
    }
    #[cfg(target_os = "windows")]
    {
        std::process::Command::new("explorer")
            .args(["/select,", &path_str])
            .spawn()
            .map_err(|e| format!("explorer: {e}"))?;
    }
    #[cfg(all(unix, not(target_os = "macos")))]
    {
        if let Some(parent) = path.parent() {
            std::process::Command::new("xdg-open")
                .arg(parent)
                .spawn()
                .map_err(|e| format!("xdg-open: {e}"))?;
        }
    }
    Ok(path_str)
}
