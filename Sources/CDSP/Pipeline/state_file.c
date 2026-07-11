#include "Pipeline/state_file.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct dsp_state_s {
  char config_path[1024];
  bool has_config_path;
  bool mute[5];
  double volume[5];
};

dsp_state_t* dsp_state_create(void) {
  dsp_state_t* state = (dsp_state_t*)calloc(1, sizeof(dsp_state_t));
  return state;
}

void dsp_state_free(dsp_state_t* state) { free(state); }

/**
 * @brief Helper function to trim trailing whitespace and newline characters
 * from a string.
 *
 * Modifies the input string in-place by replacing trailing whitespace, carriage
 * returns, and line feeds with null terminators.
 *
 * @param str The string to trim.
 */
static void trim_trailing(char* str) {
  size_t len = strlen(str);
  while (len > 0 && (str[len - 1] == '\r' || str[len - 1] == '\n' ||
                     isspace((unsigned char)str[len - 1]))) {
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
  // Parser state machine mode:
  // 0: Root level key-value pairs
  // 1: Processing the elements of the 'mute' list
  // 2: Processing the elements of the 'volume' list
  int mode = 0;
  int mute_idx = 0;
  int vol_idx = 0;

  while (fgets(line, sizeof(line), fp)) {
    trim_trailing(line);

    // skip empty or comment or doc-start lines
    if (line[0] == '\0' || line[0] == '#' || strcmp(line, "---") == 0) {
      continue;
    }

    // YAML-like parser: Check indentation to determine if we exited a list.
    // List elements are expected to be indented (e.g. by 2 spaces).
    // If indentation is less than 2 spaces, we assume we have returned to the
    // root level.
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
              size_t copylen = vlen - 2;
              if (copylen >= sizeof(out_state->config_path)) {
                copylen = sizeof(out_state->config_path) - 1;
              }
              strncpy(out_state->config_path, val + 1, copylen);
              out_state->config_path[copylen] = '\0';
            } else {
              strncpy(out_state->config_path, val + 1,
                      sizeof(out_state->config_path) - 1);
              out_state->config_path[sizeof(out_state->config_path) - 1] = '\0';
            }
          } else {
            strncpy(out_state->config_path, val,
                    sizeof(out_state->config_path) - 1);
            out_state->config_path[sizeof(out_state->config_path) - 1] = '\0';
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
    } else if (mode == 1) {  // mute list
      if (trimmed[0] == '-' && (trimmed[1] == ' ' || trimmed[1] == '\t')) {
        char* val = trimmed + 2;
        while (*val == ' ' || *val == '\t') val++;
        if (mute_idx < 5) {
          out_state->mute[mute_idx++] =
              (strcmp(val, "true") == 0 || strcmp(val, "True") == 0);
        }
      }
    } else if (mode == 2) {  // volume list
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

  // Save to a temporary file first, then rename to the target filename.
  // This ensures an atomic write, preventing corruption of the state file
  // if the process is interrupted or crashes during write.
  char tmp_name[1024];
  int written = snprintf(tmp_name, sizeof(tmp_name), "%s.tmp", filename);
  if (written < 0 || (size_t)written >= sizeof(tmp_name)) {
    return false;
  }

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

const char* dsp_state_get_config_path(const dsp_state_t* state) {
  return state ? state->config_path : NULL;
}

void dsp_state_set_config_path(dsp_state_t* state, const char* path) {
  if (!state) return;
  if (path) {
    strncpy(state->config_path, path, sizeof(state->config_path) - 1);
    state->config_path[sizeof(state->config_path) - 1] = '\0';
    state->has_config_path = true;
  } else {
    state->config_path[0] = '\0';
    state->has_config_path = false;
  }
}

bool dsp_state_has_config_path(const dsp_state_t* state) {
  return state ? state->has_config_path : false;
}

void dsp_state_set_has_config_path(dsp_state_t* state, bool has_path) {
  if (state) state->has_config_path = has_path;
}

bool dsp_state_get_mute(const dsp_state_t* state, int index) {
  if (state && index >= 0 && index < 5) {
    return state->mute[index];
  }
  return false;
}

void dsp_state_set_mute(dsp_state_t* state, int index, bool mute) {
  if (state && index >= 0 && index < 5) {
    state->mute[index] = mute;
  }
}

double dsp_state_get_volume(const dsp_state_t* state, int index) {
  if (state && index >= 0 && index < 5) {
    return state->volume[index];
  }
  return 0.0;
}

void dsp_state_set_volume(dsp_state_t* state, int index, double volume) {
  if (state && index >= 0 && index < 5) {
    state->volume[index] = volume;
  }
}
