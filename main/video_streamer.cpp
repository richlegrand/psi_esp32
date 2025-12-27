/**
 * VideoStreamer Implementation
 *
 * Complete H.264 video pipeline: camera → encoder → queue → RTP
 */

#include "video_streamer.hpp"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include <cstring>
#include <unistd.h>

// libdatachannel headers
#include "rtc/frameinfo.hpp"

// Device paths (ESP32-P4 V4L2 devices)
#define CAMERA_DEV_PATH   "/dev/video0"   // MIPI-CSI camera
#define ENCODER_DEV_PATH  "/dev/video11"  // H.264 encoder

static const char* TAG = "VideoStreamer";

// Global flag for frame timing logs (synchronized with h264rtppacketizer)
extern bool g_log_frame_timing;

//=============================================================================
// Constructor / Destructor
//=============================================================================

VideoStreamer::VideoStreamer(uint32_t width, uint32_t height, uint32_t fps)
    : width_(width), height_(height), fps_(fps),
      cap_fd_(-1), m2m_fd_(-1),
      send_queue_(nullptr),
      capture_task_(nullptr), send_task_(nullptr),
      running_(false), force_keyframe_(false),
      video_start_pts_(0), capture_frame_count_(0),
      frames_in_encoder_(0), frames_skipped_(0) {

    for (int i = 0; i < CAM_BUFFER_COUNT; i++) {
        cap_buffer_[i] = nullptr;
        cap_buffer_len_[i] = 0;
    }

    for (int i = 0; i < ENCODER_OUTPUT_BUFFERS; i++) {
        m2m_cap_buffer_[i] = nullptr;
        m2m_cap_buffer_len_[i] = 0;
    }
}

VideoStreamer::~VideoStreamer() {
    stopStreaming();
}

//=============================================================================
// Initialization
//=============================================================================

bool VideoStreamer::initCamera() {
    struct v4l2_capability cap;
    struct v4l2_format format;
    struct v4l2_requestbuffers req;
    struct v4l2_buffer buf;

    // Open camera device
    cap_fd_ = ::open(CAMERA_DEV_PATH, O_RDWR);
    if (cap_fd_ < 0) {
        ESP_LOGE(TAG, "Failed to open camera: %s", CAMERA_DEV_PATH);
        return false;
    }

    // Query capabilities
    if (ioctl(cap_fd_, VIDIOC_QUERYCAP, &cap) < 0) {
        ESP_LOGE(TAG, "Failed to query camera capabilities");
        return false;
    }
    ESP_LOGI(TAG, "Camera: %s", cap.card);

    // Set format (YUV420 for H.264 encoder input)
    memset(&format, 0, sizeof(format));
    format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    format.fmt.pix.width = width_;
    format.fmt.pix.height = height_;
    format.fmt.pix.pixelformat = V4L2_PIX_FMT_YUV420;

    if (ioctl(cap_fd_, VIDIOC_S_FMT, &format) < 0) {
        ESP_LOGE(TAG, "Failed to set camera format");
        return false;
    }

    // Request buffers
    memset(&req, 0, sizeof(req));
    req.count = CAM_BUFFER_COUNT;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;

    if (ioctl(cap_fd_, VIDIOC_REQBUFS, &req) < 0) {
        ESP_LOGE(TAG, "Failed to request camera buffers");
        return false;
    }

    // Map buffers
    for (int i = 0; i < CAM_BUFFER_COUNT; i++) {
        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;

        if (ioctl(cap_fd_, VIDIOC_QUERYBUF, &buf) < 0) {
            ESP_LOGE(TAG, "Failed to query camera buffer %d", i);
            return false;
        }

        cap_buffer_[i] = (uint8_t*)mmap(NULL, buf.length, PROT_READ | PROT_WRITE,
                                        MAP_SHARED, cap_fd_, buf.m.offset);
        if (cap_buffer_[i] == MAP_FAILED) {
            ESP_LOGE(TAG, "Failed to map camera buffer %d", i);
            cap_buffer_[i] = nullptr;
            return false;
        }
        cap_buffer_len_[i] = buf.length;

        // Queue buffer
        if (ioctl(cap_fd_, VIDIOC_QBUF, &buf) < 0) {
            ESP_LOGE(TAG, "Failed to queue camera buffer %d", i);
            return false;
        }
    }

    ESP_LOGI(TAG, "Camera initialized: %dx%d YUV420", width_, height_);
    return true;
}

