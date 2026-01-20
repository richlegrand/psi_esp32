/**
 * PSI HTTP Server Test Application
 *
 * Header for C HTTP server that works with WebRTC DataChannel transport.
 */

#ifndef HTTPD_TEST_H
#define HTTPD_TEST_H

#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Start the HTTP server with all handlers registered
 * Returns ESP_OK on success, error code on failure
 */
esp_err_t httpd_test_start(void);

/**
 * Stop the HTTP server
 * Returns ESP_OK on success, error code on failure
 */
esp_err_t httpd_test_stop(void);

/**
 * Get the server handle (for adding more handlers externally)
 */
httpd_handle_t httpd_test_get_handle(void);

#ifdef __cplusplus
}
#endif

#endif // HTTPD_TEST_H
