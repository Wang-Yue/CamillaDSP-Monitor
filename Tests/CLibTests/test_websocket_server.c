#if defined(__linux__)
#define _GNU_SOURCE
#endif
#include <arpa/inet.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "../../Sources/CDSP/Audio/processing_parameters.h"
#include "../../Sources/CDSP/Backend/audio_backend.h"
#include "../../Sources/CDSP/Backend/backend_error.h"
#include "../../Sources/CDSP/Server/websocket_server.h"
#include "test_support.h"

static bool mock_get_status(void* ctx, state_update_t* out_status) {
  (void)ctx;
  if (out_status) {
    out_status->state = PROCESSING_STATE_INACTIVE;
    out_status->stop_reason.type = STOP_REASON_NONE;
  }
  return true;
}

static processing_parameters_t* mock_params = NULL;

static bool mock_get_processing_parameters(void* ctx, void** out_params) {
  (void)ctx;
  if (out_params) {
    *out_params = mock_params;
  }
  return true;
}

static audio_backend_error_type_t simulated_error_type =
    AUDIO_BACKEND_ERR_COMMAND_SEND;
static const char* simulated_error_message = "Simulated error message";

static device_error_type_t simulated_cap_error_type = DEVICE_ERROR_OTHER;
static const char* simulated_cap_error_message = "Simulated cap error";
static bool simulate_cap_error = false;

static bool mock_get_device_capabilities(void* ctx, const char* backend,
                                         const char* device, bool is_capture,
                                         audio_device_descriptor_t** out_desc,
                                         device_error_t* out_err) {
  (void)ctx;
  (void)backend;
  (void)device;
  (void)is_capture;
  (void)out_desc;

  if (simulate_cap_error) {
    if (out_err) {
      device_error_init(out_err, simulated_cap_error_type,
                        simulated_cap_error_message);
    }
    return false;
  }
  return true;
}

static bool simulate_set_config_error = false;
static char* received_config_json = NULL;

static bool mock_set_config_json(void* ctx, const char* json_str,
                                 audio_backend_error_t* out_err) {
  (void)ctx;
  if (simulate_set_config_error) {
    if (out_err) {
      out_err->type = simulated_error_type;
      snprintf(out_err->message, sizeof(out_err->message), "%s",
               simulated_error_message);
    }
    return false;
  }
  if (received_config_json) free(received_config_json);
  received_config_json = strdup(json_str);
  return true;
}

static void mock_set_fader_volume(void* ctx, fader_t fader, float db,
                                  bool instant) {
  (void)ctx;
  if (mock_params) {
    processing_parameters_set_target_volume_for_fader(mock_params, (double)db,
                                                      fader);
    if (instant) {
      processing_parameters_set_current_volume_for_fader(mock_params,
                                                         (double)db, fader);
    }
  }
}

static void mock_set_fader_mute(void* ctx, fader_t fader, bool mute) {
  (void)ctx;
  if (mock_params) {
    processing_parameters_set_muted_for_fader(mock_params, mute, fader);
  }
}

static char* mock_active_config = NULL;
static char* mock_prev_config = NULL;
static char* mock_state_file_path = NULL;
static char* mock_config_path = NULL;
static bool mock_dirty = false;

static const char* mock_get_state_file(void* ctx) {
  (void)ctx;
  return mock_state_file_path;
}
static bool mock_is_state_dirty(void* ctx) {
  (void)ctx;
  return mock_dirty;
}
static char* mock_get_config_path(void* ctx) {
  (void)ctx;
  return mock_config_path ? strdup(mock_config_path) : NULL;
}
static void mock_set_config_path(void* ctx, const char* path) {
  (void)ctx;
  if (mock_config_path) free(mock_config_path);
  mock_config_path = path ? strdup(path) : NULL;
}
static bool mock_get_active_config_json(void* ctx, char** out_json) {
  (void)ctx;
  if (mock_active_config) {
    *out_json = strdup(mock_active_config);
    return true;
  }
  *out_json = NULL;
  return false;
}
static bool mock_get_previous_config_json(void* ctx, char** out_json) {
  (void)ctx;
  if (mock_prev_config) {
    *out_json = strdup(mock_prev_config);
    return true;
  }
  *out_json = NULL;
  return false;
}

