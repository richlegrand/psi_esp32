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
#define LOG_TAG "httpd_test"
#define LOGI(fmt, ...) ESP_LOGI(LOG_TAG, fmt, ##__VA_ARGS__)
#define LOGE(fmt, ...) ESP_LOGE(LOG_TAG, fmt, ##__VA_ARGS__)
#define FILE_BASE_PATH "/littlefs"
#else
#define LOGI(fmt, ...) printf(fmt "\n", ##__VA_ARGS__)
#define LOGE(fmt, ...) printf("ERROR: " fmt "\n", ##__VA_ARGS__)
#define FILE_BASE_PATH ".."
#endif

// Root page handler
static esp_err_t root_handler(httpd_req_t *req) {
    LOGI("Root handler called");
    const char* html =
        "<!DOCTYPE html>\n"
        "<html lang=\"en\">\n"
        "<head>\n"
        "    <meta charset=\"UTF-8\">\n"
        "    <meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\">\n"
        "    <title>PSI ESP32 Device</title>\n"
        "</head>\n"
        "<body>\n"
        "\n"
        "<h1>Hello from ESP32!</h1>\n"
        "<p>This is a simple page served by an ESP32 device over WebRTC DataChannel.</p>\n"
        "\n"
        "<h2>Images:</h2>\n"
        "<img src=\"/static/images/image1.jpg\" alt=\"Image 1\" width=\"300\">\n"
        "<br>\n"
        "<img src=\"/static/images/image2.jpg\" alt=\"Image 2\" width=\"300\">\n"
        "</body>\n"
        "</html>\n";

    httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
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
    char *chunk = heap_caps_malloc(4096, MALLOC_CAP_INTERNAL);
    if (!chunk) {
        close(fd);
        LOGE("Failed to allocate buffer in Internal RAM");
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    }
#else
    char chunk[4096];
    char *chunk_ptr = chunk;
#endif

    ssize_t bytes_read;
    esp_err_t result = ESP_OK;

#ifdef ESP_PLATFORM
    while ((bytes_read = read(fd, chunk, 4096)) > 0) {
#else
    while ((bytes_read = read(fd, chunk_ptr, 4096)) > 0) {
#endif
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
    httpd_register_uri_handler(g_server, &uri_image1);
    httpd_register_uri_handler(g_server, &uri_image2);

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
