//! Optional HTTP wire tracing for DeviceApi → frontend (`pd-trace` events).

use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::OnceLock;

use serde_json::json;
use tauri::{AppHandle, Emitter};

static TRACE_ENABLED: AtomicBool = AtomicBool::new(true);
static APP: OnceLock<AppHandle> = OnceLock::new();

const BODY_CAP: usize = 2048;

pub fn init(app: AppHandle) {
    let _ = APP.set(app);
}

pub fn set_enabled(enabled: bool) {
    TRACE_ENABLED.store(enabled, Ordering::Relaxed);
}

pub fn is_enabled() -> bool {
    TRACE_ENABLED.load(Ordering::Relaxed)
}

fn truncate(s: &str) -> String {
    if s.len() <= BODY_CAP {
        s.to_string()
    } else {
        format!("{}…(+{} bytes)", &s[..BODY_CAP], s.len() - BODY_CAP)
    }
}

/// Emit a structured wire event when tracing is enabled.
pub fn emit(
    phase: &str,
    action: &str,
    method: &str,
    url: &str,
    summary: &str,
    detail: serde_json::Value,
) {
    if !is_enabled() {
        return;
    }
    let Some(app) = APP.get() else {
        return;
    };

    let path = url
        .split("://")
        .nth(1)
        .and_then(|rest| rest.find('/').map(|i| &rest[i..]))
        .unwrap_or(url);

    let payload = json!({
        "phase": phase,
        "action": action,
        "method": method,
        "url": url,
        "path": path,
        "summary": summary,
        "level": if phase == "error" { "error" } else { "info" },
        "detail": detail,
        "transport": "wifi",
    });

    log::info!("pd-trace {phase} {method} {path}: {summary}");
    let _ = crate::trace_file::append_event(payload.clone());
    let _ = app.emit("pd-trace", payload);
}

pub fn emit_http_result(
    action: &str,
    method: &str,
    url: &str,
    status: u16,
    body: &str,
    ok: bool,
) {
    let phase = if ok { "response" } else { "error" };
    let summary = format!("{method} → HTTP {status}");
    emit(
        phase,
        action,
        method,
        url,
        &summary,
        json!({
            "status": status,
            "body": truncate(body),
            "ok": ok,
        }),
    );
}

pub fn emit_http_request(action: &str, method: &str, url: &str, body: Option<&str>) {
    emit(
        "request",
        action,
        method,
        url,
        &format!("{method} {url}"),
        json!({
            "body": body.map(truncate),
        }),
    );
}
