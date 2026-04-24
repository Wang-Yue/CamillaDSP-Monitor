use camillalib::{
    config, utils::countertimer, ControllerMessage, ExitState, ProcessingState, SharedConfigs,
    StatusStructs, StopReason,
};
use crossbeam_channel::{bounded, Sender};
use log::debug;
use parking_lot::{Mutex, RwLock};
use std::sync::Arc;

mod engine;
mod spectrum;
mod types;

use engine::run_engine;
use spectrum::SpectrumAnalyzer;
pub use types::{DspError, DspState, DspStatus, DspVuLevels};

pub struct CamillaEngine {
    tx_command: Sender<ControllerMessage>,
    status_structs: StatusStructs,
    spectrum_analyzer: Arc<RwLock<SpectrumAnalyzer>>,
}

impl Default for CamillaEngine {
    fn default() -> Self {
        Self::new()
    }
}

impl CamillaEngine {
    pub fn new() -> Self {
        let (tx_command, rx_command) = bounded(10);
        let spectrum_analyzer = Arc::new(RwLock::new(SpectrumAnalyzer::new()));

        let capture_status = Arc::new(RwLock::new(camillalib::CaptureStatus {
            measured_samplerate: 0,
            update_interval: 1000,
            signal_range: 0.0,
            rate_adjust: 0.0,
            state: ProcessingState::Inactive,
            signal_rms: countertimer::ValueHistory::new(1024, 2),
            signal_peak: countertimer::ValueHistory::new(1024, 2),
            used_channels: Vec::new(),
        }));
        let playback_status = Arc::new(RwLock::new(camillalib::PlaybackStatus {
            buffer_level: 0,
            clipped_samples: 0,
            update_interval: 1000,
            signal_rms: countertimer::ValueHistory::new(1024, 2),
            signal_peak: countertimer::ValueHistory::new(1024, 2),
        }));
        let processing_params = Arc::new(camillalib::ProcessingParameters::default());
        let processing_status = Arc::new(RwLock::new(camillalib::ProcessingStatus {
            stop_reason: StopReason::None,
        }));

        let status_structs = StatusStructs {
            capture: capture_status.clone(),
            playback: playback_status.clone(),
            processing: processing_params.clone(),
            status: processing_status.clone(),
        };

        let engine = CamillaEngine {
            tx_command: tx_command.clone(),
            status_structs: status_structs.clone(),
            spectrum_analyzer: spectrum_analyzer.clone(),
        };

        let status_structs_clone = status_structs.clone();
        let spectrum_analyzer_clone = spectrum_analyzer;
        std::thread::spawn(move || {
            let active_config = Arc::new(Mutex::new(None));
            let previous_config = Arc::new(Mutex::new(None));

            loop {
                debug!("FFI Engine: Wait for config");
                loop {
                    let has_config = (*active_config.lock()).is_some();
                    if has_config && rx_command.is_empty() {
                        break;
                    }
                    match rx_command.recv() {
                        Ok(ControllerMessage::ConfigChanged(new_conf)) => {
                            *active_config.lock() = Some(*new_conf);
                            camillalib::set_stop_reason(
                                &status_structs_clone.status,
                                StopReason::None,
                            );
                        }
                        Ok(ControllerMessage::Stop) => {
                            *active_config.lock() = None;
                        }
                        Ok(ControllerMessage::Exit) => return,
                        Err(_) => return,
                    }
                }

                let shared_configs = SharedConfigs {
                    active: active_config.clone(),
                    previous: previous_config.clone(),
                };

                let exitstatus = run_engine(
                    shared_configs,
                    status_structs_clone.clone(),
                    rx_command.clone(),
                    spectrum_analyzer_clone.clone(),
                );
                debug!("FFI Engine: Processing ended with status {:?}", exitstatus);

                if let Ok(ExitState::Exit) = exitstatus { return }
            }
        });

        engine
    }

    pub fn set_config(&self, json: String) -> Result<(), DspError> {
        let conf: config::Configuration =
            serde_json::from_str(&json).map_err(|_| DspError::Error)?;
        self.tx_command
            .send(ControllerMessage::ConfigChanged(Box::new(conf)))
            .map_err(|_| DspError::Error)
    }

    pub fn stop(&self) {
        let _ = self.tx_command.send(ControllerMessage::Stop);
    }

    pub fn set_volume(&self, fader: u32, volume: f32) {
        self.status_structs
            .processing
            .set_target_volume(fader as usize, volume);
    }

    pub fn set_mute(&self, fader: u32, mute: bool) {
        self.status_structs
            .processing
            .set_mute(fader as usize, mute);
    }

    pub fn get_status(&self) -> DspStatus {
        let cap = self.status_structs.capture.read();
        let stat = self.status_structs.status.read();
        let (stop_reason, stop_reason_rate) = types::parse_stop_reason(&stat.stop_reason);
        DspStatus {
            state: DspState::from(cap.state),
            stop_reason,
            stop_reason_rate,
        }
    }

    pub fn get_vu_levels(&self) -> DspVuLevels {
        let pb = self.status_structs.playback.read();
        let cap = self.status_structs.capture.read();

        let to_db = |linear: f32| camillalib::utils::decibels::linear_to_db(linear);

        let pb_rms = pb
            .signal_rms
            .last_sqrt()
            .map(|r| r.values.iter().map(|&v| to_db(v)).collect::<Vec<f32>>())
            .unwrap_or_default();
        let pb_peak = pb
            .signal_peak
            .last()
            .map(|r| r.values.iter().map(|&v| to_db(v)).collect::<Vec<f32>>())
            .unwrap_or_default();
        let cap_rms = cap
            .signal_rms
            .last_sqrt()
            .map(|r| r.values.iter().map(|&v| to_db(v)).collect::<Vec<f32>>())
            .unwrap_or_default();
        let cap_peak = cap
            .signal_peak
            .last()
            .map(|r| r.values.iter().map(|&v| to_db(v)).collect::<Vec<f32>>())
            .unwrap_or_default();

        DspVuLevels {
            playback_rms: pb_rms,
            playback_peak: pb_peak,
            capture_rms: cap_rms,
            capture_peak: cap_peak,
        }
    }

    pub fn get_available_devices(&self, backend: String, input: bool) -> Vec<String> {
        camillalib::list_available_devices(&backend, input)
            .into_iter()
            .map(|(name, _)| name)
            .collect()
    }

    pub fn get_device_capabilities(&self, backend: String, device: String, input: bool) -> String {
        match camillalib::get_device_capabilities(&backend, &device, input) {
            Ok(caps) => serde_json::to_string(&caps).unwrap_or_default(),
            Err(e) => format!("Error: {:?}", e),
        }
    }

    pub fn get_spectrum_bands(&self) -> Vec<f32> {
        self.spectrum_analyzer.write().compute_spectrum()
    }
}

uniffi::include_scaffolding!("api");
