// Playback thread body. Drains the processing→playback SPSC queue
// and writes each chunk to the playback backend. Also runs the
// rate-adjust control loop: averages the (device-ring + queued-chunks)
// fill level, and once per `adjustPeriod` seconds feeds the average
// to `PIRateController`.
//
// State ownership
// ---------------
// The rate-adjust state — controller, averager, stopwatch, last
// published speed — is local to this loop. The output speed is
// applied either directly to the capture clock (when the capture
// device exposes a tunable clock — BlackHole 0.5.0+) or published
// via `shared.resamplerRatio` so the processing thread picks it up
// on its next chunk.
//
// Audio-thread invariants
// -----------------------
//   * No allocations in the steady state. The controller and
//     averager are constructed once at init; the stopwatch is a
//     plain UInt64 nanosecond timestamp.
//   * No locks. The shared SPSC queue + semaphore carries chunks
//     and wakeups.
//   * The rate-adjust info logger fires at most once per
//     `adjustPeriod` (~10 s default), so its formatting cost is
//     negligible per chunk.

#include "engine_playback_loop.h"
#include "thread_priority.h"
#include "Logging/app_logger.h"
#include <stdlib.h>
#include <stdio.h>
#include <math.h>

/// Apply a rate-adjust output. Skip if the speed change is
/// negligible (< 1 ppm) so we don't churn the resampler ratio
/// pointlessly.
static void apply_speed(engine_playback_loop_t* loop, double speed, double* last_speed, double average) {
    bool changed = fabs(speed - *last_speed) > 0.000001;
    logger_t logger = logger_create("dsp.playback");
    if (changed) {
        *last_speed = speed;
        if (loop->pitch_supported && loop->capture) {
            capture_backend_set_pitch(loop->capture, speed);
        } else if (loop->shared && loop->shared->resampler_ratio) {
            atomic_double_set(loop->shared->resampler_ratio, speed);
        }
        const char* method_str = loop->pitch_supported ? "pitch" : "resampler";
        logger_info(&logger, "Rate adjust: buffer=%f target=%d speed=%f via %s",
                    log_arg_double(average), log_arg_int((int64_t)loop->target_level), log_arg_double(speed), log_arg_string(method_str));
    } else {
        logger_debug(&logger, "Rate adjust: buffer=%f, keeping speed=%f",
                     log_arg_double(average), log_arg_double(*last_speed), log_arg_none(), log_arg_none());
    }
}

static void log_rate_adjust_mode(engine_playback_loop_t* loop) {
    logger_t logger = logger_create("dsp.playback");
    if (loop->rate_adjust_enabled) {
        const char* method_str = loop->pitch_supported ? "capture clock pitch" : "resampler ratio";
        logger_info(&logger, "Rate adjustment enabled (period=%fs, target_level=%d, method=%s)",
                    log_arg_double(loop->adjust_period), log_arg_int((int64_t)loop->target_level), log_arg_string(method_str), log_arg_none());
    } else {
        logger_info(&logger, "Rate adjustment disabled (enable_rate_adjust not set in config)",
                    log_arg_none(), log_arg_none(), log_arg_none(), log_arg_none());
    }
}

engine_playback_loop_t* engine_playback_loop_create(
    engine_shared_state_t* shared,
    capture_backend_t* capture,
    playback_backend_t* playback,
    processing_parameters_t* processing_params,
    size_t pipeline_rate,
    size_t chunk_size,
    bool rate_adjust_enabled,
    double adjust_period,
    int target_level,
    engine_stop_callback_t on_stop,
    void* on_stop_ctx
) {
    engine_playback_loop_t* loop = (engine_playback_loop_t*)calloc(1, sizeof(engine_playback_loop_t));
    if (!loop) return NULL;
    loop->shared = shared;
    loop->capture = capture;
    loop->playback = playback;
    loop->processing_params = processing_params;
    loop->pipeline_rate = pipeline_rate;
    loop->chunk_size = chunk_size;
    loop->pitch_supported = capture ? capture_backend_pitch_control_supported(capture) : false;
    loop->rate_adjust_enabled = rate_adjust_enabled;
    loop->adjust_period = adjust_period;
    loop->target_level = target_level;
    loop->on_stop = on_stop;
    loop->on_stop_ctx = on_stop_ctx;
    return loop;
}

