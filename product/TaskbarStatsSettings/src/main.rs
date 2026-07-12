#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

use eframe::egui::{
    self, Align, Button, CentralPanel, Color32, CornerRadius, FontId, Frame, Layout, Margin,
    RichText, Stroke, Ui, Vec2,
};
use serde::{Deserialize, Serialize};
use std::{
    fs,
    path::{Path, PathBuf},
    process::Command,
};

const CODEX_DESIGN: &str = "codex-status";
const WEATHER_DESIGN: &str = "weather-static";

fn main() -> eframe::Result {
    let options = eframe::NativeOptions {
        viewport: egui::ViewportBuilder::default()
            .with_title("TaskbarStats Settings")
            .with_inner_size([980.0, 660.0])
            .with_min_inner_size([760.0, 520.0]),
        ..Default::default()
    };

    eframe::run_native(
        "TaskbarStats Settings",
        options,
        Box::new(|cc| {
            configure_style(&cc.egui_ctx);
            Ok(Box::new(SettingsApp::load()))
        }),
    )
}

#[derive(Default, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
struct WidgetSettings {
    active_design: String,
}

struct DesignCard {
    id: &'static str,
    title: &'static str,
    subtitle: &'static str,
    detail: &'static str,
    accent: Color32,
}

struct SettingsApp {
    app_dir: PathBuf,
    active_design: String,
    status: String,
}

impl SettingsApp {
    fn load() -> Self {
        let app_dir = std::env::current_exe()
            .ok()
            .and_then(|path| path.parent().map(Path::to_path_buf))
            .unwrap_or_else(|| std::env::current_dir().unwrap_or_else(|_| PathBuf::from(".")));

        let settings = read_widget_settings(&app_dir);
        Self {
            app_dir,
            active_design: normalize_design(&settings.active_design).to_owned(),
            status: "Ready".to_owned(),
        }
    }

    fn select_design(&mut self, design_id: &str) {
        self.active_design = normalize_design(design_id).to_owned();
        match write_widget_settings(&self.app_dir, &self.active_design) {
            Ok(()) => {
                self.status = format!(
                    "Active widget changed to {}",
                    display_design(&self.active_design)
                );
            }
            Err(error) => {
                self.status = format!("Settings could not be saved: {error}");
            }
        }
    }

    fn open_widget_libraries(&mut self) {
        let directory = self.app_dir.join("WidgetLibraries");
        if let Err(error) = fs::create_dir_all(&directory) {
            self.status = format!("WidgetLibraries could not be created: {error}");
            return;
        }

        let readme_path = directory.join("README.txt");
        if !readme_path.exists() {
            let _ = fs::write(
                &readme_path,
                "TaskbarStats widget design packs will live in this folder.\r\n",
            );
        }

        match Command::new("explorer").arg(&directory).spawn() {
            Ok(_) => self.status = format!("Opened {}", directory.display()),
            Err(error) => self.status = format!("Folder could not be opened: {error}"),
        }
    }
}

impl eframe::App for SettingsApp {
    fn update(&mut self, ctx: &egui::Context, _frame: &mut eframe::Frame) {
        CentralPanel::default()
            .frame(Frame::new().fill(Color32::from_rgb(12, 16, 23)))
            .show(ctx, |ui| {
                ui.horizontal(|ui| {
                    sidebar(ui);
                    content(ui, self);
                });
            });
    }
}

fn configure_style(ctx: &egui::Context) {
    let mut style = (*ctx.style()).clone();
    style.visuals.window_fill = Color32::from_rgb(12, 16, 23);
    style.visuals.panel_fill = Color32::from_rgb(12, 16, 23);
    style.visuals.widgets.inactive.bg_fill = Color32::from_rgb(24, 30, 40);
    style.visuals.widgets.hovered.bg_fill = Color32::from_rgb(35, 43, 55);
    style.visuals.widgets.active.bg_fill = Color32::from_rgb(43, 54, 70);
    style.visuals.selection.bg_fill = Color32::from_rgb(20, 184, 166);
    style.spacing.item_spacing = Vec2::new(12.0, 12.0);
    ctx.set_style(style);
}

fn sidebar(ui: &mut Ui) {
    Frame::new()
        .fill(Color32::from_rgb(18, 23, 31))
        .inner_margin(Margin::same(24))
        .show(ui, |ui| {
            ui.set_width(230.0);
            ui.vertical(|ui| {
                ui.label(
                    RichText::new("TaskbarStats")
                        .color(Color32::from_rgb(248, 250, 252))
                        .font(FontId::proportional(22.0)),
                );
                ui.add_space(28.0);
                nav_item(ui, "Widget Library", true);
                nav_item(ui, "Design Settings", false);
                ui.with_layout(Layout::bottom_up(Align::LEFT), |ui| {
                    ui.label(
                        RichText::new("Settings runs outside Explorer")
                            .color(Color32::from_rgb(100, 116, 139))
                            .size(12.0),
                    );
                });
            });
        });
}

fn nav_item(ui: &mut Ui, label: &str, active: bool) {
    let fill = if active {
        Color32::from_rgb(30, 41, 59)
    } else {
        Color32::TRANSPARENT
    };
    let text = if active {
        Color32::from_rgb(248, 250, 252)
    } else {
        Color32::from_rgb(148, 163, 184)
    };

    Frame::new()
        .fill(fill)
        .corner_radius(CornerRadius::same(8))
        .inner_margin(Margin::symmetric(12, 9))
        .show(ui, |ui| {
            ui.set_width(176.0);
            ui.label(RichText::new(label).color(text).size(14.0));
        });
}

