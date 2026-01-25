/**
 * PSI HTTP Server Test Application
 *
 * Pure C implementation of HTTP handlers that work with both:
 * - ESP-IDF's native HTTP server
 * - WebRTC DataChannel transport (via httpd_server.cpp)
 *
 * This file demonstrates the ESP-IDF HTTP server API compatibility.
 */

#include "esp_http_server.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef ESP_PLATFORM
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#define LOG_TAG "httpd_test"
#define LOGI(fmt, ...) ESP_LOGI(LOG_TAG, fmt, ##__VA_ARGS__)
#define LOGE(fmt, ...) ESP_LOGE(LOG_TAG, fmt, ##__VA_ARGS__)
#define FILE_BASE_PATH "/littlefs"

// Video streamer C API (from httpd_server.cpp)
extern esp_err_t httpd_video_freeze(httpd_handle_t handle);
extern esp_err_t httpd_video_unfreeze(httpd_handle_t handle);
extern bool httpd_video_is_frozen(httpd_handle_t handle);
extern uint8_t* httpd_video_get_rgb_buffer(httpd_handle_t handle, uint32_t* width, uint32_t* height);
#else
#define LOGI(fmt, ...) printf(fmt "\n", ##__VA_ARGS__)
#define LOGE(fmt, ...) printf("ERROR: " fmt "\n", ##__VA_ARGS__)
#define FILE_BASE_PATH ".."
#endif

#define CHUNK_SIZE  0x2000

// Forward declaration
httpd_handle_t httpd_test_get_handle(void);

// Generic file handler - serves files from littlefs
static esp_err_t file_handler(httpd_req_t *req, const char* filepath) {
    LOGI("File handler: %s", filepath);

    int fd = open(filepath, O_RDONLY);
    if (fd < 0) {
        LOGE("Failed to open file: %s", filepath);
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "File not found");
    }

    struct stat st;
    if (fstat(fd, &st) < 0) {
        close(fd);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to stat file");
    }

    // Set content type based on extension
    if (strstr(filepath, ".html")) {
        httpd_resp_set_type(req, "text/html");
    } else if (strstr(filepath, ".js")) {
        httpd_resp_set_type(req, "application/javascript");
    } else if (strstr(filepath, ".css")) {
        httpd_resp_set_type(req, "text/css");
    } else if (strstr(filepath, ".jpg") || strstr(filepath, ".jpeg")) {
        httpd_resp_set_type(req, "image/jpeg");
    } else if (strstr(filepath, ".png")) {
        httpd_resp_set_type(req, "image/png");
    } else {
        httpd_resp_set_type(req, "application/octet-stream");
    }

    LOGI("Streaming file: %ld bytes", (long)st.st_size);

#ifdef ESP_PLATFORM
    char *chunk = heap_caps_malloc(CHUNK_SIZE, MALLOC_CAP_INTERNAL);
    if (!chunk) {
        close(fd);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    }
#else
    char chunk_buf[CHUNK_SIZE];
    char *chunk = chunk_buf;
#endif

    ssize_t bytes_read;
    esp_err_t result = ESP_OK;

    while ((bytes_read = read(fd, chunk, CHUNK_SIZE)) > 0) {
        if (httpd_resp_send_chunk(req, chunk, bytes_read) != ESP_OK) {
            result = ESP_FAIL;
            break;
        }
    }

    close(fd);
#ifdef ESP_PLATFORM
    heap_caps_free(chunk);
#endif

    if (result == ESP_OK) {
        httpd_resp_send_chunk(req, NULL, 0);
    }
    return result;
}

// Root page handler - serves index.html
static esp_err_t root_handler(httpd_req_t *req) {
    LOGI("Root handler called");
    return file_handler(req, FILE_BASE_PATH "/index.html");
}

// App.js handler
static esp_err_t appjs_handler(httpd_req_t *req) {
    return file_handler(req, FILE_BASE_PATH "/static/app.js");
}

