#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};
use std::{
    collections::{HashMap, HashSet},
    fs,
    io::Read,
    os::windows::ffi::OsStrExt,
    os::windows::process::CommandExt,
    path::{Path, PathBuf},
    process::Command,
    sync::{Mutex, OnceLock},
    time::{Duration, SystemTime, UNIX_EPOCH},
};
use windows_sys::Win32::{
    Foundation::LocalFree,
    Security::Cryptography::{
        CryptProtectData, CryptUnprotectData, CRYPTPROTECT_UI_FORBIDDEN, CRYPT_INTEGER_BLOB,
    },
    Storage::FileSystem::{MoveFileExW, MOVEFILE_REPLACE_EXISTING, MOVEFILE_WRITE_THROUGH},
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
const REMOTE_LIBRARY_BASE_URL: &str = "https://pfcsoft.com/twidget_library";
const MAX_WIDGET_PACKAGE_BYTES: usize = 250 * 1024 * 1024;
const MAX_WIDGET_PACKAGE_FILES: usize = 5000;
const MAX_LIBRARY_RESPONSE_BYTES: usize = 256 * 1024;
const REVIEW_LIFETIME_SECONDS: i64 = 30 * 60;
const CREATE_NO_WINDOW: u32 = 0x08000000;
const VOICE_HELPER_SERVICE: &str = "TaskbarWidgetsVoiceCapture";

fn replace_file_atomic(source: &Path, target: &Path) -> Result<(), String> {
    let source_wide = source
        .as_os_str()
        .encode_wide()
        .chain(std::iter::once(0))
        .collect::<Vec<_>>();
    let target_wide = target
        .as_os_str()
        .encode_wide()
        .chain(std::iter::once(0))
        .collect::<Vec<_>>();
    let result = unsafe {
        MoveFileExW(
            source_wide.as_ptr(),
            target_wide.as_ptr(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH,
        )
    };
    if result == 0 {
        Err(std::io::Error::last_os_error().to_string())
    } else {
        Ok(())
    }
}

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
    discord_real_time_voice_enabled: Option<bool>,
    #[serde(default)]
    media_dark_mode: Option<bool>,
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
    #[serde(default)]
    id: String,
    #[serde(default)]
    instance_id: String,
    #[serde(default)]
    widget_id: String,
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
    #[serde(default)]
    widget_ids: Vec<String>,
    #[serde(default)]
    instance_ids: Vec<String>,
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
    widget_catalog: serde_json::Value,
    community_widgets_dir: String,
    community_update_state: serde_json::Value,
    web_render_health: serde_json::Value,
    voice_helper_installed: bool,
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
struct WidgetInstallRequest {
    schema_version: u32,
    request_id: String,
    source: String,
    created_at_unix: i64,
}

#[derive(Clone, Debug, Serialize)]
#[serde(rename_all = "camelCase")]
struct WidgetInstallPreview {
    source: String,
    schema_version: u64,
    id: String,
    version: String,
    display_name: String,
    description: String,
    author_name: String,
    author_website: Option<String>,
    permissions: serde_json::Value,
    provider_type: String,
    renderer_type: String,
    renderer_changed: bool,
    already_installed: bool,
    installed_version: Option<String>,
    is_update: bool,
    review_token: String,
    package_sha256: String,
    content_sha256: String,
    executable_files: Vec<String>,
    run_as: String,
}

#[derive(Clone)]
struct PendingWidgetReview {
    directory: PathBuf,
    content_sha256: String,
    created_at_unix: i64,
    preview: WidgetInstallPreview,
}

#[derive(Serialize)]
#[serde(rename_all = "camelCase")]
struct RemoteWidgetDownload {
    source: String,
    advertised_permissions: serde_json::Value,
}

static PENDING_WIDGET_REVIEWS: OnceLock<Mutex<HashMap<String, PendingWidgetReview>>> =
    OnceLock::new();

#[derive(Clone, Deserialize, Serialize)]
#[serde(rename_all = "camelCase")]
struct RemoteAuthor {
    name: String,
    website: Option<String>,
}

#[derive(Deserialize)]
#[serde(rename_all = "camelCase")]
struct RemoteLibraryIndex {
    schema_version: u32,
    widgets: Vec<String>,
}

#[derive(Clone, Deserialize, Serialize)]
#[serde(rename_all = "camelCase")]
struct RemoteWidgetInfo {
    schema_version: u32,
    id: String,
    version: String,
    display_name: String,
    description: String,
    author: RemoteAuthor,
    #[serde(default)]
    category: String,
    #[serde(rename = "package")]
    package_file: String,
    sha256: String,
    #[serde(default)]
    permissions: serde_json::Value,
    #[serde(default = "default_remote_renderer")]
    renderer: String,
    #[serde(default)]
    preview: Option<String>,
    #[serde(skip_deserializing, default)]
    preview_url: Option<String>,
}

fn default_remote_renderer() -> String {
    "declarative".to_owned()
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
        discord_real_time_voice_enabled: Some(false),
        media_dark_mode: Some(true),
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
    let Some(defaults) = system_meter_defaults(&widget.design) else {
        return;
    };
    let version = widget
        .settings
        .get("meterStyleVersion")
        .and_then(|value| value.as_u64());
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

    if let Some(override_dir) = std::env::var_os("TASKBARWIDGETS_INSTALL_DIR") {
        let override_dir = PathBuf::from(override_dir);
        if override_dir.join("TaskbarWidgets.exe").is_file() {
            return Ok(override_dir);
        }
        return Err("TASKBARWIDGETS_INSTALL_DIR does not contain TaskbarWidgets.exe.".to_owned());
    }

    if exe_dir.join("TaskbarWidgets.exe").exists() {
        return Ok(exe_dir);
    }

    // A source-built Settings executable lives below the repository and has no
    // sibling loader. Prefer the real per-user installation so its controls
    // update the same Data/config.json consumed by the running Explorer loader.
    if cfg!(debug_assertions) {
        if let Some(local) = std::env::var_os("LOCALAPPDATA") {
            let installed = PathBuf::from(local).join("Programs").join("TaskbarWidgets");
            if installed.join("TaskbarWidgets.exe").is_file() {
                return Ok(installed);
            }
        }
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

fn community_widgets_dir() -> Result<PathBuf, String> {
    let local = std::env::var_os("LOCALAPPDATA")
        .ok_or_else(|| "LOCALAPPDATA is unavailable.".to_owned())?;
    Ok(PathBuf::from(local)
        .join("TaskbarWidgets")
        .join("CommunityWidgets"))
}

fn is_community_design(id: &str) -> bool {
    id.len() >= 3
        && id.len() <= 128
        && id.contains('.')
        && id.chars().all(|value| {
            value.is_ascii_lowercase() || value.is_ascii_digit() || value == '.' || value == '-'
        })
}

fn is_safe_instance_id(id: &str) -> bool {
    !id.is_empty()
        && id.len() <= 160
        && id.chars().all(|value| {
            value.is_ascii_alphanumeric() || value == '.' || value == '-' || value == '_'
        })
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
    settings.widget_move_x = Some(
        settings
            .widget_move_x
            .unwrap_or_else(|| -(settings.widget_offset_px.unwrap_or(0).min(480) as i32))
            .clamp(-640, 640),
    );
    settings.widgets = Some(normalize_widget_instances(
        settings.widgets,
        &settings.active_design,
        settings.enabled.unwrap_or(true),
        settings.widget_move_x.unwrap_or(0),
    ));
    settings.rotation_interval_secs =
        Some(settings.rotation_interval_secs.unwrap_or(30).clamp(5, 3600));
    settings.refresh_interval_secs =
        Some(settings.refresh_interval_secs.unwrap_or(30).clamp(1, 3600));
    settings
}

fn read_update_status_from(app_dir: &Path) -> UpdateStatus {
    let mut status: UpdateStatus = fs::read_to_string(app_dir.join("update-status.json"))
        .ok()
        .and_then(|data| serde_json::from_str(&data).ok())
        .unwrap_or_default();
    // update-status.json is intentionally preserved across upgrades and can
    // therefore describe the previous loader. The Settings binary version is
    // the authoritative installed version shown in the UI.
    status.current_version = Some(env!("CARGO_PKG_VERSION").to_owned());
    status
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
        .and_then(|data| {
            serde_json::from_str::<SnapshotEnvelope<StorageSourcesSnapshot>>(&data).ok()
        })
        .and_then(|snapshot| {
            if snapshot.schema_version == Some(1)
                && snapshot.widget_id.as_deref() == Some(SYSTEM_STORAGE_DESIGN)
                && snapshot.status.as_deref() == Some("ok")
            {
                snapshot.data
            } else {
                None
            }
        })
        .unwrap_or_default();
    let network = fs::read_to_string(app_dir.join("State").join("system-network.json"))
        .ok()
        .and_then(|data| {
            serde_json::from_str::<SnapshotEnvelope<NetworkSourcesSnapshot>>(&data).ok()
        })
        .and_then(|snapshot| {
            if snapshot.schema_version == Some(1)
                && snapshot.widget_id.as_deref() == Some(SYSTEM_NETWORK_DESIGN)
                && snapshot.status.as_deref() == Some("ok")
            {
                snapshot.data
            } else {
                None
            }
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
        _ if is_community_design(id) => id,
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
        if !known.contains(&widget.design.as_str()) && !is_community_design(&widget.design) {
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

    let mut instance_ids = HashSet::new();
    for (index, widget) in widgets.iter_mut().enumerate() {
        if !is_safe_instance_id(&widget.id) || !instance_ids.insert(widget.id.clone()) {
            let base = if is_safe_instance_id(&widget.design) {
                widget.design.as_str()
            } else {
                "widget"
            };
            let mut suffix = index + 1;
            loop {
                let candidate = format!("{base}-{suffix}");
                suffix += 1;
                if instance_ids.insert(candidate.clone()) {
                    widget.id = candidate;
                    break;
                }
            }
        }
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

fn config_widget_design(widget: &ConfigWidget) -> &str {
    if widget.widget_id.is_empty() {
        &widget.id
    } else {
        &widget.widget_id
    }
}

fn sensitive_setting_key(key: &str) -> bool {
    let key = key.to_ascii_lowercase();
    key.contains("secret")
        || key.contains("token")
        || key.contains("password")
        || key.contains("apikey")
}

fn dpapi_transform(value: &str, protect: bool) -> Option<Vec<u8>> {
    let mut bytes = if protect {
        value.as_bytes().to_vec()
    } else {
        let encoded = value.strip_prefix("dpapi:")?;
        if encoded.len() % 2 != 0 {
            return None;
        }
        (0..encoded.len())
            .step_by(2)
            .map(|index| u8::from_str_radix(&encoded[index..index + 2], 16).ok())
            .collect::<Option<Vec<_>>>()?
    };
    let mut input = CRYPT_INTEGER_BLOB {
        cbData: bytes.len() as u32,
        pbData: bytes.as_mut_ptr(),
    };
    let mut output = CRYPT_INTEGER_BLOB {
        cbData: 0,
        pbData: std::ptr::null_mut(),
    };
    let success = unsafe {
        if protect {
            CryptProtectData(
                &mut input,
                std::ptr::null(),
                std::ptr::null(),
                std::ptr::null_mut(),
                std::ptr::null_mut(),
                CRYPTPROTECT_UI_FORBIDDEN,
                &mut output,
            )
        } else {
            CryptUnprotectData(
                &mut input,
                std::ptr::null_mut(),
                std::ptr::null(),
                std::ptr::null_mut(),
                std::ptr::null_mut(),
                CRYPTPROTECT_UI_FORBIDDEN,
                &mut output,
            )
        }
    };
    if success == 0 || output.pbData.is_null() {
        return None;
    }
    let result =
        unsafe { std::slice::from_raw_parts(output.pbData, output.cbData as usize).to_vec() };
    unsafe {
        LocalFree(output.pbData.cast());
    }
    Some(result)
}

fn protect_secret(value: &str) -> String {
    if value.is_empty() || value.starts_with("dpapi:") {
        return value.to_owned();
    }
    dpapi_transform(value, true)
        .map(|bytes| {
            format!(
                "dpapi:{}",
                bytes
                    .iter()
                    .map(|byte| format!("{byte:02x}"))
                    .collect::<String>()
            )
        })
        .unwrap_or_default()
}

fn unprotect_secret(value: &str) -> String {
    if !value.starts_with("dpapi:") {
        return value.to_owned();
    }
    dpapi_transform(value, false)
        .and_then(|bytes| String::from_utf8(bytes).ok())
        .unwrap_or_default()
}

fn transform_config_secrets(config: &mut ConfigV2, protect: bool) {
    for widget in &mut config.widgets {
        for (key, value) in &mut widget.settings {
            if !sensitive_setting_key(key) {
                continue;
            }
            if let Some(text) = value.as_str() {
                *value = serde_json::Value::String(if protect {
                    protect_secret(text)
                } else {
                    unprotect_secret(text)
                });
            }
        }
    }
}

fn config_to_transport(mut config: ConfigV2) -> WidgetSettings {
    transform_config_secrets(&mut config, false);
    let active = config
        .widgets
        .iter()
        .find(|widget| widget.enabled)
        .map(|widget| {
            if widget.widget_id.is_empty() {
                widget.id.clone()
            } else {
                widget.widget_id.clone()
            }
        })
        .unwrap_or_else(|| CODEX_DESIGN.to_owned());
    let active_widget = config
        .widgets
        .iter()
        .find(|widget| config_widget_design(widget) == active);
    let codex = config
        .widgets
        .iter()
        .find(|widget| config_widget_design(widget) == CODEX_DESIGN);
    let weather = config
        .widgets
        .iter()
        .find(|widget| config_widget_design(widget) == WEATHER_DESIGN);
    let discord = config
        .widgets
        .iter()
        .find(|widget| config_widget_design(widget) == DISCORD_DESIGN);
    let media = config
        .widgets
        .iter()
        .find(|widget| config_widget_design(widget) == MEDIA_DESIGN);

    WidgetSettings {
        active_design: active.clone(),
        enabled: Some(active_widget.map(|widget| widget.enabled).unwrap_or(true)),
        refresh_interval_secs: Some(30),
        widget_offset_px: Some(
            active_widget
                .map(|widget| widget.position.offset_px.unsigned_abs())
                .unwrap_or(0),
        ),
        widget_move_x: Some(
            active_widget
                .map(|widget| widget.position.offset_px)
                .unwrap_or(0),
        ),
        widgets: Some(
            config
                .widgets
                .iter()
                .map(|widget| WidgetInstanceSettings {
                    id: if widget.instance_id.is_empty() {
                        widget.id.clone()
                    } else {
                        widget.instance_id.clone()
                    },
                    design: if widget.widget_id.is_empty() {
                        widget.id.clone()
                    } else {
                        widget.widget_id.clone()
                    },
                    enabled: widget.enabled,
                    move_x: widget.position.offset_px,
                    position_pct: Some(widget.position.anchor_percent),
                    order: widget.order,
                    settings: widget.settings.clone(),
                })
                .collect(),
        ),
        rotation_enabled: Some(config.layout.mode == "rotation"),
        rotation_interval_secs: Some(config.rotation.interval_seconds),
        rotation_designs: Some(if config.rotation.widget_ids.is_empty() {
            config
                .rotation
                .instance_ids
                .iter()
                .filter_map(|instance_id| {
                    config
                        .widgets
                        .iter()
                        .find(|widget| &widget.instance_id == instance_id)
                        .map(|widget| {
                            if widget.widget_id.is_empty() {
                                widget.id.clone()
                            } else {
                                widget.widget_id.clone()
                            }
                        })
                })
                .collect()
        } else {
            config.rotation.widget_ids
        }),
        codex_api_endpoint: Some(setting_string(codex, "apiEndpoint", "")),
        codex_project_filter: Some(setting_string(codex, "projectFilter", "")),
        weather_city: Some(setting_string(weather, "city", "Istanbul")),
        weather_temp_unit: Some(setting_string(weather, "temperatureUnit", "C")),
        discord_enabled: Some(discord.map(|widget| widget.enabled).unwrap_or(false)),
        discord_background_enabled: Some(setting_bool(discord, "backgroundEnabled", true)),
        discord_real_time_voice_enabled: Some(setting_bool(
            discord,
            "realTimeVoiceEnabled",
            false,
        )),
        media_dark_mode: Some(setting_bool(media, "darkMode", true)),
    }
}

fn string_setting(
    map: &mut serde_json::Map<String, serde_json::Value>,
    key: &str,
    value: Option<&String>,
) {
    map.insert(
        key.to_owned(),
        serde_json::Value::String(value.cloned().unwrap_or_default()),
    );
}

fn transport_to_config(settings: &WidgetSettings) -> ConfigV2 {
    let widgets = settings
        .widgets
        .clone()
        .unwrap_or_else(default_widget_instances);
    let config_widgets = widgets
        .into_iter()
        .map(|widget| {
            let mut values = widget.settings.clone();
            match widget.design.as_str() {
                CODEX_DESIGN => {
                    string_setting(
                        &mut values,
                        "apiEndpoint",
                        settings.codex_api_endpoint.as_ref(),
                    );
                    string_setting(
                        &mut values,
                        "projectFilter",
                        settings.codex_project_filter.as_ref(),
                    );
                }
                WEATHER_DESIGN => {
                    string_setting(&mut values, "city", settings.weather_city.as_ref());
                    string_setting(
                        &mut values,
                        "temperatureUnit",
                        settings.weather_temp_unit.as_ref(),
                    );
                }
                DISCORD_DESIGN => {
                    values.remove("clientId");
                    values.remove("clientSecret");
                    values.remove("redirectUri");
                    values.insert(
                        "backgroundEnabled".to_owned(),
                        serde_json::Value::Bool(
                            settings.discord_background_enabled.unwrap_or(true),
                        ),
                    );
                    values.insert(
                        "realTimeVoiceEnabled".to_owned(),
                        serde_json::Value::Bool(
                            settings.discord_real_time_voice_enabled.unwrap_or(false),
                        ),
                    );
                }
                MEDIA_DESIGN => {
                    values.insert(
                        "darkMode".to_owned(),
                        serde_json::Value::Bool(settings.media_dark_mode.unwrap_or(true)),
                    );
                }
                _ => {}
            }
            ConfigWidget {
                id: widget.id.clone(),
                instance_id: widget.id,
                widget_id: widget.design,
                enabled: widget.enabled,
                order: widget.order,
                position: WidgetPosition {
                    anchor_percent: widget.position_pct.unwrap_or(100).clamp(0, 100),
                    offset_px: widget.move_x.clamp(-640, 640),
                },
                settings: values,
            }
        })
        .collect();

    ConfigV2 {
        config_version: 3,
        layout: LayoutConfig {
            mode: if settings.rotation_enabled.unwrap_or(false) {
                "rotation"
            } else {
                "row"
            }
            .to_owned(),
        },
        widgets: config_widgets,
        rotation: RotationConfig {
            interval_seconds: settings.rotation_interval_secs.unwrap_or(30).clamp(5, 3600),
            widget_ids: normalize_rotation_designs(settings.rotation_designs.clone()),
            instance_ids: settings
                .widgets
                .as_ref()
                .map(|widgets| {
                    widgets
                        .iter()
                        .filter(|widget| widget.enabled)
                        .map(|widget| widget.id.clone())
                        .collect()
                })
                .unwrap_or_default(),
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
        widget_catalog: fs::read_to_string(dir.join("Runtime").join("WidgetCatalog.json"))
            .ok()
            .and_then(|json| serde_json::from_str(&json).ok())
            .unwrap_or_else(|| serde_json::json!({ "schemaVersion": 2, "widgets": [] })),
        community_widgets_dir: community_widgets_dir()?.display().to_string(),
        community_update_state: fs::read_to_string(
            dir.join("Runtime").join("community-widget-updates.json"),
        )
        .ok()
        .and_then(|json| serde_json::from_str(&json).ok())
        .unwrap_or_else(
            || serde_json::json!({ "schemaVersion": 1, "status": "idle", "updates": [] }),
        ),
        web_render_health: fs::read_to_string(dir.join("Runtime").join("web-render-host.json"))
            .ok()
            .and_then(|json| serde_json::from_str(&json).ok())
            .unwrap_or_else(|| {
                serde_json::json!({
                    "schemaVersion": 1,
                    "status": "stopped",
                    "error": null
                })
            }),
        voice_helper_installed: voice_helper_installed(),
    })
}

fn voice_helper_installed() -> bool {
    Command::new("sc.exe")
        .args(["query", VOICE_HELPER_SERVICE])
        .creation_flags(CREATE_NO_WINDOW)
        .status()
        .map(|status| status.success())
        .unwrap_or(false)
}

fn run_elevated(file: &Path, argument: &str) -> Result<(), String> {
    if !file.is_file() {
        return Err(format!("Voice helper was not found: {}", file.display()));
    }
    let verb = std::ffi::OsStr::new("runas")
        .encode_wide()
        .chain(std::iter::once(0))
        .collect::<Vec<_>>();
    let file = file
        .as_os_str()
        .encode_wide()
        .chain(std::iter::once(0))
        .collect::<Vec<_>>();
    let arguments = std::ffi::OsStr::new(argument)
        .encode_wide()
        .chain(std::iter::once(0))
        .collect::<Vec<_>>();
    let result = unsafe {
        ShellExecuteW(
            std::ptr::null_mut(),
            verb.as_ptr(),
            file.as_ptr(),
            arguments.as_ptr(),
            std::ptr::null(),
            0,
        )
    };
    if result <= 32 {
        Err(format!("Windows could not launch the helper (code {result})."))
    } else {
        Ok(())
    }
}

#[tauri::command]
fn install_voice_helper() -> Result<(), String> {
    let helper = install_dir()?.join("TaskbarWidgets.VoiceCapture.exe");
    run_elevated(&helper, "--install")
}

#[tauri::command]
fn uninstall_voice_helper() -> Result<(), String> {
    let helper = install_dir()?.join("TaskbarWidgets.VoiceCapture.exe");
    run_elevated(&helper, "--uninstall")
}

#[link(name = "shell32")]
extern "system" {
    fn ShellExecuteW(
        hwnd: *mut std::ffi::c_void,
        operation: *const u16,
        file: *const u16,
        parameters: *const u16,
        directory: *const u16,
        show_command: i32,
    ) -> isize;
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
    normalized.rotation_interval_secs = Some(
        normalized
            .rotation_interval_secs
            .unwrap_or(30)
            .clamp(5, 3600),
    );

    let mut config = transport_to_config(&normalized);
    transform_config_secrets(&mut config, true);
    let json = serde_json::to_string_pretty(&config).map_err(|e| e.to_string())?;
    let path = dir.join("config.json");
    let temp = dir.join("config.json.tmp");
    fs::write(&temp, format!("{json}\n")).map_err(|e| e.to_string())?;
    replace_file_atomic(&temp, &path)
}

#[tauri::command]
fn consume_settings_open_request() -> Result<Option<String>, String> {
    let path = data_dir(&install_dir()?)
        .join("Runtime")
        .join("settings-open-request.json");
    if !path.exists() {
        return Ok(None);
    }
    let request = fs::read_to_string(&path)
        .map_err(|e| e.to_string())
        .and_then(|json| {
            serde_json::from_str::<SettingsOpenRequest>(&json).map_err(|e| e.to_string())
        });
    let _ = fs::remove_file(&path);
    let request = request?;
    let known = [
        SYSTEM_CPU_DESIGN,
        SYSTEM_STORAGE_DESIGN,
        SYSTEM_NETWORK_DESIGN,
        SYSTEM_MEMORY_DESIGN,
    ];
    let runtime_known = fs::read_to_string(
        data_dir(&install_dir()?)
            .join("Runtime")
            .join("WidgetCatalog.json"),
    )
    .ok()
    .and_then(|json| serde_json::from_str::<serde_json::Value>(&json).ok())
    .and_then(|catalog| {
        catalog
            .get("widgets")
            .and_then(|widgets| widgets.as_array())
            .cloned()
    })
    .map(|widgets| {
        widgets.iter().any(|widget| {
            widget.get("valid").and_then(|value| value.as_bool()) == Some(true)
                && widget.get("id").and_then(|value| value.as_str())
                    == Some(request.widget_id.as_str())
        })
    })
    .unwrap_or(false);
    let now = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map(|value| value.as_secs() as i64)
        .unwrap_or_default();
    if request.schema_version != 1
        || request.request_id.is_empty()
        || (!known.contains(&request.widget_id.as_str()) && !runtime_known)
        || request.created_at_unix <= 0
        || (now - request.created_at_unix).abs() > 300
    {
        return Ok(None);
    }
    Ok(Some(request.widget_id))
}

#[tauri::command]
fn consume_widget_install_request() -> Result<Option<String>, String> {
    let path = data_dir(&install_dir()?)
        .join("Runtime")
        .join("widget-install-request.json");
    if !path.exists() {
        return Ok(None);
    }
    let request = fs::read_to_string(&path)
        .map_err(|e| e.to_string())
        .and_then(|json| {
            serde_json::from_str::<WidgetInstallRequest>(&json).map_err(|e| e.to_string())
        });
    let _ = fs::remove_file(&path);
    let request = request?;
    let now = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|value| value.as_secs() as i64)
        .unwrap_or_default();
    let source = PathBuf::from(&request.source);
    if request.schema_version != 1
        || request.request_id.is_empty()
        || request.created_at_unix <= 0
        || (now - request.created_at_unix).abs() > 300
        || !source.is_file()
        || !source
            .extension()
            .and_then(|value| value.to_str())
            .is_some_and(|value| value.eq_ignore_ascii_case("twidget"))
    {
        return Ok(None);
    }
    Ok(Some(source.display().to_string()))
}

#[tauri::command]
fn open_widget_libraries() -> Result<(), String> {
    let dir = community_widgets_dir()?;
    fs::create_dir_all(&dir).map_err(|e| e.to_string())?;

    Command::new("explorer")
        .arg(&dir)
        .spawn()
        .map(|_| ())
        .map_err(|e| e.to_string())
}

fn safe_widget_id_from_manifest(directory: &Path) -> Result<String, String> {
    let manifest: serde_json::Value = serde_json::from_str(
        &fs::read_to_string(directory.join("widget.json")).map_err(|e| e.to_string())?,
    )
    .map_err(|e| format!("widget.json: {e}"))?;
    let id = manifest
        .get("id")
        .and_then(|value| value.as_str())
        .ok_or_else(|| "widget.json id is missing.".to_owned())?;
    if !is_community_design(id) {
        return Err("Widget id must use reverse-domain format.".to_owned());
    }
    Ok(id.to_owned())
}

fn known_v4_permission(id: &str) -> bool {
    matches!(
        id,
        "accounts.list.read"
            | "accounts.profile.read"
            | "accounts.history.read"
            | "accounts.tokens.read"
            | "accounts.active.write"
            | "accounts.delete"
            | "filesystem.read"
            | "filesystem.write"
            | "filesystem.delete"
            | "filesystem.watch"
            | "filesystem.all"
            | "registry.read"
            | "registry.write"
            | "registry.delete"
            | "registry.all"
            | "process.list"
            | "process.start"
            | "process.stop"
            | "process.control"
            | "process.inject"
            | "shell.execute"
            | "shell.openExternal"
            | "network.internet"
            | "network.local"
            | "network.listen"
            | "network.unrestricted"
            | "windows.win32"
            | "windows.winrt"
            | "windows.com"
            | "windows.wmi"
            | "clipboard.read"
            | "clipboard.write"
            | "notifications.show"
            | "camera"
            | "microphone"
            | "location"
            | "bluetooth"
            | "usb"
            | "media.sessions.read"
            | "media.playback.control"
            | "steam.downloads.read"
            | "steam.client.control"
            | "discord.state.read"
            | "system.metrics.read"
            | "taskbar.control"
            | "settings.read"
            | "settings.write"
            | "system.fullAccess"
            | "system.administrator"
            | "system.startup"
            | "system.background"
    )
}

fn validate_v4_permissions(permissions: &serde_json::Value) -> Result<(), String> {
    let object = permissions
        .as_object()
        .ok_or_else(|| "schema v4 permissions must be an object.".to_owned())?;
    if object
        .keys()
        .any(|key| key != "required" && key != "optional")
    {
        return Err("schema v4 permissions supports only required and optional.".to_owned());
    }
    let required = object
        .get("required")
        .and_then(serde_json::Value::as_array)
        .ok_or_else(|| "schema v4 permissions.required must be an array.".to_owned())?;
    let optional = object
        .get("optional")
        .map(|value| {
            value
                .as_array()
                .ok_or_else(|| "schema v4 permissions.optional must be an array.".to_owned())
        })
        .transpose()?
        .cloned()
        .unwrap_or_default();
    let mut seen = HashSet::new();
    for (is_optional, values) in [(false, required.as_slice()), (true, optional.as_slice())] {
        if values.len() > 64 {
            return Err("A widget may request at most 64 permissions per list.".to_owned());
        }
        for value in values {
            let request = value
                .as_object()
                .ok_or_else(|| "Every permission request must be an object.".to_owned())?;
            if request
                .keys()
                .any(|key| key != "id" && key != "scope" && key != "reason")
            {
                return Err("A permission request contains an unsupported property.".to_owned());
            }
            let id = request
                .get("id")
                .and_then(serde_json::Value::as_str)
                .ok_or_else(|| "Permission id is required.".to_owned())?;
            if !known_v4_permission(id) {
                return Err(format!("Unknown permission id '{id}'."));
            }
            if !seen.insert(id.to_owned()) {
                return Err(format!("Permission '{id}' is requested more than once."));
            }
            let reason = request
                .get("reason")
                .and_then(serde_json::Value::as_str)
                .map(str::trim)
                .unwrap_or_default();
            if !(3..=300).contains(&reason.len()) {
                return Err(format!(
                    "Permission '{id}' reason must be between 3 and 300 characters."
                ));
            }
            if let Some(scope) = request.get("scope") {
                let valid = scope
                    .as_str()
                    .is_some_and(|text| !text.is_empty() && text.len() <= 1024)
                    || scope.as_array().is_some_and(|items| {
                        items.len() <= 64
                            && items.iter().all(|item| {
                                item.as_str()
                                    .is_some_and(|text| !text.is_empty() && text.len() <= 1024)
                            })
                    });
                if !valid {
                    return Err(format!("Permission '{id}' has an invalid scope."));
                }
            }
            let _ = is_optional;
        }
    }
    Ok(())
}

fn v4_has_required_permission(permissions: &serde_json::Value, id: &str) -> bool {
    permissions
        .get("required")
        .and_then(serde_json::Value::as_array)
        .is_some_and(|values| {
            values
                .iter()
                .any(|value| value.get("id").and_then(serde_json::Value::as_str) == Some(id))
        })
}

fn hash_file(path: &Path) -> Result<String, String> {
    let bytes = fs::read(path).map_err(|e| e.to_string())?;
    Ok(format!("{:x}", Sha256::digest(bytes)))
}

fn hash_widget_tree(directory: &Path) -> Result<String, String> {
    fn collect(root: &Path, current: &Path, files: &mut Vec<PathBuf>) -> Result<(), String> {
        for entry in fs::read_dir(current).map_err(|e| e.to_string())? {
            let entry = entry.map_err(|e| e.to_string())?;
            let file_type = entry.file_type().map_err(|e| e.to_string())?;
            if file_type.is_symlink() {
                return Err("Symbolic links are not allowed.".to_owned());
            }
            if file_type.is_dir() {
                collect(root, &entry.path(), files)?;
            } else if file_type.is_file() {
                files.push(
                    entry
                        .path()
                        .strip_prefix(root)
                        .map_err(|e| e.to_string())?
                        .to_path_buf(),
                );
            }
        }
        Ok(())
    }

    let mut files = Vec::new();
    collect(directory, directory, &mut files)?;
    files.sort();
    let mut digest = Sha256::new();
    for relative in files {
        digest.update(relative.to_string_lossy().replace('\\', "/").as_bytes());
        digest.update([0]);
        digest.update(fs::read(directory.join(&relative)).map_err(|e| e.to_string())?);
        digest.update([0xff]);
    }
    Ok(format!("{:x}", digest.finalize()))
}

fn executable_inventory(directory: &Path) -> Result<Vec<String>, String> {
    fn visit(root: &Path, current: &Path, values: &mut Vec<String>) -> Result<(), String> {
        for entry in fs::read_dir(current).map_err(|e| e.to_string())? {
            let entry = entry.map_err(|e| e.to_string())?;
            let file_type = entry.file_type().map_err(|e| e.to_string())?;
            if file_type.is_dir() {
                visit(root, &entry.path(), values)?;
            } else if file_type.is_file()
                && entry
                    .path()
                    .extension()
                    .and_then(|value| value.to_str())
                    .is_some_and(|extension| {
                        matches!(
                            extension.to_ascii_lowercase().as_str(),
                            "exe" | "dll" | "ps1" | "cmd" | "bat" | "py" | "js"
                        )
                    })
            {
                values.push(
                    entry
                        .path()
                        .strip_prefix(root)
                        .map_err(|e| e.to_string())?
                        .to_string_lossy()
                        .replace('\\', "/"),
                );
            }
        }
        Ok(())
    }
    let mut values = Vec::new();
    visit(directory, directory, &mut values)?;
    values.sort();
    Ok(values)
}

fn install_preview_from_directory(
    directory: &Path,
    source: &Path,
) -> Result<WidgetInstallPreview, String> {
    let manifest: serde_json::Value = serde_json::from_str(
        &fs::read_to_string(directory.join("widget.json")).map_err(|e| e.to_string())?,
    )
    .map_err(|e| format!("widget.json: {e}"))?;
    let schema_version = manifest
        .get("schemaVersion")
        .and_then(|value| value.as_u64())
        .unwrap_or_default();
    if !matches!(schema_version, 2 | 3 | 4) {
        return Err("Only widget schemaVersion 2, 3 or 4 packages are supported.".to_owned());
    }
    let id = safe_widget_id_from_manifest(directory)?;
    let required = |key: &str| {
        manifest
            .get(key)
            .and_then(|value| value.as_str())
            .filter(|value| !value.trim().is_empty())
            .map(str::to_owned)
            .ok_or_else(|| format!("widget.json {key} is missing."))
    };
    let author = manifest
        .get("author")
        .and_then(|value| value.as_object())
        .ok_or_else(|| "widget.json author is required.".to_owned())?;
    let author_name = author
        .get("name")
        .and_then(|value| value.as_str())
        .filter(|value| !value.trim().is_empty())
        .ok_or_else(|| "widget.json author.name is required.".to_owned())?
        .to_owned();
    let author_website = author
        .get("website")
        .and_then(|value| value.as_str())
        .map(str::to_owned);
    if author_website
        .as_deref()
        .is_some_and(|value| !value.starts_with("https://"))
    {
        return Err("author.website must use HTTPS.".to_owned());
    }
    let permissions = manifest
        .get("permissions")
        .cloned()
        .unwrap_or_else(|| serde_json::json!({}));
    if !permissions.is_object() {
        return Err("widget.json permissions must be an object.".to_owned());
    }
    if schema_version == 4 {
        validate_v4_permissions(&permissions)?;
    }
    let renderer_type = if matches!(schema_version, 3 | 4) {
        let renderer = manifest
            .get("renderer")
            .and_then(|value| value.as_object())
            .ok_or_else(|| {
                format!("schemaVersion {schema_version} requires widget.json renderer.")
            })?;
        let renderer_type = renderer
            .get("type")
            .and_then(|value| value.as_str())
            .ok_or_else(|| "renderer.type is required.".to_owned())?;
        if schema_version == 3 && renderer_type != "web" {
            return Err("schemaVersion 3 currently supports only renderer.type web.".to_owned());
        }
        if schema_version == 4 && !matches!(renderer_type, "web" | "declarative" | "native") {
            return Err(
                "schemaVersion 4 renderer.type must be web, declarative, or native."
                    .to_owned(),
            );
        }
        let renderer_entry = renderer
            .get("entry")
            .and_then(|value| value.as_str())
            .ok_or_else(|| "renderer.entry is required.".to_owned())?;
        let entry_path = Path::new(renderer_entry);
        let expected_extension = if renderer_type == "web" {
            ".html"
        } else {
            ".json"
        };
        if entry_path.is_absolute()
            || renderer_entry.contains("..")
            || !renderer_entry
                .to_ascii_lowercase()
                .ends_with(expected_extension)
            || !directory.join(entry_path).is_file()
        {
            return Err(format!(
                "renderer.entry must be a contained {expected_extension} file."
            ));
        }
        if matches!(renderer_type, "web" | "native") {
            if let Some(expanded) = renderer
                .get("expandedSize")
                .and_then(|value| value.as_object())
            {
                let width = expanded
                    .get("width")
                    .and_then(|value| value.as_u64())
                    .unwrap_or_default();
                let height = expanded
                    .get("height")
                    .and_then(|value| value.as_u64())
                    .unwrap_or_default();
                if !(32..=640).contains(&width) || !(24..=480).contains(&height) {
                    return Err("renderer.expandedSize is outside the supported range.".to_owned());
                }
            }
        }
        if renderer_type == "native" {
            if let Some(expanded_entry) =
                renderer.get("expandedEntry").and_then(|value| value.as_str())
            {
                let expanded_path = Path::new(expanded_entry);
                if expanded_path.is_absolute()
                    || expanded_entry.contains("..")
                    || !expanded_entry.to_ascii_lowercase().ends_with(".json")
                    || !directory.join(expanded_path).is_file()
                {
                    return Err(
                        "renderer.expandedEntry must be a contained .json file.".to_owned(),
                    );
                }
            }
        }
        renderer_type.to_owned()
    } else {
        "declarative".to_owned()
    };
    let runtime = manifest.get("runtime").and_then(|value| value.as_object());
    let run_as = runtime
        .and_then(|value| value.get("runAs"))
        .and_then(|value| value.as_str())
        .unwrap_or("user")
        .to_owned();
    if let Some(runtime) = runtime {
        if schema_version != 4 {
            return Err("A process runtime requires schemaVersion 4.".to_owned());
        }
        if runtime.get("type").and_then(|value| value.as_str()) != Some("process") {
            return Err("runtime.type must be process.".to_owned());
        }
        let runtime_entry = runtime
            .get("entry")
            .and_then(|value| value.as_str())
            .ok_or_else(|| "runtime.entry is required.".to_owned())?;
        if Path::new(runtime_entry).is_absolute()
            || runtime_entry.contains("..")
            || !directory.join(runtime_entry).is_file()
        {
            return Err("runtime.entry must be a contained package file.".to_owned());
        }
        if !v4_has_required_permission(&permissions, "system.fullAccess") {
            return Err(
                "A process runtime must request required permission system.fullAccess.".to_owned(),
            );
        }
        if permissions
            .get("optional")
            .and_then(serde_json::Value::as_array)
            .is_some_and(|values| !values.is_empty())
        {
            return Err(
                "A full-access process cannot use optional permissions because they cannot be technically restricted."
                    .to_owned(),
            );
        }
        if run_as == "administrator"
            && !v4_has_required_permission(&permissions, "system.administrator")
        {
            return Err(
                "An administrator runtime must request required permission system.administrator."
                    .to_owned(),
            );
        }
        if !matches!(run_as.as_str(), "user" | "administrator") {
            return Err("runtime.runAs must be user or administrator.".to_owned());
        }
        let protocol = runtime
            .get("protocol")
            .and_then(|value| value.as_str())
            .unwrap_or("json-lines-v1");
        let lifetime = runtime
            .get("lifetime")
            .and_then(|value| value.as_str())
            .unwrap_or("persistent");
        let working_directory = runtime
            .get("workingDirectory")
            .and_then(|value| value.as_str())
            .unwrap_or("package");
        if !matches!(protocol, "none" | "json-lines-v1")
            || lifetime != "persistent"
            || !matches!(working_directory, "package" | "data")
        {
            return Err("runtime contains an unsupported option.".to_owned());
        }
        if run_as == "administrator" && protocol != "none" {
            return Err("Administrator runtimes currently require protocol none.".to_owned());
        }
        if let Some(arguments) = runtime.get("arguments") {
            let arguments = arguments
                .as_array()
                .ok_or_else(|| "runtime.arguments must be an array.".to_owned())?;
            if arguments.len() > 64
                || arguments
                    .iter()
                    .any(|value| value.as_str().is_none_or(|argument| argument.len() > 1024))
            {
                return Err("runtime.arguments exceeds the supported limits.".to_owned());
            }
        }
    }
    let provider_type = runtime
        .map(|_| "process")
        .or_else(|| {
            manifest
                .pointer("/entry/provider/type")
                .and_then(|value| value.as_str())
        })
        .unwrap_or("none")
        .to_owned();
    let version = required("version")?;
    let installed_manifest = community_widgets_dir()?.join(&id).join("widget.json");
    let installed_json = fs::read_to_string(installed_manifest)
        .ok()
        .and_then(|json| serde_json::from_str::<serde_json::Value>(&json).ok());
    let installed_version = installed_json.as_ref().and_then(|manifest| {
        manifest
            .get("version")
            .and_then(|value| value.as_str())
            .map(str::to_owned)
    });
    let installed_renderer = installed_json.as_ref().map(|manifest| {
        match manifest
            .get("schemaVersion")
            .and_then(|value| value.as_u64())
        {
            Some(3 | 4) => manifest
                .pointer("/renderer/type")
                .and_then(|value| value.as_str())
                .unwrap_or("declarative"),
            _ => "declarative",
        }
    });
    let is_update = installed_version.as_deref().is_some_and(|installed| {
        compare_widget_versions(&version, installed).is_some_and(|ordering| ordering.is_gt())
    });
    Ok(WidgetInstallPreview {
        source: source.display().to_string(),
        schema_version,
        id: id.clone(),
        version,
        display_name: required("displayName")?,
        description: required("description")?,
        author_name,
        author_website,
        permissions,
        provider_type,
        renderer_changed: installed_renderer.is_some_and(|value| value != renderer_type.as_str()),
        renderer_type,
        already_installed: community_widgets_dir()?.join(id).exists(),
        installed_version,
        is_update,
        review_token: String::new(),
        package_sha256: String::new(),
        content_sha256: String::new(),
        executable_files: executable_inventory(directory)?,
        run_as,
    })
}

fn compare_widget_versions(left: &str, right: &str) -> Option<std::cmp::Ordering> {
    fn parts(value: &str) -> Option<Vec<u64>> {
        let values = value
            .split('.')
            .map(str::parse::<u64>)
            .collect::<Result<Vec<_>, _>>()
            .ok()?;
        if matches!(values.len(), 3 | 4) {
            Some(values)
        } else {
            None
        }
    }
    let mut left = parts(left)?;
    let mut right = parts(right)?;
    left.resize(4, 0);
    right.resize(4, 0);
    Some(left.cmp(&right))
}

fn unique_staging_directory(prefix: &str) -> PathBuf {
    let ticks = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|value| value.as_nanos())
        .unwrap_or_default();
    std::env::temp_dir().join(format!(
        "TaskbarWidgets-{prefix}-{}-{ticks}",
        std::process::id()
    ))
}

fn extract_widget_source(source: &Path, staging: &Path) -> Result<(), String> {
    if staging.exists() {
        fs::remove_dir_all(staging).map_err(|e| e.to_string())?;
    }
    fs::create_dir_all(staging).map_err(|e| e.to_string())?;
    if source.is_dir() {
        let mut files = 0usize;
        let mut bytes = 0u64;
        copy_widget_directory(source, staging, &mut files, &mut bytes)?;
        return Ok(());
    }
    if !source
        .extension()
        .and_then(|value| value.to_str())
        .is_some_and(|value| value.eq_ignore_ascii_case("twidget"))
    {
        return Err("Choose a widget folder or .twidget package.".to_owned());
    }
    let file = fs::File::open(source).map_err(|e| e.to_string())?;
    let mut archive = zip::ZipArchive::new(file).map_err(|e| e.to_string())?;
    if archive.len() > MAX_WIDGET_PACKAGE_FILES {
        return Err(format!(
            "Package contains more than {MAX_WIDGET_PACKAGE_FILES} files."
        ));
    }
    let mut total = 0u64;
    for index in 0..archive.len() {
        let mut entry = archive.by_index(index).map_err(|e| e.to_string())?;
        if entry
            .unix_mode()
            .map(|mode| mode & 0o170000 == 0o120000)
            .unwrap_or(false)
        {
            return Err("Package contains a symbolic link.".to_owned());
        }
        let relative = entry
            .enclosed_name()
            .ok_or_else(|| "Package contains an unsafe path.".to_owned())?;
        total = total.saturating_add(entry.size());
        if total > MAX_WIDGET_PACKAGE_BYTES as u64 {
            return Err("Package exceeds 250 MB.".to_owned());
        }
        let destination = staging.join(relative);
        if entry.is_dir() {
            fs::create_dir_all(&destination).map_err(|e| e.to_string())?;
        } else {
            if let Some(parent) = destination.parent() {
                fs::create_dir_all(parent).map_err(|e| e.to_string())?;
            }
            let mut output = fs::File::create(destination).map_err(|e| e.to_string())?;
            std::io::copy(&mut entry, &mut output).map_err(|e| e.to_string())?;
        }
    }
    Ok(())
}

fn pending_widget_reviews() -> &'static Mutex<HashMap<String, PendingWidgetReview>> {
    PENDING_WIDGET_REVIEWS.get_or_init(|| Mutex::new(HashMap::new()))
}

fn current_unix() -> i64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|value| value.as_secs() as i64)
        .unwrap_or_default()
}

fn cleanup_expired_reviews(reviews: &mut HashMap<String, PendingWidgetReview>) {
    let now = current_unix();
    let expired = reviews
        .iter()
        .filter(|(_, review)| now - review.created_at_unix > REVIEW_LIFETIME_SECONDS)
        .map(|(token, review)| (token.clone(), review.directory.clone()))
        .collect::<Vec<_>>();
    for (token, directory) in expired {
        reviews.remove(&token);
        if directory.exists() {
            let _ = fs::remove_dir_all(directory);
        }
    }
}

#[tauri::command]
fn inspect_community_widget(
    source: String,
    advertised_permissions: Option<serde_json::Value>,
) -> Result<WidgetInstallPreview, String> {
    let source = PathBuf::from(source);
    if !source.exists() {
        return Err("The selected widget package no longer exists.".to_owned());
    }
    let staging = unique_staging_directory("review");
    let result = (|| {
        extract_widget_source(&source, &staging)?;
        let mut preview = install_preview_from_directory(&staging, &source)?;
        if let Some(advertised) = advertised_permissions {
            if advertised != preview.permissions {
                return Err(
                    "The package permissions do not match the permissions advertised by the remote library."
                        .to_owned(),
                );
            }
        }
        let content_sha256 = hash_widget_tree(&staging)?;
        let package_sha256 = if source.is_file() {
            hash_file(&source)?
        } else {
            content_sha256.clone()
        };
        let nonce = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .map(|value| value.as_nanos())
            .unwrap_or_default();
        let review_token = format!(
            "{:x}",
            Sha256::digest(format!(
                "{}:{}:{}:{nonce}",
                content_sha256,
                source.display(),
                std::process::id()
            ))
        );
        preview.review_token = review_token.clone();
        preview.package_sha256 = package_sha256;
        preview.content_sha256 = content_sha256.clone();
        let pending = PendingWidgetReview {
            directory: staging.clone(),
            content_sha256,
            created_at_unix: current_unix(),
            preview: preview.clone(),
        };
        let mut reviews = pending_widget_reviews()
            .lock()
            .map_err(|_| "Widget review store is unavailable.".to_owned())?;
        cleanup_expired_reviews(&mut reviews);
        reviews.insert(review_token, pending);
        Ok(preview)
    })();
    if result.is_err() && staging.exists() {
        let _ = fs::remove_dir_all(staging);
    }
    result
}

fn copy_widget_directory(
    source: &Path,
    target: &Path,
    files: &mut usize,
    bytes: &mut u64,
) -> Result<(), String> {
    fs::create_dir_all(target).map_err(|e| e.to_string())?;
    for entry in fs::read_dir(source).map_err(|e| e.to_string())? {
        let entry = entry.map_err(|e| e.to_string())?;
        let file_type = entry.file_type().map_err(|e| e.to_string())?;
        if file_type.is_symlink() {
            return Err("Symbolic links are not allowed.".to_owned());
        }
        let destination = target.join(entry.file_name());
        if file_type.is_dir() {
            copy_widget_directory(&entry.path(), &destination, files, bytes)?;
        } else if file_type.is_file() {
            *files += 1;
            *bytes += entry.metadata().map_err(|e| e.to_string())?.len();
            if *files > MAX_WIDGET_PACKAGE_FILES || *bytes > MAX_WIDGET_PACKAGE_BYTES as u64 {
                return Err("Package exceeds 5000 files or 250 MB.".to_owned());
            }
            fs::copy(entry.path(), destination).map_err(|e| e.to_string())?;
        }
    }
    Ok(())
}

fn widget_approvals_dir() -> Result<PathBuf, String> {
    community_widgets_dir()?
        .parent()
        .map(|path| path.join("WidgetApprovals"))
        .ok_or_else(|| "Community data directory is unavailable.".to_owned())
}

fn write_widget_approval(
    preview: &WidgetInstallPreview,
    granted_optional_permissions: &[String],
) -> Result<(), String> {
    let optional_ids = preview
        .permissions
        .get("optional")
        .and_then(serde_json::Value::as_array)
        .map(|values| {
            values
                .iter()
                .filter_map(|value| value.get("id").and_then(serde_json::Value::as_str))
                .collect::<HashSet<_>>()
        })
        .unwrap_or_default();
    if granted_optional_permissions
        .iter()
        .any(|id| !optional_ids.contains(id.as_str()))
    {
        return Err(
            "The optional permission grant does not match the reviewed manifest.".to_owned(),
        );
    }
    let directory = widget_approvals_dir()?;
    fs::create_dir_all(&directory).map_err(|e| e.to_string())?;
    let path = directory.join(format!("{}.json", preview.id));
    let temporary = path.with_extension("json.tmp");
    fs::write(
        &temporary,
        serde_json::to_vec_pretty(&serde_json::json!({
            "schemaVersion": 1,
            "widgetId": preview.id,
            "version": preview.version,
            "packageSha256": preview.package_sha256,
            "contentSha256": preview.content_sha256,
            "permissions": preview.permissions,
            "grantedOptional": granted_optional_permissions,
            "approvedAtUnix": current_unix()
        }))
        .map_err(|e| e.to_string())?,
    )
    .map_err(|e| e.to_string())?;
    replace_file_atomic(&temporary, &path)
}

#[tauri::command]
fn install_community_widget(
    review_token: String,
    granted_optional_permissions: Vec<String>,
    replace_existing: bool,
) -> Result<String, String> {
    let pending = {
        let mut reviews = pending_widget_reviews()
            .lock()
            .map_err(|_| "Widget review store is unavailable.".to_owned())?;
        cleanup_expired_reviews(&mut reviews);
        reviews
            .remove(&review_token)
            .ok_or_else(|| "The permission review expired. Inspect the package again.".to_owned())?
    };
    let staging = pending.directory.clone();
    let result = (|| {
        if current_unix() - pending.created_at_unix > REVIEW_LIFETIME_SECONDS {
            return Err("The permission review expired. Inspect the package again.".to_owned());
        }
        if hash_widget_tree(&staging)? != pending.content_sha256 {
            return Err("The reviewed package changed before installation.".to_owned());
        }
        let preview = install_preview_from_directory(&staging, Path::new(&pending.preview.source))?;
        if preview.id != pending.preview.id
            || preview.version != pending.preview.version
            || preview.permissions != pending.preview.permissions
        {
            return Err("The reviewed manifest changed before installation.".to_owned());
        }
        let mut approved_preview = pending.preview.clone();
        approved_preview.executable_files = preview.executable_files;
        let id = approved_preview.id.clone();
        let root = community_widgets_dir()?;
        let target = root.join(&id);
        if target.exists() {
            if !replace_existing {
                return Err(format!("{id} is already installed."));
            }
            if !approved_preview.is_update {
                return Err(format!(
                    "The package version {} is not newer than the installed version {}.",
                    approved_preview.version,
                    approved_preview
                        .installed_version
                        .as_deref()
                        .unwrap_or("unknown")
                ));
            }
            let backups = root
                .parent()
                .ok_or_else(|| "Community data directory is unavailable.".to_owned())?
                .join("UpdateBackups");
            fs::create_dir_all(&backups).map_err(|e| e.to_string())?;
            let backup = backups.join(format!(
                "{}-{}",
                id,
                SystemTime::now()
                    .duration_since(UNIX_EPOCH)
                    .map(|value| value.as_nanos())
                    .unwrap_or_default()
            ));
            fs::rename(&target, &backup)
                .map_err(|e| format!("Installed widget could not be staged for update: {e}"))?;
            if let Err(error) = fs::rename(&staging, &target) {
                let _ = fs::rename(&backup, &target);
                return Err(format!(
                    "Widget update failed and the previous version was restored: {error}"
                ));
            }
            if let Err(error) =
                write_widget_approval(&approved_preview, &granted_optional_permissions)
            {
                let _ = fs::remove_dir_all(&target);
                let _ = fs::rename(&backup, &target);
                return Err(format!(
                    "Widget approval could not be saved and the previous version was restored: {error}"
                ));
            }
            let _ = fs::remove_dir_all(backup);
            return Ok(id);
        }
        if replace_existing {
            return Err(format!("{id} is not installed and cannot be updated."));
        }
        fs::rename(&staging, &target).map_err(|e| e.to_string())?;
        if let Err(error) = write_widget_approval(&approved_preview, &granted_optional_permissions)
        {
            let _ = fs::remove_dir_all(&target);
            return Err(format!(
                "Widget approval could not be saved and installation was rolled back: {error}"
            ));
        }
        Ok(id)
    })();
    if staging.exists() {
        let _ = fs::remove_dir_all(staging);
    }
    result
}

fn remote_http_client() -> Result<reqwest::blocking::Client, String> {
    reqwest::blocking::Client::builder()
        .connect_timeout(Duration::from_secs(4))
        .timeout(Duration::from_secs(10))
        .user_agent("TaskbarWidgets-CommunityLibrary/1")
        .build()
        .map_err(|e| e.to_string())
}

fn read_limited_response(
    response: reqwest::blocking::Response,
    maximum: usize,
) -> Result<Vec<u8>, String> {
    let response = response
        .error_for_status()
        .map_err(|e| format!("Library request failed: {e}"))?;
    if response
        .content_length()
        .is_some_and(|value| value > maximum as u64)
    {
        return Err("Library response exceeds the allowed size.".to_owned());
    }
    let mut bytes = Vec::new();
    response
        .take((maximum + 1) as u64)
        .read_to_end(&mut bytes)
        .map_err(|e| e.to_string())?;
    if bytes.len() > maximum {
        return Err("Library response exceeds the allowed size.".to_owned());
    }
    Ok(bytes)
}

fn safe_remote_file_name(value: &str, extension: &str) -> bool {
    !value.is_empty()
        && value.len() <= 180
        && value.ends_with(extension)
        && value.chars().all(|character| {
            character.is_ascii_alphanumeric() || matches!(character, '.' | '-' | '_')
        })
}

fn validate_remote_widget_info(
    mut info: RemoteWidgetInfo,
    expected_id: &str,
) -> Result<RemoteWidgetInfo, String> {
    if info.schema_version != 1 || info.id != expected_id || !is_community_design(&info.id) {
        return Err(format!("Invalid info.json for {expected_id}."));
    }
    if info.display_name.trim().is_empty()
        || info.description.trim().is_empty()
        || info.author.name.trim().is_empty()
    {
        return Err(format!(
            "{expected_id}/info.json is missing display, description, or author data."
        ));
    }
    if info
        .author
        .website
        .as_deref()
        .is_some_and(|value| !value.starts_with("https://"))
    {
        return Err(format!(
            "{expected_id}/info.json author website must use HTTPS."
        ));
    }
    if !safe_remote_file_name(&info.package_file, ".twidget")
        || info.sha256.len() != 64
        || !info
            .sha256
            .chars()
            .all(|character| character.is_ascii_hexdigit())
    {
        return Err(format!(
            "{expected_id}/info.json has an invalid package or SHA-256."
        ));
    }
    if info.renderer != "declarative" && info.renderer != "native" && info.renderer != "web" {
        return Err(format!(
            "{expected_id}/info.json renderer must be declarative, native, or web."
        ));
    }
    if !info.permissions.is_object() {
        info.permissions = serde_json::json!({});
    }
    if let Some(preview) = info.preview.as_deref() {
        if !safe_remote_file_name(preview, ".png") {
            return Err(format!(
                "{expected_id}/info.json preview must be a PNG file name."
            ));
        }
        info.preview_url = Some(format!("{REMOTE_LIBRARY_BASE_URL}/{expected_id}/{preview}"));
    }
    Ok(info)
}

fn fetch_remote_widget_info_with(
    client: &reqwest::blocking::Client,
    widget_id: &str,
) -> Result<RemoteWidgetInfo, String> {
    if !is_community_design(widget_id) {
        return Err("Invalid remote widget id.".to_owned());
    }
    let url = format!("{REMOTE_LIBRARY_BASE_URL}/{widget_id}/info.json");
    let bytes = read_limited_response(
        client
            .get(url)
            .send()
            .map_err(|e| format!("Library is unavailable: {e}"))?,
        MAX_LIBRARY_RESPONSE_BYTES,
    )?;
    let info = serde_json::from_slice::<RemoteWidgetInfo>(&bytes)
        .map_err(|e| format!("{widget_id}/info.json is invalid: {e}"))?;
    validate_remote_widget_info(info, widget_id)
}

#[tauri::command]
fn fetch_remote_library() -> Result<Vec<RemoteWidgetInfo>, String> {
    let client = remote_http_client()?;
    let index_url = format!("{REMOTE_LIBRARY_BASE_URL}/index.json");
    let bytes = read_limited_response(
        client
            .get(index_url)
            .send()
            .map_err(|e| format!("Community library is not ready or unavailable: {e}"))?,
        MAX_LIBRARY_RESPONSE_BYTES,
    )?;
    let index = serde_json::from_slice::<RemoteLibraryIndex>(&bytes)
        .map_err(|e| format!("Community library index.json is invalid: {e}"))?;
    if index.schema_version != 1 || index.widgets.len() > 200 {
        return Err(
            "Community library index.json has an unsupported schema or too many entries."
                .to_owned(),
        );
    }
    let mut unique = HashSet::new();
    let mut widgets = Vec::new();
    for id in index.widgets {
        if !unique.insert(id.clone()) {
            continue;
        }
        widgets.push(fetch_remote_widget_info_with(&client, &id)?);
    }
    Ok(widgets)
}

#[tauri::command]
fn download_remote_widget(widget_id: String) -> Result<RemoteWidgetDownload, String> {
    let client = remote_http_client()?;
    let info = fetch_remote_widget_info_with(&client, &widget_id)?;
    let package_url = format!(
        "{REMOTE_LIBRARY_BASE_URL}/{}/{}",
        info.id, info.package_file
    );
    let bytes = read_limited_response(
        client
            .get(package_url)
            .send()
            .map_err(|e| format!("Widget download failed: {e}"))?,
        MAX_WIDGET_PACKAGE_BYTES,
    )?;
    let actual_hash = format!("{:x}", Sha256::digest(&bytes));
    if !actual_hash.eq_ignore_ascii_case(&info.sha256) {
        return Err("Downloaded widget SHA-256 does not match info.json.".to_owned());
    }
    let downloads = community_widgets_dir()?
        .parent()
        .ok_or_else(|| "Community data directory is unavailable.".to_owned())?
        .join("Downloads");
    fs::create_dir_all(&downloads).map_err(|e| e.to_string())?;
    let target = downloads.join(format!("{}-{}.twidget", info.id, info.version));
    let temporary = target.with_extension("twidget.tmp");
    fs::write(&temporary, bytes).map_err(|e| e.to_string())?;
    replace_file_atomic(&temporary, &target)?;
    Ok(RemoteWidgetDownload {
        source: target.display().to_string(),
        advertised_permissions: info.permissions,
    })
}

#[tauri::command]
fn remove_community_widget(widget_id: String) -> Result<(), String> {
    if !is_community_design(&widget_id) {
        return Err("Invalid community widget id.".to_owned());
    }
    let root = community_widgets_dir()?;
    let target = root.join(&widget_id);
    if target.parent() != Some(root.as_path()) {
        return Err("Unsafe community widget path.".to_owned());
    }
    if target.exists() {
        fs::remove_dir_all(target).map_err(|e| e.to_string())?;
    }
    let approval = widget_approvals_dir()?.join(format!("{widget_id}.json"));
    if approval.exists() {
        fs::remove_file(approval).map_err(|e| e.to_string())?;
    }
    Ok(())
}

#[tauri::command]
fn read_community_widget_log(instance_id: String) -> Result<String, String> {
    if instance_id.len() > 160
        || !instance_id.chars().all(|value| {
            value.is_ascii_alphanumeric() || value == '.' || value == '-' || value == '_'
        })
    {
        return Err("Invalid widget instance id.".to_owned());
    }
    let local = std::env::var_os("LOCALAPPDATA")
        .ok_or_else(|| "LOCALAPPDATA is unavailable.".to_owned())?;
    let path = PathBuf::from(local)
        .join("TaskbarWidgets")
        .join("WidgetLogs")
        .join(format!("{instance_id}.log"));
    let value = fs::read_to_string(path).unwrap_or_default();
    Ok(if value.len() > 65536 {
        value[value.len() - 65536..].to_owned()
    } else {
        value
    })
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
        return Err(format!(
            "TaskbarWidgets.exe was not found: {}",
            exe.display()
        ));
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
    fn settings_version_ignores_stale_update_cache() {
        let root = std::env::temp_dir().join(format!(
            "taskbar-widgets-version-cache-{}",
            std::process::id()
        ));
        let _ = fs::remove_dir_all(&root);
        fs::create_dir_all(&root).unwrap();
        fs::write(
            root.join("update-status.json"),
            r#"{"state":"current","currentVersion":"0.0.1"}"#,
        )
        .unwrap();

        let status = read_update_status_from(&root);
        assert_eq!(
            status.current_version.as_deref(),
            Some(env!("CARGO_PKG_VERSION"))
        );
        let _ = fs::remove_dir_all(root);
    }

    #[test]
    fn system_widget_settings_round_trip() {
        let mut settings = default_settings();
        let widgets = settings.widgets.as_mut().expect("default widgets");
        let network = widgets
            .iter_mut()
            .find(|widget| widget.design == SYSTEM_NETWORK_DESIGN)
            .expect("system network widget");
        network.enabled = true;
        network
            .settings
            .insert("displayMode".to_owned(), serde_json::json!("pie"));
        network
            .settings
            .insert("interfaceId".to_owned(), serde_json::json!("adapter-test"));

        let config = transport_to_config(&settings);
        assert_eq!(config.config_version, 3);
        let saved = config
            .widgets
            .iter()
            .find(|widget| widget.id == SYSTEM_NETWORK_DESIGN)
            .unwrap();
        assert_eq!(
            saved.settings.get("displayMode"),
            Some(&serde_json::json!("pie"))
        );

        let restored = config_to_transport(config);
        let restored_network = restored
            .widgets
            .unwrap()
            .into_iter()
            .find(|widget| widget.design == SYSTEM_NETWORK_DESIGN)
            .unwrap();
        assert_eq!(
            restored_network.settings.get("interfaceId"),
            Some(&serde_json::json!("adapter-test"))
        );
    }

    #[test]
    fn community_secret_uses_dpapi() {
        let encrypted = protect_secret("local-test-secret");
        assert!(encrypted.starts_with("dpapi:"));
        assert!(!encrypted.contains("local-test-secret"));
        assert_eq!(unprotect_secret(&encrypted), "local-test-secret");
    }

    fn remote_info(package_file: &str) -> RemoteWidgetInfo {
        RemoteWidgetInfo {
            schema_version: 1,
            id: "com.example.clock".to_owned(),
            version: "1.0.0".to_owned(),
            display_name: "Example Clock".to_owned(),
            description: "Example".to_owned(),
            author: RemoteAuthor {
                name: "Example".to_owned(),
                website: Some("https://example.com".to_owned()),
            },
            category: "Time".to_owned(),
            package_file: package_file.to_owned(),
            sha256: "a".repeat(64),
            permissions: serde_json::json!({}),
            renderer: "declarative".to_owned(),
            preview: Some("preview.png".to_owned()),
            preview_url: None,
        }
    }

    #[test]
    fn remote_widget_info_accepts_safe_same_folder_files() {
        let info = validate_remote_widget_info(
            remote_info("com.example.clock.twidget"),
            "com.example.clock",
        )
        .expect("valid remote widget info");
        assert!(info
            .preview_url
            .unwrap()
            .ends_with("/com.example.clock/preview.png"));
    }

    #[test]
    fn remote_widget_info_rejects_path_traversal() {
        let result =
            validate_remote_widget_info(remote_info("../clock.twidget"), "com.example.clock");
        assert!(result.is_err());
    }

    #[test]
    fn widget_update_requires_a_higher_numeric_version() {
        assert!(compare_widget_versions("1.0.1", "1.0.0").is_some_and(|value| value.is_gt()));
        assert!(compare_widget_versions("1.10.0", "1.9.9").is_some_and(|value| value.is_gt()));
        assert!(compare_widget_versions("1.0.0", "1.0.0.0").is_some_and(|value| value.is_eq()));
        assert!(compare_widget_versions("1.0", "1.0.0").is_none());
    }

    #[test]
    fn web_widget_install_preview_is_sandbox_labeled() {
        let id = format!("com.example.webtest{}", std::process::id());
        let root = std::env::temp_dir().join(&id);
        let ui = root.join("ui");
        fs::create_dir_all(&ui).unwrap();
        fs::write(
            ui.join("index.html"),
            "<!doctype html><title>Web test</title>",
        )
        .unwrap();
        fs::write(
            root.join("widget.json"),
            serde_json::json!({
                "schemaVersion": 3,
                "id": id,
                "version": "1.0.0",
                "displayName": "Web test",
                "description": "Web install preview test",
                "author": { "name": "pfc", "website": "https://pfcsoft.com" },
                "size": { "width": 170, "height": 32 },
                "renderer": {
                    "type": "web",
                    "entry": "ui/index.html",
                    "expandedSize": { "width": 360, "height": 180 }
                },
                "permissions": { "storage": true }
            })
            .to_string(),
        )
        .unwrap();
        let preview =
            install_preview_from_directory(&root, &root).expect("schema v3 install preview");
        assert_eq!(preview.renderer_type, "web");
        assert_eq!(preview.author_name, "pfc");
        let _ = fs::remove_dir_all(root);
    }

    #[test]
    fn schema_v4_process_preview_requires_declared_full_access() {
        let id = format!("com.example.v4test{}", std::process::id());
        let root = std::env::temp_dir().join(&id);
        let ui = root.join("ui");
        fs::create_dir_all(&ui).unwrap();
        fs::write(ui.join("index.html"), "<!doctype html><title>V4</title>").unwrap();
        fs::write(root.join("provider.ps1"), "Write-Output '{}'").unwrap();
        let manifest = |permissions: serde_json::Value| {
            serde_json::json!({
                "schemaVersion": 4,
                "id": id,
                "version": "1.0.0",
                "displayName": "V4 process",
                "description": "V4 process preview test",
                "author": { "name": "pfc", "website": "https://pfcsoft.com" },
                "size": { "width": 170, "height": 32 },
                "renderer": { "type": "web", "entry": "ui/index.html" },
                "runtime": {
                    "type": "process",
                    "entry": "provider.ps1",
                    "protocol": "json-lines-v1"
                },
                "permissions": permissions
            })
        };
        fs::write(
            root.join("widget.json"),
            manifest(serde_json::json!({ "required": [], "optional": [] })).to_string(),
        )
        .unwrap();
        assert!(install_preview_from_directory(&root, &root)
            .unwrap_err()
            .contains("system.fullAccess"));

        fs::write(
            root.join("widget.json"),
            manifest(serde_json::json!({
                "required": [{
                    "id": "system.fullAccess",
                    "reason": "Runs the package provider with the user's rights."
                }],
                "optional": []
            }))
            .to_string(),
        )
        .unwrap();
        let preview = install_preview_from_directory(&root, &root).expect("valid v4 preview");
        assert_eq!(preview.schema_version, 4);
        assert_eq!(preview.provider_type, "process");
        assert_eq!(preview.executable_files, vec!["provider.ps1"]);
        let _ = fs::remove_dir_all(root);
    }

    #[test]
    fn installed_content_hash_matches_loader_contract() {
        let root = std::env::temp_dir().join(format!(
            "taskbar-widgets-hash-contract-{}",
            std::process::id()
        ));
        let _ = fs::remove_dir_all(&root);
        fs::create_dir_all(root.join("sub")).unwrap();
        fs::write(root.join("a.txt"), b"alpha").unwrap();
        fs::write(root.join("sub").join("b.bin"), [0_u8, 1, 255]).unwrap();
        assert_eq!(
            hash_widget_tree(&root).unwrap(),
            "8a29aa8ea3b60d2ae5f62ea72e7b6cbedcaa1aee31956113610db88072c2caae"
        );
        let _ = fs::remove_dir_all(root);
    }

    #[test]
    fn atomic_replace_overwrites_existing_approval_file() {
        let root = std::env::temp_dir().join(format!(
            "taskbar-widgets-atomic-replace-{}",
            std::process::id()
        ));
        let _ = fs::remove_dir_all(&root);
        fs::create_dir_all(&root).unwrap();
        let target = root.join("approval.json");
        let temporary = root.join("approval.json.tmp");
        fs::write(&target, b"old").unwrap();
        fs::write(&temporary, b"new").unwrap();
        replace_file_atomic(&temporary, &target).unwrap();
        assert_eq!(fs::read(&target).unwrap(), b"new");
        assert!(!temporary.exists());
        let _ = fs::remove_dir_all(root);
    }
}

fn main() {
    tauri::Builder::default()
        .plugin(tauri_plugin_dialog::init())
        .plugin(tauri_plugin_shell::init())
        .invoke_handler(tauri::generate_handler![
            load_state,
            consume_settings_open_request,
            consume_widget_install_request,
            save_settings,
            open_widget_libraries,
            inspect_community_widget,
            install_community_widget,
            fetch_remote_library,
            download_remote_widget,
            remove_community_widget,
            read_community_widget_log,
            run_loader_command,
            start_loader_command,
            control_runtime,
            install_voice_helper,
            uninstall_voice_helper,
            launch_downloaded_installer
        ])
        .run(tauri::generate_context!())
        .expect("failed to run TaskbarWidgets settings");
}
