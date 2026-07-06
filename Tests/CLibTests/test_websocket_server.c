#if defined(__linux__)
#define _GNU_SOURCE
#endif
#include "test_support.h"
#include "../../Sources/CDSP/Server/websocket_server.h"
#include "../../Sources/CDSP/Backend/backend_error.h"
#include "../../Sources/CDSP/Backend/audio_backend.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <string.h>

static bool mock_get_status(void* ctx, state_update_t* out_status) {
    (void)ctx;
    if (out_status) {
        out_status->state = PROCESSING_STATE_INACTIVE;
        out_status->stop_reason.type = STOP_REASON_NONE;
    }
    return true;
}

static dsp_engine_interface_t mock_engine = {
    .ctx = NULL,
    .get_status = mock_get_status
};

TEST(test_websocket_commands) {
    active_config_path_t* path = active_config_path_create(NULL);
    websocket_server_t* server = websocket_server_create(54321, "127.0.0.1", path);
    ASSERT_TRUE(server != NULL);
    websocket_server_set_engine(server, &mock_engine);
    
    bool started = websocket_server_start(server);
    ASSERT_TRUE(started);
    
    usleep(100000); // 100ms for server to start listening
    
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
    active_config_path_free(path);
}

TEST(test_websocket_handle_command_direct) {
    active_config_path_t* path = active_config_path_create("/tmp/config.json");
    websocket_server_t* server = websocket_server_create(54322, "127.0.0.1", path);
    websocket_server_set_engine(server, &mock_engine);
    
    char resp[4096];
    websocket_server_handle_command(server, 0, "\"GetVersion\"", resp, sizeof(resp));
    ASSERT_TRUE(strstr(resp, "\"GetVersion\"") != NULL);
    ASSERT_TRUE(strstr(resp, "\"Ok\"") != NULL);
    ASSERT_TRUE(strstr(resp, "\"CamillaDSP-C-Embedded 2.0.0\"") != NULL);
    
    websocket_server_handle_command(server, 0, "\"GetState\"", resp, sizeof(resp));
    ASSERT_TRUE(strstr(resp, "\"GetState\"") != NULL);
    ASSERT_TRUE(strstr(resp, "\"Ok\"") != NULL);
    ASSERT_TRUE(strstr(resp, "\"Inactive\"") != NULL);
    
    websocket_server_handle_command(server, 0, "\"GetConfigFilePath\"", resp, sizeof(resp));
    ASSERT_TRUE(strstr(resp, "\"/tmp/config.json\"") != NULL);
    
    websocket_server_free(server);
    active_config_path_free(path);
}

TEST(test_backend_error_description) {
    backend_error_t err;
    backend_error_init(&err, BACKEND_ERROR_DEVICE_NOT_FOUND, "Test device");
    char buf[256];
    backend_error_description(&err, buf, sizeof(buf));
    ASSERT_STR_EQ("Device not found: Test device", buf);
}

TEST_MAIN()
