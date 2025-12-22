/**
 * PSI WebRTC Server for ESP32
 *
 * Provides WebRTC connectivity with SWSP protocol support for HTTP-like
 * request/response over DataChannel.
 *
 * This module implements the ESP-IDF httpd API (httpd_resp_send, etc.)
 * over WebRTC DataChannel. Handlers written for ESP-IDF's HTTP server
 * work unchanged with this WebRTC transport.
 *
 * Ported from libdatachannel_device/httpd_server.cpp
 */

#ifndef HTTPD_SERVER_HPP
#define HTTPD_SERVER_HPP

#include "rtc/rtc.hpp"
#include "esp_http_server.h"
#include "esp_websocket_client.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

#include <string>
#include <vector>
#include <map>
#include <memory>
#include <mutex>
#include <atomic>

// SWSP Protocol Constants
const uint16_t FLAG_SYN = 0x0001;  // Metadata frame
const uint16_t FLAG_FIN = 0x0004;  // Final frame

// Forward declarations
class WebRTCSession;
class WebRTCServer;
class HandlerDispatcher;

//=============================================================================
// Internal Context Structure (stored in httpd_req_t.aux)
//=============================================================================

struct httpd_req_aux {
    // Parsed SWSP request
    std::string method_str;
    std::string uri_str;
    std::map<std::string, std::string> headers;
    std::vector<uint8_t> body;

    // Response state
    int status_code = 200;
    std::string status_str = "200 OK";
    std::string content_type = "text/html";
    std::map<std::string, std::string> response_headers;
    bool headers_sent = false;

    // SWSP metadata
    uint32_t stream_id;
    std::shared_ptr<WebRTCSession> session;
};

//=============================================================================
// WebRTCSession Class
//=============================================================================

class WebRTCSession : public std::enable_shared_from_this<WebRTCSession> {
public:
    WebRTCSession(const std::string& client_id,
                  std::shared_ptr<rtc::PeerConnection> pc,
                  std::shared_ptr<rtc::DataChannel> dc);

    ~WebRTCSession();

    // Getters
    std::string getClientId() const { return client_id_; }
    std::shared_ptr<rtc::DataChannel> getDataChannel() const { return dc_; }
    bool isConnected() const { return dc_ && dc_->isOpen(); }

    // Send SWSP frame
    void sendSwspFrame(uint32_t stream_id, uint16_t flags, const std::vector<uint8_t>& payload);
    void sendSwspFrame(uint32_t stream_id, uint16_t flags, const std::string& payload);

    // Handle incoming SWSP frame
    void handleSwspFrame(const rtc::binary& frame);

    // Set handler registry (from server)
    void setHandlers(const std::vector<httpd_uri_t>* handlers) {
        handlers_ = handlers;
    }

private:
    std::string client_id_;
    std::shared_ptr<rtc::PeerConnection> pc_;
    std::shared_ptr<rtc::DataChannel> dc_;

    // Handler registry (shared, read-only)
    const std::vector<httpd_uri_t>* handlers_ = nullptr;

    // Find handler for URI and method
    const httpd_uri_t* findHandler(const char* uri, httpd_method_t method);
};

//=============================================================================
// WebRTCServer Class
//=============================================================================

class WebRTCServer {
public:
    WebRTCServer(const std::string& uid, const std::string& server_url);
    ~WebRTCServer();

    // Lifecycle
    void start();
    void stop();

    // Session management
    void addSession(const std::string& client_id, std::shared_ptr<WebRTCSession> session);
    void removeSession(const std::string& client_id);
    std::shared_ptr<WebRTCSession> getSession(const std::string& client_id);

    // HTTP handler registration (matches ESP-IDF API)
    esp_err_t registerHandler(const httpd_uri_t* uri_handler);

private:
    static constexpr size_t MAX_SESSIONS = 4;  // Limited for ESP32 memory

    std::string uid_;
    std::string server_url_;
    esp_websocket_client_handle_t ws_client_ = nullptr;

    // Session registry
    std::map<std::string, std::shared_ptr<WebRTCSession>> sessions_;
    std::mutex sessions_mutex_;

    // PeerConnection registry (for adding remote candidates)
    std::map<std::string, std::shared_ptr<rtc::PeerConnection>> peer_connections_;

    // HTTP handlers
    std::vector<httpd_uri_t> uri_handlers_;

    // Reconnection state
    std::atomic<bool> running_{false};

    // Message buffer for fragmented WebSocket messages
    std::string ws_message_buffer_;

    // WebSocket event handling
    static void websocketEventHandler(void* handler_args, esp_event_base_t base,
                                       int32_t event_id, void* event_data);
    void handleWebSocketMessage(const std::string& message);

    // Signaling
    void handleRequest(const std::string& client_id);
    void handleAnswer(const std::string& client_id, const std::string& sdp);
    void handleCandidate(const std::string& client_id, const std::string& candidate,
                         const std::string& mid);
    void sendSignalingMessage(const std::string& message);
};

//=============================================================================
// Handler Dispatcher - Executes HTTP handlers on Internal RAM stack
//=============================================================================
//
// Problem: WebRTC DataChannel callbacks execute in libdatachannel ThreadPool
// worker threads, which have PSRAM stacks (configured by esp32_configure_pthread_psram).
// File I/O operations (open/read from LittleFS) disable the cache, making PSRAM
// inaccessible. If the stack is in PSRAM, this triggers an assertion:
// "esp_task_stack_is_sane_cache_disabled()"
//
// Solution: Dispatch HTTP handler execution to a dedicated FreeRTOS task with
// an Internal RAM stack. This task safely performs file I/O operations.
//
class HandlerDispatcher {
public:
    // Singleton access
    static HandlerDispatcher& Instance() {
        static HandlerDispatcher instance;
        return instance;
    }

    // Delete copy/move for singleton
    HandlerDispatcher(const HandlerDispatcher&) = delete;
    HandlerDispatcher& operator=(const HandlerDispatcher&) = delete;

    // Initialize the dispatcher task (call once at startup)
    void initialize();

    // Execute a handler on the Internal RAM task
    // Blocks until handler completes and returns the result
    esp_err_t executeHandler(httpd_req_t* req, const httpd_uri_t* handler);

private:
    HandlerDispatcher() = default;
    ~HandlerDispatcher() = default;

    struct HandlerRequest {
        httpd_req_t* req;
        const httpd_uri_t* handler;
        SemaphoreHandle_t completion_sem;
        esp_err_t result;
    };

    QueueHandle_t request_queue_ = nullptr;
    TaskHandle_t handler_task_ = nullptr;
    bool initialized_ = false;

    static void handlerTaskEntry(void* arg);
    void handlerTaskLoop();
};

#endif // HTTPD_SERVER_HPP
