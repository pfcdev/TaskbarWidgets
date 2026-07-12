#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

use eframe::egui::{
    self, pos2, vec2, Align, Align2, Button, CentralPanel, Color32, CornerRadius, FontId, Frame,
    Layout, Margin, RichText, ScrollArea, Sense, Stroke, Ui, Vec2,
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
            .with_inner_size([1040.0, 680.0])
            .with_min_inner_size([820.0, 540.0]),
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

#[derive(Clone, Copy)]
enum PreviewKind {
    Codex,
    Weather,
}

#[derive(Clone, Copy)]
struct DesignCard {
    id: &'static str,
    title: &'static str,
    tag: &'static str,
    metric: &'static str,
    accent: Color32,
    preview: PreviewKind,
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
            Ok(()) => self.status = format!("Active: {}", display_design(&self.active_design)),
            Err(error) => self.status = format!("Save failed: {error}"),
        }
    }

    fn open_widget_libraries(&mut self) {
        let directory = self.app_dir.join("WidgetLibraries");
        if let Err(error) = fs::create_dir_all(&directory) {
            self.status = format!("Folder failed: {error}");
            return;
        }

        let readme_path = directory.join("README.txt");
        if !readme_path.exists() {
            let _ = fs::write(&readme_path, "TaskbarStats widget design packs.\r\n");
        }

        match Command::new("explorer").arg(&directory).spawn() {
            Ok(_) => self.status = "WidgetLibraries opened".to_owned(),
            Err(error) => self.status = format!("Open failed: {error}"),
        }
    }
}

impl eframe::App for SettingsApp {
    fn update(&mut self, ctx: &egui::Context, _frame: &mut eframe::Frame) {
        egui::SidePanel::left("taskbar_stats_sidebar")
            .exact_width(270.0)
            .resizable(false)
            .frame(Frame::new().fill(Color32::from_rgb(20, 22, 25)))
            .show(ctx, |ui| {
                sidebar(ui, &self.active_design);
            });

        CentralPanel::default()
            .frame(Frame::new().fill(bg()))
            .show(ctx, |ui| {
                content(ui, self);
            });
    }
}

fn configure_style(ctx: &egui::Context) {
    let mut style = (*ctx.style()).clone();
    style.visuals.window_fill = bg();
    style.visuals.panel_fill = bg();
    style.visuals.widgets.inactive.bg_fill = surface_2();
    style.visuals.widgets.hovered.bg_fill = Color32::from_rgb(43, 48, 55);
    style.visuals.widgets.active.bg_fill = Color32::from_rgb(50, 57, 65);
    style.visuals.selection.bg_fill = teal();
    style.spacing.item_spacing = Vec2::new(12.0, 12.0);
    ctx.set_style(style);
}

fn sidebar(ui: &mut Ui, active_design: &str) {
    ui.set_min_height(ui.available_height());
    ui.add_space(24.0);
    ui.vertical_centered_justified(|ui| {
        ui.label(
            RichText::new("TaskbarStats")
                .color(text())
                .font(FontId::proportional(23.0)),
        );
        ui.label(RichText::new("Widget Studio").color(muted()).size(12.0));
    });

    ui.add_space(30.0);
    ui.vertical(|ui| {
        ui.add_space(0.0);
        nav_item(ui, "Library", true);
        nav_item(ui, "Packs", false);
        nav_item(ui, "Runtime", false);
    });

    ui.with_layout(Layout::bottom_up(Align::LEFT), |ui| {
        ui.add_space(24.0);
        side_status(ui, active_design);
    });
}

fn side_status(ui: &mut Ui, active_design: &str) {
    Frame::new()
        .fill(Color32::from_rgb(27, 30, 34))
        .stroke(Stroke::new(1.0, Color32::from_rgb(46, 51, 58)))
        .corner_radius(CornerRadius::same(8))
        .inner_margin(Margin::same(14))
        .show(ui, |ui| {
            ui.set_width(176.0);
            ui.label(RichText::new("Active Design").color(muted()).size(11.0));
            ui.add_space(4.0);
            ui.label(
                RichText::new(display_design(active_design))
                    .color(text())
                    .size(15.0),
            );
        });
}

