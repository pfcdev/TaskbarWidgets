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
const MEDIA_DESIGN: &str = "media-player";
const STEAM_DESIGN: &str = "steam-download";
const SYSTEM_CPU_DESIGN: &str = "system-cpu";
const SYSTEM_STORAGE_DESIGN: &str = "system-storage";
const SYSTEM_NETWORK_DESIGN: &str = "system-network";
const SYSTEM_MEMORY_DESIGN: &str = "system-memory";

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
    #[serde(default)]
    settings: serde_json::Map<String, serde_json::Value>,
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

#[derive(Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
struct ConfigV2 {
    config_version: u32,
    layout: LayoutConfig,
    widgets: Vec<ConfigWidget>,
    rotation: RotationConfig,
}

#[derive(Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
struct LayoutConfig {
    mode: String,
}

#[derive(Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
struct ConfigWidget {
    id: String,
    enabled: bool,
    order: i32,
    position: WidgetPosition,
    #[serde(default)]
    settings: serde_json::Map<String, serde_json::Value>,
}

#[derive(Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
struct WidgetPosition {
    anchor_percent: i32,
    offset_px: i32,
}

#[derive(Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
struct RotationConfig {
    interval_seconds: u32,
    widget_ids: Vec<String>,
}

#[derive(Default, Deserialize)]
#[serde(rename_all = "camelCase")]
struct SnapshotEnvelope<T> {
    schema_version: Option<u32>,
    widget_id: Option<String>,
    status: Option<String>,
    data: Option<T>,
}

#[derive(Default, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
struct UpdateStatus {
    state: Option<String>,
    current_version: Option<String>,
    latest_version: Option<String>,
    update_available: Option<bool>,
    message: Option<String>,
    installer_path: Option<String>,
    progress_percent: Option<f64>,
    downloaded_bytes: Option<u64>,
    total_bytes: Option<u64>,
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
    system_sources: SystemSources,
}

#[derive(Default, Serialize)]
#[serde(rename_all = "camelCase")]
struct SystemSources {
    disks: Vec<SourceOption>,
    interfaces: Vec<SourceOption>,
}

#[derive(Clone, Default, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
struct SourceOption {
    id: String,
    name: String,
}

#[derive(Default, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
struct SettingsOpenRequest {
    schema_version: u32,
    request_id: String,
    widget_id: String,
    created_at_unix: i64,
}

#[derive(Default, Deserialize)]
#[serde(rename_all = "camelCase")]
struct StorageSourcesSnapshot {
    #[serde(default)]
    available_disks: Vec<SourceOption>,
}

#[derive(Default, Deserialize)]
#[serde(rename_all = "camelCase")]
struct NetworkSourcesSnapshot {
    #[serde(default)]
    available_interfaces: Vec<SourceOption>,
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
            MEDIA_DESIGN.to_owned(),
            STEAM_DESIGN.to_owned(),
            SYSTEM_CPU_DESIGN.to_owned(),
            SYSTEM_STORAGE_DESIGN.to_owned(),
            SYSTEM_NETWORK_DESIGN.to_owned(),
            SYSTEM_MEMORY_DESIGN.to_owned(),
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
        MEDIA_DESIGN,
        STEAM_DESIGN,
        SYSTEM_CPU_DESIGN,
        SYSTEM_STORAGE_DESIGN,
        SYSTEM_NETWORK_DESIGN,
        SYSTEM_MEMORY_DESIGN,
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
        settings: system_meter_defaults(design).unwrap_or_default(),
    })
    .collect()
}

