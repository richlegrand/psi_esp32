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
#include "esp_websocket_client.h"

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

/// Global WebSocket client for signaling
esp_websocket_client_handle_t ws_client = nullptr;

/// Global WebRTC configuration (needed for event handler)
Configuration* global_rtcConfig = nullptr;

/// Buffer for fragmented WebSocket messages
static string ws_message_buffer;

/// Send message via WebSocket
bool sendWebSocketMessage(const string& message);

/// WebSocket event handler
static void websocket_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data);

/// Creates peer connection and client representation
shared_ptr<Client> createPeerConnection(const Configuration &config, string id);

/// Creates stream
shared_ptr<Stream> createStream(const string h264Samples, const unsigned fps);

/// Add client to stream
void addToStream(shared_ptr<Client> client, bool isAddingVideo);

/// Start stream
void startStream();

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
    if (!ws_client || !esp_websocket_client_is_connected(ws_client)) {
        ESP_LOGE(TAG, "WebSocket not connected");
        return false;
    }

    ESP_LOGI(TAG, "Sending WebSocket message, len=%d", message.length());
    int ret = esp_websocket_client_send_text(ws_client, message.c_str(), message.length(), portMAX_DELAY);
    if (ret < 0) {
        ESP_LOGE(TAG, "Failed to send WebSocket message, ret=%d", ret);
        return false;
    }

    ESP_LOGI(TAG, "Sent WebSocket message successfully, len=%d", ret);
    return true;
}

