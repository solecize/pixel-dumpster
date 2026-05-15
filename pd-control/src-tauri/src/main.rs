// Prevents additional console window on Windows in release, DO NOT REMOVE!!
#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

mod commands;
mod daemon_api;
mod device_api;
mod discovery;
mod flasher;
mod pi_installer;
mod serial_wizard;

fn main() {
    env_logger::init();

    tauri::Builder::default()
        .plugin(tauri_plugin_shell::init())
        .manage(commands::AppState::default())
        .invoke_handler(tauri::generate_handler![
            commands::discover_devices,
            commands::stop_discovery,
            commands::add_manual_device,
            commands::device_status,
            commands::device_play,
            commands::device_stop,
            commands::device_list_content,
            commands::device_config,
            commands::device_set_config,
            commands::device_layout,
            commands::device_set_layout,
            commands::device_preview_layout,
            commands::device_test_start,
            commands::device_test_stop,
            commands::device_panel_select,
            commands::daemon_status,
            commands::daemon_config,
            commands::daemon_reload,
            commands::daemon_inject_event,
            commands::daemon_log,
            commands::list_serial_ports,
            commands::check_flash_tool,
            commands::check_idf_installed,
            commands::flash_device,
            commands::pi_test_connection,
            commands::pi_check_daemon,
            commands::pi_install_daemon,
            commands::pi_uninstall_daemon,
            commands::wizard_connect,
            commands::wizard_disconnect,
            commands::wizard_send,
            commands::wizard_reboot,
            commands::wizard_poll,
            commands::upload_content_to_device,
        ])
        .setup(|app| {
            log::info!("Pixel Dumpster Control starting");

            #[cfg(target_os = "macos")]
            {
                use tauri::image::Image;
                use tauri::menu::{AboutMetadataBuilder, MenuBuilder, SubmenuBuilder, PredefinedMenuItem};

                let png_data = include_bytes!("../icons/128x128.png");
                let img = image::load_from_memory(png_data).expect("failed to decode icon PNG");
                let rgba = img.to_rgba8();
                let (w, h) = rgba.dimensions();
                let icon = Image::new_owned(rgba.into_raw(), w, h);
                let about_meta = AboutMetadataBuilder::new()
                    .name(Some("Pixel Dumpster Control"))
                    .version(Some("0.1.0"))
                    .icon(Some(icon))
                    .build();

                let about = PredefinedMenuItem::about(app, Some("About Pixel Dumpster Control"), Some(about_meta))?;
                let quit = PredefinedMenuItem::quit(app, Some("Quit Pixel Dumpster Control"))?;
                let separator = PredefinedMenuItem::separator(app)?;

                let app_menu = SubmenuBuilder::new(app, "Pixel Dumpster Control")
                    .item(&about)
                    .item(&separator)
                    .item(&quit)
                    .build()?;

                let edit_menu = SubmenuBuilder::new(app, "Edit")
                    .undo()
                    .redo()
                    .separator()
                    .cut()
                    .copy()
                    .paste()
                    .select_all()
                    .build()?;

                let window_menu = SubmenuBuilder::new(app, "Window")
                    .minimize()
                    .close_window()
                    .build()?;

                let menu = MenuBuilder::new(app)
                    .item(&app_menu)
                    .item(&edit_menu)
                    .item(&window_menu)
                    .build()?;

                app.set_menu(menu)?;
            }

            Ok(())
        })
        .run(tauri::generate_context!())
        .expect("error while running tauri application");
}