fn system_meter_defaults(design: &str) -> Option<serde_json::Map<String, serde_json::Value>> {
    let value = match design {
        SYSTEM_CPU_DESIGN => serde_json::json!({
            "meterStyleVersion": 1, "displayMode": "bar", "refreshSeconds": 3.0,
            "outlineColor": "#FFFFFFFF", "showIndividualCores": true,
            "combineLogicalCores": false, "separateUtilization": true,
            "systemColor": "#FFFFFFFF", "userColor": "systemAccent", "cpuColor": "systemAccent"
        }),
        SYSTEM_STORAGE_DESIGN => serde_json::json!({
            "meterStyleVersion": 1, "displayMode": "text", "refreshSeconds": 3.0,
            "outlineColor": "#FFFFFFFF", "readColor": "#FFFFFFFF", "writeColor": "#FFFFFFFF",
            "diskId": "_Total"
        }),
        SYSTEM_NETWORK_DESIGN => serde_json::json!({
            "meterStyleVersion": 1, "displayMode": "text", "refreshSeconds": 3.0,
            "outlineColor": "#FFFFFFFF", "receiveColor": "systemAccent", "sendColor": "systemAccent",
            "interfaceId": "all", "autoBandwidth": true, "bandwidthKiloBytes": 125000.0
        }),
        SYSTEM_MEMORY_DESIGN => serde_json::json!({
            "meterStyleVersion": 1, "displayMode": "pie", "refreshSeconds": 3.0,
            "outlineColor": "systemAccent", "usedColor": "systemAccent"
        }),
        _ => return None,
    };
    value.as_object().cloned()
}

fn normalize_system_meter_settings(widget: &mut WidgetInstanceSettings) {
    let Some(defaults) = system_meter_defaults(&widget.design) else { return; };
    let version = widget.settings.get("meterStyleVersion").and_then(|value| value.as_u64());
    if version != Some(1) {
        widget.settings = defaults;
    }
}

fn install_dir() -> Result<PathBuf, String> {
    let exe = std::env::current_exe().map_err(|e| e.to_string())?;
    let exe_dir = exe
        .parent()
        .map(Path::to_path_buf)
        .ok_or_else(|| "Application directory could not be resolved.".to_owned())?;

    if exe_dir.join("TaskbarWidgets.exe").exists() {
        return Ok(exe_dir);
    }

    for ancestor in exe_dir.ancestors() {
        let artifact_dir = ancestor.join("artifacts").join("TaskbarWidgets");
        if artifact_dir.join("TaskbarWidgets.exe").exists() {
            return Ok(artifact_dir);
        }
    }

    Ok(exe_dir)
}

fn data_dir(install_dir: &Path) -> PathBuf {
    install_dir.join("Data")
}

fn read_settings_from(data_dir: &Path) -> WidgetSettings {
    let config_path = data_dir.join("config.json");
    let mut settings = fs::read_to_string(&config_path)
        .ok()
        .and_then(|data| serde_json::from_str::<ConfigV2>(&data).ok())
        .map(config_to_transport)
        .or_else(|| {
            fs::read_to_string(data_dir.join("widget-settings.json"))
                .ok()
                .and_then(|data| serde_json::from_str::<WidgetSettings>(&data).ok())
        })
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
    fs::read_to_string(app_dir.join("State").join("media-player.json"))
        .ok()
        .and_then(|data| serde_json::from_str::<SnapshotEnvelope<MediaStatus>>(&data).ok())
        .and_then(|snapshot| snapshot.data)
        .unwrap_or_default()
}

fn read_system_sources_from(app_dir: &Path) -> SystemSources {
    let storage = fs::read_to_string(app_dir.join("State").join("system-storage.json"))
        .ok()
        .and_then(|data| serde_json::from_str::<SnapshotEnvelope<StorageSourcesSnapshot>>(&data).ok())
        .and_then(|snapshot| {
            if snapshot.schema_version == Some(1) && snapshot.widget_id.as_deref() == Some(SYSTEM_STORAGE_DESIGN) && snapshot.status.as_deref() == Some("ok") { snapshot.data } else { None }
        })
        .unwrap_or_default();
    let network = fs::read_to_string(app_dir.join("State").join("system-network.json"))
        .ok()
        .and_then(|data| serde_json::from_str::<SnapshotEnvelope<NetworkSourcesSnapshot>>(&data).ok())
        .and_then(|snapshot| {
            if snapshot.schema_version == Some(1) && snapshot.widget_id.as_deref() == Some(SYSTEM_NETWORK_DESIGN) && snapshot.status.as_deref() == Some("ok") { snapshot.data } else { None }
        })
        .unwrap_or_default();
    SystemSources {
        disks: storage.available_disks,
        interfaces: network.available_interfaces,
    }
}

