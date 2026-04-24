use crate::spectrum::SpectrumAnalyzer;
use camillalib::{
    audiodevice, audiodevice::AudioMessage, config, processing, CommandMessage, ControllerMessage,
    ExitState, ProcessingState, SharedConfigs, StatusMessage, StatusStructs, StopReason,
};
use crossbeam_channel::{bounded, never, select, unbounded, Receiver};
use parking_lot::{RwLock, RwLockUpgradableReadGuard};
use std::sync::{Arc, Barrier};

pub fn run_engine(
    shared_configs: SharedConfigs,
    status_structs: StatusStructs,
    rx_ctrl: Receiver<ControllerMessage>,
    spectrum_analyzer: Arc<RwLock<SpectrumAnalyzer>>,
) -> Result<ExitState, String> {
    let mut is_starting = true;
    let mut active_config = match shared_configs.active.lock().clone() {
        Some(cfg) => cfg,
        None => {
            return Ok(ExitState::Exit);
        }
    };

    let chunksize = active_config.devices.chunksize;
    let samplerate = active_config.devices.samplerate;
    let generation;

    // Explicitly reset analyzer with new format before starting threads
    {
        let mut sa = spectrum_analyzer.write();
        sa.reset(samplerate as u32, chunksize);
        generation = sa.generation;
    }

    let (tx_pb, rx_pb) = bounded(active_config.devices.queuelimit());
    let (tx_cap, rx_cap) = bounded(active_config.devices.queuelimit());
    let (tx_cap_raw, rx_cap_raw) = bounded(active_config.devices.queuelimit());

    let (tx_status, rx_status) = unbounded();
    let tx_status_pb = tx_status.clone();
    let tx_status_cap = tx_status;

    let (tx_command_cap, rx_command_cap) = unbounded();
    let (tx_pipeconf, rx_pipeconf) = unbounded();

    let barrier = Arc::new(Barrier::new(4));
    let barrier_pb = barrier.clone();
    let barrier_cap = barrier.clone();
    let barrier_proc = barrier.clone();

    // Spawn the tap proxy thread
    let tx_cap_clone = tx_cap.clone();
    let spectrum_analyzer_tap = spectrum_analyzer.clone();
    std::thread::spawn(move || {
        while let Ok(msg) = rx_cap_raw.recv() {
            if let AudioMessage::Audio(ref chunk) = msg {
                let channels = chunk.channels;
                let frames = chunk.valid_frames;
                let mut mono = Vec::with_capacity(frames);

                if chunk.waveforms.len() >= channels && channels >= 2 {
                    // Planar Stereo: sum L and R
                    for i in 0..frames {
                        let sum = (chunk.waveforms[0][i] + chunk.waveforms[1][i]) * 0.5;
                        mono.push(sum as f32);
                    }
                } else if chunk.waveforms.len() == 1 && channels >= 2 {
                    // Interleaved Stereo: sum L/R from single vector
                    let data = &chunk.waveforms[0];
                    for i in 0..frames {
                        let base = i * channels;
                        let sum = (data[base] + data[base + 1]) * 0.5;
                        mono.push(sum as f32);
                    }
                } else if !chunk.waveforms.is_empty() {
                    // Mono or other: take first channel/samples
                    let data = &chunk.waveforms[0];
                    for &sample in data.iter().take(frames) {
                        mono.push(sample as f32);
                    }
                }

                if !mono.is_empty() {
                    spectrum_analyzer_tap.write().add_samples(&mono, generation);
                }
            }
            if tx_cap_clone.send(msg).is_err() {
                break;
            }
        }
    });

    let conf_pb = active_config.clone();
    let conf_cap = active_config.clone();
    let conf_proc = active_config.clone();

    processing::run_processing(
        conf_proc,
        barrier_proc,
        tx_pb,
        rx_cap,
        rx_pipeconf,
        status_structs.processing.clone(),
    );

    let mut playback_dev = audiodevice::new_playback_device(conf_pb.devices);
    let pb_handle = playback_dev
        .start(rx_pb, barrier_pb, tx_status_pb, status_structs.playback)
        .map_err(|e| e.to_string())?;

    let used_channels = config::used_capture_channels(&active_config);
    {
        let mut capture_status = status_structs.capture.write();
        camillalib::update_capture_state(&mut capture_status, ProcessingState::Starting);
        capture_status.used_channels = used_channels;
    }

    let mut capture_dev = audiodevice::new_capture_device(conf_cap.devices);
    let cap_handle = capture_dev
        .start(
            tx_cap_raw,
            barrier_cap,
            tx_status_cap,
            rx_command_cap,
            status_structs.capture.clone(),
            status_structs.processing.clone(),
        )
        .map_err(|e| e.to_string())?;

    let mut pb_ready = false;
    let mut cap_ready = false;

    loop {
        let ctrl_ch = if is_starting {
            never()
        } else {
            rx_ctrl.clone()
        };
        select! {
            recv(ctrl_ch) -> msg  => {
                match msg {
                    Ok(ControllerMessage::ConfigChanged(new_conf)) => {
                        if !ctrl_ch.is_empty() {
                            continue;
                        }
                        status_structs.processing.set_processing_load(0.0);
                        status_structs.processing.set_resampler_load(0.0);
                        let comp = config::config_diff(&active_config, &new_conf);
                        match comp {
                            config::ConfigChange::Pipeline
                            | config::ConfigChange::MixerParameters
                            | config::ConfigChange::FilterParameters { .. } => {
                                tx_pipeconf.send((comp, *new_conf.clone())).unwrap();
                                active_config = *new_conf;
                                *shared_configs.active.lock() = Some(active_config.clone());
                                let used_channels = config::used_capture_channels(&active_config);
                                status_structs.capture.write().used_channels = used_channels;
                            }
                            config::ConfigChange::Devices => {
                                let _ = tx_command_cap.send(CommandMessage::Exit);
                                pb_handle.join().unwrap();
                                cap_handle.join().unwrap();
                                *shared_configs.active.lock() = Some(*new_conf);
                                return Ok(ExitState::Restart);
                            }
                            config::ConfigChange::None => {}
                        };
                    },
                    Ok(ControllerMessage::Stop) => {
                        let _ = tx_command_cap.send(CommandMessage::Exit);
                        pb_handle.join().unwrap();
                        cap_handle.join().unwrap();
                        {
                            let mut active_cfg_shared = shared_configs.active.lock();
                            let mut prev_cfg_shared = shared_configs.previous.lock();
                            *active_cfg_shared = None;
                            *prev_cfg_shared = Some(active_config);
                        }
                        return Ok(ExitState::Restart);
                    },
                    Ok(ControllerMessage::Exit) => {
                        let _ = tx_command_cap.send(CommandMessage::Exit);
                        pb_handle.join().unwrap();
                        cap_handle.join().unwrap();
                        *shared_configs.previous.lock() = Some(active_config);
                        return Ok(ExitState::Exit);
                    },
                    Err(err) => {
                        return Err(err.to_string());
                    }
                }
            },
            recv(rx_status) -> msg => {
                match msg {
                    Ok(msg) => match msg {
                        StatusMessage::PlaybackReady => {
                            pb_ready = true;
                            if cap_ready {
                                barrier.wait();
                                is_starting = false;
                            }
                        }
                        StatusMessage::CaptureReady => {
                            cap_ready = true;
                            if pb_ready {
                                barrier.wait();
                                is_starting = false;
                                camillalib::set_stop_reason(
                                    &status_structs.status,
                                    StopReason::None,
                                );
                            }
                        }
                        StatusMessage::PlaybackError(message) => {
                            let _ = tx_command_cap.send(CommandMessage::Exit);
                            if is_starting {
                                barrier.wait();
                            }
                            camillalib::set_stop_reason(
                                &status_structs.status,
                                StopReason::PlaybackError(message),
                            );
                            cap_handle.join().unwrap();
                            {
                                let mut active_cfg_shared = shared_configs.active.lock();
                                let mut prev_cfg_shared = shared_configs.previous.lock();
                                *active_cfg_shared = None;
                                *prev_cfg_shared = Some(active_config);
                            }
                            camillalib::set_capture_state(
                                &status_structs.capture,
                                ProcessingState::Inactive,
                            );
                            return Ok(ExitState::Restart);
                        }
                        StatusMessage::CaptureError(message) => {
                            if is_starting {
                                barrier.wait();
                            }
                            camillalib::set_stop_reason(
                                &status_structs.status,
                                StopReason::CaptureError(message),
                            );
                            pb_handle.join().unwrap();
                            {
                                let mut active_cfg_shared = shared_configs.active.lock();
                                let mut prev_cfg_shared = shared_configs.previous.lock();
                                *active_cfg_shared = None;
                                *prev_cfg_shared = Some(active_config);
                            }
                            camillalib::set_capture_state(
                                &status_structs.capture,
                                ProcessingState::Inactive,
                            );
                            return Ok(ExitState::Restart);
                        }
                        StatusMessage::PlaybackFormatChange(rate) => {
                            let _ = tx_command_cap.send(CommandMessage::Exit);
                            if is_starting {
                                barrier.wait();
                            }
                            camillalib::set_stop_reason(
                                &status_structs.status,
                                StopReason::PlaybackFormatChange(rate),
                            );
                            cap_handle.join().unwrap();
                            {
                                let mut active_cfg_shared = shared_configs.active.lock();
                                let mut prev_cfg_shared = shared_configs.previous.lock();
                                *active_cfg_shared = None;
                                *prev_cfg_shared = Some(active_config);
                            }
                            camillalib::set_capture_state(
                                &status_structs.capture,
                                ProcessingState::Inactive,
                            );
                            return Ok(ExitState::Restart);
                        }
                        StatusMessage::CaptureFormatChange(rate) => {
                            if is_starting {
                                barrier.wait();
                            }
                            camillalib::set_stop_reason(
                                &status_structs.status,
                                StopReason::CaptureFormatChange(rate),
                            );
                            pb_handle.join().unwrap();
                            {
                                let mut active_cfg_shared = shared_configs.active.lock();
                                let mut prev_cfg_shared = shared_configs.previous.lock();
                                *active_cfg_shared = None;
                                *prev_cfg_shared = Some(active_config);
                            }
                            camillalib::set_capture_state(
                                &status_structs.capture,
                                ProcessingState::Inactive,
                            );
                            return Ok(ExitState::Restart);
                        }
                        StatusMessage::PlaybackDone => {
                            {
                                let stat = status_structs.status.upgradable_read();
                                if stat.stop_reason == StopReason::None {
                                    camillalib::update_stop_reason(
                                        &mut RwLockUpgradableReadGuard::upgrade(stat),
                                        StopReason::Done,
                                    );
                                }
                            }
                            {
                                let mut active_cfg_shared = shared_configs.active.lock();
                                let mut prev_cfg_shared = shared_configs.previous.lock();
                                *active_cfg_shared = None;
                                *prev_cfg_shared = Some(active_config);
                            }
                            pb_handle.join().unwrap();
                            cap_handle.join().unwrap();
                            return Ok(ExitState::Restart);
                        }
                        StatusMessage::CaptureDone => {}
                        StatusMessage::SetSpeed(speed) => {
                            let _ = tx_command_cap.send(CommandMessage::SetSpeed { speed });
                        }
                        StatusMessage::SetVolume(vol) => {
                            status_structs.processing.set_target_volume(0, vol);
                        }
                        StatusMessage::SetMute(mute) => {
                            status_structs.processing.set_mute(0, mute);
                        }
                    },
                    Err(_) => {
                        camillalib::set_stop_reason(
                            &status_structs.status,
                            StopReason::UnknownError(
                                "Capture, Playback and Processing threads have exited"
                                    .to_string(),
                            ),
                        );
                        camillalib::set_capture_state(
                            &status_structs.capture,
                            ProcessingState::Inactive,
                        );
                        return Ok(ExitState::Restart);
                    }
                }
            }
        }
    }
}