fn nav_item(ui: &mut Ui, label: &str, active: bool) {
    let fill = if active {
        Color32::from_rgb(35, 39, 45)
    } else {
        Color32::TRANSPARENT
    };
    let text_color = if active { text() } else { muted() };

    Frame::new()
        .fill(fill)
        .corner_radius(CornerRadius::same(8))
        .inner_margin(Margin::symmetric(12, 10))
        .show(ui, |ui| {
            ui.set_width(178.0);
            ui.horizontal(|ui| {
                ui.label(RichText::new(if active { "●" } else { "○" }).color(teal()));
                ui.label(RichText::new(label).color(text_color).size(14.0));
            });
        });
}

fn content(ui: &mut Ui, app: &mut SettingsApp) {
    Frame::new()
        .fill(bg())
        .inner_margin(Margin::symmetric(36, 28))
        .show(ui, |ui| {
            ScrollArea::vertical()
                .auto_shrink([false, false])
                .show(ui, |ui| {
                    top_bar(ui, app);
                    ui.add_space(22.0);

                    design_grid(ui, app);

                    ui.add_space(18.0);
                    lower_bar(ui, app);

                    ui.add_space(18.0);
                    ui.label(RichText::new(&app.status).color(muted()).size(12.0));
                });
        });
}

fn top_bar(ui: &mut Ui, app: &SettingsApp) {
    ui.horizontal(|ui| {
        ui.vertical(|ui| {
            ui.label(
                RichText::new("Widget Library")
                    .color(text())
                    .font(FontId::proportional(32.0)),
            );
            ui.label(RichText::new("Taskbar designs").color(muted()).size(13.0));
        });

        ui.with_layout(Layout::right_to_left(Align::TOP), |ui| {
            pill(
                ui,
                &format!("Active  {}", display_design(&app.active_design)),
                teal(),
            );
        });
    });
}

fn lower_bar(ui: &mut Ui, app: &mut SettingsApp) {
    Frame::new()
        .fill(surface())
        .stroke(Stroke::new(1.0, Color32::from_rgb(45, 49, 56)))
        .corner_radius(CornerRadius::same(8))
        .inner_margin(Margin::symmetric(18, 14))
        .show(ui, |ui| {
            ui.horizontal(|ui| {
                ui.vertical(|ui| {
                    ui.label(RichText::new("Design Packs").color(text()).size(16.0));
                    ui.label(RichText::new("WidgetLibraries").color(muted()).size(12.0));
                });
                ui.with_layout(Layout::right_to_left(Align::Center), |ui| {
                    if ui
                        .add_sized([126.0, 32.0], Button::new("Open folder"))
                        .clicked()
                    {
                        app.open_widget_libraries();
                    }
                });
            });
        });
}

fn design_grid(ui: &mut Ui, app: &mut SettingsApp) {
    let cards = design_cards();
    let available = ui.available_width().max(320.0);
    let gap = 18.0;
    let columns = if available >= 1180.0 {
        3
    } else if available >= 720.0 {
        2
    } else {
        1
    };
    let card_width =
        ((available - gap * (columns as f32 - 1.0)) / columns as f32).clamp(320.0, 520.0);
    let card_height = 250.0;

    for row in cards.chunks(columns) {
        ui.horizontal(|ui| {
            ui.spacing_mut().item_spacing.x = gap;
            for card in row {
                design_card(ui, app, *card, vec2(card_width, card_height));
            }
        });
        ui.add_space(gap);
    }
}