fn normalize_design(id: &str) -> &str {
    match id {
        WEATHER_DESIGN => WEATHER_DESIGN,
        DISCORD_DESIGN => DISCORD_DESIGN,
        MEDIA_DESIGN => MEDIA_DESIGN,
        STEAM_DESIGN => STEAM_DESIGN,
        SYSTEM_CPU_DESIGN => SYSTEM_CPU_DESIGN,
        SYSTEM_STORAGE_DESIGN => SYSTEM_STORAGE_DESIGN,
        SYSTEM_NETWORK_DESIGN => SYSTEM_NETWORK_DESIGN,
        SYSTEM_MEMORY_DESIGN => SYSTEM_MEMORY_DESIGN,
        _ => CODEX_DESIGN,
    }
}

fn normalize_rotation_designs(saved: Option<Vec<String>>) -> Vec<String> {
    let source = saved.unwrap_or_else(|| {
        vec![
            CODEX_DESIGN.to_owned(),
            WEATHER_DESIGN.to_owned(),
            DISCORD_DESIGN.to_owned(),
            MEDIA_DESIGN.to_owned(),
            STEAM_DESIGN.to_owned(),
            SYSTEM_CPU_DESIGN.to_owned(),
            SYSTEM_STORAGE_DESIGN.to_owned(),
            SYSTEM_NETWORK_DESIGN.to_owned(),
            SYSTEM_MEMORY_DESIGN.to_owned(),
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
        MEDIA_DESIGN,
        STEAM_DESIGN,
        SYSTEM_CPU_DESIGN,
        SYSTEM_STORAGE_DESIGN,
        SYSTEM_NETWORK_DESIGN,
        SYSTEM_MEMORY_DESIGN,
    ];

    widgets.retain(|widget| widget.design != "btc-fees" && widget.id != "btc-fees");
    for widget in &mut widgets {
        if widget.design.is_empty() {
            widget.design = widget.id.clone();
        }
        if widget.id.is_empty() {
            widget.id = widget.design.clone();
        }
        if !known.contains(&widget.design.as_str()) {
            widget.enabled = false;
        }
        widget.move_x = widget.move_x.clamp(-640, 640);
        widget.position_pct = Some(widget.position_pct.unwrap_or(100).clamp(0, 100));
        normalize_system_meter_settings(widget);
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
                settings: system_meter_defaults(design).unwrap_or_default(),
            });
        }
    }

    for (index, widget) in widgets.iter_mut().enumerate() {
        widget.order = index as i32;
    }

    widgets
}

fn setting_string(widget: Option<&ConfigWidget>, key: &str, fallback: &str) -> String {
    widget
        .and_then(|item| item.settings.get(key))
        .and_then(|value| value.as_str())
        .unwrap_or(fallback)
        .to_owned()
}

fn setting_bool(widget: Option<&ConfigWidget>, key: &str, fallback: bool) -> bool {
    widget
        .and_then(|item| item.settings.get(key))
        .and_then(|value| value.as_bool())
        .unwrap_or(fallback)
}

