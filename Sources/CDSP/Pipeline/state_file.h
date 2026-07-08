#ifndef CLIB_PIPELINE_STATE_FILE_H
#define CLIB_PIPELINE_STATE_FILE_H

#include <stdbool.h>

struct dsp_state_s;
typedef struct dsp_state_s dsp_state_t;

dsp_state_t* dsp_state_create(void);
void dsp_state_free(dsp_state_t* state);

bool dsp_state_load(const char* filename, dsp_state_t* out_state);
bool dsp_state_save(const char* filename, const dsp_state_t* state);

const char* dsp_state_get_config_path(const dsp_state_t* state);
void dsp_state_set_config_path(dsp_state_t* state, const char* path);
bool dsp_state_has_config_path(const dsp_state_t* state);
void dsp_state_set_has_config_path(dsp_state_t* state, bool has_path);
bool dsp_state_get_mute(const dsp_state_t* state, int index);
void dsp_state_set_mute(dsp_state_t* state, int index, bool mute);
double dsp_state_get_volume(const dsp_state_t* state, int index);
void dsp_state_set_volume(dsp_state_t* state, int index, double volume);

#endif  // CLIB_PIPELINE_STATE_FILE_H
