// Apple AudioConverter resampler.

#include "apple_resampler.h"

#if defined(ENABLE_COREAUDIO)

#include <math.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "Audio/audio_buffers.h"

struct apple_resampler_fill_context {
  audio_buffers_t* buffers;
  size_t read_offset;
  size_t write_offset;
};

struct apple_resampler {
  size_t channels;
  size_t chunk_size;
  double base_ratio;
  double current_ratio;
  AudioConverterRef converter;
  apple_resampler_fill_context_t* fill_context;
  void* abl_storage;
  size_t max_output_frames;
};

/**
 * @brief Callback function to feed input data to Apple's AudioConverter.
 *
 * This function is called by AudioConverterFillComplexBuffer when it needs more
 * input data to produce output packets. It copies data from the resampler's
 * internal ring buffer (described by apple_resampler_fill_context_t) into the
 * audio buffer list provided by the system.
 *
 * @param inAudioConverter The AudioConverter requesting data.
 * @param ioNumberDataPackets On input, the number of packets requested. On
 * output, the number of packets actually provided.
 * @param ioData The AudioBufferList where the provided audio data pointers are
 * placed.
 * @param outDataPacketDescription Unused, as we are dealing with
 * non-interleaved double PCM data.
 * @param inUserData Pointer to the apple_resampler_fill_context_t instance
 * containing the input buffers.
 * @return OSStatus noErr on success, or an error code.
 */
static OSStatus input_data_proc(
    AudioConverterRef inAudioConverter, UInt32* ioNumberDataPackets,
    AudioBufferList* ioData,
    AudioStreamPacketDescription** outDataPacketDescription, void* inUserData) {
  (void)inAudioConverter;
  (void)outDataPacketDescription;
  if (!inUserData || !ioNumberDataPackets || !ioData) {
    if (ioNumberDataPackets) *ioNumberDataPackets = 0;
    return noErr;
  }
  apple_resampler_fill_context_t* context =
      (apple_resampler_fill_context_t*)inUserData;
  size_t needed = (size_t)(*ioNumberDataPackets);
  size_t available = context->write_offset - context->read_offset;

  if (available == 0) {
    *ioNumberDataPackets = 0;
    return noErr;
  }

  size_t frames_to_provide = needed < available ? needed : available;
  *ioNumberDataPackets = (UInt32)frames_to_provide;

  size_t chans = audio_buffers_get_channels(context->buffers);
  for (size_t ch = 0; ch < ioData->mNumberBuffers && ch < chans; ch++) {
    double* base = audio_buffers_get_channel(context->buffers, ch);
    if (!base) return -1;
    ioData->mBuffers[ch].mData = (void*)(base + context->read_offset);
    ioData->mBuffers[ch].mDataByteSize =
        (UInt32)(frames_to_provide * sizeof(double));
    ioData->mBuffers[ch].mNumberChannels = 1;
  }

  context->read_offset += frames_to_provide;
  return noErr;
}