/// WebSocket event handler
static void websocket_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;

    switch (event_id) {
        case WEBSOCKET_EVENT_CONNECTED:
            ESP_LOGI(TAG, "WebSocket connected to signaling server");
            break;

        case WEBSOCKET_EVENT_DISCONNECTED:
            ESP_LOGI(TAG, "WebSocket disconnected");
            break;

        case WEBSOCKET_EVENT_DATA:
            if (data->op_code == 0x01 && data->data_len > 0) {  // Text frame
                // Append received data to buffer
                ws_message_buffer.append((char*)data->data_ptr, data->data_len);

                // Check if we have a complete JSON message (starts with { and ends with })
                bool is_complete = false;
                if (!ws_message_buffer.empty() &&
                    ws_message_buffer.front() == '{' &&
                    ws_message_buffer.back() == '}') {
                    is_complete = true;
                }

                if (is_complete) {
                    ESP_LOGI(TAG, "Received complete WebSocket message (len=%d)", ws_message_buffer.length());

                    // Parse JSON message
                    cJSON *message = cJSON_Parse(ws_message_buffer.c_str());

                    if (!message) {
                        ESP_LOGE(TAG, "Failed to parse JSON message");
                        ws_message_buffer.clear();
                        break;
                    }

                    cJSON *id_item = cJSON_GetObjectItem(message, "id");
                    if (!id_item || !cJSON_IsString(id_item)) {
                        cJSON_Delete(message);
                        ws_message_buffer.clear();
                        break;
                    }
                    string id = id_item->valuestring;

                    cJSON *type_item = cJSON_GetObjectItem(message, "type");
                    if (!type_item || !cJSON_IsString(type_item)) {
                        cJSON_Delete(message);
                        ws_message_buffer.clear();
                        break;
                    }
                    string type = type_item->valuestring;

                    if (type == "request") {
                        ESP_LOGI(TAG, "Received request from client: %s", id.c_str());
                        // Dispatch to main thread to avoid stack overflow in websocket_task
                        MainThread->dispatch([id]() {
                            ESP_LOGI(TAG, "Creating peer connection for client: %s", id.c_str());
                            clients.emplace(id, createPeerConnection(*global_rtcConfig, id));
                        });
                    }
                    else if (type == "answer") {
                        ESP_LOGI(TAG, "Received answer from client: %s", id.c_str());
                        // Extract SDP now before cJSON_Delete
                        cJSON *sdp_item = cJSON_GetObjectItem(message, "sdp");
                        if (sdp_item && cJSON_IsString(sdp_item)) {
                            string sdp = sdp_item->valuestring;
                            // Dispatch to main thread to avoid stack issues
                            MainThread->dispatch([id, sdp, type]() {
                                if (auto jt = clients.find(id); jt != clients.end()) {
                                    auto pc = jt->second->peerConnection;
                                    auto description = Description(sdp, type);
                                    pc->setRemoteDescription(description);
                                }
                            });
                        }
                    }

                    cJSON_Delete(message);
                    ws_message_buffer.clear();
                } else {
                    ESP_LOGD(TAG, "Received WebSocket fragment (len=%d), buffering...", data->data_len);
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

shared_ptr<Client> createPeerConnection(const Configuration &config, string id) {
    auto pc = make_shared<PeerConnection>(config);
    auto client = make_shared<Client>(pc);

    pc->onStateChange([id](PeerConnection::State state) {
        ESP_LOGI(TAG, "State: %s", state == PeerConnection::State::Connected ? "Connected" : "Disconnected");
        if (state == PeerConnection::State::Disconnected ||
            state == PeerConnection::State::Failed ||
            state == PeerConnection::State::Closed) {
            clients.erase(id);
        }
    });

    pc->onGatheringStateChange([id, pc](PeerConnection::GatheringState state) {
        ESP_LOGI(TAG, "Gathering State: %s",
                 state == PeerConnection::GatheringState::Complete ? "Complete" : "In Progress");
        if (state == PeerConnection::GatheringState::Complete) {
            auto description = pc->localDescription();
            if (description.has_value()) {
                ESP_LOGI(TAG, "Sending offer to client: %s", id.c_str());
                cJSON *message = cJSON_CreateObject();
                cJSON_AddStringToObject(message, "id", id.c_str());
                cJSON_AddStringToObject(message, "type", description->typeString().c_str());
                cJSON_AddStringToObject(message, "sdp", string(description.value()).c_str());

                char *message_str = cJSON_Print(message);
                if (message_str) {
                    sendWebSocketMessage(string(message_str));
                    free(message_str);
                }
                cJSON_Delete(message);
            }
        }
    });

    // Add video track
    auto video = Description::Video("video-stream");
    video.addH264Codec(102);
    video.addSSRC(1, "video-stream", "stream1", "video-stream");
    auto videoTrack = pc->addTrack(video);

    // Setup RTP for H.264
    auto videoRtpConfig = make_shared<RtpPacketizationConfig>(1, "video-stream", 102, H264RtpPacketizer::ClockRate);
    auto h264Packetizer = make_shared<H264RtpPacketizer>(H264RtpPacketizer::Separator::Length, videoRtpConfig);

    // Add RTCP SR reporter for video
    auto videoSrReporter = make_shared<RtcpSrReporter>(videoRtpConfig);
    h264Packetizer->addToChain(videoSrReporter);

    // Add RTCP NACK responder for video
    auto videoNackResponder = make_shared<RtcpNackResponder>();
    h264Packetizer->addToChain(videoNackResponder);

    // Set media handler for video
    videoTrack->setMediaHandler(h264Packetizer);

    // Set onOpen callback for video track
    videoTrack->onOpen([id, wc = make_weak_ptr(client)]() {
        ESP_LOGI(TAG, "Video track opened for client: %s", id.c_str());
        MainThread->dispatch([wc]() {
            if (auto c = wc.lock()) {
                addToStream(c, true);
            }
        });
    });

    client->video = make_shared<ClientTrackData>(videoTrack, videoSrReporter);

    // Add audio track
    auto audio = Description::Audio("audio-stream");
    audio.addOpusCodec(111);
    audio.addSSRC(2, "audio-stream", "stream1", "audio-stream");
    auto audioTrack = pc->addTrack(audio);

    // Setup RTP for Opus
    auto audioRtpConfig = make_shared<RtpPacketizationConfig>(2, "audio-stream", 111, OpusRtpPacketizer::DefaultClockRate);
    auto opusPacketizer = make_shared<OpusRtpPacketizer>(audioRtpConfig);

    // Add RTCP SR reporter for audio
    auto audioSrReporter = make_shared<RtcpSrReporter>(audioRtpConfig);
    opusPacketizer->addToChain(audioSrReporter);

    // Add RTCP NACK responder for audio
    auto audioNackResponder = make_shared<RtcpNackResponder>();
    opusPacketizer->addToChain(audioNackResponder);

    // Set media handler for audio
    audioTrack->setMediaHandler(opusPacketizer);

    // Set onOpen callback for audio track
    audioTrack->onOpen([id, wc = make_weak_ptr(client)]() {
        ESP_LOGI(TAG, "Audio track opened for client: %s", id.c_str());
        MainThread->dispatch([wc]() {
            if (auto c = wc.lock()) {
                addToStream(c, false);
            }
        });
    });

    client->audio = make_shared<ClientTrackData>(audioTrack, audioSrReporter);

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
        string streamType = type == Stream::StreamSourceType::Video ? "video" : "audio";

        // Get tracks for the given type
        for (auto& [id, client] : clients) {
            if (client->getState() == Client::State::Ready) {
                auto optTrackData = type == Stream::StreamSourceType::Video ? client->video : client->audio;
                if (optTrackData.has_value()) {
                    tracks.emplace_back(id, optTrackData.value());
                }
            }
        }

        for (auto& track : tracks) {
            ESP_LOGD(TAG, "Sending %s sample size: %zu to client %s",
                     streamType.c_str(), sample.size(), track.id.c_str());
            try {
                // Check if track is open before sending
                if (!track.trackData->track->isOpen()) {
                    ESP_LOGW(TAG, "Track not open yet for client %s", track.id.c_str());
                    continue;
                }
                // Send sample with timestamp
                track.trackData->track->sendFrame(rtc::binary(sample), std::chrono::duration<double, std::micro>(sampleTime));
            } catch (const std::exception &e) {
                ESP_LOGE(TAG, "Unable to send %s packet: %s", streamType.c_str(), e.what());
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
    try {
        ESP_LOGI(TAG, "addToStream: isAddingVideo=%d, state=%d", isAddingVideo, (int)client->getState());

        if (client->getState() == Client::State::Waiting) {
            client->setState(isAddingVideo ? Client::State::WaitingForAudio : Client::State::WaitingForVideo);
            ESP_LOGI(TAG, "addToStream: State changed to %d", (int)client->getState());
        } else if ((client->getState() == Client::State::WaitingForAudio && !isAddingVideo)
                   || (client->getState() == Client::State::WaitingForVideo && isAddingVideo)) {

            // Audio and video tracks are collected now
            if (!client->video.has_value() || !client->audio.has_value()) {
                ESP_LOGE(TAG, "addToStream: video or audio track missing!");
                return;
            }
            ESP_LOGI(TAG, "addToStream: Both tracks available");
            auto video = client->video.value();

            // Skip initial NALUs - SRTP replay protection rejects backwards timestamps
            // if (avStream.has_value()) {
            //     ESP_LOGI(TAG, "addToStream: Sending initial NALUs");
            //     sendInitialNalus(avStream.value(), video);
            //     ESP_LOGI(TAG, "addToStream: Initial NALUs sent");
            // }

            ESP_LOGI(TAG, "addToStream: Setting state to Ready");
            client->setState(Client::State::Ready);
        }
        if (client->getState() == Client::State::Ready) {
            ESP_LOGI(TAG, "addToStream: Starting stream");
            startStream();
            ESP_LOGI(TAG, "addToStream: Stream started");
        }
    } catch (const std::exception& e) {
        ESP_LOGE(TAG, "addToStream: Exception caught: %s", e.what());
    }
}

void startStream() {
    if (!avStream.has_value()) {
        avStream = createStream(h264SamplesDirectory, fps);
    }

    avStream.value()->start();
    ESP_LOGI(TAG, "Stream started");
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

    // Static configuration that stays alive
    static Configuration rtcConfig;
    rtcConfig.iceServers.push_back({"stun:stun.l.google.com:19302"});
    global_rtcConfig = &rtcConfig;

    // Connect to signaling server using esp_websocket_client
    string ws_uri = wsServerUrl + "/server";  // ws://192.168.1.248:8000/server

    esp_websocket_client_config_t websocket_cfg = {};
    websocket_cfg.uri = ws_uri.c_str();
    websocket_cfg.task_stack = 8192;  // Increase stack size from default (4096)
    websocket_cfg.buffer_size = 2048;  // Increase buffer size to handle large SDP messages

    ESP_LOGI(TAG, "Connecting to WebSocket signaling server: %s", ws_uri.c_str());
    ws_client = esp_websocket_client_init(&websocket_cfg);
    if (!ws_client) {
        ESP_LOGE(TAG, "Failed to initialize WebSocket client");
        return;
    }

    // Register event handler
    esp_websocket_register_events(ws_client, WEBSOCKET_EVENT_ANY, websocket_event_handler, NULL);

    // Start WebSocket client (connects asynchronously)
    esp_websocket_client_start(ws_client);

    startStream();

    ESP_LOGI(TAG, "Streamer ready, waiting for connections...");

    // Keep running forever - monitor heap including DMA which is limited
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(500)); // 500ms second delay
        size_t free_heap = esp_get_free_heap_size();
        size_t free_dma = heap_caps_get_free_size(MALLOC_CAP_DMA);
        size_t free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        ESP_LOGI(TAG, "Heap: %lu KB free | DMA: %lu KB | Internal: %lu KB | Clients: %zu",
                 free_heap / 1024, free_dma / 1024, free_internal / 1024, clients.size());
    }

}