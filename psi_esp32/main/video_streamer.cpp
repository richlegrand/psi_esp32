/**
 * VideoStreamer Implementation
 *
 * Complete H.264 video pipeline: camera → encoder → queue → RTP
 */

#include "video_streamer.hpp"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_memory_utils.h"
#include "esp_video_ioctl.h"
#include "esp_cam_sensor_types.h"
#include <cstring>
#include <algorithm>
#include <unistd.h>
#include <dirent.h>
#include <strings.h>

// libdatachannel headers
#include "rtc/frameinfo.hpp"

// Device paths (ESP32-P4 V4L2 devices)
#define CAMERA_DEV_PATH   "/dev/video0"   // MIPI-CSI camera
#define ENCODER_DEV_PATH  "/dev/video11"  // H.264 encoder

static const char* TAG = "VideoStreamer";

// Global flag for frame timing logs (synchronized with h264rtppacketizer)
extern bool g_log_frame_timing;

//=============================================================================
// Video Source Detection
//=============================================================================

VideoSource VideoStreamer::detectVideoSource() {
    // Check if /littlefs/frames directory exists
    DIR* dir = opendir("/littlefs/frames");
    if (!dir) {
        ESP_LOGI(TAG, "No /littlefs/frames directory found, using camera");
        return VideoSource::CAMERA;
    }

    // Check if directory contains any JPEG files
    bool has_jpegs = false;
    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        const char* name = entry->d_name;
        size_t len = strlen(name);

        // Check for .jpg or .jpeg extension (case-insensitive)
        if (len > 4) {
            const char* ext = name + len - 4;
            if (strcasecmp(ext, ".jpg") == 0) {
                has_jpegs = true;
                break;
            }
        }
        if (len > 5) {
            const char* ext = name + len - 5;
            if (strcasecmp(ext, ".jpeg") == 0) {
                has_jpegs = true;
                break;
            }
        }
    }
    closedir(dir);

    if (has_jpegs) {
        ESP_LOGI(TAG, "Found JPEG files in /littlefs/frames, using JPEG playback mode");
        return VideoSource::JPEG_FILES;
    } else {
        ESP_LOGI(TAG, "No JPEG files found in /littlefs/frames, using camera");
        return VideoSource::CAMERA;
    }
}

//=============================================================================
// Constructor / Destructor
//=============================================================================