bool VideoStreamer::initEncoder() {
    struct v4l2_capability cap;
    struct v4l2_format format;
    struct v4l2_requestbuffers req;
    struct v4l2_buffer buf;
    struct v4l2_ext_controls controls;
    struct v4l2_ext_control control[4];

    // Open encoder device
    m2m_fd_ = ::open(ENCODER_DEV_PATH, O_RDWR);
    if (m2m_fd_ < 0) {
        ESP_LOGE(TAG, "Failed to open H.264 encoder: %s", ENCODER_DEV_PATH);
        return false;
    }

    // Query capabilities
    if (ioctl(m2m_fd_, VIDIOC_QUERYCAP, &cap) < 0) {
        ESP_LOGE(TAG, "Failed to query encoder capabilities");
        return false;
    }
    ESP_LOGI(TAG, "Encoder: %s", cap.card);

    // Configure encoder parameters
    controls.ctrl_class = V4L2_CID_CODEC_CLASS;
    controls.count = 4;
    controls.controls = control;

    control[0].id = V4L2_CID_MPEG_VIDEO_H264_I_PERIOD;
    control[0].value = fps_;  // Keyframe every second

    control[1].id = V4L2_CID_MPEG_VIDEO_BITRATE;
    control[1].value = (width_ * height_ * fps_) / 8;

    control[2].id = V4L2_CID_MPEG_VIDEO_H264_MIN_QP;
    control[2].value = 10;

    control[3].id = V4L2_CID_MPEG_VIDEO_H264_MAX_QP;
    control[3].value = 35;

    if (ioctl(m2m_fd_, VIDIOC_S_EXT_CTRLS, &controls) < 0) {
        ESP_LOGE(TAG, "Failed to set encoder parameters: errno=%d (%s)", errno, strerror(errno));
        ESP_LOGE(TAG, "Encoder may use default settings - expect poor performance!");
    } else {
        ESP_LOGI(TAG, "Encoder configured: I-period=%d, bitrate=%d, QP=%d-%d",
                 control[0].value, control[1].value, control[2].value, control[3].value);
    }

    // Set encoder input format (YUV420)
    memset(&format, 0, sizeof(format));
    format.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    format.fmt.pix.width = width_;
    format.fmt.pix.height = height_;
    format.fmt.pix.pixelformat = V4L2_PIX_FMT_YUV420;
    format.fmt.pix.field = V4L2_FIELD_NONE;

    if (ioctl(m2m_fd_, VIDIOC_S_FMT, &format) < 0) {
        ESP_LOGE(TAG, "Failed to set encoder input format");
        return false;
    }

    // Request encoder input buffer (USERPTR - pass camera buffers directly)
    memset(&req, 0, sizeof(req));
    req.count = 1;
    req.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    req.memory = V4L2_MEMORY_USERPTR;

    if (ioctl(m2m_fd_, VIDIOC_REQBUFS, &req) < 0) {
        ESP_LOGE(TAG, "Failed to request encoder input buffer");
        return false;
    }

    // Set encoder output format (H.264)
    memset(&format, 0, sizeof(format));
    format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    format.fmt.pix.width = width_;
    format.fmt.pix.height = height_;
    format.fmt.pix.pixelformat = V4L2_PIX_FMT_H264;

    if (ioctl(m2m_fd_, VIDIOC_S_FMT, &format) < 0) {
        ESP_LOGE(TAG, "Failed to set encoder output format");
        return false;
    }

    // Request encoder output buffers
    memset(&req, 0, sizeof(req));
    req.count = ENCODER_OUTPUT_BUFFERS;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;

    if (ioctl(m2m_fd_, VIDIOC_REQBUFS, &req) < 0) {
        ESP_LOGE(TAG, "Failed to request encoder output buffers");
        return false;
    }

    // Map and queue encoder output buffers
    for (int i = 0; i < ENCODER_OUTPUT_BUFFERS; i++) {
        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;

        if (ioctl(m2m_fd_, VIDIOC_QUERYBUF, &buf) < 0) {
            ESP_LOGE(TAG, "Failed to query encoder output buffer %d", i);
            return false;
        }

        m2m_cap_buffer_[i] = (uint8_t*)mmap(NULL, buf.length, PROT_READ | PROT_WRITE,
                                            MAP_SHARED, m2m_fd_, buf.m.offset);
        if (m2m_cap_buffer_[i] == MAP_FAILED) {
            ESP_LOGE(TAG, "Failed to map encoder output buffer %d", i);
            m2m_cap_buffer_[i] = nullptr;
            return false;
        }
        m2m_cap_buffer_len_[i] = buf.length;
        ESP_LOGI(TAG, "Encoder output buffer %d: %zu bytes", i, m2m_cap_buffer_len_[i]);

        // Queue buffer
        if (ioctl(m2m_fd_, VIDIOC_QBUF, &buf) < 0) {
            ESP_LOGE(TAG, "Failed to queue encoder output buffer %d", i);
            return false;
        }
    }

    ESP_LOGI(TAG, "H.264 encoder initialized with %d output buffers", ENCODER_OUTPUT_BUFFERS);
    return true;
}