fn design_card(ui: &mut Ui, app: &mut SettingsApp, card: DesignCard, size: Vec2) {
    let selected = app.active_design == card.id;
    let (rect, response) = ui.allocate_exact_size(size, Sense::click());
    let painter = ui.painter_at(rect);
    let hovered = response.hovered();
    let fill = if selected {
        Color32::from_rgb(30, 34, 38)
    } else if hovered {
        Color32::from_rgb(29, 33, 38)
    } else {
        surface()
    };
    let stroke = if selected {
        Stroke::new(1.6, card.accent)
    } else {
        Stroke::new(1.0, Color32::from_rgb(45, 49, 56))
    };

    painter.rect_filled(rect, CornerRadius::same(8), fill);
    painter.rect_stroke(
        rect,
        CornerRadius::same(8),
        stroke,
        egui::StrokeKind::Inside,
    );

    let preview = egui::Rect::from_min_size(
        rect.left_top() + vec2(18.0, 18.0),
        vec2(rect.width() - 36.0, 104.0),
    );
    paint_widget_preview(ui, preview, card);

    painter.text(
        rect.left_top() + vec2(20.0, 142.0),
        Align2::LEFT_TOP,
        card.title,
        FontId::proportional(18.0),
        text(),
    );
    painter.text(
        rect.left_top() + vec2(20.0, 169.0),
        Align2::LEFT_TOP,
        card.tag,
        FontId::proportional(12.0),
        muted(),
    );

    if selected {
        let badge =
            egui::Rect::from_min_size(rect.right_top() + vec2(-88.0, 142.0), vec2(68.0, 26.0));
        painter.rect_filled(
            badge,
            CornerRadius::same(8),
            Color32::from_rgba_unmultiplied(card.accent.r(), card.accent.g(), card.accent.b(), 42),
        );
        painter.rect_stroke(
            badge,
            CornerRadius::same(8),
            Stroke::new(1.0, card.accent),
            egui::StrokeKind::Inside,
        );
        painter.text(
            badge.center(),
            Align2::CENTER_CENTER,
            "Active",
            FontId::proportional(12.0),
            text(),
        );
    }

    let meta = egui::Rect::from_min_size(
        rect.left_top() + vec2(20.0, 194.0),
        vec2(rect.width() - 40.0, 30.0),
    );
    painter.rect_filled(meta, CornerRadius::same(6), Color32::from_rgb(25, 28, 32));
    painter.text(
        meta.left_center() + vec2(12.0, 0.0),
        Align2::LEFT_CENTER,
        "Surface",
        FontId::proportional(12.0),
        muted(),
    );
    painter.text(
        meta.right_center() + vec2(-12.0, 0.0),
        Align2::RIGHT_CENTER,
        card.metric,
        FontId::proportional(12.0),
        card.accent,
    );

    let button = egui::Rect::from_min_size(
        rect.left_bottom() + vec2(20.0, -44.0),
        vec2(rect.width() - 40.0, 28.0),
    );
    let button_fill = if selected { surface_2() } else { card.accent };
    painter.rect_filled(button, CornerRadius::same(6), button_fill);
    painter.text(
        button.center(),
        Align2::CENTER_CENTER,
        if selected { "Selected" } else { "Use this" },
        FontId::proportional(12.0),
        if selected {
            muted()
        } else {
            Color32::from_rgb(8, 12, 16)
        },
    );

    if response.clicked() {
        app.select_design(card.id);
    }
}

fn paint_widget_preview(ui: &Ui, rect: egui::Rect, card: DesignCard) {
    let painter = ui.painter_at(rect);
    painter.rect_filled(rect, CornerRadius::same(8), Color32::from_rgb(14, 16, 18));

    let taskbar = rect.shrink2(vec2(14.0, 20.0));
    painter.rect_filled(
        taskbar,
        CornerRadius::same(8),
        Color32::from_rgb(27, 30, 34),
    );

    match card.preview {
        PreviewKind::Codex => draw_codex_preview(ui, taskbar, card.accent),
        PreviewKind::Weather => draw_weather_preview(ui, taskbar, card.accent),
    }
}

