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

pub enum DspStopReason {
    None,
    Done,
    CaptureError { message: String },
    PlaybackError { message: String },
    CaptureFormatChange { rate: u32 },
    PlaybackFormatChange { rate: u32 },
    UnknownError { message: String },
}

impl From<&StopReason> for DspStopReason {
    fn from(r: &StopReason) -> Self {
        match r {
            StopReason::None => DspStopReason::None,
            StopReason::Done => DspStopReason::Done,
            StopReason::CaptureError(msg) => DspStopReason::CaptureError {
                message: msg.clone(),
            },
            StopReason::PlaybackError(msg) => DspStopReason::PlaybackError {
                message: msg.clone(),
            },
            StopReason::CaptureFormatChange(rate) => {
                DspStopReason::CaptureFormatChange { rate: *rate as u32 }
            }
            StopReason::PlaybackFormatChange(rate) => {
                DspStopReason::PlaybackFormatChange { rate: *rate as u32 }
            }
            StopReason::UnknownError(msg) => DspStopReason::UnknownError {
                message: msg.clone(),
            },
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
    pub stop_reason: DspStopReason,
}

#[derive(Debug)]
pub enum DspError {
    ConfigParseError { message: String },
    CommandSendError { message: String },
    InvalidSamplerate { message: String },
    SpectrumComputeError { message: String },
}

impl std::fmt::Display for DspError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "{:?}", self)
    }
}

impl std::error::Error for DspError {}

pub struct DspSpectrum {
    pub frequencies: Vec<f32>,
    pub magnitudes: Vec<f32>,
}