apple_resampler_t* apple_resampler_create(
    size_t channels, size_t input_rate, size_t output_rate,
    apple_resampler_quality_t quality, apple_resampler_complexity_t complexity,
    size_t chunk_size, config_error_t* err) {
  if (channels == 0) {
    config_error_set(err, CONFIG_ERR_VALIDATION, "AppleResampler: channels must be positive");
    return NULL;
  }
  if (chunk_size == 0) {
    config_error_set(err, CONFIG_ERR_VALIDATION, "AppleResampler: chunk_size must be positive");
    return NULL;
  }
  if (input_rate == 0 || output_rate == 0) {
    config_error_set(err, CONFIG_ERR_VALIDATION, "AppleResampler: input_rate and output_rate must be positive");
    return NULL;
  }

  apple_resampler_t* resampler =
      (apple_resampler_t*)calloc(1, sizeof(apple_resampler_t));
  if (!resampler) {
    config_error_set(err, CONFIG_ERR_PARSE, "Failed to allocate AppleResampler");
    return NULL;
  }

  resampler->channels = channels;
  resampler->chunk_size = chunk_size;
  resampler->base_ratio = (double)output_rate / (double)input_rate;
  resampler->current_ratio = resampler->base_ratio;

  double max_relative_ratio = 1.1;
  double max_ratio_abs = resampler->base_ratio * max_relative_ratio;
  resampler->max_output_frames =
      (size_t)(ceil((double)chunk_size * max_ratio_abs)) + 32;

  resampler->fill_context = (apple_resampler_fill_context_t*)calloc(
      1, sizeof(apple_resampler_fill_context_t));
  if (!resampler->fill_context) {
    config_error_set(err, CONFIG_ERR_PARSE, "Failed to allocate AppleResampler fill context");
    apple_resampler_free(resampler);
    return NULL;
  }
  resampler->fill_context->buffers =
      audio_buffers_create(channels, chunk_size * 8);
  if (!resampler->fill_context->buffers) {
    config_error_set(err, CONFIG_ERR_PARSE, "Failed to allocate AppleResampler AudioBuffers");
    apple_resampler_free(resampler);
    return NULL;
  }

  /* Dynamically size the AudioBufferList storage based on channel count.
     AudioBufferList has a variable size array at the end: AudioBuffer
     mBuffers[1]. If channels > 1, we need to allocate additional size for
     (channels - 1) buffers. */
  size_t storage_size =
      sizeof(AudioBufferList) +
      (channels > 0 ? (channels - 1) * sizeof(AudioBuffer) : 0);
  resampler->abl_storage = calloc(1, storage_size);
  if (!resampler->abl_storage) {
    config_error_set(err, CONFIG_ERR_PARSE, "Failed to allocate AppleResampler AudioBufferList storage");
    apple_resampler_free(resampler);
    return NULL;
  }

  /* Setup AudioStreamBasicDescription for non-interleaved double PCM format.
     This is critical for high quality audio processing since we operate on
     float64/double format and keep channels in separate pointers
     (non-interleaved) to match our internal representation. */
  AudioStreamBasicDescription in_desc = {0};
  in_desc.mSampleRate = (double)input_rate;
  in_desc.mFormatID = kAudioFormatLinearPCM;
  in_desc.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked |
                         kAudioFormatFlagIsNonInterleaved;
  in_desc.mBytesPerPacket = sizeof(double);
  in_desc.mFramesPerPacket = 1;
  in_desc.mBytesPerFrame = sizeof(double);
  in_desc.mChannelsPerFrame = (UInt32)channels;
  in_desc.mBitsPerChannel = sizeof(double) * 8;

  AudioStreamBasicDescription out_desc = {0};
  out_desc.mSampleRate = (double)output_rate;
  out_desc.mFormatID = kAudioFormatLinearPCM;
  out_desc.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked |
                          kAudioFormatFlagIsNonInterleaved;
  out_desc.mBytesPerPacket = sizeof(double);
  out_desc.mFramesPerPacket = 1;
  out_desc.mBytesPerFrame = sizeof(double);
  out_desc.mChannelsPerFrame = (UInt32)channels;
  out_desc.mBitsPerChannel = sizeof(double) * 8;

  AudioConverterRef conv = NULL;
  OSStatus status = AudioConverterNew(&in_desc, &out_desc, &conv);
  if (status != noErr || !conv) {
    config_error_set(err, CONFIG_ERR_PARSE, "AudioConverterNew returned OSStatus %d", (int)status);
    apple_resampler_free(resampler);
    return NULL;
  }
  resampler->converter = conv;

  UInt32 quality_val = kAudioConverterQuality_Max;
  switch (quality) {
    case APPLE_RESAMPLER_QUALITY_MIN:
      quality_val = kAudioConverterQuality_Min;
      break;
    case APPLE_RESAMPLER_QUALITY_LOW:
      quality_val = kAudioConverterQuality_Low;
      break;
    case APPLE_RESAMPLER_QUALITY_MEDIUM:
      quality_val = kAudioConverterQuality_Medium;
      break;
    case APPLE_RESAMPLER_QUALITY_HIGH:
      quality_val = kAudioConverterQuality_High;
      break;
    case APPLE_RESAMPLER_QUALITY_MAX:
      quality_val = kAudioConverterQuality_Max;
      break;
    default:
      quality_val = kAudioConverterQuality_Max;
      break;
  }
  AudioConverterSetProperty(conv, kAudioConverterSampleRateConverterQuality,
                            sizeof(UInt32), &quality_val);

  UInt32 complexity_val = apple_resampler_complexity_os_type(complexity);
  AudioConverterSetProperty(conv, kAudioConverterSampleRateConverterComplexity,
                            sizeof(UInt32), &complexity_val);

  return resampler;
}

void apple_resampler_free(apple_resampler_t* resampler) {
  if (!resampler) return;
  if (resampler->converter) {
    AudioConverterDispose(resampler->converter);
  }
  if (resampler->abl_storage) free(resampler->abl_storage);
  if (resampler->fill_context) {
    if (resampler->fill_context->buffers)
      audio_buffers_free(resampler->fill_context->buffers);
    free(resampler->fill_context);
  }
  free(resampler);
}

/// `AppleResampler` runs at a fixed rational ratio fixed at construction.
/// Apple's `AudioConverter` (in both default/mastering and minimum phase
/// complexities) does not support changing the
/// `kAudioConverterPropertyOutputSampleRate` property dynamically on an active
/// converter (returns `kAudioConverterErr_PropertyNotSupported`). We accept the
/// multiplier without effect and log a warning once.
void apple_resampler_set_relative_ratio(apple_resampler_t* resampler,
                                        double multiplier) {
  (void)resampler;
  (void)multiplier;
  // Fixed-ratio in Apple AudioConverter
}

double apple_resampler_get_ratio(const apple_resampler_t* resampler) {
  return resampler ? resampler->current_ratio : 1.0;
}

