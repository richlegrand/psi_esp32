/**
 * PSI WebRTC Server for ESP32
 *
 * Provides WebRTC connectivity with SWSP protocol support for HTTP-like
 * request/response over DataChannel.
 *
 * Ported from libdatachannel_device/httpd_server.cpp
 */

#include "httpd_server.hpp"
#include "video_streamer.hpp"
#include <cJSON.h>
#include <cstring>
#include "esp_log.h"
#include "esp_crt_bundle.h"
#include "esp32_psram_init.h"
#include "esp_timer.h"

// libdatachannel headers for video streaming
#include "rtc/h264rtppacketizer.hpp"
#include "rtc/rtcpsrreporter.hpp"
#include "rtc/rtcpnackresponder.hpp"
#include "rtc/frameinfo.hpp"

static const char* TAG = "WebRTC";

using namespace rtc;

//=============================================================================
// Frame Timing Control
//=============================================================================

// Global flag to synchronize logging across all pipeline layers
bool g_log_frame_timing = false;

//=============================================================================
// WebRTCSession Implementation
//=============================================================================

WebRTCSession::WebRTCSession(const std::string& client_id,
                             std::shared_ptr<PeerConnection> pc,
                             std::shared_ptr<DataChannel> dc)
    : client_id_(client_id), pc_(pc), dc_(dc) {
    ESP_LOGI(TAG, "WebRTCSession created for client: %s", client_id.c_str());
}

WebRTCSession::~WebRTCSession() {
    ESP_LOGI(TAG, "WebRTCSession destroyed for client: %s", client_id_.c_str());
}

void WebRTCSession::sendSwspFrame(uint32_t stream_id, uint16_t flags, const std::vector<uint8_t>& payload) {
    if (!dc_ || !dc_->isOpen()) {
        ESP_LOGE(TAG, "DataChannel not open, cannot send frame");
        return;
    }

    // Build SWSP frame: [stream_id:4][flags:2][length:2][payload:N]
    std::vector<uint8_t> frame(8 + payload.size());

    // Little Endian encoding
    frame[0] = stream_id & 0xFF;
    frame[1] = (stream_id >> 8) & 0xFF;
    frame[2] = (stream_id >> 16) & 0xFF;
    frame[3] = (stream_id >> 24) & 0xFF;

    frame[4] = flags & 0xFF;
    frame[5] = (flags >> 8) & 0xFF;

    uint16_t length = payload.size();
    frame[6] = length & 0xFF;
    frame[7] = (length >> 8) & 0xFF;

    // Copy payload
    std::copy(payload.begin(), payload.end(), frame.begin() + 8);

    // Send
    dc_->send(reinterpret_cast<const std::byte*>(frame.data()), frame.size());
}

void WebRTCSession::sendSwspFrame(uint32_t stream_id, uint16_t flags, const std::string& payload) {
    std::vector<uint8_t> data(payload.begin(), payload.end());
    sendSwspFrame(stream_id, flags, data);
}

const httpd_uri_t* WebRTCSession::findHandler(const char* uri, httpd_method_t method) {
    if (!handlers_) return nullptr;

    for (const auto& h : *handlers_) {
        if (strcmp(h.uri, uri) == 0 && (h.method == method || h.method == HTTP_ANY)) {
            return &h;
        }
    }
    return nullptr;
}