fn config_to_transport(config: ConfigV2) -> WidgetSettings {
    let active = config
        .widgets
        .iter()
        .find(|widget| widget.enabled && normalize_design(&widget.id) == widget.id)
        .map(|widget| widget.id.clone())
        .unwrap_or_else(|| CODEX_DESIGN.to_owned());
    let active_widget = config.widgets.iter().find(|widget| widget.id == active);
    let codex = config.widgets.iter().find(|widget| widget.id == CODEX_DESIGN);
    let weather = config.widgets.iter().find(|widget| widget.id == WEATHER_DESIGN);
    let discord = config.widgets.iter().find(|widget| widget.id == DISCORD_DESIGN);
    let media = config.widgets.iter().find(|widget| widget.id == MEDIA_DESIGN);

    WidgetSettings {
        active_design: active.clone(),
        enabled: Some(active_widget.map(|widget| widget.enabled).unwrap_or(true)),
        refresh_interval_secs: Some(30),
        widget_offset_px: Some(active_widget.map(|widget| widget.position.offset_px.unsigned_abs()).unwrap_or(0)),
        widget_move_x: Some(active_widget.map(|widget| widget.position.offset_px).unwrap_or(0)),
        widgets: Some(config.widgets.iter().map(|widget| WidgetInstanceSettings {
            id: widget.id.clone(),
            design: widget.id.clone(),
            enabled: widget.enabled,
            move_x: widget.position.offset_px,
            position_pct: Some(widget.position.anchor_percent),
            order: widget.order,
            settings: widget.settings.clone(),
        }).collect()),
        rotation_enabled: Some(config.layout.mode == "rotation"),
        rotation_interval_secs: Some(config.rotation.interval_seconds),
        rotation_designs: Some(config.rotation.widget_ids),
        codex_api_endpoint: Some(setting_string(codex, "apiEndpoint", "")),
        codex_project_filter: Some(setting_string(codex, "projectFilter", "")),
        weather_city: Some(setting_string(weather, "city", "Istanbul")),
        weather_temp_unit: Some(setting_string(weather, "temperatureUnit", "C")),
        discord_enabled: Some(discord.map(|widget| widget.enabled).unwrap_or(false)),
        discord_background_enabled: Some(setting_bool(discord, "backgroundEnabled", true)),
        media_dark_mode: Some(setting_bool(media, "darkMode", true)),
        discord_client_id: Some(setting_string(discord, "clientId", "")),
        discord_client_secret: Some(setting_string(discord, "clientSecret", "")),
        discord_redirect_uri: Some(setting_string(discord, "redirectUri", "http://127.0.0.1/callback")),
    }
}

fn string_setting(map: &mut serde_json::Map<String, serde_json::Value>, key: &str, value: Option<&String>) {
    map.insert(key.to_owned(), serde_json::Value::String(value.cloned().unwrap_or_default()));
}

fn transport_to_config(settings: &WidgetSettings) -> ConfigV2 {
    let widgets = settings.widgets.clone().unwrap_or_else(default_widget_instances);
    let config_widgets = widgets.into_iter().map(|widget| {
        let mut values = widget.settings.clone();
        match widget.design.as_str() {
            CODEX_DESIGN => {
                string_setting(&mut values, "apiEndpoint", settings.codex_api_endpoint.as_ref());
                string_setting(&mut values, "projectFilter", settings.codex_project_filter.as_ref());
            }
            WEATHER_DESIGN => {
                string_setting(&mut values, "city", settings.weather_city.as_ref());
                string_setting(&mut values, "temperatureUnit", settings.weather_temp_unit.as_ref());
            }
            DISCORD_DESIGN => {
                values.insert("backgroundEnabled".to_owned(), serde_json::Value::Bool(settings.discord_background_enabled.unwrap_or(true)));
                string_setting(&mut values, "clientId", settings.discord_client_id.as_ref());
                string_setting(&mut values, "clientSecret", settings.discord_client_secret.as_ref());
                string_setting(&mut values, "redirectUri", settings.discord_redirect_uri.as_ref());
            }
            MEDIA_DESIGN => {
                values.insert("darkMode".to_owned(), serde_json::Value::Bool(settings.media_dark_mode.unwrap_or(true)));
            }
            _ => {}
        }
        ConfigWidget {
            id: widget.design,
            enabled: widget.enabled,
            order: widget.order,
            position: WidgetPosition {
                anchor_percent: widget.position_pct.unwrap_or(100).clamp(0, 100),
                offset_px: widget.move_x.clamp(-640, 640),
            },
            settings: values,
        }
    }).collect();

    ConfigV2 {
        config_version: 2,
        layout: LayoutConfig {
            mode: if settings.rotation_enabled.unwrap_or(false) { "rotation" } else { "row" }.to_owned(),
        },
        widgets: config_widgets,
        rotation: RotationConfig {
            interval_seconds: settings.rotation_interval_secs.unwrap_or(30).clamp(5, 3600),
            widget_ids: normalize_rotation_designs(settings.rotation_designs.clone()),
        },
    }
}