static dsp_engine_interface_t mock_engine = {
    .ctx = NULL,
    .get_status = mock_get_status,
    .get_processing_parameters = mock_get_processing_parameters,
    .set_config_json = mock_set_config_json,
    .get_device_capabilities = mock_get_device_capabilities,
    .set_fader_volume = mock_set_fader_volume,
    .set_fader_mute = mock_set_fader_mute,
    .get_state_file = mock_get_state_file,
    .is_state_dirty = mock_is_state_dirty,
    .get_config_path = mock_get_config_path,
    .set_config_path = mock_set_config_path,
    .get_active_config_json = mock_get_active_config_json,
    .get_previous_config_json = mock_get_previous_config_json};

TEST(test_websocket_commands) {
  websocket_server_t* server = websocket_server_create(54321, "127.0.0.1");
  ASSERT_TRUE(server != NULL);
  websocket_server_set_engine(server, &mock_engine);

  bool started = websocket_server_start(server);
  ASSERT_TRUE(started);

  usleep(100000);  // 100ms for server to start listening

  int sock = socket(AF_INET, SOCK_STREAM, 0);
  ASSERT_TRUE(sock >= 0);

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons(54321);
  inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

  int conn_res = connect(sock, (struct sockaddr*)&addr, sizeof(addr));
  ASSERT_EQ(0, conn_res);

  // Send GetVersion command
  const char* cmd1 = "\"GetVersion\"";
  send(sock, cmd1, strlen(cmd1), 0);

  char buf[4096];
  memset(buf, 0, sizeof(buf));
  ssize_t n = recv(sock, buf, sizeof(buf) - 1, 0);
  ASSERT_TRUE(n > 0);
  ASSERT_TRUE(strstr(buf, "\"GetVersion\"") != NULL);
  ASSERT_TRUE(strstr(buf, "\"Ok\"") != NULL);
  ASSERT_TRUE(strstr(buf, "\"CamillaDSP-C-Embedded 2.0.0\"") != NULL);

  // Send GetState command
  const char* cmd2 = "\"GetState\"";
  send(sock, cmd2, strlen(cmd2), 0);

  memset(buf, 0, sizeof(buf));
  n = recv(sock, buf, sizeof(buf) - 1, 0);
  ASSERT_TRUE(n > 0);
  ASSERT_TRUE(strstr(buf, "\"GetState\"") != NULL);
  ASSERT_TRUE(strstr(buf, "\"Ok\"") != NULL);
  ASSERT_TRUE(strstr(buf, "\"Inactive\"") != NULL);

  close(sock);
  websocket_server_stop(server);
  websocket_server_free(server);
}

