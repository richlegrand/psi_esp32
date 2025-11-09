/**
 * ESP-IDF WebSocket wrapper
 * Provides a familiar API similar to libdatachannel WebSocket
 * but uses ESP-IDF transport underneath to avoid static initialization issues
 */

#pragma once

#include <string>
#include <functional>
#include <memory>
#include "esp_transport_ws.h"
#include "esp_transport_tcp.h"
#include "esp_log.h"

class EspWebSocket {
public:
    // Callback types similar to libdatachannel
    using OnOpenCallback = std::function<void()>;
    using OnMessageCallback = std::function<void(const std::string& message)>;
    using OnCloseCallback = std::function<void()>;
    using OnErrorCallback = std::function<void(const std::string& error)>;

    EspWebSocket();
    ~EspWebSocket();

    // API similar to libdatachannel WebSocket
    void onOpen(OnOpenCallback callback) { mOnOpen = callback; }
    void onMessage(OnMessageCallback callback) { mOnMessage = callback; }
    void onClosed(OnCloseCallback callback) { mOnClose = callback; }
    void onError(OnErrorCallback callback) { mOnError = callback; }

    bool open(const std::string& url);
    void close();
    bool send(const std::string& message);

    bool isOpen() const { return mConnected; }

private:
    esp_transport_handle_t mTcpTransport = nullptr;
    esp_transport_handle_t mWsTransport = nullptr;
    bool mConnected = false;

    OnOpenCallback mOnOpen;
    OnMessageCallback mOnMessage;
    OnCloseCallback mOnClose;
    OnErrorCallback mOnError;

    static const char* TAG;
};