void VideoStreamer::cleanup() {
    // Unmap camera buffers
    for (int i = 0; i < CAM_BUFFER_COUNT; i++) {
        if (cap_buffer_[i] && cap_buffer_[i] != MAP_FAILED) {
            munmap(cap_buffer_[i], cap_buffer_len_[i]);
            cap_buffer_[i] = nullptr;
        }
    }

    if (cap_fd_ >= 0) {
        ::close(cap_fd_);
        cap_fd_ = -1;
    }

    // Unmap encoder buffers
    for (int i = 0; i < ENCODER_OUTPUT_BUFFERS; i++) {
        if (m2m_cap_buffer_[i] && m2m_cap_buffer_[i] != MAP_FAILED) {
            munmap(m2m_cap_buffer_[i], m2m_cap_buffer_len_[i]);
            m2m_cap_buffer_[i] = nullptr;
        }
    }

    if (m2m_fd_ >= 0) {
        ::close(m2m_fd_);
        m2m_fd_ = -1;
    }
}

//=============================================================================
// Track Management
//=============================================================================

bool VideoStreamer::addTrack(const std::string& client_id, std::shared_ptr<rtc::Track> track) {
    if (!track) {
        ESP_LOGE(TAG, "Track is null");
        return false;
    }

    ESP_LOGI(TAG, "Adding track for client: %s", client_id.c_str());

    // Add track to map
    {
        std::lock_guard<std::mutex> lock(tracks_mutex_);
        tracks_[client_id] = track;
    }

    // Start streaming if this is the first track
    if (!running_) {
        if (!startStreaming()) {
            ESP_LOGE(TAG, "Failed to start streaming");
            std::lock_guard<std::mutex> lock(tracks_mutex_);
            tracks_.erase(client_id);
            return false;
        }
    }

    ESP_LOGI(TAG, "Track added for client: %s (total tracks: %d)",
             client_id.c_str(), (int)tracks_.size());
    return true;
}

void VideoStreamer::removeTrack(const std::string& client_id) {
    ESP_LOGI(TAG, "Removing track for client: %s", client_id.c_str());

    bool should_stop = false;
    {
        std::lock_guard<std::mutex> lock(tracks_mutex_);
        auto it = tracks_.find(client_id);
        if (it != tracks_.end()) {
            tracks_.erase(it);
            ESP_LOGI(TAG, "Track removed for client: %s (remaining: %d)",
                     client_id.c_str(), (int)tracks_.size());

            // Stop streaming if no more tracks
            if (tracks_.empty()) {
                should_stop = true;
            }
        }
    }

    if (should_stop) {
        ESP_LOGI(TAG, "No more tracks, stopping streaming");
        stopStreaming();
    }
}

