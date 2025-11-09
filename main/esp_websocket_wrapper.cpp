/**
 * ESP-IDF WebSocket wrapper implementation
 */

#include "esp_websocket_wrapper.hpp"
#include <cstring>

const char* EspWebSocket::TAG = "EspWebSocket";

EspWebSocket::EspWebSocket() {
    ESP_LOGD(TAG, "Creating ESP WebSocket wrapper");
}

EspWebSocket::~EspWebSocket() {
    close();
}

bool EspWebSocket::open(const std::string& url) {
    ESP_LOGI(TAG, "Opening WebSocket to: %s", url.c_str());

    // Create TCP transport
    mTcpTransport = esp_transport_tcp_init();
    if (!mTcpTransport) {
        ESP_LOGE(TAG, "Failed to initialize TCP transport");
        if (mOnError) mOnError("Failed to initialize TCP transport");
        return false;
    }

    // Create WebSocket transport
    mWsTransport = esp_transport_ws_init(mTcpTransport);
    if (!mWsTransport) {
        ESP_LOGE(TAG, "Failed to initialize WebSocket transport");
        if (mOnError) mOnError("Failed to initialize WebSocket transport");
        esp_transport_destroy(mTcpTransport);
        mTcpTransport = nullptr;
        return false;
    }

    // Parse URL - simplified version (assumes ws://host:port/path format)
    // For production, you'd want more robust URL parsing
    std::string host;
    int port = 80;
    std::string path = "/";

    if (url.substr(0, 5) == "ws://") {
        size_t start = 5;
        size_t colon_pos = url.find(':', start);
        size_t slash_pos = url.find('/', start);

        if (colon_pos != std::string::npos && (slash_pos == std::string::npos || colon_pos < slash_pos)) {
            // URL has port
            host = url.substr(start, colon_pos - start);
            size_t port_end = (slash_pos != std::string::npos) ? slash_pos : url.length();
            port = std::stoi(url.substr(colon_pos + 1, port_end - colon_pos - 1));
            if (slash_pos != std::string::npos) {
                path = url.substr(slash_pos);
            }
        } else if (slash_pos != std::string::npos) {
            // URL has path but no port
            host = url.substr(start, slash_pos - start);
            path = url.substr(slash_pos);
        } else {
            // URL has only host
            host = url.substr(start);
        }
    } else {
        ESP_LOGE(TAG, "Invalid WebSocket URL format");
        if (mOnError) mOnError("Invalid WebSocket URL format");
        return false;
    }

    ESP_LOGD(TAG, "Parsed URL - Host: %s, Port: %d, Path: %s", host.c_str(), port, path.c_str());

    // Set WebSocket path
    esp_transport_ws_set_path(mWsTransport, path.c_str());

    // Connect
    int ret = esp_transport_connect(mWsTransport, host.c_str(), port, 5000);
    if (ret < 0) {
        ESP_LOGE(TAG, "WebSocket connection failed");
        if (mOnError) mOnError("WebSocket connection failed");
        close();
        return false;
    }

    mConnected = true;
    ESP_LOGI(TAG, "WebSocket connected successfully");
    if (mOnOpen) mOnOpen();

    return true;
}

void EspWebSocket::close() {
    if (mWsTransport) {
        esp_transport_close(mWsTransport);
        esp_transport_destroy(mWsTransport);
        mWsTransport = nullptr;
    }

    if (mTcpTransport) {
        esp_transport_destroy(mTcpTransport);
        mTcpTransport = nullptr;
    }

    if (mConnected) {
        mConnected = false;
        ESP_LOGI(TAG, "WebSocket closed");
        if (mOnClose) mOnClose();
    }
}

bool EspWebSocket::send(const std::string& message) {
    if (!mConnected || !mWsTransport) {
        ESP_LOGE(TAG, "WebSocket not connected");
        return false;
    }

    int ret = esp_transport_ws_send_raw(mWsTransport, WS_TRANSPORT_OPCODES_TEXT,
                                       message.c_str(), message.length(), 5000);
    if (ret < 0) {
        ESP_LOGE(TAG, "Failed to send WebSocket message");
        if (mOnError) mOnError("Failed to send message");
        return false;
    }

    ESP_LOGD(TAG, "Sent WebSocket message: %s", message.c_str());
    return true;
}