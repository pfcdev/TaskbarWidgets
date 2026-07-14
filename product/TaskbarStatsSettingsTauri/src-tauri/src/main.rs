#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

use serde::{Deserialize, Serialize};
use std::{
    fs,
    path::{Path, PathBuf},
    process::Command,
};

const CODEX_DESIGN: &str = "codex-status";
const WEATHER_DESIGN: &str = "weather-static";
const DISCORD_DESIGN: &str = "discord-voice";
const BTC_DESIGN: &str = "btc-fees";
const MEDIA_DESIGN: &str = "media-player";
const STEAM_DESIGN: &str = "steam-download";

#[derive(Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
struct WidgetInstanceSettings {
    id: String,
    design: String,
    #[serde(default)]
    enabled: bool,
    #[serde(default)]
    move_x: i32,
    #[serde(default)]
    position_pct: Option<i32>,
    #[serde(default)]
    order: i32,
}

#[derive(Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
struct WidgetSettings {
    active_design: String,
    #[serde(default)]
    enabled: Option<bool>,
    #[serde(default)]
    refresh_interval_secs: Option<u32>,
    #[serde(default)]
    widget_offset_px: Option<u32>,
    #[serde(default)]
    widget_move_x: Option<i32>,
    #[serde(default)]
    widgets: Option<Vec<WidgetInstanceSettings>>,
    #[serde(default)]
    rotation_enabled: Option<bool>,
    #[serde(default)]
    rotation_interval_secs: Option<u32>,
    #[serde(default)]
    rotation_designs: Option<Vec<String>>,
    #[serde(default)]
    codex_api_endpoint: Option<String>,
    #[serde(default)]
    codex_project_filter: Option<String>,
    #[serde(default)]
    weather_city: Option<String>,
    #[serde(default)]
    weather_temp_unit: Option<String>,
    #[serde(default)]
    discord_enabled: Option<bool>,
    #[serde(default)]
    discord_background_enabled: Option<bool>,
    #[serde(default)]
    media_dark_mode: Option<bool>,
    #[serde(default)]
    discord_client_id: Option<String>,
    #[serde(default)]
    discord_client_secret: Option<String>,
    #[serde(default)]
    discord_redirect_uri: Option<String>,
}

#[derive(Default, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
struct UpdateStatus {
    state: Option<String>,
    current_version: Option<String>,
    latest_version: Option<String>,
    update_available: Option<bool>,
    message: Option<String>,
    updated_at_unix: Option<i64>,
}

#[derive(Default, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
struct MediaStatus {
    loaded: Option<bool>,
    active: Option<bool>,
    playing: Option<bool>,
    stale: Option<bool>,
    title: Option<String>,
    artist: Option<String>,
    source_app: Option<String>,
    metadata_source: Option<String>,
    session_count: Option<i32>,
    error: Option<String>,
    updated_at_unix: Option<i64>,
}

#[derive(Serialize)]
#[serde(rename_all = "camelCase")]
struct AppState {
    app_dir: String,
    settings: WidgetSettings,
    update_status: UpdateStatus,
    media_status: MediaStatus,
}

fn default_settings() -> WidgetSettings {
    WidgetSettings {
        active_design: CODEX_DESIGN.to_owned(),
        enabled: Some(true),
        refresh_interval_secs: Some(30),
        widget_offset_px: Some(0),
        widget_move_x: Some(0),
        widgets: Some(default_widget_instances()),
        rotation_enabled: Some(false),
        rotation_interval_secs: Some(30),
        rotation_designs: Some(vec![
            CODEX_DESIGN.to_owned(),
            WEATHER_DESIGN.to_owned(),
            DISCORD_DESIGN.to_owned(),
            BTC_DESIGN.to_owned(),
            MEDIA_DESIGN.to_owned(),
            STEAM_DESIGN.to_owned(),
        ]),
        codex_api_endpoint: None,
        codex_project_filter: None,
        weather_city: Some("Istanbul".to_owned()),
        weather_temp_unit: Some("C".to_owned()),
        discord_enabled: Some(false),
        discord_background_enabled: Some(true),
        media_dark_mode: Some(true),
        discord_client_id: None,
        discord_client_secret: None,
        discord_redirect_uri: Some("http://127.0.0.1/callback".to_owned()),
    }
}