void WebRTCSession::handleSwspFrame(const rtc::binary& frame_data) {
    if (frame_data.size() < 8) {
        ESP_LOGE(TAG, "Frame too small: %d", (int)frame_data.size());
        return;
    }

    // Parse SWSP header (Little Endian)
    const uint8_t* data = reinterpret_cast<const uint8_t*>(frame_data.data());

    uint32_t stream_id = data[0] | (data[1] << 8) | (data[2] << 16) | (data[3] << 24);
    uint16_t flags = data[4] | (data[5] << 8);
    uint16_t length = data[6] | (data[7] << 8);

    std::vector<uint8_t> payload(data + 8, data + 8 + length);

    ESP_LOGI(TAG, "Received frame: stream_id=%lu flags=0x%04x length=%d",
             (unsigned long)stream_id, flags, length);

    // Parse request (FLAG_SYN | FLAG_FIN)
    if ((flags & FLAG_SYN) && (flags & FLAG_FIN)) {
        // Parse JSON request
        std::string json_str(payload.begin(), payload.end());

        cJSON* req_json = cJSON_Parse(json_str.c_str());
        if (!req_json) {
            ESP_LOGE(TAG, "Failed to parse request JSON");
            return;
        }

        ESP_LOGI(TAG, "Request: %s", json_str.c_str());

        // Build httpd_req_t
        httpd_req_t req = {};
        auto aux = new httpd_req_aux();

        aux->stream_id = stream_id;
        aux->session = shared_from_this();

        cJSON* method_item = cJSON_GetObjectItem(req_json, "method");
        cJSON* pathname_item = cJSON_GetObjectItem(req_json, "pathname");

        if (method_item && cJSON_IsString(method_item)) {
            aux->method_str = method_item->valuestring;
        }
        if (pathname_item && cJSON_IsString(pathname_item)) {
            aux->uri_str = pathname_item->valuestring;
        }

        // Set method
        if (aux->method_str == "GET") req.method = HTTP_GET;
        else if (aux->method_str == "POST") req.method = HTTP_POST;
        else if (aux->method_str == "PUT") req.method = HTTP_PUT;
        else if (aux->method_str == "DELETE") req.method = HTTP_DELETE;
        else req.method = HTTP_GET;

        // Copy URI (cast away const since we're constructing the request ourselves)
        char* uri_ptr = const_cast<char*>(req.uri);
        strncpy(uri_ptr, aux->uri_str.c_str(), sizeof(req.uri) - 1);
        uri_ptr[sizeof(req.uri) - 1] = '\0';

        req.aux = aux;
        req.content_len = 0;  // TODO: parse body

        cJSON_Delete(req_json);

        // Find and dispatch handler to Internal RAM task
        const httpd_uri_t* handler = findHandler(req.uri, (httpd_method_t)req.method);
        if (handler) {
            req.user_ctx = handler->user_ctx;

            // CRITICAL: Execute handler on Internal RAM task to allow file I/O
            // The current thread (libdatachannel ThreadPool worker) has PSRAM stack
            esp_err_t result = HandlerDispatcher::Instance().executeHandler(&req, handler);
            if (result != ESP_OK) {
                ESP_LOGE(TAG, "Handler returned error: %d", result);
            }
        } else {
            ESP_LOGE(TAG, "No handler found for: %s", req.uri);
            // Send 404
            httpd_resp_send_err(&req, HTTPD_404_NOT_FOUND, "Not Found");
        }

        // Cleanup
        delete aux;
    }
}

//=============================================================================
// WebRTCServer Implementation
//=============================================================================

WebRTCServer::WebRTCServer(const std::string& uid, const std::string& server_url)
    : uid_(uid), server_url_(server_url) {
    ESP_LOGI(TAG, "WebRTCServer created for UID: %s", uid.c_str());

    // Create video streamer (1280x720 @ 25fps)
    video_streamer_ = std::make_unique<VideoStreamer>(1280, 720, 25);
}

WebRTCServer::~WebRTCServer() {
    stop();
}

void WebRTCServer::websocketEventHandler(void* handler_args, esp_event_base_t base,
                                          int32_t event_id, void* event_data) {
    // Configure pthread to use PSRAM for the WebSocket client's task
    // This ensures PeerConnection creation (which spawns libjuice threads) uses PSRAM stacks
    // Call this once per event to ensure config is set (it's idempotent)
    esp32_ensure_pthread_psram();

    WebRTCServer* server = static_cast<WebRTCServer*>(handler_args);
    esp_websocket_event_data_t* data = static_cast<esp_websocket_event_data_t*>(event_data);

    // Log all events for debugging
    //ESP_LOGI(TAG, "WebSocket event: %d", (int)event_id);

    switch (event_id) {
        case WEBSOCKET_EVENT_CONNECTED:
            ESP_LOGI(TAG, "WebSocket connected to signaling server");
            break;

        case WEBSOCKET_EVENT_DISCONNECTED:
        case WEBSOCKET_EVENT_CLOSED:
            ESP_LOGW(TAG, "WebSocket disconnected/closed (event %d)", (int)event_id);

            // Initiate reconnection (built-in will handle retries if it fails)
            if (server->running_ && server->ws_client_) {
                ESP_LOGI(TAG, "Attempting to reconnect...");
                // Brief delay before reconnecting (seems to help connection succeed)
                vTaskDelay(pdMS_TO_TICKS(1000));
                esp_websocket_client_start(server->ws_client_);
            }
            break;

        case WEBSOCKET_EVENT_DATA:
            if (data->op_code == 0x01 && data->data_len > 0) {  // Text frame
                // Append received data to buffer
                server->ws_message_buffer_.append((char*)data->data_ptr, data->data_len);

                // Check if we have a complete JSON message
                bool is_complete = false;
                if (!server->ws_message_buffer_.empty() &&
                    server->ws_message_buffer_.front() == '{' &&
                    server->ws_message_buffer_.back() == '}') {
                    is_complete = true;
                }

                if (is_complete) {
                    ESP_LOGI(TAG, "Received complete WebSocket message (len=%d)",
                             (int)server->ws_message_buffer_.length());
                    server->handleWebSocketMessage(server->ws_message_buffer_);
                    server->ws_message_buffer_.clear();
                } else {
                    ESP_LOGD(TAG, "Received WebSocket fragment (len=%d), buffering...",
                             data->data_len);
                }
            }
            break;

        case WEBSOCKET_EVENT_ERROR:
            ESP_LOGE(TAG, "WebSocket error");
            break;

        default:
            break;
    }
}

