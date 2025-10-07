/**
 * ESP32 libdatachannel streamer
 * Adapted from libdatachannel streamer example
 * 
 * Streams H.264 video from LittleFS to WebRTC clients
 */

#include <cJSON.h>
#include "rtc/rtc.hpp"
#include "h264fileparser.hpp"
#include "opusfileparser.hpp"
#include "stream.hpp"
#include "helpers.hpp"
#include "esp_log.h"
#include "streamer.hpp"
#include "esp_transport_ws.h"
#include "esp_transport_tcp.h"

#include <chrono>
#include <unordered_map>
#include <memory>
#include <string>

using namespace rtc;
using namespace std;
using namespace std::chrono_literals;

static const char* TAG = "Streamer";

template <class T> weak_ptr<T> make_weak_ptr(shared_ptr<T> ptr) { return ptr; }

/// all connected clients
unordered_map<string, shared_ptr<Client>> clients{};

/// Global WebSocket transport for signaling
esp_transport_handle_t ws_transport = nullptr;

/// Send message via WebSocket
bool sendWebSocketMessage(const string& message);

/// Creates peer connection and client representation
shared_ptr<Client> createPeerConnection(const Configuration &config, string id);

/// Creates stream
shared_ptr<Stream> createStream(const string h264Samples, const unsigned fps);

/// Add client to stream
void addToStream(shared_ptr<Client> client, bool isAddingVideo);

/// Start stream
void startStream();

/// WebSocket receive task function
static void wsReceiveTask(void* param);

/// Main dispatch queue (initialized in startStreamer())
std::unique_ptr<DispatchQueue> MainThread;

/// Audio and video stream
optional<shared_ptr<Stream>> avStream = nullopt;

// ESP32 hardcoded configuration
const string h264SamplesDirectory = "/littlefs/h264";
const string wsServerUrl = "ws://192.168.1.248:8000";
const unsigned fps = 30;

/// Send message via WebSocket
bool sendWebSocketMessage(const string& message) {
    if (!ws_transport) {
        ESP_LOGE(TAG, "WebSocket not connected");
        return false;
    }

    int ret = esp_transport_ws_send_raw(ws_transport, WS_TRANSPORT_OPCODES_TEXT,
                                       message.c_str(), message.length(), 5000);
    if (ret < 0) {
        ESP_LOGE(TAG, "Failed to send WebSocket message");
        return false;
    }

    ESP_LOGD(TAG, "Sent WebSocket message: %s", message.c_str());
    return true;
}

/// Handle incoming WebSocket message
void wsOnMessage(const char* message_str, Configuration config) {
    printf("*0\n");
    cJSON *message = cJSON_Parse(message_str);
    if (!message) {
        ESP_LOGE(TAG, "Failed to parse JSON message");
        return;
    }

    printf("*1\n");
    cJSON *id_item = cJSON_GetObjectItem(message, "id");
    if (!id_item || !cJSON_IsString(id_item)) {
        cJSON_Delete(message);
        return;
    }
    string id = id_item->valuestring;

    printf("*2\n");
    cJSON *type_item = cJSON_GetObjectItem(message, "type");
    if (!type_item || !cJSON_IsString(type_item)) {
        cJSON_Delete(message);
        return;
    }
    string type = type_item->valuestring;

#if 1
    if (type == "request") {
        printf("*3\n");
        clients.emplace(id, createPeerConnection(config, id));
    } 
    else if (type == "answer") {
        printf("*4\n");
        if (auto jt = clients.find(id); jt != clients.end()) {
            auto pc = jt->second->peerConnection;
            cJSON *sdp_item = cJSON_GetObjectItem(message, "sdp");
            if (sdp_item && cJSON_IsString(sdp_item)) {
                string sdp = sdp_item->valuestring;
                printf("*5\n");
                auto description = Description(sdp, type);
                printf("*6\n");
                pc->setRemoteDescription(description);
            }
        }
    }
#endif

    printf("*7\n");
    cJSON_Delete(message);
    printf("*8\n");
}