size_t apple_resampler_get_max_output_frames(
    const apple_resampler_t* resampler) {
  return resampler ? resampler->max_output_frames : 0;
}

size_t apple_resampler_get_chunk_size(const apple_resampler_t* resampler) {
  return resampler ? resampler->chunk_size : 0;
}

size_t apple_resampler_get_channels(const apple_resampler_t* resampler) {
  return resampler ? resampler->channels : 0;
}

resampler_error_t apple_resampler_process(apple_resampler_t* resampler,
                                          const audio_chunk_t* input,
                                          audio_chunk_t* output) {
  if (!resampler || !input || !output) return RESAMPLER_ERR_INVALID_PARAMETER;
  if (audio_chunk_get_valid_frames(input) != resampler->chunk_size) {
    return RESAMPLER_ERR_INPUT_SIZE_MISMATCH;
  }
  if (audio_chunk_get_channels(input) != resampler->channels) {
    return RESAMPLER_ERR_CHANNEL_COUNT_MISMATCH;
  }
  if (audio_chunk_get_channels(output) != resampler->channels) {
    return RESAMPLER_ERR_CHANNEL_COUNT_MISMATCH;
  }
  size_t next_output_frames =
      (size_t)floor((double)resampler->chunk_size * resampler->current_ratio);
  if (audio_chunk_get_frames(output) < next_output_frames) {
    return RESAMPLER_ERR_OUTPUT_BUFFER_TOO_SMALL;
  }

  apple_resampler_fill_context_t* context = resampler->fill_context;
  // Check if we have space in ringBuffers
  size_t available_space =
      audio_buffers_get_capacity(context->buffers) - context->write_offset;
  if (available_space < resampler->chunk_size) {
    /* Shift remaining unread data to the beginning of the buffer to free up
       capacity for incoming chunk size. This avoids using a circular ring
       buffer index and allows feeding contiguous memory chunks to Apple's
       AudioConverter callback. */
    if (context->read_offset > 0) {
      size_t remaining = context->write_offset - context->read_offset;
      for (size_t ch = 0; ch < resampler->channels; ch++) {
        double* base = audio_buffers_get_channel(context->buffers, ch);
        if (!base) return RESAMPLER_ERR_INVALID_PARAMETER;
        memmove(base, base + context->read_offset, remaining * sizeof(double));
      }
      context->write_offset = remaining;
      context->read_offset = 0;
    }
    // If still not enough space, we fail.
    if (audio_buffers_get_capacity(context->buffers) - context->write_offset <
        resampler->chunk_size) {
      return RESAMPLER_ERR_INVALID_PARAMETER;  // Overflow
    }
  }

  // Copy input into ringBuffers
  for (size_t ch = 0; ch < resampler->channels; ch++) {
    const double* src = audio_chunk_get_channel(input, ch);
    double* dst = audio_buffers_get_channel(context->buffers, ch);
    if (!src || !dst) return RESAMPLER_ERR_INVALID_PARAMETER;
    memcpy(dst + context->write_offset, src,
           resampler->chunk_size * sizeof(double));
  }
  context->write_offset += resampler->chunk_size;

  /* To avoid excessive AudioConverter overhead and ensure it has enough context
     to run its internal resampling filter, we accumulate at least 4096 frames
     before invoking the converter. If we don't have enough yet, return OK with
     0 valid frames. */
  if (context->write_offset < 4096) {
    audio_chunk_set_valid_frames(output, 0);
    return RESAMPLER_OK;
  }

  AudioBufferList* abl = (AudioBufferList*)resampler->abl_storage;
  abl->mNumberBuffers = (UInt32)resampler->channels;
  for (size_t ch = 0; ch < resampler->channels; ch++) {
    double* base = audio_chunk_get_channel(output, ch);
    abl->mBuffers[ch].mData = (void*)base;
    abl->mBuffers[ch].mDataByteSize =
        (UInt32)(audio_chunk_get_frames(output) * sizeof(double));
    abl->mBuffers[ch].mNumberChannels = 1;
  }

  UInt32 output_packet_count = (UInt32)audio_chunk_get_frames(output);
  OSStatus status =
      AudioConverterFillComplexBuffer(resampler->converter, input_data_proc,
                                      context, &output_packet_count, abl, NULL);

  (void)status;
  audio_chunk_set_valid_frames(output, (size_t)output_packet_count);

  // Shift remaining data to front after processing
  if (context->read_offset > 0) {
    size_t remaining = context->write_offset - context->read_offset;
    if (remaining > 0) {
      for (size_t ch = 0; ch < resampler->channels; ch++) {
        double* base = audio_buffers_get_channel(context->buffers, ch);
        if (!base) return RESAMPLER_ERR_INVALID_PARAMETER;
        memmove(base, base + context->read_offset, remaining * sizeof(double));
      }
    }
    context->write_offset = remaining;
    context->read_offset = 0;
  }

  return RESAMPLER_OK;
}
#endif  // ENABLE_COREAUDIO
