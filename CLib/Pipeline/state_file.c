#include "Pipeline/state_file.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

static void trim_trailing(char* str) {
    size_t len = strlen(str);
    while (len > 0 && (str[len - 1] == '\r' || str[len - 1] == '\n' || isspace((unsigned char)str[len - 1]))) {
        str[len - 1] = '\0';
        len--;
    }
}

bool dsp_state_load(const char* filename, dsp_state_t* out_state) {
    if (!filename || !out_state) return false;
    FILE* fp = fopen(filename, "r");
    if (!fp) return false;

    memset(out_state, 0, sizeof(dsp_state_t));

    char line[1024];
    int mode = 0; // 0 = root, 1 = mute list, 2 = volume list
    int mute_idx = 0;
    int vol_idx = 0;

    while (fgets(line, sizeof(line), fp)) {
        trim_trailing(line);
        
        // skip empty or comment or doc-start lines
        if (line[0] == '\0' || line[0] == '#' || strcmp(line, "---") == 0) {
            continue;
        }

        // Check indentation to determine if we exited a list
        int indent = 0;
        while (line[indent] == ' ' || line[indent] == '\t') {
            indent++;
        }

        if (indent < 2) {
            mode = 0;
        }

        char* trimmed = line + indent;

        if (mode == 0) {
            if (strncmp(trimmed, "config_path:", 12) == 0) {
                char* val = trimmed + 12;
                while (*val == ' ' || *val == '\t') val++;
                if (strcmp(val, "null") != 0 && val[0] != '\0') {
                    // strip quotes if any
                    if (val[0] == '"' || val[0] == '\'') {
                        size_t vlen = strlen(val);
                        if (vlen > 2 && val[vlen - 1] == val[0]) {
                            strncpy(out_state->config_path, val + 1, vlen - 2);
                            out_state->config_path[vlen - 2] = '\0';
                        } else {
                            strncpy(out_state->config_path, val + 1, sizeof(out_state->config_path) - 1);
                        }
                    } else {
                        strncpy(out_state->config_path, val, sizeof(out_state->config_path) - 1);
                    }
                    out_state->has_config_path = true;
                }
            } else if (strncmp(trimmed, "mute:", 5) == 0) {
                mode = 1;
                mute_idx = 0;
            } else if (strncmp(trimmed, "volume:", 7) == 0) {
                mode = 2;
                vol_idx = 0;
            }
        } else if (mode == 1) { // mute list
            if (trimmed[0] == '-' && (trimmed[1] == ' ' || trimmed[1] == '\t')) {
                char* val = trimmed + 2;
                while (*val == ' ' || *val == '\t') val++;
                if (mute_idx < 5) {
                    out_state->mute[mute_idx++] = (strcmp(val, "true") == 0 || strcmp(val, "True") == 0);
                }
            }
        } else if (mode == 2) { // volume list
            if (trimmed[0] == '-' && (trimmed[1] == ' ' || trimmed[1] == '\t')) {
                char* val = trimmed + 2;
                while (*val == ' ' || *val == '\t') val++;
                if (vol_idx < 5) {
                    out_state->volume[vol_idx++] = atof(val);
                }
            }
        }
    }

    fclose(fp);
    return true;
}

bool dsp_state_save(const char* filename, const dsp_state_t* state) {
    if (!filename || !state) return false;
    
    // Save to a temporary file first, then rename (atomic write)
    char tmp_name[1024];
    snprintf(tmp_name, sizeof(tmp_name), "%s.tmp", filename);

    FILE* fp = fopen(tmp_name, "w");
    if (!fp) return false;

    fprintf(fp, "---\n");
    if (state->has_config_path) {
        fprintf(fp, "config_path: \"%s\"\n", state->config_path);
    } else {
        fprintf(fp, "config_path: null\n");
    }
    
    fprintf(fp, "mute:\n");
    for (int i = 0; i < 5; i++) {
        fprintf(fp, "  - %s\n", state->mute[i] ? "true" : "false");
    }

    fprintf(fp, "volume:\n");
    for (int i = 0; i < 5; i++) {
        fprintf(fp, "  - %.6f\n", state->volume[i]);
    }

    fclose(fp);

    if (rename(tmp_name, filename) != 0) {
        remove(tmp_name);
        return false;
    }

    return true;
}