void WebRTCServer::handleWebSocketMessage(const std::string& message) {
    cJSON* json = cJSON_Parse(message.c_str());
    if (!json) {
        ESP_LOGE(TAG, "Failed to parse signaling message");
        return;
    }

    cJSON* type_item = cJSON_GetObjectItem(json, "type");
    cJSON* client_id_item = cJSON_GetObjectItem(json, "client_id");

    if (!type_item || !cJSON_IsString(type_item)) {
        cJSON_Delete(json);
        return;
    }

    std::string type = type_item->valuestring;
    std::string client_id = (client_id_item && cJSON_IsString(client_id_item))
                            ? client_id_item->valuestring : "";

    if (type == "registered") {
        ESP_LOGI(TAG, "Registered! URL: https://%s/%s", server_url_.c_str(), uid_.c_str());
    } else if (type == "request") {
        // Browser requests connection - we create the offer
        ESP_LOGI(TAG, "Received connection request from client: %s", client_id.c_str());
        handleRequest(client_id);
    } else if (type == "answer") {
        // Browser sends answer - we set remote description
        cJSON* sdp_item = cJSON_GetObjectItem(json, "sdp");
        if (sdp_item && cJSON_IsString(sdp_item)) {
            std::string sdp = sdp_item->valuestring;

            // Handle double-encoded SDP
            if (sdp.rfind("{\"type\":\"answer\"", 0) == 0) {
                cJSON* sub_json = cJSON_Parse(sdp.c_str());
                if (sub_json) {
                    cJSON* inner_sdp = cJSON_GetObjectItem(sub_json, "sdp");
                    if (inner_sdp && cJSON_IsString(inner_sdp)) {
                        sdp = inner_sdp->valuestring;
                    }
                    cJSON_Delete(sub_json);
                }
            }

            handleAnswer(client_id, sdp);
        }
    } else if (type == "candidate") {
        cJSON* cand_item = cJSON_GetObjectItem(json, "candidate");
        if (cand_item) {
            cJSON* cand_str_item = cJSON_GetObjectItem(cand_item, "candidate");
            cJSON* mid_item = cJSON_GetObjectItem(cand_item, "sdpMid");

            if (cand_str_item && cJSON_IsString(cand_str_item) &&
                mid_item && cJSON_IsString(mid_item)) {
                std::string candidate = cand_str_item->valuestring;
                std::string mid = mid_item->valuestring;

                // Remove "candidate:" prefix if present
                if (candidate.rfind("candidate:", 0) == 0) {
                    candidate = candidate.substr(10);
                }

                handleCandidate(client_id, candidate, mid);
            }
        }
    }

    cJSON_Delete(json);
}

