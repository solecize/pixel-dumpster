use reqwest::Client;
use serde::{de::DeserializeOwned, Deserialize, Serialize};

use crate::http_trace;

// Field names below are renamed per-direction to match what the ESP32
// firmware actually sends on the wire (`sequence`, `frame`, `frames`,
// `images`) while keeping the existing camelCase-ish contract the
// frontend already expects (`is_sequence`, `current_frame`, `frame_count`,
// `items`).
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
    #[serde(default)]
    pub achieved_fps: Option<f32>,
    #[serde(default)]
    pub cache: Option<String>,
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

    async fn send_traced(
        &self,
        action: &str,
        method: &str,
        url: String,
        req: reqwest::RequestBuilder,
        req_body: Option<&str>,
    ) -> Result<String, String> {
        http_trace::emit_http_request(action, method, &url, req_body);
        let resp = req.send().await.map_err(|e| {
            let msg = Self::map_reqwest_err(e, action);
            http_trace::emit(
                "error",
                action,
                method,
                &url,
                &msg,
                serde_json::json!({ "error": msg }),
            );
            msg
        })?;
        let status = resp.status().as_u16();
        let body = resp.text().await.unwrap_or_default();
        let ok = (200..300).contains(&status);
        http_trace::emit_http_result(action, method, &url, status, &body, ok);
        if !ok {
            return Err(format!("{action} failed: HTTP {status} ({body})"));
        }
        Ok(body)
    }

    async fn get_json<T: DeserializeOwned>(&self, action: &str, path: &str) -> Result<T, String> {
        let url = format!("{}{}", self.base_url, path);
        let req = self.client.get(&url);
        let body = self.send_traced(action, "GET", url, req, None).await?;
        serde_json::from_str(&body).map_err(|e| format!("{action}: bad JSON: {e} (body={body})"))
    }

    async fn post_value(
        &self,
        action: &str,
        path: &str,
        body: &serde_json::Value,
        timeout: Option<std::time::Duration>,
    ) -> Result<serde_json::Value, String> {
        let url = format!("{}{}", self.base_url, path);
        let body_str = body.to_string();
        let mut req = self.client.post(&url).json(body);
        if let Some(t) = timeout {
            req = req.timeout(t);
        }
        let resp_body = self
            .send_traced(action, "POST", url, req, Some(&body_str))
            .await?;
        if resp_body.trim().is_empty() {
            return Ok(serde_json::json!({ "ok": true }));
        }
        Ok(serde_json::from_str(&resp_body)
            .unwrap_or_else(|_| serde_json::json!({ "ok": true, "raw": resp_body })))
    }

    pub async fn status(&self) -> Result<DeviceStatus, String> {
        self.get_json("status", "/api/status").await
    }

    pub async fn device_log(&self) -> Result<serde_json::Value, String> {
        self.get_json("log", "/api/log").await
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
        self.post_value(
            "play",
            "/api/play",
            &body,
            Some(std::time::Duration::from_secs(60)),
        )
        .await
    }

    pub async fn stop(&self) -> Result<serde_json::Value, String> {
        self.post_value("stop", "/api/stop", &serde_json::json!({}), None)
            .await
    }

    pub async fn list_content(&self) -> Result<ContentList, String> {
        self.get_json("list", "/api/content").await
    }

    pub async fn delete_content(&self, path: &str) -> Result<serde_json::Value, String> {
        self.post_value(
            "delete",
            "/api/content/delete",
            &serde_json::json!({ "path": path }),
            None,
        )
        .await
    }

    pub async fn rename_content(
        &self,
        from: &str,
        to: &str,
    ) -> Result<serde_json::Value, String> {
        self.post_value(
            "rename",
            "/api/content/rename",
            &serde_json::json!({ "from": from, "to": to }),
            None,
        )
        .await
    }

    pub async fn set_content_meta(
        &self,
        path: &str,
        fps: u32,
    ) -> Result<serde_json::Value, String> {
        self.post_value(
            "set_meta",
            "/api/content/meta",
            &serde_json::json!({ "path": path, "fps": fps }),
            None,
        )
        .await
    }

    pub async fn config(&self) -> Result<serde_json::Value, String> {
        self.get_json("config", "/api/config").await
    }

    pub async fn set_config(&self, config: serde_json::Value) -> Result<serde_json::Value, String> {
        self.post_value("set_config", "/api/config", &config, None)
            .await
    }

    pub async fn wizard_config(&self) -> Result<serde_json::Value, String> {
        self.get_json("wizard_config", "/wizard").await
    }

    pub async fn wizard_set_config(
        &self,
        config: serde_json::Value,
    ) -> Result<serde_json::Value, String> {
        self.post_value("wizard_set_config", "/wizard", &config, None)
            .await
    }

    pub async fn layout(&self) -> Result<serde_json::Value, String> {
        self.get_json("layout", "/api/config/layout").await
    }

    pub async fn set_layout(&self, layout: serde_json::Value) -> Result<serde_json::Value, String> {
        self.post_value("set_layout", "/api/config/layout", &layout, None)
            .await
    }

    pub async fn preview_layout(
        &self,
        layout: serde_json::Value,
    ) -> Result<serde_json::Value, String> {
        self.post_value("preview_layout", "/api/config/preview", &layout, None)
            .await
    }

    pub async fn test_start(
        &self,
        pattern: &str,
        brightness: Option<i32>,
    ) -> Result<serde_json::Value, String> {
        let mut body = serde_json::json!({ "pattern": pattern });
        if let Some(b) = brightness {
            body["brightness"] = serde_json::json!(b);
        }
        self.post_value("test_start", "/api/test/start", &body, None)
            .await
    }

    pub async fn test_stop(&self) -> Result<serde_json::Value, String> {
        self.post_value("test_stop", "/api/test/stop", &serde_json::json!({}), None)
            .await
    }

    pub async fn panel_select(&self, panel_index: i32) -> Result<serde_json::Value, String> {
        self.post_value(
            "panel_select",
            "/api/test/panel_select",
            &serde_json::json!({ "panel_index": panel_index }),
            None,
        )
        .await
    }
}