// Image handler - serves static files using streaming
static esp_err_t image_handler(httpd_req_t *req) {
    LOGI("Image handler called for: %s", req->uri);

    // Construct file path
    char filepath[256];
    snprintf(filepath, sizeof(filepath), "%s/%s", FILE_BASE_PATH, req->uri + 1); // Skip leading '/'

    // Open file
    int fd = open(filepath, O_RDONLY);
    if (fd < 0) {
        LOGE("Failed to open file: %s", filepath);
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "File not found");
    }

    // Get file size
    struct stat st;
    if (fstat(fd, &st) < 0) {
        close(fd);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to stat file");
    }

    // Set content type based on extension
    const char* uri = req->uri;
    if (strstr(uri, ".jpg") || strstr(uri, ".jpeg")) {
        httpd_resp_set_type(req, "image/jpeg");
    } else if (strstr(uri, ".png")) {
        httpd_resp_set_type(req, "image/png");
    } else if (strstr(uri, ".gif")) {
        httpd_resp_set_type(req, "image/gif");
    } else {
        httpd_resp_set_type(req, "application/octet-stream");
    }

    LOGI("Streaming file: %ld bytes", (long)st.st_size);

    // Allocate buffer in Internal RAM (required for flash access when stack is in PSRAM)
#ifdef ESP_PLATFORM
    char *chunk = heap_caps_malloc(CHUNK_SIZE, MALLOC_CAP_INTERNAL);
    if (!chunk) {
        close(fd);
        LOGE("Failed to allocate buffer in Internal RAM");
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    }
#else
    char chunk[CHUNK_SIZE];
    char *chunk_ptr = chunk;
#endif

    ssize_t bytes_read;
    esp_err_t result = ESP_OK;
    int chunk_num = 0;

#ifdef ESP_PLATFORM
    while ((bytes_read = read(fd, chunk, CHUNK_SIZE)) > 0) {
#else
    while ((bytes_read = read(fd, chunk_ptr, CHUNK_SIZE)) > 0) {
#endif
        if (httpd_resp_send_chunk(req, chunk, bytes_read) != ESP_OK) {
            result = ESP_FAIL;
            break;
        }
        chunk_num++;
    }

    close(fd);

#ifdef ESP_PLATFORM
    heap_caps_free(chunk);
#endif

    if (result == ESP_OK) {
        // Send final chunk to end response
        httpd_resp_send_chunk(req, NULL, 0);
    }

    return result;
}

// Simple hello handler
static esp_err_t hello_handler(httpd_req_t *req) {
    LOGI("Hello handler called");
    const char* resp = "Hello World from ESP32 over WebRTC DataChannel!";
    httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

#ifdef ESP_PLATFORM
// Video freeze handler - POST /api/freeze to freeze, DELETE /api/freeze to unfreeze
static esp_err_t freeze_handler(httpd_req_t *req) {
    LOGI("Freeze handler called: %s %s",
         req->method == HTTP_POST ? "POST" : "DELETE", req->uri);

    httpd_handle_t server = httpd_test_get_handle();
    if (!server) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Server not ready");
    }

    esp_err_t result;
    if (req->method == HTTP_POST) {
        result = httpd_video_freeze(server);
        if (result == ESP_OK) {
            httpd_resp_set_type(req, "application/json");
            httpd_resp_send(req, "{\"status\":\"freezing\"}", HTTPD_RESP_USE_STRLEN);
        } else if (result == ESP_ERR_NOT_SUPPORTED) {
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "CV pipeline not enabled");
        } else {
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Freeze failed");
        }
    } else {
        // DELETE - unfreeze
        result = httpd_video_unfreeze(server);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"status\":\"live\"}", HTTPD_RESP_USE_STRLEN);
    }

    return ESP_OK;
}

// Video status handler - GET /api/video/status
static esp_err_t video_status_handler(httpd_req_t *req) {
    LOGI("Video status handler called");

    httpd_handle_t server = httpd_test_get_handle();
    if (!server) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Server not ready");
    }

    bool frozen = httpd_video_is_frozen(server);

    char response[128];
    snprintf(response, sizeof(response),
             "{\"frozen\":%s}",
             frozen ? "true" : "false");

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, response, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}
#endif