void WebRTCServer::handleRequest(const std::string& client_id) {
    ESP_LOGI(TAG, "Received connection request from client: %s", client_id.c_str());

    // Create PeerConnection with ICE servers
    Configuration config;
    config.iceServers.emplace_back("stun:stun.l.google.com:19302");
    // Add TURN servers if needed
    // config.iceServers.emplace_back("turn:...", port, "user", "pass", IceServer::RelayType::TurnUdp);

    auto pc = std::make_shared<PeerConnection>(config);

    // Store PeerConnection for later candidate additions
    {
        std::lock_guard<std::mutex> lock(sessions_mutex_);
        peer_connections_[client_id] = pc;
    }

    // Handle ICE candidates
    pc->onLocalCandidate([this, client_id](Candidate candidate) {
        ESP_LOGI(TAG, "New local candidate");

        cJSON* msg = cJSON_CreateObject();
        cJSON_AddStringToObject(msg, "type", "candidate");
        cJSON_AddStringToObject(msg, "client_id", client_id.c_str());

        cJSON* cand_obj = cJSON_CreateObject();
        std::string cand_str = "candidate:" + candidate.candidate();
        cJSON_AddStringToObject(cand_obj, "candidate", cand_str.c_str());
        cJSON_AddStringToObject(cand_obj, "sdpMid", candidate.mid().c_str());
        cJSON_AddNumberToObject(cand_obj, "sdpMLineIndex", 0);
        cJSON_AddItemToObject(msg, "candidate", cand_obj);

        char* msg_str = cJSON_PrintUnformatted(msg);
        if (msg_str) {
            sendSignalingMessage(msg_str);
            free(msg_str);
        }
        cJSON_Delete(msg);
    });

    // Handle local description (send OFFER, not answer)
    pc->onLocalDescription([this, client_id](Description description) {
        ESP_LOGI(TAG, "Local Description Ready (Offer)");

        cJSON* msg = cJSON_CreateObject();
        cJSON_AddStringToObject(msg, "type", "offer");
        cJSON_AddStringToObject(msg, "sdp", std::string(description).c_str());
        cJSON_AddStringToObject(msg, "client_id", client_id.c_str());

        char* msg_str = cJSON_PrintUnformatted(msg);
        if (msg_str) {
            sendSignalingMessage(msg_str);
            free(msg_str);
        }
        cJSON_Delete(msg);
    });

    // Create DataChannel (device creates it, not browser)
    ESP_LOGI(TAG, "Creating datachannel...");
    auto dc = pc->createDataChannel("http");
    ESP_LOGI(TAG, "Datachannel created");

    // Store DataChannel in a shared location accessible by callbacks
    // This avoids the reference cycle while keeping it alive
    {
        std::lock_guard<std::mutex> lock(sessions_mutex_);
        // Store in peer_connections_ map - we'll create session when channel opens
    }

    // Set up onClosed early (doesn't need session)
    dc->onClosed([this, client_id]() {
        ESP_LOGI(TAG, "DataChannel closed for client: %s", client_id.c_str());
        removeSession(client_id);
    });

    // Set up onOpen to create session
    dc->onOpen([this, client_id, pc, dc]() {
        ESP_LOGI(TAG, "DataChannel opened for client: %s", client_id.c_str());

        auto session = std::make_shared<WebRTCSession>(client_id, pc, dc);

        dc->onMessage([session](std::variant<rtc::binary, std::string> data) {
            if (std::holds_alternative<rtc::binary>(data)) {
                session->handleSwspFrame(std::get<rtc::binary>(data));
            }
        });

        addSession(client_id, session);
    });

    // Add video track (will be included in offer)
    ESP_LOGI(TAG, "Adding video track...");
    const uint8_t payloadType = 96;
    const uint32_t ssrc = std::hash<std::string>{}(client_id) & 0xFFFFFFFF;  // Unique SSRC per client
    const std::string cname = "video-stream";

    Description::Video media(cname, Description::Direction::SendOnly);
    media.addH264Codec(payloadType);
    media.addSSRC(ssrc, cname, "stream1", cname);
    ESP_LOGI(TAG, "Calling pc->addTrack()...");
    auto video_track = pc->addTrack(media);
    ESP_LOGI(TAG, "pc->addTrack() returned");

    // Create RTP configuration
    auto rtpConfig = std::make_shared<RtpPacketizationConfig>(ssrc, cname, payloadType, H264RtpPacketizer::ClockRate);

    // Create H.264 packetizer - use StartSequence for Annex-B format from ESP32 encoder
    auto packetizer = std::make_shared<H264RtpPacketizer>(NalUnit::Separator::StartSequence, rtpConfig);

    // Add RTCP SR handler
    auto srReporter = std::make_shared<RtcpSrReporter>(rtpConfig);
    packetizer->addToChain(srReporter);

    // Add RTCP NACK handler (with reduced size for ESP32 memory constraints)
    auto nackResponder = std::make_shared<RtcpNackResponder>();
    packetizer->addToChain(nackResponder);

    // Set media handler
    video_track->setMediaHandler(packetizer);

    // Set onOpen callback - add track to video streamer
    video_track->onOpen([this, client_id, video_track]() {
        ESP_LOGI(TAG, "Video track opened for client: %s", client_id.c_str());

        if (video_streamer_) {
            video_streamer_->addTrack(client_id, video_track);
        }
    });

    // Set onClosed callback - remove track from video streamer
    video_track->onClosed([this, client_id]() {
        ESP_LOGI(TAG, "Video track closed for client: %s", client_id.c_str());

        if (video_streamer_) {
            video_streamer_->removeTrack(client_id);
        }
    });

    ESP_LOGI(TAG, "Added video track for client: %s (SSRC: %u)", client_id.c_str(), ssrc);

    // Create offer (no remote description yet)
    ESP_LOGI(TAG, "Calling setLocalDescription() to create offer...");
    pc->setLocalDescription();
    ESP_LOGI(TAG, "setLocalDescription() returned, offer will be sent via callback");
}

void WebRTCServer::handleAnswer(const std::string& client_id, const std::string& sdp) {
    ESP_LOGI(TAG, "Received answer from client: %s", client_id.c_str());

    std::shared_ptr<PeerConnection> pc;
    {
        std::lock_guard<std::mutex> lock(sessions_mutex_);
        auto it = peer_connections_.find(client_id);
        if (it == peer_connections_.end()) {
            ESP_LOGE(TAG, "No PeerConnection found for client: %s", client_id.c_str());
            return;
        }
        pc = it->second;
    }

    // Set remote description (browser's answer)
    pc->setRemoteDescription(Description(sdp, "answer"));
    ESP_LOGI(TAG, "Remote description set for client: %s", client_id.c_str());
}

void WebRTCServer::handleCandidate(const std::string& client_id, const std::string& candidate,
                                    const std::string& mid) {
    ESP_LOGI(TAG, "Received candidate from client: %s", client_id.c_str());

    std::shared_ptr<PeerConnection> pc;
    {
        std::lock_guard<std::mutex> lock(sessions_mutex_);
        auto it = peer_connections_.find(client_id);
        if (it == peer_connections_.end()) {
            ESP_LOGE(TAG, "No PeerConnection found for client: %s", client_id.c_str());
            return;
        }
        pc = it->second;
    }

    try {
        pc->addRemoteCandidate(Candidate(candidate, mid));
        ESP_LOGI(TAG, "Added remote candidate");
    } catch (const std::exception& e) {
        ESP_LOGE(TAG, "Failed to add candidate: %s", e.what());
    }
}