#[tauri::command]
fn load_state() -> Result<AppState, String> {
    let install = install_dir()?;
    let dir = data_dir(&install);
    fs::create_dir_all(&dir).map_err(|e| e.to_string())?;
    Ok(AppState {
        app_dir: install.display().to_string(),
        settings: read_settings_from(&dir),
        update_status: read_update_status_from(&dir),
        media_status: read_media_status_from(&dir),
        system_sources: read_system_sources_from(&dir),
    })
}

#[tauri::command]
fn save_settings(settings: WidgetSettings) -> Result<(), String> {
    let install = install_dir()?;
    let dir = data_dir(&install);
    fs::create_dir_all(&dir).map_err(|e| e.to_string())?;
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

    let config = transport_to_config(&normalized);
    let json = serde_json::to_string_pretty(&config).map_err(|e| e.to_string())?;
    let path = dir.join("config.json");
    let temp = dir.join("config.json.tmp");
    fs::write(&temp, format!("{json}\n")).map_err(|e| e.to_string())?;
    fs::rename(temp, path).map_err(|e| e.to_string())
}

#[tauri::command]
fn consume_settings_open_request() -> Result<Option<String>, String> {
    let path = data_dir(&install_dir()?).join("Runtime").join("settings-open-request.json");
    if !path.exists() {
        return Ok(None);
    }
    let request = fs::read_to_string(&path)
        .map_err(|e| e.to_string())
        .and_then(|json| serde_json::from_str::<SettingsOpenRequest>(&json).map_err(|e| e.to_string()));
    let _ = fs::remove_file(&path);
    let request = request?;
    let known = [SYSTEM_CPU_DESIGN, SYSTEM_STORAGE_DESIGN, SYSTEM_NETWORK_DESIGN, SYSTEM_MEMORY_DESIGN];
    let now = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map(|value| value.as_secs() as i64)
        .unwrap_or_default();
    if request.schema_version != 1 || request.request_id.is_empty() ||
       !known.contains(&request.widget_id.as_str()) ||
       request.created_at_unix <= 0 || (now - request.created_at_unix).abs() > 300 {
        return Ok(None);
    }
    Ok(Some(request.widget_id))
}