shared_ptr<Client> createPeerConnection(const Configuration &config, string id) {
    printf("**0\n");
    auto pc = make_shared<PeerConnection>(config);
    printf("**0.5\n");
    auto client = make_shared<Client>(pc);

    printf("**1\n");
    pc->onStateChange([id](PeerConnection::State state) {
        ESP_LOGI(TAG, "State: %s", state == PeerConnection::State::Connected ? "Connected" : "Disconnected");
        if (state == PeerConnection::State::Disconnected ||
            state == PeerConnection::State::Failed ||
            state == PeerConnection::State::Closed) {
            clients.erase(id);
        }
    });

    printf("**2\n");
    pc->onGatheringStateChange([id, pc](PeerConnection::GatheringState state) {
        ESP_LOGI(TAG, "Gathering State: %s",
                 state == PeerConnection::GatheringState::Complete ? "Complete" : "In Progress");
        if (state == PeerConnection::GatheringState::Complete) {
            auto description = pc->localDescription();
            if (description.has_value()) {
                cJSON *message = cJSON_CreateObject();
                cJSON_AddStringToObject(message, "id", id.c_str());
                cJSON_AddStringToObject(message, "type", description->typeString().c_str());
                cJSON_AddStringToObject(message, "sdp", string(description.value()).c_str());

                char *message_str = cJSON_Print(message);
                sendWebSocketMessage(string(message_str));
                free(message_str);
                cJSON_Delete(message);
            }
        }
    });

    printf("**4\n");
    auto video = Description::Video("video", Description::Direction::SendOnly);
    video.addH264Codec(102);  // payload type
    video.setBitrate(3000); // 3Mbps

    auto track = pc->addTrack(video);
    
    printf("**5\n");
    // Setup RTP for H.264
    uint32_t ssrc = 42; // arbitrary SSRC
    auto rtpConfig = make_shared<RtpPacketizationConfig>(
        ssrc, "video", 102, H264RtpPacketizer::ClockRate);
    
    auto packetizer = make_shared<H264RtpPacketizer>(
        H264RtpPacketizer::Separator::StartSequence, rtpConfig);
    
    printf("**6\n");
    // Add RTCP SR reporter
    auto srReporter = make_shared<RtcpSrReporter>(rtpConfig);
    packetizer->addToChain(srReporter);
    
    // Set media handler
    track->setMediaHandler(packetizer);
    
    client->video = make_shared<ClientTrackData>(track, srReporter);
    client->setState(Client::State::WaitingForVideo);
    printf("**7\n");

    // Trigger offer creation and ICE gathering
    pc->setLocalDescription();

    return client;
};

shared_ptr<Stream> createStream(const string h264Samples, const unsigned fps) {
    auto video = make_shared<H264FileParser>(h264Samples, fps, true);

    // Create a dummy audio stream (FileParser will return empty samples since no .opus files exist)
    auto audio = make_shared<OPUSFileParser>("/littlefs/opus", true);

    auto stream = make_shared<Stream>(video, audio);
    
    stream->onSample([](Stream::StreamSourceType type, uint64_t sampleTime, rtc::binary sample) {
        vector<ClientTrack> tracks{};
        for (auto& [id, client] : clients) {
            if (client->getState() == Client::State::Ready) {
                if (type == Stream::StreamSourceType::Video && client->video.has_value()) {
                    tracks.emplace_back(id, client->video.value());
                }
            }
        }

        for (auto& track : tracks) {
            ESP_LOGD(TAG, "Sending video sample size: %zu to client %s", 
                     sample.size(), track.id.c_str());
            try {
                // Send sample with timestamp
                track.trackData->track->sendFrame(sample, 
                    std::chrono::duration<double, std::micro>(sampleTime));
            } catch (const std::exception &e) {
                ESP_LOGE(TAG, "Unable to send video packet: %s", e.what());
            }
        }
    });

    return stream;
}

/// Send previous key frame so browser can show something to user
void sendInitialNalus(shared_ptr<Stream> stream, shared_ptr<ClientTrackData> video) {
    auto h264 = dynamic_cast<H264FileParser *>(stream->video.get());
    auto initialNalus = h264->initialNALUS();

    // send previous NALU key frame so users don't have to wait to see stream works
    if (!initialNalus.empty()) {
        const double frameDuration_s = double(h264->getSampleDuration_us()) / (1000 * 1000);
        const uint32_t frameTimestampDuration = video->sender->rtpConfig->secondsToTimestamp(frameDuration_s);
        video->sender->rtpConfig->timestamp = video->sender->rtpConfig->startTimestamp - frameTimestampDuration * 2;
        video->track->send(initialNalus);
        video->sender->rtpConfig->timestamp += frameTimestampDuration;
        // Send initial NAL units again to start stream in firefox browser
        video->track->send(initialNalus);
    }
}