fn content(ui: &mut Ui, app: &mut SettingsApp) {
    Frame::new()
        .fill(Color32::from_rgb(12, 16, 23))
        .inner_margin(Margin::symmetric(34, 30))
        .show(ui, |ui| {
            ui.vertical(|ui| {
                ui.label(
                    RichText::new("Widget Library")
                        .color(Color32::from_rgb(248, 250, 252))
                        .font(FontId::proportional(34.0)),
                );
                ui.label(
                    RichText::new("Choose the design hosted in the taskbar. Explorer only renders the selected widget.")
                        .color(Color32::from_rgb(148, 163, 184))
                        .size(14.0),
                );
                ui.add_space(22.0);

                ui.horizontal_wrapped(|ui| {
                    for card in design_cards() {
                        design_card(ui, app, card);
                    }
                    add_library_card(ui, app);
                });

                ui.with_layout(Layout::bottom_up(Align::LEFT), |ui| {
                    ui.label(
                        RichText::new(&app.status)
                            .color(Color32::from_rgb(148, 163, 184))
                            .size(12.0),
                    );
                    ui.label(
                        RichText::new(format!("Data folder: {}", app.app_dir.display()))
                            .color(Color32::from_rgb(100, 116, 139))
                            .size(12.0),
                    );
                });
            });
        });
}

fn design_card(ui: &mut Ui, app: &mut SettingsApp, card: DesignCard) {
    let selected = app.active_design == card.id;
    let stroke = if selected {
        Stroke::new(1.5, card.accent)
    } else {
        Stroke::new(1.0, Color32::from_rgb(48, 57, 72))
    };
    let fill = if selected {
        Color32::from_rgb(27, 41, 54)
    } else {
        Color32::from_rgb(23, 29, 39)
    };

    Frame::new()
        .fill(fill)
        .stroke(stroke)
        .corner_radius(CornerRadius::same(8))
        .inner_margin(Margin::same(18))
        .show(ui, |ui| {
            ui.set_min_size(Vec2::new(280.0, 146.0));
            ui.vertical(|ui| {
                ui.horizontal(|ui| {
                    ui.label(RichText::new("■").color(card.accent).size(20.0));
                    ui.vertical(|ui| {
                        ui.label(
                            RichText::new(card.title)
                                .color(Color32::from_rgb(248, 250, 252))
                                .size(18.0),
                        );
                        ui.label(
                            RichText::new(card.subtitle)
                                .color(Color32::from_rgb(148, 163, 184))
                                .size(12.0),
                        );
                    });
                });
                ui.add_space(12.0);
                ui.label(
                    RichText::new(card.detail)
                        .color(Color32::from_rgb(203, 213, 225))
                        .size(12.0),
                );
                ui.with_layout(Layout::bottom_up(Align::RIGHT), |ui| {
                    let label = if selected { "Active" } else { "Use design" };
                    if ui
                        .add_sized([116.0, 30.0], Button::new(label).fill(card.accent))
                        .clicked()
                    {
                        app.select_design(card.id);
                    }
                });
            });
        });
}

fn add_library_card(ui: &mut Ui, app: &mut SettingsApp) {
    Frame::new()
        .fill(Color32::from_rgb(20, 28, 36))
        .stroke(Stroke::new(1.0, Color32::from_rgb(48, 57, 72)))
        .corner_radius(CornerRadius::same(8))
        .inner_margin(Margin::same(18))
        .show(ui, |ui| {
            ui.set_min_size(Vec2::new(280.0, 146.0));
            ui.label(
                RichText::new("Add Library")
                    .color(Color32::from_rgb(248, 250, 252))
                    .size(18.0),
            );
            ui.label(
                RichText::new("Open the folder where future design packs will be placed.")
                    .color(Color32::from_rgb(148, 163, 184))
                    .size(12.0),
            );
            ui.with_layout(Layout::bottom_up(Align::RIGHT), |ui| {
                if ui
                    .add_sized([116.0, 30.0], Button::new("Open folder"))
                    .clicked()
                {
                    app.open_widget_libraries();
                }
            });
        });
}

fn design_cards() -> [DesignCard; 2] {
    [
        DesignCard {
            id: CODEX_DESIGN,
            title: "Codex Status",
            subtitle: "Antigravity and Codex quota design",
            detail: "Rate limit, reset, weekly quota, and 30-day token metrics.",
            accent: Color32::from_rgb(56, 189, 248),
        },
        DesignCard {
            id: WEATHER_DESIGN,
            title: "Static Weather",
            subtitle: "Simple test design",
            detail: "Istanbul, 24 C, fixed condition, and humidity placeholder.",
            accent: Color32::from_rgb(248, 197, 85),
        },
    ]
}

fn read_widget_settings(app_dir: &Path) -> WidgetSettings {
    let path = app_dir.join("widget-settings.json");
    fs::read_to_string(path)
        .ok()
        .and_then(|data| serde_json::from_str(&data).ok())
        .unwrap_or_else(|| WidgetSettings {
            active_design: CODEX_DESIGN.to_owned(),
        })
}

fn write_widget_settings(app_dir: &Path, active_design: &str) -> std::io::Result<()> {
    let settings = WidgetSettings {
        active_design: normalize_design(active_design).to_owned(),
    };
    let data = serde_json::to_string_pretty(&settings)
        .unwrap_or_else(|_| format!("{{\n  \"activeDesign\": \"{}\"\n}}\n", CODEX_DESIGN));
    fs::write(app_dir.join("widget-settings.json"), format!("{data}\n"))
}

fn normalize_design(design_id: &str) -> &str {
    match design_id {
        WEATHER_DESIGN => WEATHER_DESIGN,
        _ => CODEX_DESIGN,
    }
}

fn display_design(design_id: &str) -> &'static str {
    match design_id {
        WEATHER_DESIGN => "Static Weather",
        _ => "Codex Status",
    }
}
