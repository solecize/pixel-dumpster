//! BLE Nordic UART link for wizard + control NDJSON (secondary to WiFi/HTTP).

use std::collections::VecDeque;
use std::path::Path;
use std::sync::Arc;
use std::time::Duration;

use base64::{engine::general_purpose::STANDARD as B64, Engine as _};
use btleplug::api::{
    Central, CharPropFlags, Manager as _, Peripheral as _, ScanFilter, WriteType,
};
use btleplug::platform::{Adapter, Manager, Peripheral};
use futures_util::StreamExt;
use serde::Serialize;
use serde_json::Value;
use tokio::sync::Mutex;
use uuid::Uuid;

/// Raw bytes per upload_chunk (base64 keeps the NDJSON line under ATT/line limits).
const UPLOAD_CHUNK_RAW: usize = 150;

const NUS_RX: Uuid = Uuid::from_u128(0x6e400002_b5a3_f393_e0a9_e50e24dcca9e);
const NUS_TX: Uuid = Uuid::from_u128(0x6e400003_b5a3_f393_e0a9_e50e24dcca9e);

#[derive(Clone, Serialize)]
pub struct BleDeviceInfo {
    pub id: String,
    pub name: String,
    pub rssi: Option<i16>,
}

struct BleConn {
    peripheral: Peripheral,
    rx_chr: btleplug::api::Characteristic,
    lines: Arc<Mutex<VecDeque<String>>>,
    #[allow(dead_code)]
    notify_buf: Arc<Mutex<Vec<u8>>>, /* kept alive for notify task */
}

pub struct BleState {
    conn: Option<BleConn>,
    /// Serializes command/response so a status poll cannot steal a play ack
    /// (which previously made the UI think content was missing and re-upload).
    io: Arc<Mutex<()>>,
}

impl BleState {
    pub fn new() -> Self {
        Self {
            conn: None,
            io: Arc::new(Mutex::new(())),
        }
    }
}

pub type SharedBleState = Arc<Mutex<BleState>>;

pub fn new_shared_state() -> SharedBleState {
    Arc::new(Mutex::new(BleState::new()))
}

async fn io_lock(state: &SharedBleState) -> tokio::sync::OwnedMutexGuard<()> {
    let io = state.lock().await.io.clone();
    io.lock_owned().await
}

async fn adapter() -> Result<Adapter, String> {
    let manager = Manager::new().await.map_err(|e| format!("BLE manager: {e}"))?;
    let adapters = manager
        .adapters()
        .await
        .map_err(|e| format!("BLE adapters: {e}"))?;
    adapters
        .into_iter()
        .next()
        .ok_or_else(|| "No Bluetooth adapter found".to_string())
}

pub async fn scan(name_filter: Option<String>, timeout_ms: u64) -> Result<Vec<BleDeviceInfo>, String> {
    let adapter = adapter().await?;
    adapter
        .start_scan(ScanFilter::default())
        .await
        .map_err(|e| format!("BLE scan start: {e}"))?;
    tokio::time::sleep(Duration::from_millis(timeout_ms.max(1000))).await;
    let periphs = adapter
        .peripherals()
        .await
        .map_err(|e| format!("BLE peripherals: {e}"))?;
    let _ = adapter.stop_scan().await;

    let needle = name_filter
        .unwrap_or_else(|| "pixel-dumpster".into())
        .to_lowercase();
    let mut out = Vec::new();
    for p in periphs {
        let props = p.properties().await.ok().flatten();
        let name = props
            .as_ref()
            .and_then(|pr| pr.local_name.clone())
            .unwrap_or_default();
        if name.is_empty() {
            continue;
        }
        if !name.to_lowercase().contains(&needle) {
            continue;
        }
        let id = p.id().to_string();
        let rssi = props.as_ref().and_then(|pr| pr.rssi);
        out.push(BleDeviceInfo { id, name, rssi });
    }
    out.sort_by(|a, b| a.name.cmp(&b.name));
    out.dedup_by(|a, b| a.id == b.id);
    Ok(out)
}