void WebRTCServer::sendSignalingMessage(const std::string& message) {
    if (!ws_client_ || !esp_websocket_client_is_connected(ws_client_)) {
        ESP_LOGE(TAG, "WebSocket not connected");
        return;
    }

    ESP_LOGI(TAG, "Sending signaling message, len=%d", (int)message.length());
    int ret = esp_websocket_client_send_text(ws_client_, message.c_str(), message.length(), portMAX_DELAY);
    if (ret < 0) {
        ESP_LOGE(TAG, "Failed to send signaling message, ret=%d", ret);
    }
}

void WebRTCServer::addSession(const std::string& client_id, std::shared_ptr<WebRTCSession> session) {
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    if (sessions_.size() >= MAX_SESSIONS) {
        ESP_LOGE(TAG, "Max sessions reached, rejecting client: %s", client_id.c_str());
        return;
    }
    sessions_[client_id] = session;
    session->setHandlers(&uri_handlers_);
    ESP_LOGI(TAG, "Session added: %s (total: %d)", client_id.c_str(), (int)sessions_.size());
}

void WebRTCServer::removeSession(const std::string& client_id) {
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    auto it = sessions_.find(client_id);
    if (it != sessions_.end()) {
        sessions_.erase(it);
        ESP_LOGI(TAG, "Session removed: %s (total: %d)", client_id.c_str(), (int)sessions_.size());
    }
    // Also remove PeerConnection
    auto pc_it = peer_connections_.find(client_id);
    if (pc_it != peer_connections_.end()) {
        peer_connections_.erase(pc_it);
    }

    // Note: Video track cleanup handled by onClosed() callback
}

std::shared_ptr<WebRTCSession> WebRTCServer::getSession(const std::string& client_id) {
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    auto it = sessions_.find(client_id);
    return (it != sessions_.end()) ? it->second : nullptr;
}

esp_err_t WebRTCServer::registerHandler(const httpd_uri_t* uri_handler) {
    if (!uri_handler) {
        return ESP_ERR_INVALID_ARG;
    }

    // Check for duplicates
    for (const auto& h : uri_handlers_) {
        if (strcmp(h.uri, uri_handler->uri) == 0 && h.method == uri_handler->method) {
            return ESP_ERR_HTTPD_HANDLER_EXISTS;
        }
    }

    uri_handlers_.push_back(*uri_handler);
    ESP_LOGI(TAG, "Registered handler: %s", uri_handler->uri);
    return ESP_OK;
}

void WebRTCServer::start() {
    ESP_LOGI(TAG, "Starting WebRTCServer...");

    // Initialize handler dispatcher with Internal RAM stack (for file I/O)
    HandlerDispatcher::Instance().initialize();

    running_ = true;

    // Build WebSocket URL
    std::string ws_url = "wss://" + server_url_ + "/ws/device/" + uid_;

    esp_websocket_client_config_t websocket_cfg = {};
    websocket_cfg.uri = ws_url.c_str();
    // 32KB stack: Required for regex-based SDP parsing in libdatachannel
    // (complex RFC 3986 URL regex causes deep recursion)
    // Parent project dispatches SDP parsing to MainThread to avoid this
    websocket_cfg.task_stack = 32768;
    websocket_cfg.buffer_size = 4096;  // Increased from 2KB to handle large server headers
    websocket_cfg.reconnect_timeout_ms = 10000;  // Explicit to suppress warning
    websocket_cfg.network_timeout_ms = 10000;  // Explicit to suppress warning

    // WebSocket keepalive: Send PING every 60 seconds (typical production value)
    // This prevents timeout when no signaling messages are being exchanged
    // (after WebRTC connection is established, signaling goes idle)
    websocket_cfg.ping_interval_sec = 60;
    websocket_cfg.pingpong_timeout_sec = 120;  // Disconnect if no PONG received within 2 minutes
    websocket_cfg.disable_pingpong_discon = false;  // Enable auto-disconnect on timeout

    // TCP keepalive: Detect dead connections at TCP layer (typical production values)
    websocket_cfg.keep_alive_enable = true;
    websocket_cfg.keep_alive_idle = 300;     // Send TCP keepalive after 5 minutes of idle
    websocket_cfg.keep_alive_interval = 75;  // Retry every 75s (Linux standard)
    websocket_cfg.keep_alive_count = 9;      // 9 retries before considering connection dead

    // TLS configuration for secure WebSocket (WSS)
    // Use ESP-IDF's certificate bundle for CA verification
    websocket_cfg.crt_bundle_attach = esp_crt_bundle_attach;

    ESP_LOGI(TAG, "Connecting to: %s", ws_url.c_str());

    ws_client_ = esp_websocket_client_init(&websocket_cfg);
    if (!ws_client_) {
        ESP_LOGE(TAG, "Failed to initialize WebSocket client");
        return;
    }

    // Verify ping interval was set correctly
    size_t ping_interval = esp_websocket_client_get_ping_interval_sec(ws_client_);
    ESP_LOGI(TAG, "WebSocket client initialized with ping_interval=%d seconds", (int)ping_interval);

    esp_websocket_register_events(ws_client_, WEBSOCKET_EVENT_ANY, websocketEventHandler, this);
    esp_websocket_client_start(ws_client_);

    ESP_LOGI(TAG, "WebSocket client started - auto-reconnect on disconnect enabled");
}

