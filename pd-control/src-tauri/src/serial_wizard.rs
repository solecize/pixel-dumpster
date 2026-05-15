use std::io::{BufRead, BufReader, Write};
use std::sync::{Arc, Mutex};
use std::time::Duration;

/// Managed serial connection state
pub struct SerialConnection {
    port: Box<dyn serialport::SerialPort>,
    reader: BufReader<Box<dyn serialport::SerialPort>>,
}

pub struct WizardState {
    connection: Option<SerialConnection>,
}

impl WizardState {
    pub fn new() -> Self {
        Self { connection: None }
    }
}

pub type SharedWizardState = Arc<Mutex<WizardState>>;

pub fn new_shared_state() -> SharedWizardState {
    Arc::new(Mutex::new(WizardState::new()))
}

/// Open a serial port and send the hello command.
/// Returns the initial response lines.
pub fn connect(
    state: &SharedWizardState,
    port_name: &str,
    baud_rate: u32,
) -> Result<Vec<String>, String> {
    let mut guard = state.lock().map_err(|e| format!("Lock error: {}", e))?;

    // Close any existing connection
    guard.connection = None;

    let port = serialport::new(port_name, baud_rate)
        .timeout(Duration::from_millis(100))
        .open()
        .map_err(|e| format!("Failed to open {}: {}", port_name, e))?;

    let reader_port = port
        .try_clone()
        .map_err(|e| format!("Failed to clone port: {}", e))?;

    let mut conn = SerialConnection {
        port,
        reader: BufReader::new(reader_port),
    };

    // Send hello
    conn.port
        .write_all(b"{\"cmd\":\"hello\"}\n")
        .map_err(|e| format!("Write error: {}", e))?;

    // Read initial responses (wait a bit for device to respond)
    std::thread::sleep(Duration::from_millis(500));
    let lines = read_available_lines(&mut conn.reader);

    guard.connection = Some(conn);
    Ok(lines)
}

/// Disconnect and send goodbye
pub fn disconnect(state: &SharedWizardState) -> Result<(), String> {
    let mut guard = state.lock().map_err(|e| format!("Lock error: {}", e))?;

    if let Some(ref mut conn) = guard.connection {
        let _ = conn.port.write_all(b"{\"cmd\":\"goodbye\"}\n");
        let _ = conn.port.flush();
    }
    guard.connection = None;
    Ok(())
}

/// Send a JSON command and read response lines
pub fn send_command(
    state: &SharedWizardState,
    json_cmd: &str,
) -> Result<Vec<String>, String> {
    let mut guard = state.lock().map_err(|e| format!("Lock error: {}", e))?;

    let conn = guard
        .connection
        .as_mut()
        .ok_or("Not connected. Open serial connection first.")?;

    // Send the command
    let mut cmd = json_cmd.to_string();
    if !cmd.ends_with('\n') {
        cmd.push('\n');
    }
    conn.port
        .write_all(cmd.as_bytes())
        .map_err(|e| format!("Write error: {}", e))?;
    conn.port.flush().map_err(|e| format!("Flush error: {}", e))?;

    // Wait for response
    std::thread::sleep(Duration::from_millis(300));
    let lines = read_available_lines(&mut conn.reader);

    // Some commands (like wifi scan/test) take longer — poll again if empty
    if lines.is_empty() {
        std::thread::sleep(Duration::from_millis(2000));
        return Ok(read_available_lines(&mut conn.reader));
    }

    Ok(lines)
}

/// Poll for any available lines without sending a command
pub fn poll(state: &SharedWizardState) -> Result<Vec<String>, String> {
    let mut guard = state.lock().map_err(|e| format!("Lock error: {}", e))?;

    let conn = guard
        .connection
        .as_mut()
        .ok_or("Not connected")?;

    Ok(read_available_lines(&mut conn.reader))
}

/// Reboot the ESP32 by toggling RTS+DTR (hardware reset via USB serial)
pub fn reboot_device(state: &SharedWizardState) -> Result<(), String> {
    let mut guard = state.lock().map_err(|e| format!("Lock error: {}", e))?;

    if let Some(ref mut conn) = guard.connection {
        log::info!("Rebooting ESP32 via RTS/DTR toggle");
        // RTS low + DTR low = EN low (hold in reset)
        let _ = conn.port.write_request_to_send(true);
        let _ = conn.port.write_data_terminal_ready(false);
        std::thread::sleep(Duration::from_millis(100));
        // Release reset
        let _ = conn.port.write_request_to_send(false);
        let _ = conn.port.write_data_terminal_ready(false);
        std::thread::sleep(Duration::from_millis(100));
    }

    // Close the connection — device is rebooting
    guard.connection = None;
    Ok(())
}

/// Read all available complete lines from the serial port
fn read_available_lines(reader: &mut BufReader<Box<dyn serialport::SerialPort>>) -> Vec<String> {
    let mut lines = Vec::new();
    loop {
        let mut line = String::new();
        match reader.read_line(&mut line) {
            Ok(0) => break,
            Ok(_) => {
                let trimmed = line.trim().to_string();
                if !trimmed.is_empty() && trimmed.starts_with('{') {
                    lines.push(trimmed);
                }
            }
            Err(ref e) if e.kind() == std::io::ErrorKind::TimedOut => break,
            Err(ref e) if e.kind() == std::io::ErrorKind::WouldBlock => break,
            Err(_) => break,
        }
    }
    lines
}
