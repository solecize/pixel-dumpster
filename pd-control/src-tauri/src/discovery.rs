use mdns_sd::{ServiceDaemon, ServiceEvent};
use serde::Serialize;
use std::collections::HashMap;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex};
use std::time::Duration;

#[derive(Debug, Clone, Serialize)]
pub struct DiscoveredDevice {
    pub name: String,
    pub host: String,
    pub ip: String,
    pub port: u16,
    pub device_type: String, // "device" or "daemon"
    pub txt: HashMap<String, String>,
}

pub struct DiscoveryState {
    pub devices: Arc<Mutex<Vec<DiscoveredDevice>>>,
    daemon: Option<ServiceDaemon>,
    stop_flag: Arc<AtomicBool>,
}

impl DiscoveryState {
    pub fn new() -> Self {
        Self {
            devices: Arc::new(Mutex::new(Vec::new())),
            daemon: None,
            stop_flag: Arc::new(AtomicBool::new(false)),
        }
    }

    pub fn start(&mut self) {
        // Stop any existing daemon and signal old threads to exit
        self.stop();

        // Clear stale devices
        self.devices.lock().unwrap().clear();

        // Reset stop flag for new threads
        let stop_flag = Arc::new(AtomicBool::new(false));
        self.stop_flag = stop_flag.clone();

        log::info!("mDNS discovery starting — browsing _pdumpster._tcp and _dumpster-diver._tcp");

        // Create mDNS service daemon
        let mdns = match ServiceDaemon::new() {
            Ok(d) => d,
            Err(e) => {
                log::error!("Failed to create mDNS daemon: {}", e);
                return;
            }
        };

        // Browse for pixel-dumpster ESP32 devices
        let receiver_device = match mdns.browse("_pdumpster._tcp.local.") {
            Ok(r) => r,
            Err(e) => {
                log::error!("Failed to browse _pdumpster._tcp: {}", e);
                let _ = mdns.shutdown();
                return;
            }
        };

        // Browse for dumpster-diver daemons
        let receiver_daemon = match mdns.browse("_dumpster-diver._tcp.local.") {
            Ok(r) => r,
            Err(e) => {
                log::error!("Failed to browse _dumpster-diver._tcp: {}", e);
                let _ = mdns.shutdown();
                return;
            }
        };

        // Spawn listener for device discovery events
        let devices_clone = self.devices.clone();
        let stop1 = stop_flag.clone();
        std::thread::spawn(move || {
            while !stop1.load(Ordering::Relaxed) {
                match receiver_device.recv_timeout(Duration::from_millis(500)) {
                    Ok(event) => handle_service_event(&devices_clone, &event, "device"),
                    Err(e) => {
                        // Timeout is normal; Disconnected means daemon shut down
                        if !format!("{}", e).contains("timed out") {
                            log::debug!("device browse recv error: {}", e);
                            break;
                        }
                    }
                }
            }
            log::debug!("device browse listener exiting");
        });

        let devices_clone2 = self.devices.clone();
        let stop2 = stop_flag.clone();
        std::thread::spawn(move || {
            while !stop2.load(Ordering::Relaxed) {
                match receiver_daemon.recv_timeout(Duration::from_millis(500)) {
                    Ok(event) => handle_service_event(&devices_clone2, &event, "daemon"),
                    Err(e) => {
                        if !format!("{}", e).contains("timed out") {
                            log::debug!("daemon browse recv error: {}", e);
                            break;
                        }
                    }
                }
            }
            log::debug!("daemon browse listener exiting");
        });

        self.daemon = Some(mdns);
    }

    pub fn stop(&mut self) {
        // Signal listener threads to stop
        self.stop_flag.store(true, Ordering::Relaxed);

        if let Some(mdns) = self.daemon.take() {
            log::info!("mDNS discovery stopping");
            let _ = mdns.shutdown();
        }
    }

    pub fn get_devices(&self) -> Vec<DiscoveredDevice> {
        self.devices.lock().unwrap().clone()
    }

    pub fn add_manual(&self, ip: String, port: u16, device_type: String) -> DiscoveredDevice {
        let device = DiscoveredDevice {
            name: format!("{}:{}", ip, port),
            host: ip.clone(),
            ip: ip.clone(),
            port,
            device_type,
            txt: HashMap::new(),
        };
        self.devices.lock().unwrap().push(device.clone());
        device
    }
}

fn handle_service_event(
    devices: &Arc<Mutex<Vec<DiscoveredDevice>>>,
    event: &ServiceEvent,
    dtype: &str,
) {
    match event {
        ServiceEvent::ServiceResolved(info) => {
            let ip = info
                .get_addresses()
                .iter()
                .next()
                .map(|a| a.to_string())
                .unwrap_or_default();

            let txt: HashMap<String, String> = info
                .get_properties()
                .iter()
                .map(|p| (p.key().to_string(), p.val_str().to_string()))
                .collect();

            let name = txt
                .get("name")
                .cloned()
                .unwrap_or_else(|| info.get_hostname().to_string());

            let device = DiscoveredDevice {
                name,
                host: info.get_hostname().to_string(),
                ip,
                port: info.get_port(),
                device_type: dtype.to_string(),
                txt,
            };

            log::info!(
                "Discovered {}: {} at {}:{}",
                dtype,
                device.name,
                device.ip,
                device.port
            );

            let mut devs = devices.lock().unwrap();
            // Update or insert
            if let Some(existing) = devs.iter_mut().find(|d| {
                d.ip == device.ip && d.port == device.port && d.device_type == device.device_type
            }) {
                *existing = device;
            } else {
                devs.push(device);
            }
        }
        ServiceEvent::ServiceRemoved(_, fullname) => {
            log::info!("Service removed: {}", fullname);
            let mut devs = devices.lock().unwrap();
            devs.retain(|d| d.host != *fullname);
        }
        ServiceEvent::SearchStarted(service) => {
            log::info!("mDNS browse started for: {}", service);
        }
        ServiceEvent::ServiceFound(service, fullname) => {
            log::debug!("mDNS service found: {} ({})", fullname, service);
        }
        _ => {}
    }
}