void addToStream(shared_ptr<Client> client, bool isAddingVideo) {
    if (client->getState() == Client::State::Waiting) {
        client->setState(isAddingVideo ? Client::State::WaitingForAudio : Client::State::WaitingForVideo);
    } else if ((client->getState() == Client::State::WaitingForAudio && !isAddingVideo)
               || (client->getState() == Client::State::WaitingForVideo && isAddingVideo)) {

        // Audio and video tracks are collected now
        assert(client->video.has_value() && client->audio.has_value());
        auto video = client->video.value();

        if (avStream.has_value()) {
            sendInitialNalus(avStream.value(), video);
        }

        client->setState(Client::State::Ready);
    }
    if (client->getState() == Client::State::Ready) {
        startStream();
    }
}

void startStream() {
    if (!avStream.has_value()) {
        avStream = createStream(h264SamplesDirectory, fps);
    }

    avStream.value()->start();
    ESP_LOGI(TAG, "Stream started");
}

// WebSocket receive task - runs in separate FreeRTOS task
static void wsReceiveTask(void* param) {
    Configuration* config = (Configuration*)param;
    char buffer[1024];

    while (ws_transport) {
        int len = esp_transport_read(ws_transport, buffer, sizeof(buffer) - 1, 1000);
        if (len > 0) {
            buffer[len] = '\0';
            printf("*** 0\n");
            wsOnMessage(buffer, *config);
            printf("*** 1\n");
        } else if (len < 0) {
            ESP_LOGE(TAG, "WebSocket read error");
            break;
        }
        // len == 0 means timeout, continue
    }

    vTaskDelete(NULL);
}

void startStreamer() {
    InitLogger(LogLevel::Info);

    // Check memory availability before WebRTC initialization
    printf("Memory status before WebRTC init:\n");
    printf("  Total free: %d KB\n", heap_caps_get_free_size(MALLOC_CAP_DEFAULT) / 1024);
    printf("  Internal free: %d KB\n", heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024);
    printf("  DMA free: %d KB\n", heap_caps_get_free_size(MALLOC_CAP_DMA) / 1024);
    printf("  DMA+Internal free: %d KB\n", heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL) / 1024);

    // Initialize the main dispatch queue now (not as global constructor)
    MainThread = std::make_unique<DispatchQueue>("Main");

    Configuration rtcConfig;
    // Add STUN servers for NAT traversal
    rtcConfig.iceServers.push_back({"stun:stun.l.google.com:19302"});

    // Connect to signaling server using ESP-IDF WebSocket transport
    esp_transport_handle_t tcp_transport = esp_transport_tcp_init();
    if (!tcp_transport) {
        ESP_LOGE(TAG, "Failed to initialize TCP transport");
        return;
    }

    ws_transport = esp_transport_ws_init(tcp_transport);
    if (!ws_transport) {
        ESP_LOGE(TAG, "Failed to initialize WebSocket transport");
        esp_transport_destroy(tcp_transport);
        return;
    }

    // Parse WebSocket URL - simplified for ws://host:port format
    string host = "192.168.1.248";  // Extract from wsServerUrl
    int port = 8000;
    string path = "/server";  // Identify this client as "server" to the signaling server

    esp_transport_ws_set_path(ws_transport, path.c_str());

    // Connect to WebSocket server
    int ret = esp_transport_connect(ws_transport, host.c_str(), port, 5000);
    if (ret < 0) {
        ESP_LOGE(TAG, "WebSocket connection failed");
        esp_transport_destroy(ws_transport);
        esp_transport_destroy(tcp_transport);
        ws_transport = nullptr;
        return;
    }

    ESP_LOGI(TAG, "WebSocket connected to signaling server");
    startStream();

    // Start task to handle incoming WebSocket messages
    // Use static function for FreeRTOS compatibility (not lambda)
    xTaskCreate(wsReceiveTask, "ws_recv", 20480, &rtcConfig, 5, NULL);
    ESP_LOGI(TAG, "Streamer ready, waiting for connections...");

    // Keep running forever - rtcConfig must stay alive for wsReceiveTask
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(10000)); // 10 second delay
        ESP_LOGI(TAG, "Streamer active - Free heap: %lu KB", esp_get_free_heap_size() / 1024);
    }

}