#[tauri::command]
fn open_widget_libraries() -> Result<(), String> {
    let dir = data_dir(&install_dir()?).join("Legacy").join("WidgetLibraries");
    fs::create_dir_all(&dir).map_err(|e| e.to_string())?;
    let readme = dir.join("README.txt");
    if !readme.exists() {
        let _ = fs::write(&readme, "Legacy widget files are preserved here but are not executable in v1.\r\n");
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

    let dir = install_dir()?;
    let status = Command::new(dir.join("TaskbarWidgets.exe"))
        .current_dir(&dir)
        .arg(arg)
        .status()
        .map_err(|e| e.to_string())?;

    if !status.success() {
        return Err(format!("Loader command exited with {status}"));
    }

    load_state()
}

#[tauri::command]
fn start_loader_command(arg: String) -> Result<AppState, String> {
    let allowed = ["--download-update"];
    if !allowed.iter().any(|item| *item == arg) {
        return Err("Unsupported loader command.".to_owned());
    }

    let dir = install_dir()?;
    Command::new(dir.join("TaskbarWidgets.exe"))
        .current_dir(&dir)
        .arg(arg)
        .spawn()
        .map(|_| ())
        .map_err(|e| e.to_string())?;

    load_state()
}

#[tauri::command]
fn control_runtime(action: String) -> Result<AppState, String> {
    let dir = install_dir()?;
    let exe = dir.join("TaskbarWidgets.exe");
    if !exe.exists() {
        return Err(format!("TaskbarWidgets.exe was not found: {}", exe.display()));
    }

    match action.as_str() {
        "load" => {
            Command::new(&exe)
                .current_dir(&dir)
                .arg("--no-update-check")
                .spawn()
                .map(|_| ())
                .map_err(|e| format!("Runtime could not be started: {e}"))?;
        }
        "unload" => {
            let status = Command::new(&exe)
                .current_dir(&dir)
                .arg("--detach")
                .status()
                .map_err(|e| format!("Runtime unload could not be started: {e}"))?;
            if !status.success() {
                return Err(format!("Runtime unload exited with {status}"));
            }
        }
        _ => return Err("Unsupported runtime action.".to_owned()),
    }

    load_state()
}

#[tauri::command]
fn launch_downloaded_installer() -> Result<(), String> {
    let install = install_dir()?;
    let dir = data_dir(&install);
    let status = read_update_status_from(&dir);
    let installer = resolve_installer_path(&dir, &status)?;
    let working_dir = installer
        .parent()
        .map(Path::to_path_buf)
        .unwrap_or_else(|| dir.clone());
    let script_path = working_dir.join("launch-installer-detached.cmd");
    let log_path = dir.join("Logs").join("loader.log");
    let script = format!(
        "@echo off\r\n\
setlocal\r\n\
set \"SRC={}\"\r\n\
set \"DIR={}\"\r\n\
set \"LOG={}\"\r\n\
if not exist \"%DIR%\\Logs\" mkdir \"%DIR%\\Logs\" >nul 2>nul\r\n\
>>\"%LOG%\" echo %DATE% %TIME% [settings-updater] Launching detached setup: \"%SRC%\"\r\n\
start \"\" \"%SRC%\"\r\n\
exit /b 0\r\n",
        installer.display(),
        dir.display(),
        log_path.display()
    );
    fs::write(&script_path, script)
        .map_err(|e| format!("Installer launch helper could not be written: {e}"))?;

    Command::new("cmd.exe")
        .current_dir(working_dir)
        .arg("/d")
        .arg("/c")
        .arg(&script_path)
        .spawn()
        .map(|_| ())
        .map_err(|e| format!("Installer could not be launched: {e}"))
}

fn resolve_installer_path(app_dir: &Path, status: &UpdateStatus) -> Result<PathBuf, String> {
    if let Some(path) = status.installer_path.as_deref() {
        let installer = PathBuf::from(path);
        if installer.exists() {
            return Ok(installer);
        }
    }

    let latest = status
        .latest_version
        .as_deref()
        .filter(|value| !value.trim().is_empty())
        .ok_or_else(|| "No downloaded update version was found.".to_owned())?;
    let installer = app_dir
        .join("Updates")
        .join(latest)
        .join("TaskbarWidgetsSetup-x64.exe");
    if installer.exists() {
        return Ok(installer);
    }

    Err(format!(
        "Downloaded installer was not found: {}",
        installer.display()
    ))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn system_widget_settings_round_trip() {
        let mut settings = default_settings();
        let widgets = settings.widgets.as_mut().expect("default widgets");
        let network = widgets
            .iter_mut()
            .find(|widget| widget.design == SYSTEM_NETWORK_DESIGN)
            .expect("system network widget");
        network.enabled = true;
        network.settings.insert("displayMode".to_owned(), serde_json::json!("pie"));
        network.settings.insert("interfaceId".to_owned(), serde_json::json!("adapter-test"));

        let config = transport_to_config(&settings);
        let saved = config.widgets.iter().find(|widget| widget.id == SYSTEM_NETWORK_DESIGN).unwrap();
        assert_eq!(saved.settings.get("displayMode"), Some(&serde_json::json!("pie")));

        let restored = config_to_transport(config);
        let restored_network = restored.widgets.unwrap().into_iter()
            .find(|widget| widget.design == SYSTEM_NETWORK_DESIGN)
            .unwrap();
        assert_eq!(restored_network.settings.get("interfaceId"), Some(&serde_json::json!("adapter-test")));
    }
}

fn main() {
    tauri::Builder::default()
        .plugin(tauri_plugin_dialog::init())
        .plugin(tauri_plugin_shell::init())
        .invoke_handler(tauri::generate_handler![
            load_state,
            consume_settings_open_request,
            save_settings,
            open_widget_libraries,
            run_loader_command,
            start_loader_command,
            control_runtime,
            launch_downloaded_installer
        ])
        .run(tauri::generate_context!())
        .expect("failed to run TaskbarWidgets settings");
}