void engine_playback_loop_free(engine_playback_loop_t* loop) {
    if (!loop) return;
    free(loop);
}

void engine_playback_loop_run(engine_playback_loop_t* loop) {
    if (!loop) return;
    logger_t logger = logger_create("dsp.playback");
    logger_info(&logger, "Playback thread started", log_arg_none(), log_arg_none(), log_arg_none(), log_arg_none());

    set_realtime_thread_priority("Playback", loop->chunk_size, loop->pipeline_rate);
    log_rate_adjust_mode(loop);

    // Rate-adjust state lives entirely on this thread.
    pi_rate_controller_t* rate_controller = NULL;
    averager_t averager;
    stopwatch_t stopwatch;
    double last_speed = 1.0;

    if (loop->rate_adjust_enabled) {
        rate_controller = pi_rate_controller_create_default((int)loop->pipeline_rate, loop->adjust_period, loop->target_level);
        averager_init(&averager);
        stopwatch_init(&stopwatch);
        stopwatch_restart(&stopwatch);
    }

    while (!atomic_load_explicit(&loop->shared->should_stop, memory_order_acquire)) {
        engine_sem_wait(loop->shared->processed_semaphore);
        if (atomic_load_explicit(&loop->shared->should_stop, memory_order_acquire)) break;

        audio_chunk_t* chunk = NULL;
        while ((chunk = (audio_chunk_t*)spsc_queue_dequeue(loop->shared->processed_queue)) != NULL) {
            if (atomic_load_explicit(&loop->shared->should_stop, memory_order_acquire)) {
                if (rate_controller) pi_rate_controller_free(rate_controller);
                return;
            }

            size_t ring_fill = playback_backend_get_buffer_level(loop->playback);
            size_t queued_frames = spsc_queue_get_count(loop->shared->processed_queue) * loop->chunk_size;
            if (loop->processing_params) {
                atomic_double_set(&loop->processing_params->buffer_level, (double)(ring_fill + queued_frames));
            }

            if (loop->rate_adjust_enabled && rate_controller) {
                averager_add(&averager, (double)(ring_fill + queued_frames));

                if (stopwatch_elapsed_seconds(&stopwatch) >= loop->adjust_period) {
                    double avg = 0.0;
                    if (averager_get_average(&averager, &avg)) {
                        double speed = pi_rate_controller_next(rate_controller, avg);
                        stopwatch_restart(&stopwatch);
                        averager_restart(&averager);
                        apply_speed(loop, speed, &last_speed, avg);
                        if (loop->processing_params) {
                            atomic_double_set(&loop->processing_params->rate_adjust, speed);
                        }
                    }
                }
            }

            backend_error_t err;
            backend_error_init(&err, BACKEND_ERROR_NONE, "");
            bool ok = playback_backend_write(loop->playback, chunk, &err);
            if (!ok || err.type != BACKEND_ERROR_NONE) {
                logger_error(&logger, "Playback error: %s", log_arg_string(err.message), log_arg_none(), log_arg_none(), log_arg_none());
                processing_stop_reason_t reason = { .type = STOP_REASON_PLAYBACK_ERROR };
                snprintf(reason.message, sizeof(reason.message), "%s", err.message);
                if (loop->on_stop) loop->on_stop(loop->on_stop_ctx, reason);
                if (rate_controller) pi_rate_controller_free(rate_controller);
                return;
            }
        }
    }

    if (rate_controller) pi_rate_controller_free(rate_controller);
    logger_info(&logger, "Playback thread stopped", log_arg_none(), log_arg_none(), log_arg_none(), log_arg_none());
}
