use camillalib::{config, ControllerMessage, ExitState, SharedConfigs, StatusStructs, StopReason};
use crossbeam_channel::{bounded, Sender};
use log::debug;
use parking_lot::Mutex;
use std::sync::Arc;

mod types;
pub use types::{DspError, DspSpectrum, DspState, DspStatus, DspStopReason, DspVuLevels};

pub struct CamillaEngine {
    tx_command: Sender<ControllerMessage>,
    status_structs: StatusStructs,
    active_config: Arc<Mutex<Option<config::Configuration>>>,
}

impl Default for CamillaEngine {
    fn default() -> Self {
        Self::new()
    }
}

impl CamillaEngine {
    pub fn new() -> Self {
        let _ =
            env_logger::Builder::from_env(env_logger::Env::default().default_filter_or("trace"))
                .try_init();
        log::set_max_level(log::LevelFilter::Info);

        let (tx_command, rx_command) = bounded(10);
        let active_config = Arc::new(Mutex::new(None));

        let status_structs = StatusStructs::default();

        let engine = CamillaEngine {
            tx_command: tx_command.clone(),
            status_structs: status_structs.clone(),
            active_config: active_config.clone(),
        };

        let status_structs_clone = status_structs.clone();
        std::thread::spawn(move || {
            let previous_config = Arc::new(Mutex::new(None));

            loop {
                log::trace!("FFI Engine: Wait for config");
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

                let exitstatus = camillalib::engine::run(
                    shared_configs,
                    status_structs_clone.clone(),
                    rx_command.clone(),
                );
                debug!("FFI Engine: Processing ended with status {:?}", exitstatus);

                if let Ok(ExitState::Exit) = exitstatus {
                    return;
                }
            }
        });

        engine
    }

    pub fn set_config(&self, json: String) -> Result<(), DspError> {
        log::info!("FFI Engine: set_config with JSON: {}", json);
        let conf: config::Configuration =
            serde_json::from_str(&json).map_err(|_| DspError::ConfigParseError)?;
        self.tx_command
            .send(ControllerMessage::ConfigChanged(Box::new(conf)))
            .map_err(|_| DspError::CommandSendError)
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
        DspStatus {
            state: DspState::from(cap.state),
            stop_reason: types::DspStopReason::from(&stat.stop_reason),
        }
    }

    pub fn get_vu_levels(&self) -> DspVuLevels {
        let pb = self.status_structs.playback.read();
        let cap = self.status_structs.capture.read();

        macro_rules! get_db {
            ($opt:expr) => {
                $opt.map(|r| {
                    r.values
                        .iter()
                        .map(|&v| camillalib::utils::decibels::linear_to_db(v))
                        .collect::<Vec<f32>>()
                })
                .unwrap_or_default()
            };
        }

        DspVuLevels {
            playback_rms: get_db!(pb.signal_rms.last_sqrt()),
            playback_peak: get_db!(pb.signal_peak.last()),
            capture_rms: get_db!(cap.signal_rms.last_sqrt()),
            capture_peak: get_db!(cap.signal_peak.last()),
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

    pub fn get_spectrum(
        &self,
        side: String,
        channel: Option<u32>,
        min_freq: f64,
        max_freq: f64,
        n_bins: u32,
    ) -> Result<types::DspSpectrum, DspError> {
        let samplerate = self
            .active_config
            .lock()
            .as_ref()
            .map(|c| c.devices.samplerate)
            .unwrap_or(0);
        if samplerate == 0 {
            return Err(DspError::InvalidSamplerate);
        }

        let channel = channel.map(|c| c as usize);
        let n_bins = n_bins as usize;

        macro_rules! compute {
            ($status:expr) => {
                camillalib::spectrum::compute_spectrum(
                    &$status.audio_buffer,
                    min_freq,
                    max_freq,
                    n_bins,
                    channel,
                    samplerate,
                )
            };
        }

        let data = match side.as_str() {
            "capture" => compute!(self.status_structs.capture.read()),
            "playback" => compute!(self.status_structs.playback.read()),
            _ => return Err(DspError::InvalidSide),
        }
        .map_err(|_| DspError::SpectrumComputeError)?;

        Ok(types::DspSpectrum {
            frequencies: data.frequencies.to_vec(),
            magnitudes: data.magnitudes,
        })
    }

    pub fn set_log_level(&self, level: String) {
        let filter = match level.to_lowercase().as_str() {
            "off" => log::LevelFilter::Off,
            "error" => log::LevelFilter::Error,
            "warn" => log::LevelFilter::Warn,
            "info" => log::LevelFilter::Info,
            "debug" => log::LevelFilter::Debug,
            "trace" => log::LevelFilter::Trace,
            _ => log::LevelFilter::Info,
        };
        log::set_max_level(filter);
    }
}

uniffi::include_scaffolding!("api");