VideoStreamer::VideoStreamer(uint32_t output_width, uint32_t output_height, uint32_t fps, bool enable_cv)
    : cam_width_(0), cam_height_(0),  // Will be auto-detected from sensor
      output_width_(output_width), output_height_(output_height),
      fps_(fps),
      use_ppa_(false),  // Determined after querying sensor
      cv_pipeline_enabled_(enable_cv),
      video_source_(VideoSource::CAMERA),  // Will be detected below
      cap_fd_(-1), m2m_fd_(-1),
      ppa_yuv_to_rgb_(nullptr), ppa_rgb_to_yuv_(nullptr),
      rgb_buffer_(nullptr), rgb_buffer_size_(0),
      yuv_output_buffer_(nullptr), yuv_output_buffer_size_(0),
      send_queue_(nullptr),
      ppa_done_sem_(nullptr),
      capture_task_(nullptr), send_task_(nullptr),
      running_(false), force_keyframe_(false),
      video_start_pts_(0), capture_frame_count_(0),
      frames_in_encoder_(0), frames_skipped_(0),
      decoded_yuv_buffer_(nullptr), decoded_yuv_buffer_size_(0)
{
    for (int i = 0; i < CAM_BUFFER_COUNT; i++) {
        cap_buffer_[i] = nullptr;
        cap_buffer_len_[i] = 0;
    }

    for (int i = 0; i < ENCODER_OUTPUT_BUFFERS; i++) {
        m2m_cap_buffer_[i] = nullptr;
        m2m_cap_buffer_len_[i] = 0;
    }

    // Detect video source at runtime (lightweight - just checks directory)
    video_source_ = detectVideoSource();
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

    // Query current sensor format to auto-detect camera resolution
    // (Set via menuconfig: Component config → Camera Sensor → OV2710 → Default format)
    esp_cam_sensor_format_t sensor_fmt;
    if (ioctl(cap_fd_, VIDIOC_G_SENSOR_FMT, &sensor_fmt) < 0) {
        ESP_LOGE(TAG, "Failed to query sensor format");
        return false;
    }

    // Store detected camera resolution
    cam_width_ = sensor_fmt.width;
    cam_height_ = sensor_fmt.height;
    ESP_LOGI(TAG, "Detected camera resolution: %dx%d @ %d fps",
             cam_width_, cam_height_, sensor_fmt.fps);

    // Determine if PPA scaling is needed
    use_ppa_ = (output_width_ != cam_width_ || output_height_ != cam_height_);
    if (use_ppa_) {
        ESP_LOGI(TAG, "PPA scaling enabled: %dx%d → %dx%d",
                 cam_width_, cam_height_, output_width_, output_height_);
    } else {
        ESP_LOGI(TAG, "No scaling needed (output matches camera resolution)");
    }

    // Set V4L2 format (YUV420 for H.264 encoder input)
    memset(&format, 0, sizeof(format));
    format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    format.fmt.pix.width = cam_width_;
    format.fmt.pix.height = cam_height_;
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

    ESP_LOGI(TAG, "Camera initialized: %dx%d YUV420", cam_width_, cam_height_);
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
    control[0].value = fps_ * 2;  // Keyframe every 2 seconds (was 1) - reduces I-frame overhead

    control[1].id = V4L2_CID_MPEG_VIDEO_BITRATE;
    control[1].value = (getWidth() * getHeight() * fps_) / 10;  // ~576 Kbps (was 720 Kbps)

    control[2].id = V4L2_CID_MPEG_VIDEO_H264_MIN_QP;
    control[2].value = 23;  // Tighter range: 23-29 (was 10-35)

    control[3].id = V4L2_CID_MPEG_VIDEO_H264_MAX_QP;
    control[3].value = 23;  // QP variation of only 6 instead of 25

    if (ioctl(m2m_fd_, VIDIOC_S_EXT_CTRLS, &controls) < 0) {
        ESP_LOGE(TAG, "Failed to set encoder parameters: errno=%d (%s)", errno, strerror(errno));
        ESP_LOGE(TAG, "Encoder may use default settings - expect poor performance!");
    } else {
        ESP_LOGI(TAG, "Encoder configured: I-period=%d, bitrate=%d, QP=%d-%d",
                 control[0].value, control[1].value, control[2].value, control[3].value);
    }

    // Set encoder input format (YUV420) - uses output resolution (after scaling if enabled)
    memset(&format, 0, sizeof(format));
    format.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    format.fmt.pix.width = getWidth();
    format.fmt.pix.height = getHeight();
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
    format.fmt.pix.width = getWidth();
    format.fmt.pix.height = getHeight();
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

bool VideoStreamer::initPPA() {
    if (cv_pipeline_enabled_) {
        ESP_LOGI(TAG, "Initializing dual PPA pipeline for CV processing...");
    size_t internal_before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);

    // Register PPA client #1: YUV420 → RGB888 (with optional scaling)
    ppa_client_config_t ppa_config = {
        .oper_type = PPA_OPERATION_SRM,
        .max_pending_trans_num = 1,
        .data_burst_length = PPA_DATA_BURST_LENGTH_128,
    };

    esp_err_t ret = ppa_register_client(&ppa_config, &ppa_yuv_to_rgb_);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register PPA YUV→RGB client: %d", ret);
        return false;
    }

    // Register PPA client #2: RGB888 → YUV420 (after CV processing)
    ret = ppa_register_client(&ppa_config, &ppa_rgb_to_yuv_);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register PPA RGB→YUV client: %d", ret);
        ppa_unregister_client(ppa_yuv_to_rgb_);
        ppa_yuv_to_rgb_ = nullptr;
        return false;
    }

    // Allocate RGB888 buffer for CV processing (DMA + PSRAM)
    // RGB888 format: width * height * 3 bytes
    rgb_buffer_size_ = output_width_ * output_height_ * 3;
    rgb_buffer_ = (uint8_t*)heap_caps_malloc(rgb_buffer_size_,
                                              MALLOC_CAP_DMA | MALLOC_CAP_SPIRAM);
    if (!rgb_buffer_) {
        ESP_LOGE(TAG, "Failed to allocate RGB888 buffer (%zu bytes)", rgb_buffer_size_);
        ppa_unregister_client(ppa_yuv_to_rgb_);
        ppa_unregister_client(ppa_rgb_to_yuv_);
        ppa_yuv_to_rgb_ = nullptr;
        ppa_rgb_to_yuv_ = nullptr;
        return false;
    }

    // Allocate YUV420 output buffer (after RGB→YUV conversion)
    // YUV420 format: width * height * 1.5 bytes
    yuv_output_buffer_size_ = output_width_ * output_height_ * 3 / 2;
    yuv_output_buffer_ = (uint8_t*)heap_caps_malloc(yuv_output_buffer_size_,
                                                     MALLOC_CAP_DMA | MALLOC_CAP_SPIRAM);
    if (!yuv_output_buffer_) {
        ESP_LOGE(TAG, "Failed to allocate YUV output buffer (%zu bytes)", yuv_output_buffer_size_);
        heap_caps_free(rgb_buffer_);
        ppa_unregister_client(ppa_yuv_to_rgb_);
        ppa_unregister_client(ppa_rgb_to_yuv_);
        rgb_buffer_ = nullptr;
        ppa_yuv_to_rgb_ = nullptr;
        ppa_rgb_to_yuv_ = nullptr;
        return false;
    }

    // Log memory usage
    size_t internal_after = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    bool rgb_in_psram = esp_ptr_external_ram(rgb_buffer_);
    bool yuv_in_psram = esp_ptr_external_ram(yuv_output_buffer_);

    ESP_LOGI(TAG, "PPA dual pipeline initialized:");
    ESP_LOGI(TAG, "  Stage 1: YUV420 %dx%d → RGB888 %dx%d (%s scaling)",
             cam_width_, cam_height_, output_width_, output_height_,
             use_ppa_ ? "with" : "no");
    ESP_LOGI(TAG, "  Stage 2: CV processing on RGB888");
    ESP_LOGI(TAG, "  Stage 3: RGB888 → YUV420 %dx%d",
             output_width_, output_height_);
    ESP_LOGI(TAG, "  RGB buffer: %zu bytes in %s",
             rgb_buffer_size_, rgb_in_psram ? "PSRAM" : "INTERNAL RAM");
    ESP_LOGI(TAG, "  YUV buffer: %zu bytes in %s",
             yuv_output_buffer_size_, yuv_in_psram ? "PSRAM" : "INTERNAL RAM");
        ESP_LOGI(TAG, "  Internal RAM used: %zu bytes (for DMA descriptors)",
                 internal_before - internal_after);

        return true;
    } else {
        // Direct YUV→YUV scaling mode (no CV pipeline)
        ESP_LOGI(TAG, "Initializing PPA for direct YUV downscaling (NON-BLOCKING)...");
    size_t internal_before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);

    // Create semaphore for PPA completion signaling
    ppa_done_sem_ = xSemaphoreCreateBinary();
    if (!ppa_done_sem_) {
        ESP_LOGE(TAG, "Failed to create PPA semaphore");
        return false;
    }

    // Register PPA client with queue depth = 3 for triple buffering
    ppa_client_config_t ppa_config = {
        .oper_type = PPA_OPERATION_SRM,
        .max_pending_trans_num = 3,  // Allow 3 ops in flight for parallelism
        .data_burst_length = PPA_DATA_BURST_LENGTH_128,
    };

    esp_err_t ret = ppa_register_client(&ppa_config, &ppa_yuv_to_rgb_);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register PPA client: %d", ret);
        vSemaphoreDelete(ppa_done_sem_);
        ppa_done_sem_ = nullptr;
        return false;
    }

    // Register completion callback
    ppa_event_callbacks_t cbs = {
        .on_trans_done = ppaDoneCallback,
    };
    ret = ppa_client_register_event_callbacks(ppa_yuv_to_rgb_, &cbs);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register PPA callback: %d", ret);
        ppa_unregister_client(ppa_yuv_to_rgb_);
        vSemaphoreDelete(ppa_done_sem_);
        ppa_yuv_to_rgb_ = nullptr;
        ppa_done_sem_ = nullptr;
        return false;
    }

    // Allocate YUV420 output buffer for scaled frames
    yuv_output_buffer_size_ = output_width_ * output_height_ * 3 / 2;
    yuv_output_buffer_ = (uint8_t*)heap_caps_malloc(yuv_output_buffer_size_,
                                                     MALLOC_CAP_DMA | MALLOC_CAP_SPIRAM);
    if (!yuv_output_buffer_) {
        ESP_LOGE(TAG, "Failed to allocate YUV output buffer (%zu bytes)", yuv_output_buffer_size_);
        ppa_unregister_client(ppa_yuv_to_rgb_);
        ppa_yuv_to_rgb_ = nullptr;
        return false;
    }

    size_t internal_after = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    bool yuv_in_psram = esp_ptr_external_ram(yuv_output_buffer_);

    ESP_LOGI(TAG, "PPA direct scaling initialized:");
    ESP_LOGI(TAG, "  YUV420 %dx%d → YUV420 %dx%d",
             cam_width_, cam_height_, output_width_, output_height_);
    ESP_LOGI(TAG, "  YUV buffer: %zu bytes in %s",
             yuv_output_buffer_size_, yuv_in_psram ? "PSRAM" : "INTERNAL RAM");
        ESP_LOGI(TAG, "  Internal RAM used: %zu bytes (for DMA descriptors)",
                 internal_before - internal_after);

        return true;
    }
}