fn draw_codex_preview(ui: &Ui, rect: egui::Rect, accent: Color32) {
    let painter = ui.painter();
    let chip = egui::Rect::from_min_size(rect.left_top() + vec2(18.0, 11.0), vec2(190.0, 36.0));
    painter.rect_filled(chip, CornerRadius::same(8), Color32::from_rgb(36, 41, 47));
    painter.text(
        chip.left_top() + vec2(13.0, 6.0),
        Align2::LEFT_TOP,
        "Antigravity",
        FontId::proportional(12.0),
        text(),
    );
    painter.text(
        chip.right_top() + vec2(-52.0, 7.0),
        Align2::LEFT_TOP,
        "RUN",
        FontId::proportional(11.0),
        accent,
    );

    let bar_bg =
        egui::Rect::from_min_size(chip.left_bottom() + vec2(13.0, -10.0), vec2(124.0, 3.0));
    painter.rect_filled(bar_bg, CornerRadius::same(2), Color32::from_rgb(58, 63, 70));
    painter.rect_filled(
        egui::Rect::from_min_size(bar_bg.left_top(), vec2(84.0, 3.0)),
        CornerRadius::same(2),
        accent,
    );
}

fn draw_weather_preview(ui: &Ui, rect: egui::Rect, accent: Color32) {
    let painter = ui.painter();
    let chip = egui::Rect::from_min_size(rect.left_top() + vec2(18.0, 11.0), vec2(190.0, 36.0));
    painter.rect_filled(chip, CornerRadius::same(8), Color32::from_rgb(39, 35, 27));
    painter.circle_filled(pos2(chip.left() + 22.0, chip.center().y), 8.0, accent);
    painter.text(
        chip.left_top() + vec2(43.0, 5.0),
        Align2::LEFT_TOP,
        "Istanbul",
        FontId::proportional(12.0),
        text(),
    );
    painter.text(
        chip.left_top() + vec2(43.0, 20.0),
        Align2::LEFT_TOP,
        "Static weather",
        FontId::proportional(9.0),
        muted(),
    );
    painter.text(
        chip.right_top() + vec2(-48.0, 9.0),
        Align2::LEFT_TOP,
        "24 C",
        FontId::proportional(15.0),
        text(),
    );
}

fn pill(ui: &mut Ui, label: &str, accent: Color32) {
    Frame::new()
        .fill(Color32::from_rgba_unmultiplied(
            accent.r(),
            accent.g(),
            accent.b(),
            34,
        ))
        .stroke(Stroke::new(1.0, accent))
        .corner_radius(CornerRadius::same(8))
        .inner_margin(Margin::symmetric(10, 5))
        .show(ui, |ui| {
            ui.label(RichText::new(label).color(text()).size(12.0));
        });
}

fn design_cards() -> [DesignCard; 2] {
    [
        DesignCard {
            id: CODEX_DESIGN,
            title: "Codex Status",
            tag: "Quota capsule",
            metric: "Codex",
            accent: Color32::from_rgb(56, 189, 248),
            preview: PreviewKind::Codex,
        },
        DesignCard {
            id: WEATHER_DESIGN,
            title: "Static Weather",
            tag: "Weather capsule",
            metric: "Weather",
            accent: Color32::from_rgb(245, 158, 11),
            preview: PreviewKind::Weather,
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

fn bg() -> Color32 {
    Color32::from_rgb(15, 17, 19)
}

fn surface() -> Color32 {
    Color32::from_rgb(24, 27, 31)
}

fn surface_2() -> Color32 {
    Color32::from_rgb(34, 38, 43)
}

fn text() -> Color32 {
    Color32::from_rgb(241, 245, 249)
}

fn muted() -> Color32 {
    Color32::from_rgb(148, 163, 184)
}

fn teal() -> Color32 {
    Color32::from_rgb(45, 212, 191)
}