//=============================================================================
// Internal Start / Stop
//=============================================================================

bool VideoStreamer::startStreaming() {
    if (running_) {
        return true;  // Already running
    }

    ESP_LOGI(TAG, "Starting video streamer: %dx%d @ %d fps", width_, height_, fps_);

    // Initialize camera and encoder
    if (!initCamera()) {
        ESP_LOGE(TAG, "Failed to initialize camera");
        return false;
    }

    if (!initEncoder()) {
        ESP_LOGE(TAG, "Failed to initialize encoder");
        cleanup();
        return false;
    }

    // Create send queue
    send_queue_ = xQueueCreate(SEND_QUEUE_DEPTH, sizeof(QueuedFrame*));
    if (!send_queue_) {
        ESP_LOGE(TAG, "Failed to create send queue");
        cleanup();
        return false;
    }

    // Start device streams
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(cap_fd_, VIDIOC_STREAMON, &type) < 0) {
        ESP_LOGE(TAG, "Failed to start camera stream");
        cleanup();
        return false;
    }

    type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(m2m_fd_, VIDIOC_STREAMON, &type) < 0) {
        ESP_LOGE(TAG, "Failed to start encoder capture stream");
        cleanup();
        return false;
    }

    type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    if (ioctl(m2m_fd_, VIDIOC_STREAMON, &type) < 0) {
        ESP_LOGE(TAG, "Failed to start encoder output stream");
        cleanup();
        return false;
    }

    // Initialize state
    running_ = true;
    video_start_pts_ = 0;
    capture_frame_count_ = 0;
    frames_in_encoder_ = 0;
    frames_skipped_ = 0;

    // Create sender task (16KB stack, PSRAM OK - no file I/O)
    BaseType_t ret = xTaskCreate(
        sendTaskEntry,
        "video_send",
        16384,
        this,
        5,
        &send_task_
    );

    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create send task");
        stopStreaming();
        return false;
    }

    // Create capture task (16KB stack, Internal RAM - deep libdatachannel call chain)
    ret = xTaskCreate(
        captureTaskEntry,
        "video_capture",
        16384,
        this,
        5,
        &capture_task_
    );

    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create capture task");
        stopStreaming();
        return false;
    }

    ESP_LOGI(TAG, "Video streamer started");
    return true;
}

void VideoStreamer::stopStreaming() {
    if (!running_) {
        return;
    }

    ESP_LOGI(TAG, "Stopping video streamer...");
    running_ = false;

    // Wait for tasks to finish
    if (capture_task_) {
        vTaskDelay(pdMS_TO_TICKS(100));
        capture_task_ = nullptr;
    }

    if (send_task_) {
        vTaskDelay(pdMS_TO_TICKS(100));
        send_task_ = nullptr;
    }

    // Stop device streams
    if (cap_fd_ >= 0) {
        int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        ioctl(cap_fd_, VIDIOC_STREAMOFF, &type);
    }

    if (m2m_fd_ >= 0) {
        int type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
        ioctl(m2m_fd_, VIDIOC_STREAMOFF, &type);
        type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        ioctl(m2m_fd_, VIDIOC_STREAMOFF, &type);
    }

    // Clean up send queue
    if (send_queue_) {
        QueuedFrame* frame;
        while (xQueueReceive(send_queue_, &frame, 0) == pdTRUE) {
            delete frame;
        }
        vQueueDelete(send_queue_);
        send_queue_ = nullptr;
    }

    // Clean up devices
    cleanup();

    // Clear tracks
    {
        std::lock_guard<std::mutex> lock(tracks_mutex_);
        tracks_.clear();
    }

    // Print statistics
    if (frames_skipped_ > 0) {
        float skip_percent = (frames_skipped_ * 100.0f) / (capture_frame_count_ + frames_skipped_);
        ESP_LOGI(TAG, "Stopped: %lu frames captured, %u skipped (%.1f%%)",
                 (unsigned long)capture_frame_count_, frames_skipped_, skip_percent);
    } else {
        ESP_LOGI(TAG, "Stopped: %lu frames captured", (unsigned long)capture_frame_count_);
    }
}