async fn find_peripheral(id: &str) -> Result<Peripheral, String> {
    let adapter = adapter().await?;
    // Brief scan so macOS populates the peripheral list.
    let _ = adapter.start_scan(ScanFilter::default()).await;
    tokio::time::sleep(Duration::from_millis(1500)).await;
    let _ = adapter.stop_scan().await;
    let periphs = adapter
        .peripherals()
        .await
        .map_err(|e| format!("BLE peripherals: {e}"))?;
    for p in periphs {
        if p.id().to_string() == id {
            return Ok(p);
        }
    }
    Err(format!("BLE device not found: {id}"))
}

fn drain_lines(buf: &mut Vec<u8>, q: &mut VecDeque<String>) {
    loop {
        if let Some(pos) = buf.iter().position(|&b| b == b'\n') {
            let line = String::from_utf8_lossy(&buf[..=pos]).trim().to_string();
            buf.drain(..=pos);
            if line.starts_with('{') {
                q.push_back(line);
            }
        } else {
            break;
        }
    }
}

pub async fn connect(state: &SharedBleState, id: &str) -> Result<Vec<String>, String> {
    disconnect(state).await?;

    let peripheral = find_peripheral(id).await?;
    peripheral
        .connect()
        .await
        .map_err(|e| format!("BLE connect: {e}"))?;
    peripheral
        .discover_services()
        .await
        .map_err(|e| format!("BLE discover: {e}"))?;

    let chars = peripheral.characteristics();
    let rx_chr = chars
        .iter()
        .find(|c| c.uuid == NUS_RX)
        .cloned()
        .ok_or("NUS RX characteristic missing")?;
    let tx_chr = chars
        .iter()
        .find(|c| c.uuid == NUS_TX && c.properties.contains(CharPropFlags::NOTIFY))
        .cloned()
        .ok_or("NUS TX characteristic missing")?;

    let lines = Arc::new(Mutex::new(VecDeque::new()));
    let notify_buf = Arc::new(Mutex::new(Vec::new()));
    let lines_n = lines.clone();
    let buf_n = notify_buf.clone();

    peripheral
        .subscribe(&tx_chr)
        .await
        .map_err(|e| format!("BLE subscribe: {e}"))?;

    let mut notifications = peripheral
        .notifications()
        .await
        .map_err(|e| format!("BLE notifications: {e}"))?;

    tokio::spawn(async move {
        while let Some(n) = notifications.next().await {
            if n.uuid != NUS_TX {
                continue;
            }
            let mut buf = buf_n.lock().await;
            buf.extend_from_slice(&n.value);
            let mut q = lines_n.lock().await;
            drain_lines(&mut buf, &mut q);
        }
    });

    {
        let mut guard = state.lock().await;
        guard.conn = Some(BleConn {
            peripheral,
            rx_chr,
            lines,
            notify_buf,
        });
    }

    // Hello handshake (same as USB wizard)
    send_command(state, r#"{"cmd":"hello"}"#).await
}

pub async fn disconnect(state: &SharedBleState) -> Result<(), String> {
    let _io = io_lock(state).await;
    let mut guard = state.lock().await;
    if let Some(conn) = guard.conn.take() {
        let _ = conn
            .peripheral
            .write(
                &conn.rx_chr,
                b"{\"cmd\":\"goodbye\"}\n",
                WriteType::WithoutResponse,
            )
            .await;
        let _ = conn.peripheral.disconnect().await;
    }
    Ok(())
}

async fn take_lines(state: &SharedBleState) -> Vec<String> {
    let guard = state.lock().await;
    if let Some(ref conn) = guard.conn {
        let mut q = conn.lines.lock().await;
        q.drain(..).collect()
    } else {
        Vec::new()
    }
}

async fn write_raw(state: &SharedBleState, data: &[u8]) -> Result<(), String> {
    let guard = state.lock().await;
    let conn = guard.conn.as_ref().ok_or("BLE not connected")?;
    for chunk in data.chunks(180) {
        conn.peripheral
            .write(&conn.rx_chr, chunk, WriteType::WithoutResponse)
            .await
            .map_err(|e| format!("BLE write: {e}"))?;
    }
    Ok(())
}

/// Poll for NDJSON lines until `pred` matches (or timeout). Non-matching lines
/// are kept and returned alongside the match so callers can inspect them.
async fn wait_lines<F>(
    state: &SharedBleState,
    timeout_ms: u64,
    mut pred: F,
) -> Result<Vec<String>, String>
where
    F: FnMut(&Value) -> bool,
{
    let deadline = tokio::time::Instant::now() + Duration::from_millis(timeout_ms);
    let mut collected = Vec::new();
    loop {
        let lines = take_lines(state).await;
        for line in lines {
            let Ok(v) = serde_json::from_str::<Value>(&line) else {
                continue;
            };
            collected.push(line);
            if pred(&v) {
                // Brief settle for any trailing fragments.
                tokio::time::sleep(Duration::from_millis(40)).await;
                collected.extend(take_lines(state).await);
                return Ok(collected);
            }
        }
        if tokio::time::Instant::now() >= deadline {
            if collected.is_empty() {
                return Err("BLE timeout waiting for response".into());
            }
            return Ok(collected);
        }
        tokio::time::sleep(Duration::from_millis(40)).await;
    }
}

async fn wait_ack(state: &SharedBleState, cmd: &str, timeout_ms: u64) -> Result<Value, String> {
    let deadline = tokio::time::Instant::now() + Duration::from_millis(timeout_ms);
    loop {
        let lines = take_lines(state).await;
        for line in lines {
            let Ok(v) = serde_json::from_str::<Value>(&line) else {
                continue;
            };
            if v.get("type").and_then(|t| t.as_str()) != Some("ack") {
                continue;
            }
            if v.get("cmd").and_then(|c| c.as_str()) != Some(cmd) {
                continue;
            }
            if v.get("ok").and_then(|o| o.as_bool()) == Some(true) {
                return Ok(v);
            }
            let err = v
                .get("error")
                .and_then(|e| e.as_str())
                .unwrap_or("command failed");
            return Err(format!("BLE {cmd}: {err}"));
        }
        if tokio::time::Instant::now() >= deadline {
            return Err(format!("BLE {cmd}: timeout waiting for ack"));
        }
        tokio::time::sleep(Duration::from_millis(40)).await;
    }
}

pub async fn send_command(state: &SharedBleState, json_cmd: &str) -> Result<Vec<String>, String> {
    let _io = io_lock(state).await;

    let mut cmd = json_cmd.to_string();
    if !cmd.ends_with('\n') {
        cmd.push('\n');
    }

    // Prefer waiting for the matching response — not "first JSON line" —
    // so a late status notify can't steal list/set_playback/play traffic.
    let expect_cmd = serde_json::from_str::<Value>(cmd.trim())
        .ok()
        .and_then(|v| v.get("cmd").and_then(|c| c.as_str()).map(|s| s.to_string()));

    write_raw(state, cmd.as_bytes()).await?;

    match expect_cmd.as_deref() {
        Some("status") => {
            wait_lines(state, 8000, |v| {
                v.get("type").and_then(|t| t.as_str()) == Some("status")
            })
            .await
        }
        Some("list") => {
            wait_lines(state, 12000, |v| {
                v.get("type").and_then(|t| t.as_str()) == Some("list")
            })
            .await
        }
        Some(ack_cmd) => {
            // play/stop/set_playback/upload_* — wait for matching ack only.
            let ack = wait_ack(state, ack_cmd, 8000).await?;
            Ok(vec![ack.to_string()])
        }
        None => wait_lines(state, 8000, |_| true).await,
    }
}

pub async fn poll(state: &SharedBleState) -> Result<Vec<String>, String> {
    let guard = state.lock().await;
    if guard.conn.is_none() {
        return Err("BLE not connected".into());
    }
    drop(guard);
    Ok(take_lines(state).await)
}

pub async fn is_connected(state: &SharedBleState) -> bool {
    state.lock().await.conn.is_some()
}

/// Convenience: play/stop/status over BLE when WiFi is unavailable.
pub async fn play(
    state: &SharedBleState,
    path: &str,
    transition: Option<&str>,
    duration_ms: Option<i32>,
) -> Result<Vec<String>, String> {
    let _io = io_lock(state).await;
    let cmd = match (transition, duration_ms) {
        (Some(t), Some(d)) if !t.is_empty() && d > 0 => format!(
            r#"{{"cmd":"play","path":"{}","transition":"{}","duration_ms":{}}}"#,
            path, t, d
        ),
        _ => format!(r#"{{"cmd":"play","path":"{}"}}"#, path),
    };
    write_raw(state, format!("{cmd}\n").as_bytes()).await?;
    let ack = wait_ack(state, "play", 8000).await?;
    Ok(vec![ack.to_string()])
}

pub async fn stop(state: &SharedBleState) -> Result<Vec<String>, String> {
    let _io = io_lock(state).await;
    write_raw(state, b"{\"cmd\":\"stop\"}\n").await?;
    let ack = wait_ack(state, "stop", 8000).await?;
    Ok(vec![ack.to_string()])
}

pub async fn status(state: &SharedBleState) -> Result<Vec<String>, String> {
    let _io = io_lock(state).await;
    write_raw(state, b"{\"cmd\":\"status\"}\n").await?;
    wait_lines(state, 5000, |v| v.get("type").and_then(|t| t.as_str()) == Some("status")).await
}

async fn send_expect_ack(state: &SharedBleState, json_cmd: &str, ack_cmd: &str) -> Result<(), String> {
    let mut cmd = json_cmd.to_string();
    if !cmd.ends_with('\n') {
        cmd.push('\n');
    }
    write_raw(state, cmd.as_bytes()).await?;
    wait_ack(state, ack_cmd, 8000).await?;
    Ok(())
}

/// Chunked content upload over BLE NDJSON (`upload_begin` / `upload_chunk` / `upload_end`).
pub async fn upload_bytes(
    state: &SharedBleState,
    remote_path: &str,
    data: &[u8],
) -> Result<(), String> {
    if data.is_empty() {
        return Err("empty upload".into());
    }
    if !is_connected(state).await {
        return Err("BLE not connected".into());
    }

    let _io = io_lock(state).await;

    let begin = format!(
        r#"{{"cmd":"upload_begin","path":"{}","size":{}}}"#,
        remote_path,
        data.len()
    );
    if let Err(e) = send_expect_ack(state, &begin, "upload_begin").await {
        let _ = write_raw(state, b"{\"cmd\":\"upload_abort\"}\n").await;
        let _ = wait_ack(state, "upload_abort", 2000).await;
        return Err(e);
    }

    for chunk in data.chunks(UPLOAD_CHUNK_RAW) {
        let b64 = B64.encode(chunk);
        let line = format!(r#"{{"cmd":"upload_chunk","data":"{}"}}"#, b64);
        if let Err(e) = send_expect_ack(state, &line, "upload_chunk").await {
            let _ = write_raw(state, b"{\"cmd\":\"upload_abort\"}\n").await;
            let _ = wait_ack(state, "upload_abort", 2000).await;
            return Err(e);
        }
    }

    if let Err(e) = send_expect_ack(state, r#"{"cmd":"upload_end"}"#, "upload_end").await {
        let _ = write_raw(state, b"{\"cmd\":\"upload_abort\"}\n").await;
        let _ = wait_ack(state, "upload_abort", 2000).await;
        return Err(e);
    }
    Ok(())
}

pub async fn upload_path(
    state: &SharedBleState,
    local: &Path,
    remote_path: &str,
) -> Result<(), String> {
    let data = std::fs::read(local).map_err(|e| format!("read {}: {e}", local.display()))?;
    upload_bytes(state, remote_path, &data).await
}