void WebRTCServer::stop() {
    ESP_LOGI(TAG, "Stopping WebRTCServer...");

    running_ = false;

    // Clean up video streamer (will auto-stop when tracks are removed)
    if (video_streamer_) {
        video_streamer_.reset();
    }

    // Close all sessions
    {
        std::lock_guard<std::mutex> lock(sessions_mutex_);
        sessions_.clear();
        peer_connections_.clear();
    }

    // Close WebSocket
    if (ws_client_) {
        esp_websocket_client_stop(ws_client_);
        esp_websocket_client_destroy(ws_client_);
        ws_client_ = nullptr;
    }
}

//=============================================================================
// C API Implementation for httpd_resp_* functions
// These provide WebRTC DataChannel transport while maintaining ESP-IDF API
//=============================================================================

// Server context for httpd_handle_t
struct httpd_server_context {
    WebRTCServer* server;
};

extern "C" {

esp_err_t httpd_start(httpd_handle_t *handle, const httpd_config_t *config) {
    if (!handle || !config) {
        return ESP_ERR_INVALID_ARG;
    }

    // Get UID from environment or use default
    const char* uid_env = getenv("DEVICE_UID");
    std::string uid = uid_env ? uid_env : "0123456789";

    // Get server URL from environment or use default
    const char* server_env = getenv("PSI_SERVER");
    std::string server_url = server_env ? server_env : "psi.vizycam.com";

    auto ctx = new httpd_server_context();
    ctx->server = new WebRTCServer(uid, server_url);
    ctx->server->start();

    *handle = (httpd_handle_t)ctx;
    return ESP_OK;
}

esp_err_t httpd_stop(httpd_handle_t handle) {
    if (!handle) {
        return ESP_ERR_INVALID_ARG;
    }

    auto ctx = (httpd_server_context*)handle;
    ctx->server->stop();
    delete ctx->server;
    delete ctx;

    return ESP_OK;
}

esp_err_t httpd_register_uri_handler(httpd_handle_t handle, const httpd_uri_t *uri_handler) {
    if (!handle || !uri_handler) {
        return ESP_ERR_INVALID_ARG;
    }

    auto ctx = (httpd_server_context*)handle;
    return ctx->server->registerHandler(uri_handler);
}

esp_err_t httpd_unregister_uri_handler(httpd_handle_t handle, const char *uri, httpd_method_t method) {
    // TODO: implement
    return ESP_ERR_NOT_FOUND;
}

esp_err_t httpd_resp_send(httpd_req_t *r, const char *buf, ssize_t buf_len) {
    if (!r || !r->aux) {
        return ESP_ERR_INVALID_ARG;
    }

    auto aux = static_cast<httpd_req_aux*>(r->aux);

    // Handle HTTPD_RESP_USE_STRLEN
    size_t actual_len = (buf_len == HTTPD_RESP_USE_STRLEN) ? strlen(buf) : buf_len;

    // Build metadata JSON
    cJSON* metadata = cJSON_CreateObject();
    cJSON_AddNumberToObject(metadata, "status", aux->status_code);

    cJSON* headers = cJSON_CreateObject();
    cJSON_AddStringToObject(headers, "Content-Type", aux->content_type.c_str());
    cJSON_AddStringToObject(headers, "Content-Length", std::to_string(actual_len).c_str());

    // Add custom headers
    for (const auto& [key, value] : aux->response_headers) {
        cJSON_AddStringToObject(headers, key.c_str(), value.c_str());
    }
    cJSON_AddItemToObject(metadata, "headers", headers);

    char* metadata_str = cJSON_PrintUnformatted(metadata);
    if (metadata_str) {
        // Send metadata frame
        aux->session->sendSwspFrame(aux->stream_id, FLAG_SYN, std::string(metadata_str));
        free(metadata_str);
    }
    cJSON_Delete(metadata);

    // Send body in chunks (max 65535 bytes per frame due to uint16_t length field)
    const size_t MAX_CHUNK_SIZE = 65535;
    size_t offset = 0;

    while (offset < actual_len) {
        size_t chunk_size = std::min(MAX_CHUNK_SIZE, actual_len - offset);
        std::vector<uint8_t> chunk_data(buf + offset, buf + offset + chunk_size);

        // Last chunk gets FLAG_FIN, others get no flags
        uint16_t flags = (offset + chunk_size >= actual_len) ? FLAG_FIN : 0;
        aux->session->sendSwspFrame(aux->stream_id, flags, chunk_data);

        offset += chunk_size;
    }

    return ESP_OK;
}

esp_err_t httpd_resp_send_chunk(httpd_req_t *r, const char *buf, ssize_t buf_len) {
    if (!r || !r->aux) {
        return ESP_ERR_INVALID_ARG;
    }

    auto aux = static_cast<httpd_req_aux*>(r->aux);

    // First call: send metadata if not sent yet
    if (!aux->headers_sent) {
        cJSON* metadata = cJSON_CreateObject();
        cJSON_AddNumberToObject(metadata, "status", aux->status_code);

        cJSON* headers = cJSON_CreateObject();
        cJSON_AddStringToObject(headers, "Content-Type", aux->content_type.c_str());
        // Note: No Content-Length for chunked responses

        for (const auto& [key, value] : aux->response_headers) {
            cJSON_AddStringToObject(headers, key.c_str(), value.c_str());
        }
        cJSON_AddItemToObject(metadata, "headers", headers);

        char* metadata_str = cJSON_PrintUnformatted(metadata);
        if (metadata_str) {
            aux->session->sendSwspFrame(aux->stream_id, FLAG_SYN, std::string(metadata_str));
            free(metadata_str);
        }
        cJSON_Delete(metadata);
        aux->headers_sent = true;
    }

    // NULL or 0 length = end of chunks
    if (!buf || buf_len == 0) {
        std::vector<uint8_t> empty;
        aux->session->sendSwspFrame(aux->stream_id, FLAG_FIN, empty);
        return ESP_OK;
    }

    // Handle HTTPD_RESP_USE_STRLEN
    size_t actual_len = (buf_len == HTTPD_RESP_USE_STRLEN) ? strlen(buf) : buf_len;

    // Send data in chunks
    const size_t MAX_CHUNK_SIZE = 65535;
    size_t offset = 0;

    while (offset < actual_len) {
        size_t chunk_size = std::min(MAX_CHUNK_SIZE, actual_len - offset);
        std::vector<uint8_t> chunk_data(buf + offset, buf + offset + chunk_size);

        // Send chunk with no flags (not FIN yet)
        aux->session->sendSwspFrame(aux->stream_id, 0, chunk_data);

        offset += chunk_size;
    }

    return ESP_OK;
}

esp_err_t httpd_resp_set_status(httpd_req_t *r, const char *status) {
    if (!r || !r->aux || !status) {
        return ESP_ERR_INVALID_ARG;
    }

    auto aux = static_cast<httpd_req_aux*>(r->aux);
    aux->status_str = status;
    aux->status_code = atoi(status);

    return ESP_OK;
}

esp_err_t httpd_resp_set_type(httpd_req_t *r, const char *type) {
    if (!r || !r->aux || !type) {
        return ESP_ERR_INVALID_ARG;
    }

    auto aux = static_cast<httpd_req_aux*>(r->aux);
    aux->content_type = type;

    return ESP_OK;
}

esp_err_t httpd_resp_set_hdr(httpd_req_t *r, const char *field, const char *value) {
    if (!r || !r->aux || !field || !value) {
        return ESP_ERR_INVALID_ARG;
    }

    auto aux = static_cast<httpd_req_aux*>(r->aux);
    aux->response_headers[field] = value;

    return ESP_OK;
}

esp_err_t httpd_resp_send_err(httpd_req_t *r, httpd_err_code_t error, const char *msg) {
    if (!r) {
        return ESP_ERR_INVALID_ARG;
    }

    const char* status_map[] = {
        "500 Internal Server Error",
        "501 Not Implemented",
        "505 Version Not Supported",
        "400 Bad Request",
        "401 Unauthorized",
        "403 Forbidden",
        "404 Not Found",
        "405 Method Not Allowed",
        "408 Request Timeout",
        "411 Length Required",
        "413 Content Too Large",
        "414 URI Too Long",
        "431 Request Header Fields Too Large"
    };

    const char* status = (error < HTTPD_ERR_CODE_MAX) ? status_map[error] : "500 Internal Server Error";

    httpd_resp_set_status(r, status);
    httpd_resp_set_type(r, "text/plain");

    const char* error_msg = msg ? msg : status;
    return httpd_resp_send(r, error_msg, strlen(error_msg));
}

// Stub implementations for other ESP-IDF httpd functions
size_t httpd_req_get_hdr_value_len(httpd_req_t *r, const char *field) {
    if (!r || !r->aux || !field) {
        return 0;
    }
    auto aux = static_cast<httpd_req_aux*>(r->aux);
    auto it = aux->headers.find(field);
    if (it != aux->headers.end()) {
        return it->second.length();
    }
    return 0;
}

esp_err_t httpd_req_get_hdr_value_str(httpd_req_t *r, const char *field, char *val, size_t val_size) {
    if (!r || !r->aux || !field || !val) {
        return ESP_ERR_INVALID_ARG;
    }
    auto aux = static_cast<httpd_req_aux*>(r->aux);
    auto it = aux->headers.find(field);
    if (it != aux->headers.end()) {
        strncpy(val, it->second.c_str(), val_size - 1);
        val[val_size - 1] = '\0';
        return ESP_OK;
    }
    return ESP_ERR_NOT_FOUND;
}

size_t httpd_req_get_url_query_len(httpd_req_t *r) {
    // TODO: implement query string parsing
    return 0;
}

esp_err_t httpd_req_get_url_query_str(httpd_req_t *r, char *buf, size_t buf_len) {
    // TODO: implement query string parsing
    return ESP_ERR_NOT_FOUND;
}

esp_err_t httpd_query_key_value(const char *qry, const char *key, char *val, size_t val_size) {
    // TODO: implement query string parsing
    return ESP_ERR_NOT_FOUND;
}

int httpd_req_recv(httpd_req_t *r, char *buf, size_t buf_len) {
    if (!r || !r->aux || !buf) {
        return -1;
    }
    auto aux = static_cast<httpd_req_aux*>(r->aux);
    size_t copy_len = std::min(buf_len, aux->body.size());
    if (copy_len > 0) {
        memcpy(buf, aux->body.data(), copy_len);
    }
    return (int)copy_len;
}

} // extern "C"