TEST(test_websocket_handle_command_direct) {
  mock_config_path = strdup("/tmp/config.json");
  websocket_server_t* server = websocket_server_create(54322, "127.0.0.1");
  websocket_server_set_engine(server, &mock_engine);

  char resp[4096];
  websocket_server_handle_command(server, 0, "\"GetVersion\"", resp,
                                  sizeof(resp));
  ASSERT_TRUE(strstr(resp, "\"GetVersion\"") != NULL);
  ASSERT_TRUE(strstr(resp, "\"Ok\"") != NULL);
  ASSERT_TRUE(strstr(resp, "\"CamillaDSP-C-Embedded 2.0.0\"") != NULL);

  websocket_server_handle_command(server, 0, "\"GetState\"", resp,
                                  sizeof(resp));
  ASSERT_TRUE(strstr(resp, "\"GetState\"") != NULL);
  ASSERT_TRUE(strstr(resp, "\"Ok\"") != NULL);
  ASSERT_TRUE(strstr(resp, "\"Inactive\"") != NULL);

  websocket_server_handle_command(server, 0, "\"GetConfigFilePath\"", resp,
                                  sizeof(resp));
  ASSERT_TRUE(strstr(resp, "\"/tmp/config.json\"") != NULL);

  mock_params = processing_parameters_create(2, 2);
  ASSERT_TRUE(mock_params != NULL);

  websocket_server_handle_command(
      server, 0, "{\"SetFaderExternalVolume\":[0,-6.0]}", resp, sizeof(resp));
  ASSERT_TRUE(strstr(resp, "\"SetFaderExternalVolume\"") != NULL);
  ASSERT_TRUE(strstr(resp, "\"Ok\"") != NULL);

  double target_vol = processing_parameters_get_target_volume_for_fader(
      mock_params, FADER_MAIN);
  double current_vol = processing_parameters_get_current_volume_for_fader(
      mock_params, FADER_MAIN);
  ASSERT_DOUBLE_EQ(-6.0, target_vol);
  ASSERT_DOUBLE_EQ(-6.0, current_vol);

  // Test GetChannelLabels
  mock_active_config = strdup(
      "{\"devices\":{\"playback\":{\"labels\":[\"Left\",\"Right\"]},"
      "\"capture\":{\"labels\":[\"Mic\"]}}}");
  websocket_server_handle_command(server, 0, "\"GetChannelLabels\"", resp,
                                  sizeof(resp));
  ASSERT_TRUE(strstr(resp, "\"GetChannelLabels\"") != NULL);
  ASSERT_TRUE(strstr(resp, "\"Ok\"") != NULL);
  ASSERT_TRUE(strstr(resp, "\"playback\":[\"Left\",\"Right\"]") != NULL);
  ASSERT_TRUE(strstr(resp, "\"capture\":[\"Mic\"]") != NULL);
  free(mock_active_config);
  mock_active_config = NULL;

  // Test SubscribeVuLevels (simple)
  websocket_server_handle_command(server, 0, "\"SubscribeVuLevels\"", resp,
                                  sizeof(resp));
  ASSERT_TRUE(strstr(resp, "\"SubscribeVuLevels\"") != NULL);
  ASSERT_TRUE(strstr(resp, "\"Ok\"") != NULL);
  ASSERT_TRUE(server->client_sessions[0].vu_subscribed);
  ASSERT_DOUBLE_EQ(0.0, server->client_sessions[0].vu_max_rate);
  ASSERT_DOUBLE_EQ(0.0, server->client_sessions[0].vu_attack);
  ASSERT_DOUBLE_EQ(0.0, server->client_sessions[0].vu_release);

  // Test SubscribeVuLevels (with arguments)
  server->client_sessions[0].vu_subscribed = false;
  websocket_server_handle_command(server, 0,
                                  "{\"SubscribeVuLevels\":{\"max_rate\":100.0,"
                                  "\"attack\":10.0,\"release\":100.0}}",
                                  resp, sizeof(resp));
  ASSERT_TRUE(strstr(resp, "\"SubscribeVuLevels\"") != NULL);
  ASSERT_TRUE(strstr(resp, "\"Ok\"") != NULL);
  ASSERT_TRUE(server->client_sessions[0].vu_subscribed);
  ASSERT_DOUBLE_EQ(100.0, server->client_sessions[0].vu_max_rate);
  ASSERT_DOUBLE_EQ(10.0, server->client_sessions[0].vu_attack);
  ASSERT_DOUBLE_EQ(100.0, server->client_sessions[0].vu_release);

  processing_parameters_free(mock_params);
  mock_params = NULL;

  websocket_server_free(server);
  if (mock_config_path) {
    free(mock_config_path);
    mock_config_path = NULL;
  }
}

TEST(test_backend_error_description) {
  backend_error_t err;
  backend_error_init(&err, BACKEND_ERROR_DEVICE_NOT_FOUND, "Test device");
  char buf[256];
  backend_error_description(&err, buf, sizeof(buf));
  ASSERT_STR_EQ("Device not found: Test device", buf);
}