// URI handler definitions
static const httpd_uri_t uri_root = {
    .uri       = "/",
    .method    = HTTP_GET,
    .handler   = root_handler,
    .user_ctx  = NULL
};

static const httpd_uri_t uri_hello = {
    .uri       = "/hello",
    .method    = HTTP_GET,
    .handler   = hello_handler,
    .user_ctx  = NULL
};

static const httpd_uri_t uri_appjs = {
    .uri       = "/static/app.js",
    .method    = HTTP_GET,
    .handler   = appjs_handler,
    .user_ctx  = NULL
};

static const httpd_uri_t uri_image1 = {
    .uri       = "/static/images/image1.jpg",
    .method    = HTTP_GET,
    .handler   = image_handler,
    .user_ctx  = NULL
};

static const httpd_uri_t uri_image2 = {
    .uri       = "/static/images/image2.jpg",
    .method    = HTTP_GET,
    .handler   = image_handler,
    .user_ctx  = NULL
};

#ifdef ESP_PLATFORM
static const httpd_uri_t uri_freeze_post = {
    .uri       = "/api/freeze",
    .method    = HTTP_POST,
    .handler   = freeze_handler,
    .user_ctx  = NULL
};

static const httpd_uri_t uri_freeze_delete = {
    .uri       = "/api/freeze",
    .method    = HTTP_DELETE,
    .handler   = freeze_handler,
    .user_ctx  = NULL
};

static const httpd_uri_t uri_video_status = {
    .uri       = "/api/video/status",
    .method    = HTTP_GET,
    .handler   = video_status_handler,
    .user_ctx  = NULL
};
#endif

// Global server handle
static httpd_handle_t g_server = NULL;

/**
 * Start the HTTP server with all handlers registered
 * Returns ESP_OK on success, error code on failure
 */
esp_err_t httpd_test_start(void) {
    if (g_server != NULL) {
        LOGE("Server already started");
        return ESP_ERR_INVALID_STATE;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();

    LOGI("Starting HTTP server...");
    esp_err_t ret = httpd_start(&g_server, &config);
    if (ret != ESP_OK) {
        LOGE("Failed to start server: %d", ret);
        return ret;
    }

    // Register handlers
    httpd_register_uri_handler(g_server, &uri_root);
    httpd_register_uri_handler(g_server, &uri_hello);
    httpd_register_uri_handler(g_server, &uri_appjs);
    httpd_register_uri_handler(g_server, &uri_image1);
    httpd_register_uri_handler(g_server, &uri_image2);
#ifdef ESP_PLATFORM
    httpd_register_uri_handler(g_server, &uri_freeze_post);
    httpd_register_uri_handler(g_server, &uri_freeze_delete);
    httpd_register_uri_handler(g_server, &uri_video_status);
#endif

    LOGI("Server started! Handlers registered.");

    return ESP_OK;
}

/**
 * Stop the HTTP server
 * Returns ESP_OK on success, error code on failure
 */
esp_err_t httpd_test_stop(void) {
    if (g_server == NULL) {
        LOGE("Server not started");
        return ESP_ERR_INVALID_STATE;
    }

    LOGI("Stopping HTTP server...");
    esp_err_t ret = httpd_stop(g_server);
    g_server = NULL;

    return ret;
}

/**
 * Get the server handle (for adding more handlers externally)
 */
httpd_handle_t httpd_test_get_handle(void) {
    return g_server;
}

#ifndef ESP_PLATFORM
// Desktop main function for testing
int main(int argc, char** argv) {
    // Get UID from command line or use default
    if (argc > 1) {
        setenv("DEVICE_UID", argv[1], 1);
    }

    esp_err_t ret = httpd_test_start();
    if (ret != ESP_OK) {
        return 1;
    }

    printf("Server started! Waiting for connections...\n");
    printf("Try accessing: http://<client>/UID/\n");

    // Keep running
    while (g_server) {
        sleep(1);
    }

    httpd_test_stop();
    return 0;
}
#endif