//=============================================================================
// Handler Dispatcher Implementation
//=============================================================================

void HandlerDispatcher::initialize() {
    if (initialized_) {
        ESP_LOGW(TAG, "HandlerDispatcher already initialized");
        return;
    }

    // Create request queue (depth of 4 allows some pipelining)
    request_queue_ = xQueueCreate(4, sizeof(HandlerRequest*));
    if (!request_queue_) {
        ESP_LOGE(TAG, "Failed to create handler dispatcher queue");
        return;
    }

    // Create handler task with Internal RAM stack
    // CRITICAL: Must use Internal RAM because this task does file I/O (LittleFS)
    // When flash is accessed, cache is disabled, and PSRAM stacks become inaccessible!
    // Stack size: 16KB should be sufficient for file I/O operations
    // Priority: 5 (same as WebSocket task) for responsive HTTP handling
    BaseType_t ret = xTaskCreate(
        handlerTaskEntry,
        "http_handler",
        16384,  // 16KB stack in Internal RAM
        this,
        5,      // Priority
        &handler_task_
    );

    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create handler dispatcher task");
        vQueueDelete(request_queue_);
        request_queue_ = nullptr;
        return;
    }

    initialized_ = true;
    ESP_LOGI(TAG, "Handler dispatcher initialized (16KB Internal RAM stack)");
}

