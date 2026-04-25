use camillalib::{ProcessingState, StopReason};

#[derive(Clone, Copy)]
pub enum DspState {
    Running,
    Paused,
    Inactive,
    Starting,
    Stalled,
}

impl From<ProcessingState> for DspState {
    fn from(s: ProcessingState) -> Self {
        match s {
            ProcessingState::Running => DspState::Running,
            ProcessingState::Paused => DspState::Paused,
            ProcessingState::Inactive => DspState::Inactive,
            ProcessingState::Starting => DspState::Starting,
            ProcessingState::Stalled => DspState::Stalled,
        }
    }
}

pub struct DspVuLevels {
    pub playback_rms: Vec<f32>,
    pub playback_peak: Vec<f32>,
    pub capture_rms: Vec<f32>,
    pub capture_peak: Vec<f32>,
}

pub struct DspStatus {
    pub state: DspState,
    pub stop_reason: String,
    pub stop_reason_rate: Option<u32>,
}

#[derive(Debug)]
pub enum DspError {
    Error,
}

impl std::fmt::Display for DspError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "{:?}", self)
    }
}

impl std::error::Error for DspError {}

/// Helper to convert StopReason to a string and extract rate
pub fn parse_stop_reason(reason: &StopReason) -> (String, Option<u32>) {
    let rate = match reason {
        StopReason::CaptureFormatChange(r) => Some(*r as u32),
        StopReason::PlaybackFormatChange(r) => Some(*r as u32),
        _ => None,
    };
    let text = match reason {
        StopReason::None => "None",
        StopReason::Done => "Done",
        StopReason::CaptureError(_) => "CaptureError",
        StopReason::PlaybackError(_) => "PlaybackError",
        StopReason::CaptureFormatChange(_) => "CaptureFormatChange",
        StopReason::PlaybackFormatChange(_) => "PlaybackFormatChange",
        StopReason::UnknownError(_) => "UnknownError",
    };
    (text.to_string(), rate)
}

pub struct DspSpectrum {
    pub frequencies: Vec<f32>,
    pub magnitudes: Vec<f32>,
}
