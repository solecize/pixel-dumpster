use reqwest::Client;
use serde::{Deserialize, Serialize};

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct DaemonStatus {
    pub running: Option<bool>,
    pub current_system: Option<String>,
    pub last_event: Option<String>,
    pub last_event_detail: Option<String>,
    pub transport: Option<String>,
    pub device_host: Option<String>,
    pub device_port: Option<u16>,
    pub serial_device: Option<String>,
    pub uptime_seconds: Option<f64>,
    pub verbose: Option<bool>,
    pub dry_run: Option<bool>,
    pub events: Option<serde_json::Value>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct DaemonLog {
    pub lines: Vec<String>,
    pub count: u32,
}

pub struct DaemonApi {
    client: Client,
    base_url: String,
}

impl DaemonApi {
    pub fn new(ip: &str, port: u16) -> Self {
        Self {
            client: Client::builder()
                .timeout(std::time::Duration::from_secs(5))
                .build()
                .unwrap(),
            base_url: format!("http://{}:{}", ip, port),
        }
    }

    pub async fn status(&self) -> Result<DaemonStatus, String> {
        self.client
            .get(format!("{}/api/status", self.base_url))
            .send()
            .await
            .map_err(|e| e.to_string())?
            .json::<DaemonStatus>()
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

    pub async fn reload(&self) -> Result<serde_json::Value, String> {
        self.client
            .post(format!("{}/api/reload", self.base_url))
            .send()
            .await
            .map_err(|e| e.to_string())?
            .json::<serde_json::Value>()
            .await
            .map_err(|e| e.to_string())
    }

    pub async fn inject_event(
        &self,
        event_type: &str,
        system: Option<&str>,
        game: Option<&str>,
        rom_path: Option<&str>,
    ) -> Result<serde_json::Value, String> {
        let mut body = serde_json::json!({ "type": event_type });
        if let Some(s) = system {
            body["system"] = serde_json::json!(s);
        }
        if let Some(g) = game {
            body["game"] = serde_json::json!(g);
        }
        if let Some(r) = rom_path {
            body["rom_path"] = serde_json::json!(r);
        }

        self.client
            .post(format!("{}/api/event", self.base_url))
            .json(&body)
            .send()
            .await
            .map_err(|e| e.to_string())?
            .json::<serde_json::Value>()
            .await
            .map_err(|e| e.to_string())
    }

    pub async fn log(&self) -> Result<DaemonLog, String> {
        self.client
            .get(format!("{}/api/log", self.base_url))
            .send()
            .await
            .map_err(|e| e.to_string())?
            .json::<DaemonLog>()
            .await
            .map_err(|e| e.to_string())
    }
}
