/**
 * ESP32 libdatachannel streamer
 * Adapted from libdatachannel streamer example
 * 
 * Streams H.264 video from LittleFS to WebRTC clients
 */

#include <cJSON.h>
#include "rtc/rtc.hpp"
#include "h264fileparser.hpp"
#include "stream.hpp"
#include "helpers.hpp"
#include "esp_log.h"
#include "streamer.hpp"

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

/// Creates peer connection and client representation
shared_ptr<Client> createPeerConnection(const Configuration &config,
                                        weak_ptr<WebSocket> wws,
                                        string id);

/// Creates stream
shared_ptr<Stream> createStream(const string h264Samples, const unsigned fps);

/// Add client to stream
void addToStream(shared_ptr<Client> client, bool isAddingVideo);

/// Start stream
void startStream();

/// Main dispatch queue
DispatchQueue MainThread("Main");

/// Audio and video stream
optional<shared_ptr<Stream>> avStream = nullopt;

// ESP32 hardcoded configuration
const string h264SamplesDirectory = "/littlefs/h264";
const string wsServerUrl = "ws://192.168.1.100:8000";
const unsigned fps = 30;

/// Handle incoming WebSocket message
void wsOnMessage(const char* message_str, Configuration config, shared_ptr<WebSocket> ws) {
    cJSON *message = cJSON_Parse(message_str);
    if (!message) {
        ESP_LOGE(TAG, "Failed to parse JSON message");
        return;
    }

    cJSON *id_item = cJSON_GetObjectItem(message, "id");
    if (!id_item || !cJSON_IsString(id_item)) {
        cJSON_Delete(message);
        return;
    }
    string id = id_item->valuestring;

    cJSON *type_item = cJSON_GetObjectItem(message, "type");
    if (!type_item || !cJSON_IsString(type_item)) {
        cJSON_Delete(message);
        return;
    }
    string type = type_item->valuestring;

    if (type == "request") {
        clients.emplace(id, createPeerConnection(config, make_weak_ptr(ws), id));
    } else if (type == "answer") {
        if (auto jt = clients.find(id); jt != clients.end()) {
            auto pc = jt->second->peerConnection;
            cJSON *sdp_item = cJSON_GetObjectItem(message, "sdp");
            if (sdp_item && cJSON_IsString(sdp_item)) {
                string sdp = sdp_item->valuestring;
                auto description = Description(sdp, type);
                pc->setRemoteDescription(description);
            }
        }
    }
    
    cJSON_Delete(message);
}

shared_ptr<Client> createPeerConnection(const Configuration &config,
                                       weak_ptr<WebSocket> wws,
                                       string id) {
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

    pc->onGatheringStateChange([](PeerConnection::GatheringState state) {
        ESP_LOGI(TAG, "Gathering State: %s", 
                 state == PeerConnection::GatheringState::Complete ? "Complete" : "In Progress");
    });

    pc->onLocalDescription([id, wws](Description description) {
        cJSON *message = cJSON_CreateObject();
        cJSON_AddStringToObject(message, "id", id.c_str());
        cJSON_AddStringToObject(message, "type", description.typeString().c_str());
        cJSON_AddStringToObject(message, "sdp", string(description).c_str());
        
        char *message_str = cJSON_Print(message);
        if (auto ws = wws.lock()) {
            ws->send(message_str);
        }
        free(message_str);
        cJSON_Delete(message);
    });

    auto video = Description::Video("video", Description::Direction::SendOnly);
    video.addH264Codec(102);  // payload type
    video.setBitrate(3000); // 3Mbps

    auto track = pc->addTrack(video);
    
    // Setup RTP for H.264
    uint32_t ssrc = 42; // arbitrary SSRC
    auto rtpConfig = make_shared<RtpPacketizationConfig>(
        ssrc, "video", 102, H264RtpPacketizer::ClockRate);
    
    auto packetizer = make_shared<H264RtpPacketizer>(
        H264RtpPacketizer::Separator::StartSequence, rtpConfig);
    
    // Add RTCP SR reporter
    auto srReporter = make_shared<RtcpSrReporter>(rtpConfig);
    packetizer->addToChain(srReporter);
    
    // Set media handler
    track->setMediaHandler(packetizer);
    
    client->video = make_shared<ClientTrackData>(track, srReporter);
    client->setState(Client::State::WaitingForVideo);

    return client;
};

shared_ptr<Stream> createStream(const string h264Samples, const unsigned fps) {
    auto video = make_shared<H264FileParser>(h264Samples, fps, true);
    
    // Only H.264 video for now (no audio)
    auto stream = make_shared<Stream>(video, nullptr);
    
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

void startStreamer() {
    InitLogger(LogLevel::Info);

    Configuration rtcConfig;
    // Add STUN servers for NAT traversal
    rtcConfig.iceServers.push_back({"stun:stun.l.google.com:19302"});

    // Connect to signaling server
    auto ws = make_shared<WebSocket>();
    
    ws->onOpen([&]() {
        ESP_LOGI(TAG, "WebSocket connected to signaling server");
        startStream();
    });

    ws->onError([](string error) {
        ESP_LOGE(TAG, "WebSocket error: %s", error.c_str());
    });

    ws->onClosed([]() {
        ESP_LOGI(TAG, "WebSocket closed");
    });

    ws->onMessage([&](variant<binary, string> data) {
        if (holds_alternative<string>(data)) {
            wsOnMessage(get<string>(data).c_str(), rtcConfig, ws);
        }
    });

    ws->open(wsServerUrl);
    
    ESP_LOGI(TAG, "Streamer ready, waiting for connections...");
}