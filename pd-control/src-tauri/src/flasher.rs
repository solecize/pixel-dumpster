use serde::{Deserialize, Serialize};
use std::path::{Path, PathBuf};

// ESP32-S3 USB VID/PID pairs
const ESP32_S3_VID: u16 = 0x303A;
const ESP32_S3_PID_JTAG: u16 = 0x1001; // USB JTAG/serial
const ESP32_S3_PID_CDC: u16 = 0x1002; // USB CDC

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct SerialPortInfo {
    pub port_name: String,
    pub description: String,
    pub is_esp32: bool,
    pub vid: Option<u16>,
    pub pid: Option<u16>,
    pub manufacturer: Option<String>,
    pub serial_number: Option<String>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct FlashProgress {
    pub stage: String,
    pub message: String,
    pub percent: Option<f32>,
    pub done: bool,
    pub error: Option<String>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct FlashConfig {
    pub port: String,
    pub firmware_dir: Option<String>,
    pub build_from_source: bool,
    pub project_dir: Option<String>,
}

/// Locate the ELF and partition table in a build directory.
fn find_elf_and_partitions(dir: &Path) -> Option<(PathBuf, PathBuf)> {
    let elf = dir.join("pixel-dumpster.elf");
    let partitions_csv = dir.parent().map(|p| p.join("partitions.csv")).unwrap_or_default();
    // Also check in build dir parent (project root)
    if elf.exists() && partitions_csv.exists() {
        return Some((elf, partitions_csv));
    }
    // Try partitions.csv inside build dir
    let partitions_in_build = dir.join("partitions.csv");
    if elf.exists() && partitions_in_build.exists() {
        return Some((elf, partitions_in_build));
    }
    if elf.exists() {
        return Some((elf, PathBuf::new())); // no partition table found, espflash may still work
    }
    None
}

/// List available serial ports, highlighting ESP32-S3 devices.
pub fn list_serial_ports() -> Vec<SerialPortInfo> {
    let ports = match serialport::available_ports() {
        Ok(p) => p,
        Err(e) => {
            log::error!("Failed to enumerate serial ports: {}", e);
            return vec![];
        }
    };

    ports
        .into_iter()
        .map(|p| {
            let (vid, pid, manufacturer, serial_number, description, is_esp32) = match &p.port_type
            {
                serialport::SerialPortType::UsbPort(info) => {
                    let is_esp = info.vid == ESP32_S3_VID
                        && (info.pid == ESP32_S3_PID_JTAG || info.pid == ESP32_S3_PID_CDC);
                    (
                        Some(info.vid),
                        Some(info.pid),
                        info.manufacturer.clone(),
                        info.serial_number.clone(),
                        info.product
                            .clone()
                            .unwrap_or_else(|| "USB Serial".to_string()),
                        is_esp,
                    )
                }
                _ => (None, None, None, None, "Serial Port".to_string(), false),
            };

            SerialPortInfo {
                port_name: p.port_name,
                description,
                is_esp32,
                vid,
                pid,
                manufacturer,
                serial_number,
            }
        })
        .collect()
}

/// Get the cargo bin path (~/.cargo/bin)
fn cargo_bin_dir() -> Option<PathBuf> {
    dirs_next().map(|home| home.join(".cargo").join("bin"))
}

fn dirs_next() -> Option<PathBuf> {
    std::env::var_os("HOME").map(PathBuf::from)
}

/// Build a PATH that includes ~/.cargo/bin
fn augmented_path() -> String {
    let current = std::env::var("PATH").unwrap_or_default();
    if let Some(cargo_bin) = cargo_bin_dir() {
        format!("{}:{}", cargo_bin.display(), current)
    } else {
        current
    }
}

/// Locate a flash tool: prefer espflash, fall back to esptool.py
fn find_flash_tool() -> Result<(String, FlashToolKind), String> {
    // Check ~/.cargo/bin/espflash directly
    if let Some(cargo_bin) = cargo_bin_dir() {
        let espflash_path = cargo_bin.join("espflash");
        if espflash_path.exists() {
            return Ok((espflash_path.to_string_lossy().to_string(), FlashToolKind::Espflash));
        }
    }

    // Check for espflash on PATH
    if let Ok(output) = std::process::Command::new("espflash")
        .arg("--version")
        .output()
    {
        if output.status.success() {
            return Ok(("espflash".to_string(), FlashToolKind::Espflash));
        }
    }

    // Check for esptool.py (comes with ESP-IDF)
    if let Ok(output) = std::process::Command::new("esptool.py")
        .arg("version")
        .output()
    {
        if output.status.success() {
            return Ok(("esptool.py".to_string(), FlashToolKind::Esptool));
        }
    }

    // Check IDF_PATH for esptool
    if let Ok(idf_path) = std::env::var("IDF_PATH") {
        let esptool = PathBuf::from(&idf_path)
            .join("components")
            .join("esptool_py")
            .join("esptool")
            .join("esptool.py");
        if esptool.exists() {
            return Ok((esptool.to_string_lossy().to_string(), FlashToolKind::Esptool));
        }
    }

    Err("No flash tool found. Install espflash (cargo install espflash) or esptool.py (via ESP-IDF).".to_string())
}

#[derive(Debug, Clone)]
enum FlashToolKind {
    Espflash,
    Esptool,
}

/// Find pre-built firmware binaries in a directory.
/// Returns (bootloader, partition_table, app) paths.
fn find_firmware_binaries(dir: &Path) -> Result<(PathBuf, PathBuf, PathBuf), String> {
    let bootloader = dir.join("bootloader").join("bootloader.bin");
    let partition_table = dir.join("partition_table").join("partition-table.bin");
    let app = dir.join("pixel-dumpster.bin");

    if !bootloader.exists() {
        return Err(format!("Bootloader not found: {}", bootloader.display()));
    }
    if !partition_table.exists() {
        return Err(format!(
            "Partition table not found: {}",
            partition_table.display()
        ));
    }
    if !app.exists() {
        return Err(format!("App binary not found: {}", app.display()));
    }

    Ok((bootloader, partition_table, app))
}

/// Synchronous flash — runs in a blocking thread, no async.
/// Returns collected output lines on success.
pub fn flash_firmware_sync<F>(config: &FlashConfig, on_progress: F) -> Result<Vec<String>, String>
where
    F: Fn(FlashProgress),
{
    let mut collected_lines: Vec<String> = Vec::new();
    let firmware_dir = if config.build_from_source {
        return Err("Build-from-source not supported in sync mode yet".to_string());
    } else {
        config
            .firmware_dir
            .clone()
            .ok_or("firmware_dir is required when not building from source")?
    };

    let firmware_path = PathBuf::from(&firmware_dir);

    // Validate port exists
    if !PathBuf::from(&config.port).exists() {
        return Err(format!(
            "Serial port {} not found. The device may have disconnected or the port name changed after a reset.",
            config.port
        ));
    }

    let (tool_path, tool_kind) = find_flash_tool()?;

    on_progress(FlashProgress {
        stage: "flash".to_string(),
        message: format!("Using {} to flash to {}", tool_path, config.port),
        percent: Some(0.0),
        done: false,
        error: None,
    });

    let mut cmd = match tool_kind {
        FlashToolKind::Espflash => {
            if let Some((elf, partitions)) = find_elf_and_partitions(&firmware_path) {
                let mut c = std::process::Command::new(&tool_path);
                c.arg("flash")
                    .arg("-p")
                    .arg(&config.port)
                    .arg("--non-interactive");
                if partitions.exists() {
                    c.arg("--partition-table").arg(&partitions);
                }
                c.arg(&elf);

                on_progress(FlashProgress {
                    stage: "flash".to_string(),
                    message: format!("Running: {} flash -p {} --non-interactive {} {}",
                        tool_path, config.port,
                        if partitions.exists() { format!("--partition-table {}", partitions.display()) } else { String::new() },
                        elf.display()),
                    percent: None,
                    done: false,
                    error: None,
                });
                c
            } else {
                return Err("ELF file (pixel-dumpster.elf) not found in build directory".to_string());
            }
        }
        FlashToolKind::Esptool => {
            let (bootloader, partition_table, app) = find_firmware_binaries(&firmware_path)?;
            let mut c = std::process::Command::new(&tool_path);
            c.args([
                "--chip", "esp32s3",
                "--port", &config.port,
                "--baud", "460800",
                "write_flash",
                "--flash_mode", "dio",
                "--flash_size", "detect",
                "0x0",
            ])
            .arg(&bootloader)
            .arg("0x8000")
            .arg(&partition_table)
            .arg("0x10000")
            .arg(&app);
            c
        }
    };

    cmd.env("PATH", augmented_path());
    cmd.stdout(std::process::Stdio::piped());
    cmd.stderr(std::process::Stdio::piped());

    let mut child = cmd.spawn().map_err(|e| format!("Failed to start flash tool: {}", e))?;

    // Read stdout and stderr in threads, forward to progress
    let stdout = child.stdout.take();
    let stderr = child.stderr.take();

    let on_line = |line: &str, on_progress: &F| {
        let mut percent = None;
        if line.contains('/') {
            for p in line.split_whitespace() {
                if let Some((a, b)) = p.split_once('/') {
                    if let (Ok(cur), Ok(tot)) = (a.parse::<f32>(), b.parse::<f32>()) {
                        if tot > 0.0 {
                            percent = Some((cur / tot) * 100.0);
                            break;
                        }
                    }
                }
            }
        }
        on_progress(FlashProgress {
            stage: "flash".to_string(),
            message: line.to_string(),
            percent,
            done: false,
            error: None,
        });
    };

    // Read both streams in separate threads
    let (tx, rx) = std::sync::mpsc::channel::<String>();

    let tx_out = tx.clone();
    let stdout_thread = stdout.map(|out| {
        std::thread::spawn(move || {
            use std::io::BufRead;
            let reader = std::io::BufReader::new(out);
            for line in reader.lines() {
                if let Ok(l) = line {
                    let _ = tx_out.send(l);
                }
            }
        })
    });

    let tx_err = tx;
    let stderr_thread = stderr.map(|err| {
        std::thread::spawn(move || {
            use std::io::BufRead;
            let reader = std::io::BufReader::new(err);
            for line in reader.lines() {
                if let Ok(l) = line {
                    let _ = tx_err.send(l);
                }
            }
        })
    });

    // Receive lines until both threads finish
    for line in rx {
        collected_lines.push(line.clone());
        on_line(&line, &on_progress);
    }

    // Wait for threads
    if let Some(t) = stdout_thread { let _ = t.join(); }
    if let Some(t) = stderr_thread { let _ = t.join(); }

    let status = child.wait().map_err(|e| format!("Flash process error: {}", e))?;

    if !status.success() {
        let msg = format!("Flash process exited with code {}", status.code().unwrap_or(-1));
        collected_lines.push(msg.clone());
        on_progress(FlashProgress {
            stage: "flash".to_string(),
            message: msg.clone(),
            percent: None,
            done: true,
            error: Some(msg.clone()),
        });
        return Err(msg);
    }

    collected_lines.push("Flash complete! Device will restart.".to_string());
    on_progress(FlashProgress {
        stage: "flash".to_string(),
        message: "Flash complete! Device will restart.".to_string(),
        percent: Some(100.0),
        done: true,
        error: None,
    });

    Ok(collected_lines)
}