void HandlerDispatcher::handlerTaskEntry(void* arg) {
    HandlerDispatcher* self = static_cast<HandlerDispatcher*>(arg);
    self->handlerTaskLoop();
}

void HandlerDispatcher::handlerTaskLoop() {
    ESP_LOGI(TAG, "Handler dispatcher task started");

    while (true) {
        HandlerRequest* req = nullptr;

        // Wait for handler request
        if (xQueueReceive(request_queue_, &req, portMAX_DELAY) == pdTRUE && req) {
            // Execute the handler
            req->result = req->handler->handler(req->req);

            // Signal completion
            xSemaphoreGive(req->completion_sem);
        }
    }
}

esp_err_t HandlerDispatcher::executeHandler(httpd_req_t* req, const httpd_uri_t* handler) {
    if (!initialized_ || !request_queue_) {
        ESP_LOGE(TAG, "Handler dispatcher not initialized");
        return ESP_FAIL;
    }

    // Create completion semaphore
    SemaphoreHandle_t completion_sem = xSemaphoreCreateBinary();
    if (!completion_sem) {
        ESP_LOGE(TAG, "Failed to create completion semaphore");
        return ESP_FAIL;
    }

    // Create request
    HandlerRequest request = {
        .req = req,
        .handler = handler,
        .completion_sem = completion_sem,
        .result = ESP_FAIL
    };

    HandlerRequest* req_ptr = &request;

    // Send to dispatcher task
    if (xQueueSend(request_queue_, &req_ptr, pdMS_TO_TICKS(5000)) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to queue handler request (timeout)");
        vSemaphoreDelete(completion_sem);
        return ESP_FAIL;
    }

    // Wait for completion (timeout after 30 seconds for large file transfers)
    if (xSemaphoreTake(completion_sem, pdMS_TO_TICKS(30000)) != pdTRUE) {
        ESP_LOGE(TAG, "Handler execution timeout");
        vSemaphoreDelete(completion_sem);
        return ESP_FAIL;
    }

    esp_err_t result = request.result;
    vSemaphoreDelete(completion_sem);

    return result;
}

