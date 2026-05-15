use reqwest::Client;
use serde::{Deserialize, Serialize};

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct DeviceStatus {
    pub playing: Option<bool>,
    pub path: Option<String>,
    pub is_sequence: Option<bool>,
    pub current_frame: Option<u32>,
    pub total_frames: Option<u32>,
    pub fps: Option<f32>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ContentEntry {
    pub path: String,
    pub name: String,
    pub is_sequence: Option<bool>,
    pub frame_count: Option<u32>,
    pub fps: Option<f32>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ContentList {
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
                .timeout(std::time::Duration::from_secs(5))
                .build()
                .unwrap(),
            base_url: format!("http://{}:{}", ip, port),
        }
    }

    pub async fn status(&self) -> Result<DeviceStatus, String> {
        self.client
            .get(format!("{}/api/status", self.base_url))
            .send()
            .await
            .map_err(|e| e.to_string())?
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

        self.client
            .post(format!("{}/api/play", self.base_url))
            .json(&body)
            .send()
            .await
            .map_err(|e| e.to_string())?
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