//=============================================================================
// Capture Loop (Camera → Encoder → Queue)
//=============================================================================

void VideoStreamer::captureTaskEntry(void* arg) {
    VideoStreamer* self = static_cast<VideoStreamer*>(arg);
    self->captureLoop();
    vTaskDelete(NULL);
}

bool VideoStreamer::shouldSkipFrame() const {
    if (!send_queue_) {
        return false;
    }

    // Check how many frames are queued
    UBaseType_t queued = uxQueueMessagesWaiting(send_queue_);

    // Skip if queue is more than 75% full (6 out of 8 frames)
    // This prevents queue saturation and provides headroom for bursts
    return queued >= (SEND_QUEUE_DEPTH * 3 / 4);
}

void VideoStreamer::captureLoop() {
    ESP_LOGI(TAG, "Capture loop started (pipelined mode, %d encoder buffers)", ENCODER_OUTPUT_BUFFERS);

    struct v4l2_buffer cam_buf, enc_input_buf, enc_output_buf;
    uint64_t last_stats_time = esp_timer_get_time();

    while (running_) {
        // Try to submit camera frames to encoder (non-blocking pipeline feed)
        if (frames_in_encoder_ < ENCODER_OUTPUT_BUFFERS) {
            memset(&cam_buf, 0, sizeof(cam_buf));
            cam_buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            cam_buf.memory = V4L2_MEMORY_MMAP;

            // Get camera frame
            if (ioctl(cap_fd_, VIDIOC_DQBUF, &cam_buf) == 0) {
                // Check for backpressure (front-end skip)
                if (shouldSkipFrame()) {
                    // Skip encoding - return buffer immediately
                    ioctl(cap_fd_, VIDIOC_QBUF, &cam_buf);
                    frames_skipped_++;
                    ESP_LOGI(TAG, "Skipped frame (queue depth=%u)",
                             uxQueueMessagesWaiting(send_queue_));
                    continue;  // Don't try to read from encoder this iteration
                }

                // Submit to encoder
                memset(&enc_input_buf, 0, sizeof(enc_input_buf));
                enc_input_buf.index = 0;
                enc_input_buf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
                enc_input_buf.memory = V4L2_MEMORY_USERPTR;
                enc_input_buf.m.userptr = (unsigned long)cap_buffer_[cam_buf.index];
                enc_input_buf.length = cam_buf.bytesused;

                if (ioctl(m2m_fd_, VIDIOC_QBUF, &enc_input_buf) == 0) {
                    frames_in_encoder_++;
                } else {
                    ESP_LOGE(TAG, "Failed to queue frame to encoder: %s", strerror(errno));
                }

                // Return camera buffer
                ioctl(cap_fd_, VIDIOC_QBUF, &cam_buf);
            }
        }

        // Try to get encoded frames (non-blocking retrieval)
        memset(&enc_output_buf, 0, sizeof(enc_output_buf));
        enc_output_buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        enc_output_buf.memory = V4L2_MEMORY_MMAP;

        if (ioctl(m2m_fd_, VIDIOC_DQBUF, &enc_output_buf) == 0) {
            // Got an encoded frame
            uint64_t timestamp_us = esp_timer_get_time();
            bool keyframe = (enc_output_buf.flags & V4L2_BUF_FLAG_KEYFRAME) != 0;

            // Initialize PTS on first frame
            if (video_start_pts_ == 0) {
                video_start_pts_ = timestamp_us;
            }

            // Calculate relative PTS
            double pts_sec = (timestamp_us - video_start_pts_) / 1000000.0;
            std::chrono::duration<double> frameTime(pts_sec);
            rtc::FrameInfo frameInfo(frameTime);
            frameInfo.isKeyframe = keyframe;

            // Enable logging for every 5th frame
            capture_frame_count_++;
            if (capture_frame_count_ % 5 == 0) {
                g_log_frame_timing = true;
                ESP_LOGI(TAG, "Frame %lu [%s] %u B - queuing (depth=%u)",
                         (unsigned long)capture_frame_count_, keyframe ? "I" : "P",
                         enc_output_buf.bytesused, uxQueueMessagesWaiting(send_queue_));
            }

            // Allocate and queue frame (will be deleted by send task)
            QueuedFrame* frame = new QueuedFrame{
                std::vector<uint8_t>(m2m_cap_buffer_[enc_output_buf.index],
                                     m2m_cap_buffer_[enc_output_buf.index] + enc_output_buf.bytesused),
                frameInfo
            };

            if (xQueueSend(send_queue_, &frame, 0) != pdTRUE) {
                // Should never happen since we skip at front-end
                ESP_LOGW(TAG, "Send queue full despite front-end skip!");
                delete frame;
            }

            // Disable logging
            if (capture_frame_count_ % 5 == 0) {
                g_log_frame_timing = false;
            }

            // Return encoder buffers
            ioctl(m2m_fd_, VIDIOC_QBUF, &enc_output_buf);

            memset(&enc_input_buf, 0, sizeof(enc_input_buf));
            enc_input_buf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
            enc_input_buf.memory = V4L2_MEMORY_USERPTR;
            if (ioctl(m2m_fd_, VIDIOC_DQBUF, &enc_input_buf) == 0) {
                frames_in_encoder_--;
            }

            // Print statistics periodically
            uint64_t current_time = esp_timer_get_time();
            if ((current_time - last_stats_time) >= 1000000) {  // Every second
                uint64_t elapsed_us = current_time - video_start_pts_;
                float elapsed_sec = elapsed_us / 1000000.0f;
                float avg_fps = capture_frame_count_ / elapsed_sec;

                ESP_LOGI(TAG, "Frame %lu: %.1f fps (avg), %d in encoder, %u skipped",
                         (unsigned long)capture_frame_count_, avg_fps,
                         frames_in_encoder_, frames_skipped_);
                last_stats_time = current_time;
            }
        }

        // Small delay if encoder pipeline is empty
        if (frames_in_encoder_ == 0) {
            vTaskDelay(pdMS_TO_TICKS(1));
        }
    }

    ESP_LOGI(TAG, "Capture loop exited");
}

