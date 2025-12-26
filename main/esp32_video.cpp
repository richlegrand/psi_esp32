/**
 * ESP32-P4 H.264 Video Capture and Encoding Implementation
 */

#include "esp32_video.hpp"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include <cstring>
#include <unistd.h>

// Device paths (from esp_video component)
#define EXAMPLE_CAM_DEV_PATH   "/dev/video0"   // MIPI-CSI camera
#define EXAMPLE_H264_DEV_PATH  "/dev/video11"  // H.264 encoder

static const char* TAG = "esp32_video";

// Test mode: Skip callback to measure pure encoder performance
#define TEST_ENCODER_ONLY 0

ESP32Video::ESP32Video(uint32_t width, uint32_t height, uint32_t fps)
    : width_(width), height_(height), fps_(fps),
      cap_fd_(-1), m2m_fd_(-1),
      capture_task_(nullptr), running_(false), force_keyframe_(false),
      start_time_us_(0), frame_count_(0), frames_in_encoder_(0) {

    for (int i = 0; i < CAM_BUFFER_COUNT; i++) {
        cap_buffer_[i] = nullptr;
        cap_buffer_len_[i] = 0;
    }

    for (int i = 0; i < ENCODER_OUTPUT_BUFFERS; i++) {
        m2m_cap_buffer_[i] = nullptr;
        m2m_cap_buffer_len_[i] = 0;
    }
}

ESP32Video::~ESP32Video() {
    stop();
    close();
}

bool ESP32Video::open() {
    ESP_LOGI(TAG, "Opening ESP32 video: %dx%d @ %d fps", width_, height_, fps_);

    if (!initCamera()) {
        ESP_LOGE(TAG, "Failed to initialize camera");
        return false;
    }

    if (!initEncoder()) {
        ESP_LOGE(TAG, "Failed to initialize encoder");
        close();
        return false;
    }

    ESP_LOGI(TAG, "ESP32 video opened successfully");
    return true;
}