fn default_widget_instances() -> Vec<WidgetInstanceSettings> {
    [
        CODEX_DESIGN,
        WEATHER_DESIGN,
        DISCORD_DESIGN,
        BTC_DESIGN,
        MEDIA_DESIGN,
        STEAM_DESIGN,
    ]
    .iter()
    .enumerate()
    .map(|(index, design)| WidgetInstanceSettings {
        id: (*design).to_owned(),
        design: (*design).to_owned(),
        enabled: *design == CODEX_DESIGN,
        move_x: 0,
        position_pct: Some(100),
        order: index as i32,
    })
    .collect()
}

fn app_dir() -> Result<PathBuf, String> {
    let exe = std::env::current_exe().map_err(|e| e.to_string())?;
    exe.parent()
        .map(Path::to_path_buf)
        .ok_or_else(|| "Application directory could not be resolved.".to_owned())
}

fn read_settings_from(app_dir: &Path) -> WidgetSettings {
    let mut settings = fs::read_to_string(app_dir.join("widget-settings.json"))
        .ok()
        .and_then(|data| serde_json::from_str::<WidgetSettings>(&data).ok())
        .unwrap_or_else(default_settings);

    settings.active_design = normalize_design(&settings.active_design).to_owned();
    settings.rotation_designs = Some(normalize_rotation_designs(settings.rotation_designs));
    settings.widget_offset_px = Some(settings.widget_offset_px.unwrap_or(0).min(480));
    settings.widget_move_x = Some(settings.widget_move_x.unwrap_or_else(|| {
        -(settings.widget_offset_px.unwrap_or(0).min(480) as i32)
    }).clamp(-640, 640));
    settings.widgets = Some(normalize_widget_instances(
        settings.widgets,
        &settings.active_design,
        settings.enabled.unwrap_or(true),
        settings.widget_move_x.unwrap_or(0),
    ));
    settings.rotation_interval_secs = Some(settings.rotation_interval_secs.unwrap_or(30).clamp(5, 3600));
    settings.refresh_interval_secs = Some(settings.refresh_interval_secs.unwrap_or(30).clamp(1, 3600));
    settings
}

fn read_update_status_from(app_dir: &Path) -> UpdateStatus {
    fs::read_to_string(app_dir.join("update-status.json"))
        .ok()
        .and_then(|data| serde_json::from_str(&data).ok())
        .unwrap_or_default()
}

fn read_media_status_from(app_dir: &Path) -> MediaStatus {
    fs::read_to_string(app_dir.join("media-status.json"))
        .ok()
        .and_then(|data| serde_json::from_str(&data).ok())
        .unwrap_or_default()
}

fn normalize_design(id: &str) -> &str {
    match id {
        WEATHER_DESIGN => WEATHER_DESIGN,
        DISCORD_DESIGN => DISCORD_DESIGN,
        BTC_DESIGN => BTC_DESIGN,
        MEDIA_DESIGN => MEDIA_DESIGN,
        STEAM_DESIGN => STEAM_DESIGN,
        _ => CODEX_DESIGN,
    }
}

fn normalize_rotation_designs(saved: Option<Vec<String>>) -> Vec<String> {
    let source = saved.unwrap_or_else(|| {
        vec![
            CODEX_DESIGN.to_owned(),
            WEATHER_DESIGN.to_owned(),
            DISCORD_DESIGN.to_owned(),
            BTC_DESIGN.to_owned(),
            MEDIA_DESIGN.to_owned(),
            STEAM_DESIGN.to_owned(),
        ]
    });

    let mut designs = Vec::new();
    for id in source {
        let normalized = normalize_design(&id).to_owned();
        if !designs.iter().any(|existing| existing == &normalized) {
            designs.push(normalized);
        }
    }
    if designs.is_empty() {
        designs.push(CODEX_DESIGN.to_owned());
    }
    designs
}

