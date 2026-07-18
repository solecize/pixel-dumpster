use reqwest::Client;
use serde::{Deserialize, Serialize};

// Field names below are renamed per-direction to match what the ESP32
// firmware actually sends on the wire (`sequence`, `frame`, `frames`,
// `images`) while keeping the existing camelCase-ish contract the
// frontend already expects (`is_sequence`, `current_frame`, `frame_count`,
// `items`). Mismatches here previously caused deserialization to either
// fail outright (non-`Option` fields) or silently come back empty/`None`.
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct DeviceStatus {
    pub playing: Option<bool>,
    pub path: Option<String>,
    #[serde(rename(serialize = "is_sequence", deserialize = "sequence"), default)]
    pub is_sequence: Option<bool>,
    #[serde(rename(serialize = "current_frame", deserialize = "frame"), default)]
    pub current_frame: Option<u32>,
    #[serde(default)]
    pub total_frames: Option<u32>,
    #[serde(default)]
    pub fps: Option<f32>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ContentEntry {
    pub path: String,
    pub name: String,
    #[serde(rename(serialize = "is_sequence", deserialize = "sequence"), default)]
    pub is_sequence: Option<bool>,
    #[serde(rename(serialize = "frame_count", deserialize = "frames"), default)]
    pub frame_count: Option<u32>,
    #[serde(default)]
    pub fps: Option<f32>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ContentList {
    #[serde(rename(serialize = "items", deserialize = "images"))]
    pub items: Vec<ContentEntry>,
}

pub struct DeviceApi {
    client: Client,
    base_url: String,
}

impl DeviceApi {
    pub fn new(ip: &str, port: u16) -> Self {
        Self {
            // Default timeout for status/config/etc. Play uses a longer
            // per-request timeout because palette-cache builds can exceed 5s.
            client: Client::builder()
                .timeout(std::time::Duration::from_secs(15))
                .build()
                .unwrap(),
            base_url: format!("http://{}:{}", ip, port),
        }
    }

    fn map_reqwest_err(err: reqwest::Error, action: &str) -> String {
        if err.is_timeout() {
            format!(
                "{action} timed out talking to device — it may still be building a palette cache; wait and retry"
            )
        } else if err.is_connect() {
            format!(
                "could not connect to device for {action} — check Wi‑Fi / IP and Local Network permission for this app"
            )
        } else {
            err.to_string()
        }
    }

    pub async fn status(&self) -> Result<DeviceStatus, String> {
        self.client
            .get(format!("{}/api/status", self.base_url))
            .send()
            .await
            .map_err(|e| Self::map_reqwest_err(e, "status"))?
            .json::<DeviceStatus>()
            .await
            .map_err(|e| e.to_string())
    }

    pub async fn play(
        &self,
        path: &str,
        transition: Option<&str>,
        duration_ms: Option<u32>,
    ) -> Result<serde_json::Value, String> {
        let mut body = serde_json::json!({ "path": path });
        if let Some(t) = transition {
            body["transition"] = serde_json::json!(t);
        }
        if let Some(d) = duration_ms {
            body["duration_ms"] = serde_json::json!(d);
        }

        // Palette quantize + PSRAM cache build can take several seconds on play.
        self.client
            .post(format!("{}/api/play", self.base_url))
            .timeout(std::time::Duration::from_secs(60))
            .json(&body)
            .send()
            .await
            .map_err(|e| Self::map_reqwest_err(e, "play"))?
            .json::<serde_json::Value>()
            .await
            .map_err(|e| e.to_string())
    }

    pub async fn stop(&self) -> Result<serde_json::Value, String> {
        self.client
            .post(format!("{}/api/stop", self.base_url))
            .send()
            .await
            .map_err(|e| e.to_string())?
            .json::<serde_json::Value>()
            .await
            .map_err(|e| e.to_string())
    }

    pub async fn list_content(&self) -> Result<ContentList, String> {
        self.client
            .get(format!("{}/api/content", self.base_url))
            .send()
            .await
            .map_err(|e| e.to_string())?
            .json::<ContentList>()
            .await
            .map_err(|e| e.to_string())
    }

    pub async fn config(&self) -> Result<serde_json::Value, String> {
        self.client
            .get(format!("{}/api/config", self.base_url))
            .send()
            .await
            .map_err(|e| e.to_string())?
            .json::<serde_json::Value>()
            .await
            .map_err(|e| e.to_string())
    }

    pub async fn set_config(&self, config: serde_json::Value) -> Result<serde_json::Value, String> {
        self.client
            .post(format!("{}/api/config", self.base_url))
            .json(&config)
            .send()
            .await
            .map_err(|e| e.to_string())?
            .json::<serde_json::Value>()
            .await
            .map_err(|e| e.to_string())
    }

    /// GET /wizard — device identity/network settings (device_name, hostname,
    /// timezone, wifi_ssid, static_ip/gateway/netmask). Distinct from
    /// `/api/config` (transition/display/attract) and `/api/config/layout`
    /// (panel geometry) — neither of those handlers reads or writes WiFi or
    /// networking fields at all, so posting WiFi/IP changes to them is a
    /// silent no-op on the firmware side.
    pub async fn wizard_config(&self) -> Result<serde_json::Value, String> {
        self.client
            .get(format!("{}/wizard", self.base_url))
            .send()
            .await
            .map_err(|e| e.to_string())?
            .json::<serde_json::Value>()
            .await
            .map_err(|e| e.to_string())
    }

    /// POST /wizard — partial merge update of device identity/network
    /// settings. The firmware responds with an empty 200 body (no reboot),
    /// so we just surface HTTP-level success/failure rather than parsing JSON.
    pub async fn wizard_set_config(&self, config: serde_json::Value) -> Result<serde_json::Value, String> {
        let resp = self.client
            .post(format!("{}/wizard", self.base_url))
            .json(&config)
            .send()
            .await
            .map_err(|e| e.to_string())?;
        if !resp.status().is_success() {
            return Err(format!("device returned HTTP {}", resp.status()));
        }
        Ok(serde_json::json!({ "ok": true }))
    }

    pub async fn layout(&self) -> Result<serde_json::Value, String> {
        self.client
            .get(format!("{}/api/config/layout", self.base_url))
            .send()
            .await
            .map_err(|e| e.to_string())?
            .json::<serde_json::Value>()
            .await
            .map_err(|e| e.to_string())
    }

    pub async fn set_layout(&self, layout: serde_json::Value) -> Result<serde_json::Value, String> {
        self.client
            .post(format!("{}/api/config/layout", self.base_url))
            .json(&layout)
            .send()
            .await
            .map_err(|e| e.to_string())?
            .json::<serde_json::Value>()
            .await
            .map_err(|e| e.to_string())
    }

    pub async fn preview_layout(&self, layout: serde_json::Value) -> Result<serde_json::Value, String> {
        self.client
            .post(format!("{}/api/config/preview", self.base_url))
            .json(&layout)
            .send()
            .await
            .map_err(|e| e.to_string())?
            .json::<serde_json::Value>()
            .await
            .map_err(|e| e.to_string())
    }

    pub async fn test_start(&self, pattern: &str, brightness: Option<i32>) -> Result<serde_json::Value, String> {
        let mut body = serde_json::json!({ "pattern": pattern });
        if let Some(b) = brightness {
            body["brightness"] = serde_json::json!(b);
        }
        self.client
            .post(format!("{}/api/test/start", self.base_url))
            .json(&body)
            .send()
            .await
            .map_err(|e| e.to_string())?
            .json::<serde_json::Value>()
            .await
            .map_err(|e| e.to_string())
    }

    pub async fn test_stop(&self) -> Result<serde_json::Value, String> {
        self.client
            .post(format!("{}/api/test/stop", self.base_url))
            .send()
            .await
            .map_err(|e| e.to_string())?
            .json::<serde_json::Value>()
            .await
            .map_err(|e| e.to_string())
    }

    pub async fn panel_select(&self, panel_index: i32) -> Result<serde_json::Value, String> {
        self.client
            .post(format!("{}/api/test/panel_select", self.base_url))
            .json(&serde_json::json!({ "panel_index": panel_index }))
            .send()
            .await
            .map_err(|e| e.to_string())?
            .json::<serde_json::Value>()
            .await
            .map_err(|e| e.to_string())
    }
}
