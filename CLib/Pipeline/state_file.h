#ifndef CLIB_PIPELINE_STATE_FILE_H
#define CLIB_PIPELINE_STATE_FILE_H

#include <stdbool.h>

typedef struct {
    char config_path[1024];
    bool has_config_path;
    bool mute[5];
    double volume[5];
} dsp_state_t;

bool dsp_state_load(const char* filename, dsp_state_t* out_state);
bool dsp_state_save(const char* filename, const dsp_state_t* state);

#endif // CLIB_PIPELINE_STATE_FILE_H