bool VideoStreamer::initJpegMode() {
    ESP_LOGI(TAG, "Initializing JPEG playback mode...");

    // Create JPEG frame reader
    jpeg_reader_ = std::make_unique<JpegFrameReader>("/littlefs/frames");
    if (!jpeg_reader_->init()) {
        ESP_LOGE(TAG, "Failed to initialize JPEG frame reader");
        return false;
    }

    ESP_LOGI(TAG, "Found %zu JPEG frames", jpeg_reader_->getFrameCount());

    // Create JPEG decoder
    jpeg_decoder_ = std::make_unique<JpegDecoder>();

    // Allocate buffer for decoded YUV data
    // Maximum expected JPEG size: assume up to 1920x1080 for safety
    // YUV420 = width * height * 1.5 bytes
    decoded_yuv_buffer_size_ = 1920 * 1080 * 3 / 2;
    decoded_yuv_buffer_ = (uint8_t*)heap_caps_malloc(decoded_yuv_buffer_size_,
                                                      MALLOC_CAP_DMA | MALLOC_CAP_SPIRAM);
    if (!decoded_yuv_buffer_) {
        ESP_LOGE(TAG, "Failed to allocate decoded YUV buffer (%zu bytes)", decoded_yuv_buffer_size_);
        return false;
    }

    bool buffer_in_psram = esp_ptr_external_ram(decoded_yuv_buffer_);
    ESP_LOGI(TAG, "JPEG decode buffer: %zu bytes in %s",
             decoded_yuv_buffer_size_, buffer_in_psram ? "PSRAM" : "INTERNAL RAM");

    // Set use_ppa_ to false initially - will be determined per frame based on decoded size
    use_ppa_ = false;

    ESP_LOGI(TAG, "JPEG playback mode initialized successfully");
    return true;
}

