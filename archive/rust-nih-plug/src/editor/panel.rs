//! Panel view: global settings, mass measurement and the live channel list.

use crate::measurement;
use crate::registry::{self, ChannelShared, GLOBALS};
use nih_plug_egui::egui;
use std::sync::atomic::Ordering::Relaxed;

pub fn ui(ui: &mut egui::Ui, shared: &ChannelShared) {
    measurement::poll();

    ui.heading("AutoTrim — Painel de Controle");
    ui.add_space(8.0);

    ui.horizontal(|ui| {
        let mut duration = GLOBALS.meas_duration_s.load(Relaxed);
        if ui
            .add(
                egui::Slider::new(&mut duration, 1.0..=60.0)
                    .suffix(" s")
                    .text("Duração da medição"),
            )
            .changed()
        {
            GLOBALS.meas_duration_s.store(duration, Relaxed);
        }
    });
    ui.horizontal(|ui| {
        let mut max_trim = GLOBALS.max_trim_db.load(Relaxed);
        if ui
            .add(
                egui::Slider::new(&mut max_trim, 1.0..=48.0)
                    .suffix(" dB")
                    .text("Limite de trim (±)"),
            )
            .changed()
        {
            GLOBALS.max_trim_db.store(max_trim, Relaxed);
        }
    });
    ui.add_space(8.0);

    match measurement::progress() {
        Some(progress) => {
            ui.horizontal(|ui| {
                ui.add(
                    egui::ProgressBar::new(progress)
                        .desired_width(ui.available_width() - 100.0)
                        .text("medindo…"),
                );
                if ui.button("Cancelar").clicked() {
                    measurement::cancel();
                }
            });
        }
        None => {
            if ui
                .add_sized(
                    [ui.available_width(), 32.0],
                    egui::Button::new("Medir e regular todos os canais"),
                )
                .clicked()
            {
                measurement::start(GLOBALS.meas_duration_s.load(Relaxed));
            }
        }
    }

    ui.add_space(12.0);
    ui.separator();

    let channels = registry::channels();
    if channels.is_empty() {
        ui.label("Nenhum canal registrado. Insira o AutoTrim nos canais da sessão.");
    } else {
        ui.label(format!("{} canais", channels.len()));
        egui::ScrollArea::vertical().show(ui, |ui| {
            egui::Grid::new("channels")
                .num_columns(5)
                .striped(true)
                .min_col_width(80.0)
                .show(ui, |ui| {
                    ui.strong("Canal");
                    ui.strong("Entrada (pré-trim)");
                    ui.strong("Trim");
                    ui.strong("Automação");
                    ui.strong("Status");
                    ui.end_row();

                    for ch in &channels {
                        channel_row(ui, ch);
                        ui.end_row();
                    }
                });
        });
    }

    ui.add_space(12.0);
    ui.separator();
    let mut panel_mode = shared.panel_mode.load(Relaxed);
    if ui
        .checkbox(&mut panel_mode, "Modo painel (desmarque para voltar ao modo canal)")
        .changed()
    {
        shared.panel_mode.store(panel_mode, Relaxed);
    }
}

fn channel_row(ui: &mut egui::Ui, ch: &ChannelShared) {
    ui.label(ch.display_name());
    super::peak_meter(ui, ch.peak_pre_trim.load(Relaxed), 140.0);
    ui.label(super::format_db(ch.trim_db.load(Relaxed)));

    let mut automation = ch.automation_on.load(Relaxed);
    if ui.checkbox(&mut automation, "").changed() {
        ch.automation_on.store(automation, Relaxed);
    }

    if ch.measuring.load(Relaxed) {
        ui.colored_label(egui::Color32::LIGHT_BLUE, "medindo…");
    } else if ch.no_signal.load(Relaxed) {
        ui.colored_label(egui::Color32::YELLOW, "sem sinal");
    } else if !automation {
        ui.colored_label(egui::Color32::GRAY, "desativado");
    } else if ch.rider_on.load(Relaxed) {
        ui.label("rider ativo");
    } else {
        ui.label("ok");
    }
}
