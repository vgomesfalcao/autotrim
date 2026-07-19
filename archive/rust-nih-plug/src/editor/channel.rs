//! Per-channel view: name, target, trim readout, meter and toggles.

use super::UiState;
use crate::registry::{ChannelShared, GLOBALS};
use nih_plug_egui::egui;
use std::sync::atomic::Ordering::Relaxed;

pub fn ui(ui: &mut egui::Ui, shared: &ChannelShared, state: &mut UiState) {
    if !state.name_synced {
        state.name_buf = shared.name.lock().unwrap().clone();
        state.name_synced = true;
    }

    ui.heading(format!("AutoTrim — {}", shared.display_name()));
    ui.add_space(8.0);

    ui.horizontal(|ui| {
        ui.label("Nome do canal:");
        if ui
            .add(egui::TextEdit::singleline(&mut state.name_buf).desired_width(200.0))
            .changed()
        {
            *shared.name.lock().unwrap() = state.name_buf.clone();
        }
    });
    ui.add_space(12.0);

    ui.label("Entrada (pré-trim):");
    super::peak_meter(ui, shared.peak_pre_trim.load(Relaxed), ui.available_width());
    ui.add_space(12.0);

    let mut target = shared.target_db.load(Relaxed);
    if ui
        .add(
            egui::Slider::new(&mut target, -60.0..=0.0)
                .suffix(" dBFS")
                .text("Target (pico)"),
        )
        .changed()
    {
        shared.target_db.store(target, Relaxed);
    }
    ui.add_space(8.0);

    let max_trim = GLOBALS.max_trim_db.load(Relaxed);
    ui.horizontal(|ui| {
        ui.label("Trim aplicado:");
        let mut trim = shared.trim_db.load(Relaxed);
        if ui
            .add(
                egui::DragValue::new(&mut trim)
                    .speed(0.1)
                    .range(-max_trim..=max_trim)
                    .suffix(" dB"),
            )
            .changed()
        {
            shared.trim_db.store(trim, Relaxed);
        }
        if shared.no_signal.load(Relaxed) {
            ui.colored_label(egui::Color32::YELLOW, "sem sinal na última medição");
        }
        if shared.measuring.load(Relaxed) {
            ui.colored_label(egui::Color32::LIGHT_BLUE, "medindo…");
        }
    });
    ui.add_space(12.0);

    let mut automation = shared.automation_on.load(Relaxed);
    if ui
        .checkbox(&mut automation, "Automação ligada (aplica o trim)")
        .changed()
    {
        shared.automation_on.store(automation, Relaxed);
    }

    let mut rider = shared.rider_on.load(Relaxed);
    if ui
        .checkbox(&mut rider, "Modo contínuo (rider): segue o target ao vivo")
        .changed()
    {
        shared.rider_on.store(rider, Relaxed);
    }

    ui.add_space(20.0);
    ui.separator();
    let mut panel_mode = shared.panel_mode.load(Relaxed);
    if ui
        .checkbox(
            &mut panel_mode,
            "Usar esta instância como painel de controle",
        )
        .changed()
    {
        shared.panel_mode.store(panel_mode, Relaxed);
    }
    ui.small("A instância em modo painel vira o centro de controle e sai da lista de canais.");
}