//=============================================================================
// Send Loop (Queue → RTP)
//=============================================================================

void VideoStreamer::sendTaskEntry(void* arg) {
    VideoStreamer* self = static_cast<VideoStreamer*>(arg);
    self->sendLoop();
    vTaskDelete(NULL);
}

void VideoStreamer::sendLoop() {
    ESP_LOGI(TAG, "Send task started");

    QueuedFrame* frame;
    uint64_t send_frame_count = 0;
    uint64_t total_send_us = 0;

    while (true) {
        // Wait for frame (blocking)
        if (xQueueReceive(send_queue_, &frame, portMAX_DELAY) == pdTRUE) {
            if (!frame) continue;

            try {
                // Send frame to all tracks
                uint64_t send_start_us = esp_timer_get_time();

                {
                    std::lock_guard<std::mutex> lock(tracks_mutex_);
                    for (auto& [client_id, track] : tracks_) {
                        if (track && track->isOpen()) {
                            track->sendFrame(
                                reinterpret_cast<const std::byte*>(frame->data.data()),
                                frame->data.size(),
                                frame->info
                            );
                        }
                    }
                }

                uint64_t send_end_us = esp_timer_get_time();
                uint64_t send_duration_us = send_end_us - send_start_us;

                // Track statistics
                send_frame_count++;
                total_send_us += send_duration_us;

                // Log stats every 50 frames
                if (send_frame_count % 50 == 0) {
                    uint32_t avg_send_ms = (total_send_us / send_frame_count) / 1000;
                    ESP_LOGI(TAG, "Send: %llu frames, avg=%u ms/frame",
                             send_frame_count, avg_send_ms);
                }

            } catch (const std::exception& e) {
                ESP_LOGE(TAG, "Send failed: %s", e.what());
            }

            // Free the frame
            delete frame;
        }
    }
}
