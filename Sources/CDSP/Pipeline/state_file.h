/**
 * @file state_file.h
 * @brief DSP state storage and loading.
 *
 * This module manages loading and saving the DSP state (such as config path,
 * volume, and mute status) to/from a file.
 */

#ifndef CLIB_PIPELINE_STATE_FILE_H
#define CLIB_PIPELINE_STATE_FILE_H

#include <stdbool.h>

struct dsp_state_s;
/**
 * @brief Opaque structure representing the DSP state.
 */
typedef struct dsp_state_s dsp_state_t;

/**
 * @brief Create a new DSP state instance.
 *
 * @return Pointer to the created dsp_state_t, or NULL on failure.
 */
dsp_state_t* dsp_state_create(void);

/**
 * @brief Free the DSP state instance.
 *
 * @param[in] state The DSP state instance to free.
 */
void dsp_state_free(dsp_state_t* state);

/**
 * @brief Load DSP state from a file.
 *
 * @param[in] filename The path to the state file.
 * @param[out] out_state The DSP state instance to load into.
 * @return true on success, false on failure.
 */
bool dsp_state_load(const char* filename, dsp_state_t* out_state);

/**
 * @brief Save DSP state to a file.
 *
 * @param[in] filename The path to the state file.
 * @param[in] state The DSP state instance to save.
 * @return true on success, false on failure.
 */
bool dsp_state_save(const char* filename, const dsp_state_t* state);

/**
 * @brief Get the configuration path from the DSP state.
 *
 * @param[in] state The DSP state instance.
 * @return The configuration path string, or NULL if not set.
 */
const char* dsp_state_get_config_path(const dsp_state_t* state);

/**
 * @brief Set the configuration path in the DSP state.
 *
 * @param[in,out] state The DSP state instance.
 * @param[in] path The configuration path string to set.
 */
void dsp_state_set_config_path(dsp_state_t* state, const char* path);

/**
 * @brief Check if the DSP state has a configuration path.
 *
 * @param[in] state The DSP state instance.
 * @return true if it has a configuration path, false otherwise.
 */
bool dsp_state_has_config_path(const dsp_state_t* state);

/**
 * @brief Set whether the DSP state has a configuration path.
 *
 * @param[in,out] state The DSP state instance.
 * @param[in] has_path True if it has a configuration path, false otherwise.
 */
void dsp_state_set_has_config_path(dsp_state_t* state, bool has_path);

/**
 * @brief Get the mute status for a channel index from the DSP state.
 *
 * @param[in] state The DSP state instance.
 * @param[in] index The channel index.
 * @return True if muted, false otherwise.
 */
bool dsp_state_get_mute(const dsp_state_t* state, int index);

/**
 * @brief Set the mute status for a channel index in the DSP state.
 *
 * @param[in,out] state The DSP state instance.
 * @param[in] index The channel index.
 * @param[in] mute True to mute, false to unmute.
 */
void dsp_state_set_mute(dsp_state_t* state, int index, bool mute);

/**
 * @brief Get the volume for a channel index from the DSP state.
 *
 * @param[in] state The DSP state instance.
 * @param[in] index The channel index.
 * @return The volume value.
 */
double dsp_state_get_volume(const dsp_state_t* state, int index);

/**
 * @brief Set the volume for a channel index in the DSP state.
 *
 * @param[in,out] state The DSP state instance.
 * @param[in] index The channel index.
 * @param[in] volume The volume value to set.
 */
void dsp_state_set_volume(dsp_state_t* state, int index, double volume);

#endif  // CLIB_PIPELINE_STATE_FILE_H