bool ESP32Video::initCamera() {
    struct v4l2_capability cap;
    struct v4l2_format format;
    struct v4l2_requestbuffers req;
    struct v4l2_buffer buf;

    // Open camera device
    cap_fd_ = ::open(EXAMPLE_CAM_DEV_PATH, O_RDWR);
    if (cap_fd_ < 0) {
        ESP_LOGE(TAG, "Failed to open camera device: %s", EXAMPLE_CAM_DEV_PATH);
        return false;
    }

    // Query capabilities
    if (ioctl(cap_fd_, VIDIOC_QUERYCAP, &cap) < 0) {
        ESP_LOGE(TAG, "Failed to query camera capabilities");
        return false;
    }
    ESP_LOGI(TAG, "Camera: %s", cap.card);

    // Set camera format - YUV420 for H.264 encoder input
    memset(&format, 0, sizeof(format));
    format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    format.fmt.pix.width = width_;
    format.fmt.pix.height = height_;
    format.fmt.pix.pixelformat = V4L2_PIX_FMT_YUV420;

    if (ioctl(cap_fd_, VIDIOC_S_FMT, &format) < 0) {
        ESP_LOGE(TAG, "Failed to set camera format");
        return false;
    }

    // Request camera buffers
    memset(&req, 0, sizeof(req));
    req.count = CAM_BUFFER_COUNT;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;

    if (ioctl(cap_fd_, VIDIOC_REQBUFS, &req) < 0) {
        ESP_LOGE(TAG, "Failed to request camera buffers");
        return false;
    }

    // Map camera buffers
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

bool ESP32Video::initEncoder() {
    struct v4l2_capability cap;
    struct v4l2_format format;
    struct v4l2_requestbuffers req;
    struct v4l2_buffer buf;
    struct v4l2_ext_controls controls;
    struct v4l2_ext_control control[4];

    // Open encoder device
    m2m_fd_ = ::open(EXAMPLE_H264_DEV_PATH, O_RDWR);
    if (m2m_fd_ < 0) {
        ESP_LOGE(TAG, "Failed to open H.264 encoder device: %s", EXAMPLE_H264_DEV_PATH);
        return false;
    }

    // Query capabilities
    if (ioctl(m2m_fd_, VIDIOC_QUERYCAP, &cap) < 0) {
        ESP_LOGE(TAG, "Failed to query encoder capabilities");
        return false;
    }
    ESP_LOGI(TAG, "Encoder: %s", cap.card);

    // Configure H.264 encoder parameters
    controls.ctrl_class = V4L2_CID_CODEC_CLASS;
    controls.count = 4;
    controls.controls = control;

    // I-frame period
    control[0].id = V4L2_CID_MPEG_VIDEO_H264_I_PERIOD;
    control[0].value = fps_;  // Keyframe every second

    // Bitrate
    control[1].id = V4L2_CID_MPEG_VIDEO_BITRATE;
    control[1].value = (width_ * height_ * fps_) / 8;  // Match working example

    // QP range
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

    // Configure encoder input (raw YUV frames)
    memset(&format, 0, sizeof(format));
    format.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    format.fmt.pix.width = width_;
    format.fmt.pix.height = height_;
    format.fmt.pix.pixelformat = V4L2_PIX_FMT_YUV420;
    format.fmt.pix.field = V4L2_FIELD_NONE;  // Progressive (not interlaced)

    if (ioctl(m2m_fd_, VIDIOC_S_FMT, &format) < 0) {
        ESP_LOGE(TAG, "Failed to set encoder input format");
        return false;
    }

    // Request encoder input buffer (USERPTR - we'll pass camera buffers directly)
    memset(&req, 0, sizeof(req));
    req.count = 1;
    req.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    req.memory = V4L2_MEMORY_USERPTR;

    if (ioctl(m2m_fd_, VIDIOC_REQBUFS, &req) < 0) {
        ESP_LOGE(TAG, "Failed to request encoder input buffer");
        return false;
    }

    // Configure encoder output (H.264 stream)
    memset(&format, 0, sizeof(format));
    format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    format.fmt.pix.width = width_;
    format.fmt.pix.height = height_;
    format.fmt.pix.pixelformat = V4L2_PIX_FMT_H264;

    if (ioctl(m2m_fd_, VIDIOC_S_FMT, &format) < 0) {
        ESP_LOGE(TAG, "Failed to set encoder output format");
        return false;
    }

    // Request encoder output buffers (3 for pipelining)
    memset(&req, 0, sizeof(req));
    req.count = ENCODER_OUTPUT_BUFFERS;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;

    if (ioctl(m2m_fd_, VIDIOC_REQBUFS, &req) < 0) {
        ESP_LOGE(TAG, "Failed to request encoder output buffers");
        return false;
    }

    // Map and queue all encoder output buffers
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

        // Queue encoder output buffer
        if (ioctl(m2m_fd_, VIDIOC_QBUF, &buf) < 0) {
            ESP_LOGE(TAG, "Failed to queue encoder output buffer %d", i);
            return false;
        }
    }

    ESP_LOGI(TAG, "H.264 encoder initialized with %d output buffers", ENCODER_OUTPUT_BUFFERS);
    return true;
}

bool ESP32Video::start(FrameCallback callback) {
    if (running_) {
        ESP_LOGW(TAG, "Video already running");
        return false;
    }

    if (cap_fd_ < 0 || m2m_fd_ < 0) {
        ESP_LOGE(TAG, "Video not opened");
        return false;
    }

    frame_callback_ = callback;
    running_ = true;
    start_time_us_ = esp_timer_get_time();
    frame_count_ = 0;
    frames_in_encoder_ = 0;

    // Start camera stream
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(cap_fd_, VIDIOC_STREAMON, &type) < 0) {
        ESP_LOGE(TAG, "Failed to start camera stream");
        running_ = false;
        return false;
    }

    // Start encoder streams
    type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(m2m_fd_, VIDIOC_STREAMON, &type) < 0) {
        ESP_LOGE(TAG, "Failed to start encoder capture stream");
        running_ = false;
        return false;
    }

    type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    if (ioctl(m2m_fd_, VIDIOC_STREAMON, &type) < 0) {
        ESP_LOGE(TAG, "Failed to start encoder output stream");
        running_ = false;
        return false;
    }

    // Create capture task (stack in Internal RAM for file I/O safety)
    // Needs 16KB stack due to deep libdatachannel call chain during frame transmission
    BaseType_t ret = xTaskCreate(
        captureTaskEntry,
        "video_capture",
        16384,  // Stack size (16KB - deep call chain in libdatachannel)
        this,
        5,      // Priority
        &capture_task_
    );

    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create capture task");
        stop();
        return false;
    }

    ESP_LOGI(TAG, "Video capture started");
    return true;
}