fn normalize_widget_instances(
    saved: Option<Vec<WidgetInstanceSettings>>,
    active_design: &str,
    legacy_enabled: bool,
    legacy_move_x: i32,
) -> Vec<WidgetInstanceSettings> {
    let mut widgets = saved.unwrap_or_else(|| {
        let mut defaults = default_widget_instances();
        for widget in &mut defaults {
            widget.enabled = widget.design == active_design && legacy_enabled;
            widget.move_x = if widget.design == active_design {
                legacy_move_x
            } else {
                0
            };
        }
        defaults
    });

    let known = [
        CODEX_DESIGN,
        WEATHER_DESIGN,
        DISCORD_DESIGN,
        BTC_DESIGN,
        MEDIA_DESIGN,
        STEAM_DESIGN,
    ];

    for widget in &mut widgets {
        widget.design = normalize_design(&widget.design).to_owned();
        if widget.id.is_empty() {
            widget.id = widget.design.clone();
        }
        widget.move_x = widget.move_x.clamp(-640, 640);
        widget.position_pct = Some(widget.position_pct.unwrap_or(100).clamp(0, 100));
    }

    widgets.sort_by_key(|widget| widget.order);
    for (index, design) in known.iter().enumerate() {
        if !widgets.iter().any(|widget| widget.design == *design) {
            widgets.push(WidgetInstanceSettings {
                id: (*design).to_owned(),
                design: (*design).to_owned(),
                enabled: false,
                move_x: 0,
                position_pct: Some(100),
                order: index as i32,
            });
        }
    }

    for (index, widget) in widgets.iter_mut().enumerate() {
        widget.order = index as i32;
    }

    widgets
}

#[tauri::command]
fn load_state() -> Result<AppState, String> {
    let dir = app_dir()?;
    Ok(AppState {
        app_dir: dir.display().to_string(),
        settings: read_settings_from(&dir),
        update_status: read_update_status_from(&dir),
        media_status: read_media_status_from(&dir),
    })
}

#[tauri::command]
fn save_settings(settings: WidgetSettings) -> Result<(), String> {
    let dir = app_dir()?;
    let mut normalized = settings;
    normalized.active_design = normalize_design(&normalized.active_design).to_owned();
    normalized.rotation_designs = Some(normalize_rotation_designs(normalized.rotation_designs));
    normalized.widget_offset_px = Some(normalized.widget_offset_px.unwrap_or(0).min(480));
    normalized.widget_move_x = Some(normalized.widget_move_x.unwrap_or(0).clamp(-640, 640));
    normalized.widgets = Some(normalize_widget_instances(
        normalized.widgets,
        &normalized.active_design,
        normalized.enabled.unwrap_or(true),
        normalized.widget_move_x.unwrap_or(0),
    ));
    normalized.rotation_interval_secs = Some(normalized.rotation_interval_secs.unwrap_or(30).clamp(5, 3600));

    let json = serde_json::to_string_pretty(&normalized).map_err(|e| e.to_string())?;
    fs::write(dir.join("widget-settings.json"), format!("{json}\n")).map_err(|e| e.to_string())
}

#[tauri::command]
fn open_widget_libraries() -> Result<(), String> {
    let dir = app_dir()?.join("WidgetLibraries");
    fs::create_dir_all(&dir).map_err(|e| e.to_string())?;
    let readme = dir.join("README.txt");
    if !readme.exists() {
        let _ = fs::write(&readme, "TaskbarStats widget design packs.\r\n");
    }

    Command::new("explorer")
        .arg(&dir)
        .spawn()
        .map(|_| ())
        .map_err(|e| e.to_string())
}

#[tauri::command]
fn run_loader_command(arg: String) -> Result<AppState, String> {
    let allowed = ["--check-updates", "--update"];
    if !allowed.iter().any(|item| *item == arg) {
        return Err("Unsupported loader command.".to_owned());
    }

    let dir = app_dir()?;
    let status = Command::new(dir.join("TaskbarStats.exe"))
        .current_dir(&dir)
        .arg(arg)
        .status()
        .map_err(|e| e.to_string())?;

    if !status.success() {
        return Err(format!("Loader command exited with {status}"));
    }

    load_state()
}

fn main() {
    tauri::Builder::default()
        .plugin(tauri_plugin_dialog::init())
        .plugin(tauri_plugin_shell::init())
        .invoke_handler(tauri::generate_handler![
            load_state,
            save_settings,
            open_widget_libraries,
            run_loader_command
        ])
        .run(tauri::generate_context!())
        .expect("failed to run TaskbarStats settings");
}