void VideoStreamer::cleanup() {
    // Clean up camera resources (only if camera mode)
    if (video_source_ == VideoSource::CAMERA) {
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

    // Free PPA resources
    if (cv_pipeline_enabled_) {
        if (rgb_buffer_) {
            heap_caps_free(rgb_buffer_);
            rgb_buffer_ = nullptr;
            rgb_buffer_size_ = 0;
        }

        if (ppa_rgb_to_yuv_) {
            ppa_unregister_client(ppa_rgb_to_yuv_);
            ppa_rgb_to_yuv_ = nullptr;
        }
    }

    if (yuv_output_buffer_) {
        heap_caps_free(yuv_output_buffer_);
        yuv_output_buffer_ = nullptr;
        yuv_output_buffer_size_ = 0;
    }

    if (ppa_yuv_to_rgb_) {
        ppa_unregister_client(ppa_yuv_to_rgb_);
        ppa_yuv_to_rgb_ = nullptr;
    }

    // Clean up PPA semaphore
    if (ppa_done_sem_) {
        vSemaphoreDelete(ppa_done_sem_);
        ppa_done_sem_ = nullptr;
    }

    // Clean up JPEG mode resources (only if JPEG mode)
    if (video_source_ == VideoSource::JPEG_FILES) {
        if (decoded_yuv_buffer_) {
            heap_caps_free(decoded_yuv_buffer_);
            decoded_yuv_buffer_ = nullptr;
            decoded_yuv_buffer_size_ = 0;
        }

        jpeg_decoder_.reset();
        jpeg_reader_.reset();
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

    ESP_LOGI(TAG, "Starting video streamer: %dx%d @ %d fps", getWidth(), getHeight(), fps_);

    // Initialize camera if camera mode detected
    if (video_source_ == VideoSource::CAMERA) {
        if (!initCamera()) {
            ESP_LOGE(TAG, "Failed to initialize camera");
            return false;
        }
    } else {
        // JPEG mode will be lazily initialized in captureLoop (which has Internal RAM stack)
        // Cannot initialize here - this is called from libdatachannel threadpool with PSRAM stack
        ESP_LOGI(TAG, "Using JPEG playback mode (will initialize in capture task)");
    }

    if (!initEncoder()) {
        ESP_LOGE(TAG, "Failed to initialize encoder");
        cleanup();
        return false;
    }

    if (!initPPA()) {
        ESP_LOGE(TAG, "Failed to initialize PPA scaler");
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
    int type;

    // Start camera stream if camera mode
    if (video_source_ == VideoSource::CAMERA) {
        type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (ioctl(cap_fd_, VIDIOC_STREAMON, &type) < 0) {
            ESP_LOGE(TAG, "Failed to start camera stream");
            cleanup();
            return false;
        }
    }

    // Start encoder streams (common to both camera and JPEG modes)
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

//=============================================================================
// Test Processing (PSRAM Bandwidth Test)
//=============================================================================

uint32_t VideoStreamer::testProcessing() {
    // Allocate test buffer in PSRAM on first call (640x360 YUV420)
    static uint8_t* test_buffer = nullptr;
    static const uint32_t test_size = 640 * 360 * 3 / 2;  // 345,600 bytes

    if (!test_buffer) {
        test_buffer = (uint8_t*)heap_caps_malloc(test_size, MALLOC_CAP_SPIRAM);
        if (!test_buffer) {
            ESP_LOGE(TAG, "Failed to allocate test buffer in PSRAM");
            return 0;
        }
        // Fill with test pattern
        memset(test_buffer, 0xAA, test_size);
        ESP_LOGI(TAG, "Allocated %u byte test buffer in PSRAM", test_size);
    }

    // Read from PSRAM, do computation, but don't write back
    // This creates PSRAM read contention with PPA
    volatile uint32_t sum = 0;
    for (uint32_t i = 0; i < test_size; i++) {
        sum += test_buffer[i] * 2;  // PSRAM read + simple computation
    }

    // Result is discarded (volatile prevents optimization)
    return sum;
}


//=============================================================================
// PPA Configuration Helpers
//=============================================================================

ppa_srm_oper_config_t VideoStreamer::buildPpaYuvToRgb(
    uint8_t* src, uint32_t src_w, uint32_t src_h,
    uint8_t* dst, size_t dst_size, uint32_t dst_w, uint32_t dst_h,
    bool blocking) {

    return ppa_srm_oper_config_t{
        .in = {
            .buffer = src,
            .pic_w = src_w,
            .pic_h = src_h,
            .block_w = src_w,
            .block_h = src_h,
            .block_offset_x = 0,
            .block_offset_y = 0,
            .srm_cm = PPA_SRM_COLOR_MODE_YUV420,
            .yuv_range = PPA_COLOR_RANGE_LIMIT,
            .yuv_std = PPA_COLOR_CONV_STD_RGB_YUV_BT601,
        },
        .out = {
            .buffer = dst,
            .buffer_size = dst_size,
            .pic_w = dst_w,
            .pic_h = dst_h,
            .block_offset_x = 0,
            .block_offset_y = 0,
            .srm_cm = PPA_SRM_COLOR_MODE_RGB888,
            .yuv_range = PPA_COLOR_RANGE_LIMIT,
            .yuv_std = PPA_COLOR_CONV_STD_RGB_YUV_BT601,
        },
        .rotation_angle = PPA_SRM_ROTATION_ANGLE_0,
        .scale_x = (float)dst_w / (float)src_w,
        .scale_y = (float)dst_h / (float)src_h,
        .mirror_x = false,
        .mirror_y = false,
        .rgb_swap = false,
        .byte_swap = false,
        .alpha_update_mode = PPA_ALPHA_NO_CHANGE,
        .alpha_fix_val = 0,
        .mode = blocking ? PPA_TRANS_MODE_BLOCKING : PPA_TRANS_MODE_NON_BLOCKING,
        .user_data = blocking ? nullptr : (void*)ppa_done_sem_,
    };
}

ppa_srm_oper_config_t VideoStreamer::buildPpaRgbToYuv(
    uint8_t* src, uint32_t src_w, uint32_t src_h,
    uint8_t* dst, size_t dst_size,
    bool blocking) {

    return ppa_srm_oper_config_t{
        .in = {
            .buffer = src,
            .pic_w = src_w,
            .pic_h = src_h,
            .block_w = src_w,
            .block_h = src_h,
            .block_offset_x = 0,
            .block_offset_y = 0,
            .srm_cm = PPA_SRM_COLOR_MODE_RGB888,
            .yuv_range = PPA_COLOR_RANGE_LIMIT,
            .yuv_std = PPA_COLOR_CONV_STD_RGB_YUV_BT601,
        },
        .out = {
            .buffer = dst,
            .buffer_size = dst_size,
            .pic_w = src_w,  // No scaling - same as input
            .pic_h = src_h,
            .block_offset_x = 0,
            .block_offset_y = 0,
            .srm_cm = PPA_SRM_COLOR_MODE_YUV420,
            .yuv_range = PPA_COLOR_RANGE_LIMIT,
            .yuv_std = PPA_COLOR_CONV_STD_RGB_YUV_BT601,
        },
        .rotation_angle = PPA_SRM_ROTATION_ANGLE_0,
        .scale_x = 1.0f,
        .scale_y = 1.0f,
        .mirror_x = false,
        .mirror_y = false,
        .rgb_swap = false,
        .byte_swap = false,
        .alpha_update_mode = PPA_ALPHA_NO_CHANGE,
        .alpha_fix_val = 0,
        .mode = blocking ? PPA_TRANS_MODE_BLOCKING : PPA_TRANS_MODE_NON_BLOCKING,
        .user_data = blocking ? nullptr : (void*)ppa_done_sem_,
    };
}

ppa_srm_oper_config_t VideoStreamer::buildPpaYuvScale(
    uint8_t* src, uint32_t src_w, uint32_t src_h,
    uint8_t* dst, size_t dst_size, uint32_t dst_w, uint32_t dst_h,
    bool blocking) {

    return ppa_srm_oper_config_t{
        .in = {
            .buffer = src,
            .pic_w = src_w,
            .pic_h = src_h,
            .block_w = src_w,
            .block_h = src_h,
            .block_offset_x = 0,
            .block_offset_y = 0,
            .srm_cm = PPA_SRM_COLOR_MODE_YUV420,
            .yuv_range = PPA_COLOR_RANGE_LIMIT,
            .yuv_std = PPA_COLOR_CONV_STD_RGB_YUV_BT601,
        },
        .out = {
            .buffer = dst,
            .buffer_size = dst_size,
            .pic_w = dst_w,
            .pic_h = dst_h,
            .block_offset_x = 0,
            .block_offset_y = 0,
            .srm_cm = PPA_SRM_COLOR_MODE_YUV420,
            .yuv_range = PPA_COLOR_RANGE_LIMIT,
            .yuv_std = PPA_COLOR_CONV_STD_RGB_YUV_BT601,
        },
        .rotation_angle = PPA_SRM_ROTATION_ANGLE_0,
        .scale_x = (float)dst_w / (float)src_w,
        .scale_y = (float)dst_h / (float)src_h,
        .mirror_x = false,
        .mirror_y = false,
        .rgb_swap = false,
        .byte_swap = false,
        .alpha_update_mode = PPA_ALPHA_NO_CHANGE,
        .alpha_fix_val = 0,
        .mode = blocking ? PPA_TRANS_MODE_BLOCKING : PPA_TRANS_MODE_NON_BLOCKING,
        .user_data = blocking ? nullptr : (void*)ppa_done_sem_,
    };
}

//=============================================================================
// PPA Callback (ISR context)
//=============================================================================

bool VideoStreamer::ppaDoneCallback(ppa_client_handle_t ppa_client, ppa_event_data_t* event_data, void* user_data) {
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    SemaphoreHandle_t sem = (SemaphoreHandle_t)user_data;

    // Signal completion from ISR context
    xSemaphoreGiveFromISR(sem, &xHigherPriorityTaskWoken);

    return (xHigherPriorityTaskWoken == pdTRUE);
}

//=============================================================================
// Frame Acquisition Abstraction
//=============================================================================

bool VideoStreamer::acquireFrame(AcquiredFrame& frame) {
    if (video_source_ == VideoSource::CAMERA) {
        // Camera mode: DQBUF from V4L2
        struct v4l2_buffer cam_buf = {};
        cam_buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        cam_buf.memory = V4L2_MEMORY_MMAP;

        if (ioctl(cap_fd_, VIDIOC_DQBUF, &cam_buf) != 0) {
            return false;
        }

        frame.yuv_data = cap_buffer_[cam_buf.index];
        frame.width = cam_width_;
        frame.height = cam_height_;
        frame.buffer_index = cam_buf.index;
        frame.source = VideoSource::CAMERA;
        return true;

    } else {
        // JPEG mode: read and decode next frame
        if (!jpeg_reader_->readNextFrame(jpeg_buffer_)) {
            ESP_LOGE(TAG, "Failed to read JPEG frame");
            return false;
        }

        uint32_t decoded_width, decoded_height;
        if (!jpeg_decoder_->decode(jpeg_buffer_.data(), jpeg_buffer_.size(),
                                   decoded_yuv_buffer_, decoded_yuv_buffer_size_,
                                   &decoded_width, &decoded_height)) {
            ESP_LOGE(TAG, "JPEG decode failed");
            return false;
        }

        frame.yuv_data = decoded_yuv_buffer_;
        frame.width = decoded_width;
        frame.height = decoded_height;
        frame.buffer_index = -1;  // No buffer to return
        frame.source = VideoSource::JPEG_FILES;
        return true;
    }
}

void VideoStreamer::releaseFrame(AcquiredFrame& frame) {
    if (frame.source == VideoSource::CAMERA && frame.buffer_index >= 0) {
        // Return camera buffer to V4L2
        struct v4l2_buffer cam_buf = {};
        cam_buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        cam_buf.memory = V4L2_MEMORY_MMAP;
        cam_buf.index = frame.buffer_index;
        ioctl(cap_fd_, VIDIOC_QBUF, &cam_buf);
        frame.buffer_index = -1;  // Mark as released
    }
    // JPEG mode: nothing to release (buffer is reused)
}

//=============================================================================
// Processing Paths
//=============================================================================

bool VideoStreamer::processFrameCV(const AcquiredFrame& frame) {
    // CV Pipeline: YUV420 → RGB888 → callback → RGB888 → YUV420

    // Stage 1: YUV420 → RGB888 (with scaling)
    auto yuv_to_rgb_config = buildPpaYuvToRgb(
        frame.yuv_data, frame.width, frame.height,
        rgb_buffer_, rgb_buffer_size_, output_width_, output_height_,
        true  // blocking
    );

    esp_err_t ret = ppa_do_scale_rotate_mirror(ppa_yuv_to_rgb_, &yuv_to_rgb_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "PPA YUV→RGB failed: %d", ret);
        return false;
    }

    // Stage 2: Call CV callback if set
    uint8_t* rgb_to_encode = rgb_buffer_;
    if (frame_callback_) {
        int stride = output_width_ * 3;
        Frame input{rgb_buffer_, static_cast<int>(output_width_), static_cast<int>(output_height_), stride};
        Frame output = frame_callback_(input);

        // Use output frame's data (may be same as input or different)
        rgb_to_encode = output.data;
    }

    // Stage 3: RGB888 → YUV420
    auto rgb_to_yuv_config = buildPpaRgbToYuv(
        rgb_to_encode, output_width_, output_height_,
        yuv_output_buffer_, yuv_output_buffer_size_,
        true  // blocking
    );

    ret = ppa_do_scale_rotate_mirror(ppa_rgb_to_yuv_, &rgb_to_yuv_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "PPA RGB→YUV failed: %d", ret);
        return false;
    }

    return true;
}

bool VideoStreamer::processFrameDirect(const AcquiredFrame& frame) {
    // Direct YUV→YUV scaling (no CV processing) - NON-BLOCKING mode
    auto yuv_scale_config = buildPpaYuvScale(
        frame.yuv_data, frame.width, frame.height,
        yuv_output_buffer_, yuv_output_buffer_size_, output_width_, output_height_,
        false  // non-blocking
    );

    // Submit PPA operation - returns immediately
    esp_err_t ret = ppa_do_scale_rotate_mirror(ppa_yuv_to_rgb_, &yuv_scale_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "PPA YUV scaling submit failed: %d", ret);
        return false;
    }

    // === PPA is now running in hardware ===
    // Do test processing while waiting (PSRAM bandwidth test)
    testProcessing();

    // Wait for PPA to complete
    if (xSemaphoreTake(ppa_done_sem_, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGW(TAG, "PPA timeout! Skipping frame.");
        return false;
    }

    return true;
}

//=============================================================================
// Encoder Helpers
//=============================================================================

bool VideoStreamer::submitToEncoder() {
    struct v4l2_buffer enc_input_buf = {};
    enc_input_buf.index = 0;
    enc_input_buf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    enc_input_buf.memory = V4L2_MEMORY_USERPTR;
    enc_input_buf.m.userptr = (unsigned long)yuv_output_buffer_;
    enc_input_buf.length = yuv_output_buffer_size_;

    if (ioctl(m2m_fd_, VIDIOC_QBUF, &enc_input_buf) != 0) {
        ESP_LOGE(TAG, "Failed to queue frame to encoder: %s", strerror(errno));
        return false;
    }

    frames_in_encoder_++;
    return true;
}

bool VideoStreamer::dequeueEncodedFrame(bool blocking, struct v4l2_buffer& enc_output_buf) {
    memset(&enc_output_buf, 0, sizeof(enc_output_buf));
    enc_output_buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    enc_output_buf.memory = V4L2_MEMORY_MMAP;

    if (ioctl(m2m_fd_, VIDIOC_DQBUF, &enc_output_buf) != 0) {
        if (blocking) {
            ESP_LOGE(TAG, "Failed to dequeue encoded frame: %s", strerror(errno));
        }
        return false;
    }

    return true;
}

void VideoStreamer::queueFrameForSending(const struct v4l2_buffer& enc_output_buf) {
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

    // Update frame counter
    capture_frame_count_++;

    // Logging for every 5th frame
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
        ESP_LOGW(TAG, "Send queue full despite front-end skip!");
        delete frame;
    }

    if (capture_frame_count_ % 5 == 0) {
        g_log_frame_timing = false;
    }

    // Return encoder output buffer
    struct v4l2_buffer return_buf = enc_output_buf;
    ioctl(m2m_fd_, VIDIOC_QBUF, &return_buf);

    // Dequeue encoder input buffer
    struct v4l2_buffer enc_input_buf = {};
    enc_input_buf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    enc_input_buf.memory = V4L2_MEMORY_USERPTR;
    if (ioctl(m2m_fd_, VIDIOC_DQBUF, &enc_input_buf) == 0) {
        frames_in_encoder_--;
    }
}

//=============================================================================
// Capture Loop (Unified - source agnostic)
//=============================================================================

void VideoStreamer::captureLoop() {
    ESP_LOGI(TAG, "Capture loop started (%d encoder buffers)", ENCODER_OUTPUT_BUFFERS);

    uint64_t last_stats_time = esp_timer_get_time();

    // Lazy initialization for JPEG mode
    // Must happen here (not in startStreaming) because this task has Internal RAM stack
    // and can safely access flash (opendir/readdir for LittleFS)
    if (video_source_ == VideoSource::JPEG_FILES && !jpeg_reader_) {
        ESP_LOGI(TAG, "Lazy init JPEG mode (Internal RAM stack - safe for flash I/O)");
        if (!initJpegMode()) {
            ESP_LOGE(TAG, "JPEG mode initialization failed - exiting capture loop");
            running_ = false;
            return;
        }
        ESP_LOGI(TAG, "JPEG mode initialized successfully");
    }

    while (running_) {
        // Check for backpressure (front-end skip)
        if (shouldSkipFrame()) {
            frames_skipped_++;
            ESP_LOGI(TAG, "Skipped frame (queue depth=%u)", uxQueueMessagesWaiting(send_queue_));
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        // Acquire and process frames
        if (frames_in_encoder_ < ENCODER_OUTPUT_BUFFERS) {
            AcquiredFrame frame;
            if (acquireFrame(frame)) {
                // Process frame (CV pipeline with callback, or direct scaling)
                bool success = cv_pipeline_enabled_
                    ? processFrameCV(frame)
                    : processFrameDirect(frame);

                releaseFrame(frame);

                if (success) {
                    submitToEncoder();
                }
            }
        }

        // Dequeue encoded frame (non-blocking)
        struct v4l2_buffer enc_output_buf;
        if (dequeueEncodedFrame(false, enc_output_buf)) {
            queueFrameForSending(enc_output_buf);
        }

        // Print statistics periodically
        uint64_t current_time = esp_timer_get_time();
        if ((current_time - last_stats_time) >= 1000000) {
            uint64_t elapsed_us = current_time - video_start_pts_;
            float elapsed_sec = elapsed_us / 1000000.0f;
            float avg_fps = capture_frame_count_ / elapsed_sec;

            ESP_LOGI(TAG, "Frame %lu: %.1f fps (avg), %d in encoder, %u skipped",
                     (unsigned long)capture_frame_count_, avg_fps,
                     frames_in_encoder_, frames_skipped_);
            last_stats_time = current_time;
        }

        // Small delay if encoder pipeline is empty
        if (frames_in_encoder_ == 0) {
            vTaskDelay(pdMS_TO_TICKS(1));
        }

        // JPEG mode runs slower to conserve resources
        if (video_source_ == VideoSource::JPEG_FILES) {
            vTaskDelay(pdMS_TO_TICKS(333));  // ~3 fps
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

    while (running_) {
        // Wait for frame with timeout so we can check running_ periodically
        if (xQueueReceive(send_queue_, &frame, pdMS_TO_TICKS(100)) == pdTRUE) {
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

    ESP_LOGI(TAG, "Send task exiting");
}