void ESP32Video::stop() {
    if (!running_) {
        return;
    }

    ESP_LOGI(TAG, "Stopping video capture...");
    running_ = false;

    // Wait for capture task to finish
    if (capture_task_) {
        vTaskDelay(pdMS_TO_TICKS(100));  // Give task time to exit
        capture_task_ = nullptr;
    }

    // Stop streams
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

    // Print statistics
    uint64_t elapsed_us = esp_timer_get_time() - start_time_us_;
    float elapsed_sec = elapsed_us / 1000000.0f;
    float avg_fps = frame_count_ / elapsed_sec;
    ESP_LOGI(TAG, "Video stopped: %lu frames in %.1f seconds (%.1f fps)",
             frame_count_, elapsed_sec, avg_fps);
}

void ESP32Video::close() {
    stop();

    // Unmap and close camera buffers
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

    // Unmap and close encoder buffers
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

    ESP_LOGI(TAG, "Video closed");
}

void ESP32Video::forceKeyframe() {
    force_keyframe_ = true;
    ESP_LOGI(TAG, "Keyframe requested");
}

void ESP32Video::captureTaskEntry(void* arg) {
    ESP32Video* self = static_cast<ESP32Video*>(arg);
    self->captureLoop();
    vTaskDelete(NULL);
}

void ESP32Video::captureLoop() {
    ESP_LOGI(TAG, "Capture loop started (pipelined mode, %d encoder buffers)", ENCODER_OUTPUT_BUFFERS);

    struct v4l2_buffer cam_buf, enc_input_buf, enc_output_buf;
    uint64_t last_stats_time = 0;

    while (running_) {
        // Try to submit camera frames to encoder (non-blocking pipeline feed)
        if (frames_in_encoder_ < ENCODER_OUTPUT_BUFFERS) {
            uint64_t before_cam_dq = esp_timer_get_time();

            memset(&cam_buf, 0, sizeof(cam_buf));
            cam_buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            cam_buf.memory = V4L2_MEMORY_MMAP;

            // Try to get camera frame (will block if no frame ready)
            if (ioctl(cap_fd_, VIDIOC_DQBUF, &cam_buf) == 0) {
                uint64_t after_cam_dq = esp_timer_get_time();
                uint32_t cam_wait_ms = (after_cam_dq - before_cam_dq) / 1000;

                // Always log camera wait time
                ESP_LOGI(TAG, "Camera DQBUF: %u ms", cam_wait_ms);
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

            // Log frame details
            ESP_LOGI(TAG, "Encoded frame: %u bytes (%s), buffer %d, max=%zu",
                     enc_output_buf.bytesused, keyframe ? "KEY" : "P",
                     enc_output_buf.index, m2m_cap_buffer_len_[enc_output_buf.index]);

#if TEST_ENCODER_ONLY
            // Test mode: Skip callback
            frame_count_++;
#else
            // Call user callback
            if (frame_callback_ && enc_output_buf.bytesused > 0) {
                frame_callback_(m2m_cap_buffer_[enc_output_buf.index],
                              enc_output_buf.bytesused, timestamp_us, keyframe);
                frame_count_++;
            }
#endif

            // Return encoder output buffer
            ioctl(m2m_fd_, VIDIOC_QBUF, &enc_output_buf);

            // Return encoder input buffer
            memset(&enc_input_buf, 0, sizeof(enc_input_buf));
            enc_input_buf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
            enc_input_buf.memory = V4L2_MEMORY_USERPTR;
            if (ioctl(m2m_fd_, VIDIOC_DQBUF, &enc_input_buf) == 0) {
                frames_in_encoder_--;
            }

            // Print statistics periodically
            uint64_t current_time = esp_timer_get_time();
            if ((current_time - last_stats_time) >= 1000000) {  // Every second
                uint64_t elapsed_us = current_time - start_time_us_;
                float elapsed_sec = elapsed_us / 1000000.0f;
                float avg_fps = frame_count_ / elapsed_sec;

                ESP_LOGI(TAG, "Frame %lu: %.1f fps (avg), %d in pipeline, %s",
                         frame_count_, avg_fps, frames_in_encoder_,
                         keyframe ? "KEYFRAME" : "");
                last_stats_time = current_time;
            }
        }

        // Small delay if encoder pipeline is empty (prevent busy-wait)
        if (frames_in_encoder_ == 0) {
            vTaskDelay(pdMS_TO_TICKS(1));
        }
    }

    ESP_LOGI(TAG, "Capture loop exited");
}
