use serde::{Deserialize, Serialize};
use ssh2::Session;
use std::io::{Read, Write};
use std::net::TcpStream;
use std::path::Path;

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct SshConfig {
    pub host: String,
    pub port: u16,
    pub username: String,
    pub password: Option<String>,
    pub key_path: Option<String>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct PiInstallConfig {
    pub device_host: String,
    pub device_port: u16,
    pub transport: String,
    pub serial_device: Option<String>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct PiInstallProgress {
    pub step: String,
    pub message: String,
    pub step_number: u32,
    pub total_steps: u32,
    pub done: bool,
    pub error: Option<String>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct PiDaemonStatus {
    pub installed: bool,
    pub running: bool,
    pub version: Option<String>,
    pub config_exists: bool,
}

/// Connect to a Raspberry Pi via SSH and return the session.
fn connect(config: &SshConfig) -> Result<Session, String> {
    let addr = format!("{}:{}", config.host, config.port);
    let tcp = TcpStream::connect(&addr)
        .map_err(|e| format!("Failed to connect to {}: {}", addr, e))?;

    let mut session = Session::new().map_err(|e| format!("SSH session error: {}", e))?;
    session.set_tcp_stream(tcp);
    session
        .handshake()
        .map_err(|e| format!("SSH handshake failed: {}", e))?;

    // Authenticate
    if let Some(key_path) = &config.key_path {
        session
            .userauth_pubkey_file(
                &config.username,
                None,
                Path::new(key_path),
                config.password.as_deref(),
            )
            .map_err(|e| format!("SSH key auth failed: {}", e))?;
    } else if let Some(password) = &config.password {
        session
            .userauth_password(&config.username, password)
            .map_err(|e| format!("SSH password auth failed: {}", e))?;
    } else {
        // Try agent
        let mut agent = session
            .agent()
            .map_err(|e| format!("SSH agent error: {}", e))?;
        agent
            .connect()
            .map_err(|e| format!("SSH agent connect failed: {}", e))?;
        agent
            .list_identities()
            .map_err(|e| format!("SSH agent list failed: {}", e))?;

        let identities = agent.identities().map_err(|e| format!("SSH agent identities error: {}", e))?;
        let mut authed = false;
        for identity in identities {
            if agent.userauth(&config.username, &identity).is_ok() {
                authed = true;
                break;
            }
        }
        if !authed {
            return Err("SSH authentication failed: no password, key, or agent identity worked".to_string());
        }
    }

    if !session.authenticated() {
        return Err("SSH authentication failed".to_string());
    }

    Ok(session)
}

/// Test SSH connection.
pub fn test_connection(config: &SshConfig) -> Result<String, String> {
    let session = connect(config)?;
    let output = exec_command(&session, "uname -a")?;
    Ok(output.trim().to_string())
}

/// Execute a command over SSH and return stdout.
fn exec_command(session: &Session, command: &str) -> Result<String, String> {
    let mut channel = session
        .channel_session()
        .map_err(|e| format!("SSH channel error: {}", e))?;
    channel
        .exec(command)
        .map_err(|e| format!("SSH exec error: {}", e))?;

    let mut output = String::new();
    channel
        .read_to_string(&mut output)
        .map_err(|e| format!("SSH read error: {}", e))?;

    channel.wait_close().ok();
    Ok(output)
}

/// Upload a file via SFTP.
fn upload_file(session: &Session, local_path: &Path, remote_path: &str) -> Result<(), String> {
    let data = std::fs::read(local_path)
        .map_err(|e| format!("Failed to read local file {}: {}", local_path.display(), e))?;

    let sftp = session
        .sftp()
        .map_err(|e| format!("SFTP error: {}", e))?;

    let mut remote_file = sftp
        .create(Path::new(remote_path))
        .map_err(|e| format!("SFTP create {} failed: {}", remote_path, e))?;

    remote_file
        .write_all(&data)
        .map_err(|e| format!("SFTP write error: {}", e))?;

    Ok(())
}

/// Upload bytes via SFTP.
fn upload_bytes(session: &Session, data: &[u8], remote_path: &str) -> Result<(), String> {
    let sftp = session
        .sftp()
        .map_err(|e| format!("SFTP error: {}", e))?;

    let mut remote_file = sftp
        .create(Path::new(remote_path))
        .map_err(|e| format!("SFTP create {} failed: {}", remote_path, e))?;

    remote_file
        .write_all(data)
        .map_err(|e| format!("SFTP write error: {}", e))?;

    Ok(())
}

/// Check if the daemon is installed/running on the Pi.
pub fn check_daemon_status(config: &SshConfig) -> Result<PiDaemonStatus, String> {
    let session = connect(config)?;

    let installed = exec_command(&session, "test -f /opt/retropie/configs/all/dumpster-diver && echo yes || echo no")?;
    let running = exec_command(&session, "pgrep -x dumpster-diver > /dev/null 2>&1 && echo yes || echo no")?;
    let config_exists = exec_command(&session, "test -f ~/.config/dumpster-diver/config.json && echo yes || echo no")?;

    Ok(PiDaemonStatus {
        installed: installed.trim() == "yes",
        running: running.trim() == "yes",
        version: None,
        config_exists: config_exists.trim() == "yes",
    })
}

/// Install the dumpster-diver daemon on the Pi via SSH.
pub fn install_daemon<F>(
    ssh_config: &SshConfig,
    install_config: &PiInstallConfig,
    tools_dir: &str,
    on_progress: F,
) -> Result<(), String>
where
    F: Fn(PiInstallProgress),
{
    let total_steps = 6;

    on_progress(PiInstallProgress {
        step: "connect".to_string(),
        message: format!("Connecting to {}...", ssh_config.host),
        step_number: 1,
        total_steps,
        done: false,
        error: None,
    });

    let session = connect(ssh_config)?;

    // Step 2: Upload binary
    on_progress(PiInstallProgress {
        step: "upload_binary".to_string(),
        message: "Uploading dumpster-diver binary...".to_string(),
        step_number: 2,
        total_steps,
        done: false,
        error: None,
    });

    let binary_path = Path::new(tools_dir).join("dumpster-diver");
    if !binary_path.exists() {
        return Err(format!(
            "dumpster-diver binary not found at {}. Build it first.",
            binary_path.display()
        ));
    }

    // Upload to temp first, then sudo mv
    upload_file(&session, &binary_path, "/tmp/dumpster-diver")?;
    exec_command(&session, "sudo mv /tmp/dumpster-diver /opt/retropie/configs/all/dumpster-diver && sudo chmod +x /opt/retropie/configs/all/dumpster-diver")?;

    // Step 3: Upload ES event scripts
    on_progress(PiInstallProgress {
        step: "upload_scripts".to_string(),
        message: "Installing ES event scripts...".to_string(),
        step_number: 3,
        total_steps,
        done: false,
        error: None,
    });

    let events = ["game-select", "system-select", "game-start", "game-end"];
    for event in &events {
        let script_path = Path::new(tools_dir)
            .join("es-scripts")
            .join(event)
            .join("dumpster-diver.sh");

        if script_path.exists() {
            let remote_dir = format!(
                "/home/{}/.emulationstation/scripts/{}",
                ssh_config.username, event
            );
            exec_command(&session, &format!("mkdir -p {}", remote_dir))?;

            let remote_script = format!("{}/dumpster-diver.sh", remote_dir);
            upload_file(&session, &script_path, &remote_script)?;
            exec_command(&session, &format!("chmod +x {}", remote_script))?;
        }
    }

    // Step 4: Create config
    on_progress(PiInstallProgress {
        step: "config".to_string(),
        message: "Creating configuration...".to_string(),
        step_number: 4,
        total_steps,
        done: false,
        error: None,
    });

    let config_dir = format!("/home/{}/.config/dumpster-diver", ssh_config.username);
    exec_command(&session, &format!("mkdir -p {}", config_dir))?;

    let serial_section = if install_config.transport == "serial" {
        format!(
            r#""serial": {{ "device": "{}", "baud": 115200 }},"#,
            install_config.serial_device.as_deref().unwrap_or("/dev/ttyACM0")
        )
    } else {
        r#""serial": { "device": "/dev/ttyACM0", "baud": 115200 },"#.to_string()
    };

    let config_json = format!(
        r#"{{
  "device": {{
    "host": "{}",
    "port": {}
  }},
  "transport": "{}",
  {}
  "fifo": "/tmp/dumpster-diver.fifo",
  "es": {{
    "gamelists_path": "/home/{}/.emulationstation/gamelists",
    "roms_path": "/home/{}/RetroPie/roms"
  }},
  "marquee": {{
    "device_prefix": "marquees"
  }},
  "defaults": {{
    "system_fallback": "marquees/systems/default",
    "game_fallback": "marquees/default",
    "transition": "fade",
    "duration_ms": 500
  }},
  "events": {{
    "game_select": true,
    "game_launch": true,
    "game_end": true,
    "system_select": true
  }},
  "systems": {{}},
  "games": {{}}
}}"#,
        install_config.device_host,
        install_config.device_port,
        install_config.transport,
        serial_section,
        ssh_config.username,
        ssh_config.username,
    );

    // Only write config if it doesn't exist
    let existing = exec_command(
        &session,
        &format!("test -f {}/config.json && echo exists || echo new", config_dir),
    )?;
    if existing.trim() == "new" {
        upload_bytes(&session, config_json.as_bytes(), &format!("{}/config.json", config_dir))?;
    }

    // Step 5: Create FIFO
    on_progress(PiInstallProgress {
        step: "fifo".to_string(),
        message: "Creating FIFO...".to_string(),
        step_number: 5,
        total_steps,
        done: false,
        error: None,
    });

    exec_command(
        &session,
        "test -p /tmp/dumpster-diver.fifo || mkfifo /tmp/dumpster-diver.fifo",
    )?;

    // Step 6: Add to autostart
    on_progress(PiInstallProgress {
        step: "autostart".to_string(),
        message: "Configuring autostart...".to_string(),
        step_number: 6,
        total_steps,
        done: false,
        error: None,
    });

    let autostart = "/opt/retropie/configs/all/autostart.sh";
    let check = exec_command(
        &session,
        &format!("grep -q dumpster-diver {} 2>/dev/null && echo found || echo missing", autostart),
    )?;

    if check.trim() == "missing" {
        exec_command(
            &session,
            &format!(
                r#"if [ -f {} ]; then sed -i '/emulationstation/i \/opt/retropie/configs/all/dumpster-diver --verbose >> /tmp/dumpster-diver.log 2>&1 &' {}; else echo '#!/bin/bash' > {} && echo '/opt/retropie/configs/all/dumpster-diver --verbose >> /tmp/dumpster-diver.log 2>&1 &' >> {} && echo 'emulationstation #auto' >> {}; fi"#,
                autostart, autostart, autostart, autostart, autostart
            ),
        )?;
    }

    on_progress(PiInstallProgress {
        step: "done".to_string(),
        message: "Installation complete!".to_string(),
        step_number: total_steps,
        total_steps,
        done: true,
        error: None,
    });

    Ok(())
}

/// Uninstall the daemon from the Pi.
pub fn uninstall_daemon<F>(ssh_config: &SshConfig, on_progress: F) -> Result<(), String>
where
    F: Fn(PiInstallProgress),
{
    let total_steps = 4;

    on_progress(PiInstallProgress {
        step: "connect".to_string(),
        message: format!("Connecting to {}...", ssh_config.host),
        step_number: 1,
        total_steps,
        done: false,
        error: None,
    });

    let session = connect(ssh_config)?;

    // Kill running daemon
    exec_command(&session, "pkill -x dumpster-diver 2>/dev/null; true")?;

    on_progress(PiInstallProgress {
        step: "remove_binary".to_string(),
        message: "Removing binary...".to_string(),
        step_number: 2,
        total_steps,
        done: false,
        error: None,
    });
    exec_command(&session, "sudo rm -f /opt/retropie/configs/all/dumpster-diver")?;

    on_progress(PiInstallProgress {
        step: "remove_scripts".to_string(),
        message: "Removing ES scripts...".to_string(),
        step_number: 3,
        total_steps,
        done: false,
        error: None,
    });

    let events = ["game-select", "system-select", "game-start", "game-end"];
    for event in &events {
        exec_command(
            &session,
            &format!(
                "rm -f /home/{}/.emulationstation/scripts/{}/dumpster-diver.sh",
                ssh_config.username, event
            ),
        )?;
    }

    exec_command(&session, "rm -f /tmp/dumpster-diver.fifo")?;

    on_progress(PiInstallProgress {
        step: "clean_autostart".to_string(),
        message: "Cleaning autostart...".to_string(),
        step_number: 4,
        total_steps,
        done: false,
        error: None,
    });

    exec_command(
        &session,
        "sed -i '/dumpster-diver/d' /opt/retropie/configs/all/autostart.sh 2>/dev/null; true",
    )?;

    on_progress(PiInstallProgress {
        step: "done".to_string(),
        message: "Uninstall complete! Config preserved.".to_string(),
        step_number: total_steps,
        total_steps,
        done: true,
        error: None,
    });

    Ok(())
}