TEST(test_websocket_error_translation) {
  websocket_server_t* server = websocket_server_create(54323, "127.0.0.1");
  websocket_server_set_engine(server, &mock_engine);
  simulate_set_config_error = true;

  char resp[4096];

  // 1. Test ConfigValidationError translation
  simulated_error_type = AUDIO_BACKEND_ERR_CONFIG_PARSE;
  simulated_error_message = "Failed to parse JSON";
  websocket_server_handle_command(server, 0, "{\"SetConfigJson\":\"{}\"}", resp,
                                  sizeof(resp));
  ASSERT_TRUE(strstr(resp, "\"SetConfigJson\"") != NULL);
  ASSERT_TRUE(strstr(resp, "\"ConfigValidationError\"") != NULL);
  ASSERT_TRUE(strstr(resp, "Failed to parse JSON") != NULL);

  // 2. Test DeviceNotFoundError translation
  simulated_error_type = AUDIO_BACKEND_ERR_DEVICE_NOT_FOUND;
  simulated_error_message = "hw:0 not found";
  websocket_server_handle_command(server, 0, "{\"SetConfigJson\":\"{}\"}", resp,
                                  sizeof(resp));
  ASSERT_TRUE(strstr(resp, "\"SetConfigJson\"") != NULL);
  ASSERT_TRUE(strstr(resp, "\"DeviceNotFoundError\"") != NULL);
  ASSERT_TRUE(strstr(resp, "hw:0 not found") != NULL);

  // 3. Test DeviceBusyError translation
  simulated_error_type = AUDIO_BACKEND_ERR_DEVICE_BUSY;
  simulated_error_message = "hw:0 in use";
  websocket_server_handle_command(server, 0, "{\"SetConfigJson\":\"{}\"}", resp,
                                  sizeof(resp));
  ASSERT_TRUE(strstr(resp, "\"SetConfigJson\"") != NULL);
  ASSERT_TRUE(strstr(resp, "\"DeviceBusyError\"") != NULL);
  ASSERT_TRUE(strstr(resp, "hw:0 in use") != NULL);

  // 4. Test capabilities DeviceNotFoundError translation
  simulate_cap_error = true;
  simulated_cap_error_type = DEVICE_ERROR_NOT_FOUND;
  simulated_cap_error_message = "hw:0 not found";
  websocket_server_handle_command(
      server, 0, "{\"GetCaptureDeviceCapabilities\":[\"alsa\", \"hw:0\"]}",
      resp, sizeof(resp));
  ASSERT_TRUE(strstr(resp, "\"GetCaptureDeviceCapabilities\"") != NULL);
  ASSERT_TRUE(strstr(resp, "\"DeviceNotFoundError\"") != NULL);
  ASSERT_TRUE(strstr(resp, "hw:0 not found") != NULL);

  // 5. Test capabilities DeviceBusyError translation
  simulated_cap_error_type = DEVICE_ERROR_BUSY;
  simulated_cap_error_message = "hw:0 busy";
  websocket_server_handle_command(
      server, 0, "{\"GetCaptureDeviceCapabilities\":[\"alsa\", \"hw:0\"]}",
      resp, sizeof(resp));
  ASSERT_TRUE(strstr(resp, "\"GetCaptureDeviceCapabilities\"") != NULL);
  ASSERT_TRUE(strstr(resp, "\"DeviceBusyError\"") != NULL);
  ASSERT_TRUE(strstr(resp, "hw:0 busy") != NULL);

  // 6. Test capabilities Generic DeviceError translation
  simulated_cap_error_type = DEVICE_ERROR_OTHER;
  simulated_cap_error_message = "hw:0 bad driver";
  websocket_server_handle_command(
      server, 0, "{\"GetCaptureDeviceCapabilities\":[\"alsa\", \"hw:0\"]}",
      resp, sizeof(resp));
  ASSERT_TRUE(strstr(resp, "\"GetCaptureDeviceCapabilities\"") != NULL);
  ASSERT_TRUE(strstr(resp, "\"DeviceError\"") != NULL);
  ASSERT_TRUE(strstr(resp, "hw:0 bad driver") != NULL);

  simulate_cap_error = false;
  simulate_set_config_error = false;

  websocket_server_free(server);
}

TEST(test_websocket_patch_config) {
  websocket_server_t* server = websocket_server_create(54323, "127.0.0.1");
  websocket_server_set_engine(server, &mock_engine);

  mock_active_config = strdup(
      "{\n"
      "  \"devices\": {\n"
      "    \"samplerate\": 44100,\n"
      "    \"chunksize\": 1024,\n"
      "    \"capture\": {\"type\": \"File\", \"channels\": 2},\n"
      "    \"playback\": {\"type\": \"File\", \"channels\": 2}\n"
      "  },\n"
      "  \"filters\": {\n"
      "    \"mygain\": {\n"
      "      \"type\": \"Gain\",\n"
      "      \"parameters\": {\"gain\": -6.0}\n"
      "    }\n"
      "  }\n"
      "}");

  char resp[4096];
  const char* patch_cmd =
      "{\"PatchConfig\":{"
      "  \"filters\":{"
      "    \"mygain\":{"
      "      \"parameters\":{\"gain\":-3.0}"
      "    }"
      "  }"
      "}}";

  if (received_config_json) {
    free(received_config_json);
    received_config_json = NULL;
  }

  websocket_server_handle_command(server, 0, patch_cmd, resp, sizeof(resp));
  ASSERT_TRUE(strstr(resp, "\"PatchConfig\"") != NULL);
  ASSERT_TRUE(strstr(resp, "\"Ok\"") != NULL);

  ASSERT_TRUE(received_config_json != NULL);
  ASSERT_TRUE(strstr(received_config_json, "\"gain\":-3") != NULL ||
              strstr(received_config_json, "\"gain\": -3") != NULL);

  free(mock_active_config);
  mock_active_config = NULL;

  if (received_config_json) {
    free(received_config_json);
    received_config_json = NULL;
  }

  websocket_server_free(server);
}

TEST_MAIN()
