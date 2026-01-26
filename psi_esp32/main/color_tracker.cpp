/**
 * ColorTracker Implementation
 *
 * Handles freeze mode, iterative color teaching, and real-time tracking.
 * Designed as a callback processor for VideoStreamer.
 */

#include "color_tracker.hpp"
#include <cstring>
#include <algorithm>

#ifdef ESP_PLATFORM
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
static const char* TAG = "ColorTracker";
#define LOGI(fmt, ...) ESP_LOGI(TAG, fmt, ##__VA_ARGS__)
#define LOGW(fmt, ...) ESP_LOGW(TAG, fmt, ##__VA_ARGS__)
#define LOGE(fmt, ...) ESP_LOGE(TAG, fmt, ##__VA_ARGS__)
#define ALLOC_PSRAM(size) heap_caps_malloc(size, MALLOC_CAP_SPIRAM)
#define FREE_MEM(ptr) heap_caps_free(ptr)
#else
#include <cstdio>
#include <cstdlib>
#define LOGI(fmt, ...) printf("[INFO] " fmt "\n", ##__VA_ARGS__)
#define LOGW(fmt, ...) printf("[WARN] " fmt "\n", ##__VA_ARGS__)
#define LOGE(fmt, ...) printf("[ERROR] " fmt "\n", ##__VA_ARGS__)
#define ALLOC_PSRAM(size) malloc(size)
#define FREE_MEM(ptr) free(ptr)
#endif

//=============================================================================
// Constructor / Destructor
//=============================================================================

ColorTracker::ColorTracker()
    : detector_(100)  // Minimum area 100 pixels
{
    LOGI("ColorTracker created (freeze_requested=%d)", freeze_requested_.load());
}

ColorTracker::~ColorTracker() {
    if (frozen_buffers_[0]) {
        FREE_MEM(frozen_buffers_[0]);
        frozen_buffers_[0] = nullptr;
    }
    if (frozen_buffers_[1]) {
        FREE_MEM(frozen_buffers_[1]);
        frozen_buffers_[1] = nullptr;
    }
    if (original_frame_) {
        FREE_MEM(original_frame_);
        original_frame_ = nullptr;
    }
    LOGI("ColorTracker destroyed");
}

//=============================================================================
// Buffer Management
//=============================================================================

bool ColorTracker::ensureFrozenBuffer(size_t size) {
    if (frozen_buffers_[0] && frozen_buffers_[1] && frozen_frame_size_ >= size) {
        return true;
    }

    // Free existing buffers
    if (frozen_buffers_[0]) {
        FREE_MEM(frozen_buffers_[0]);
        frozen_buffers_[0] = nullptr;
    }
    if (frozen_buffers_[1]) {
        FREE_MEM(frozen_buffers_[1]);
        frozen_buffers_[1] = nullptr;
    }

    // Allocate double buffers
    frozen_buffers_[0] = static_cast<uint8_t*>(ALLOC_PSRAM(size));
    frozen_buffers_[1] = static_cast<uint8_t*>(ALLOC_PSRAM(size));

    if (!frozen_buffers_[0] || !frozen_buffers_[1]) {
        LOGE("Failed to allocate frozen frame buffers (%zu bytes each)", size);
        if (frozen_buffers_[0]) { FREE_MEM(frozen_buffers_[0]); frozen_buffers_[0] = nullptr; }
        if (frozen_buffers_[1]) { FREE_MEM(frozen_buffers_[1]); frozen_buffers_[1] = nullptr; }
        frozen_frame_size_ = 0;
        return false;
    }

    frozen_frame_size_ = size;
    front_buffer_idx_.store(0);
    LOGI("Allocated double frozen frame buffers: %zu bytes each", size);
    return true;
}

bool ColorTracker::ensureOriginalBuffer(size_t size) {
    if (original_frame_ && original_frame_size_ >= size) {
        return true;
    }

    if (original_frame_) {
        FREE_MEM(original_frame_);
    }

    original_frame_ = static_cast<uint8_t*>(ALLOC_PSRAM(size));
    if (!original_frame_) {
        LOGE("Failed to allocate original frame buffer (%zu bytes)", size);
        original_frame_size_ = 0;
        return false;
    }

    original_frame_size_ = size;
    LOGI("Allocated original frame buffer: %zu bytes", size);
    return true;
}

//=============================================================================
// Main Frame Callback
//=============================================================================

Frame ColorTracker::onFrame(const Frame& input) {
    // Handle freeze request
    if (freeze_requested_.load() && !frozen_.load()) {
        size_t size = input.size();
        if (ensureFrozenBuffer(size)) {
            // Copy to both buffers initially (they start identical)
            memcpy(frozen_buffers_[0], input.data, size);
            memcpy(frozen_buffers_[1], input.data, size);
            frozen_width_ = input.width;
            frozen_height_ = input.height;
            frozen_stride_ = input.stride;
            frozen_.store(true);
            freeze_requested_.store(false);
            LOGI("Video frozen - frame captured (%dx%d)", input.width, input.height);

            // Check if there's a pending teach request
            if (teach_pending_.load()) {
                teach_pending_.store(false);
                LOGI("Processing pending teach request");
                executePendingTeach();
            }
        }
    }

    // If frozen, return the front buffer (safe to read while back buffer is being written)
    if (frozen_.load()) {
        return Frame{
            frontBuffer(),
            frozen_width_,
            frozen_height_,
            frozen_stride_
        };
    }

    // Live mode - run tracking if enabled
    if (tracking_enabled_.load()) {
        int minAreaPixels = static_cast<int>(0.001f * input.width * input.height);
        detector_.setMinArea(minAreaPixels);

#ifdef ESP_PLATFORM
        uint64_t t0 = esp_timer_get_time();
#endif
        detector_.detect(input.data, input.width, input.height, input.stride,
                        model_, contours_);
#ifdef ESP_PLATFORM
        uint64_t t1 = esp_timer_get_time();

        // Log timing periodically
        static int frame_count = 0;
        if (++frame_count % 25 == 0) {
            LOGI("Tracking: detect=%llu us, %zu contours",
                 (unsigned long long)(t1 - t0), contours_.size());
        }
#endif

        // Render tracking overlay
        renderTrackingOverlay(input.data, input.width, input.height, input.stride);
    }

    // Return input frame (possibly modified with tracking overlay)
    return input;
}

//=============================================================================
// Freeze Control
//=============================================================================

void ColorTracker::requestFreeze() {
    if (frozen_.load()) {
        LOGW("Already frozen");
        return;
    }
    LOGI("Freeze requested - will freeze on next frame");
    freeze_requested_.store(true);
}

void ColorTracker::unfreeze() {
    if (!frozen_.load()) {
        LOGW("Not frozen");
        return;
    }

    // Cancel any ongoing teaching
    std::lock_guard<std::mutex> lock(teach_mutex_);
    if (teacher_.state() != tracking::TeachState::IDLE &&
        teacher_.state() != tracking::TeachState::ACCEPTED) {
        teacher_.cancel();
    }

    frozen_.store(false);
    LOGI("Unfrozen - resuming live video");
}

//=============================================================================
// Teaching Control
//=============================================================================

bool ColorTracker::startTeaching(float roi_x, float roi_y, float roi_w, float roi_h) {
    LOGI("startTeaching called: ROI=(%.3f, %.3f, %.3f, %.3f), frozen=%d",
         roi_x, roi_y, roi_w, roi_h, frozen_.load());

    // If not frozen, set up pending request and request freeze
    if (!frozen_.load()) {
        LOGI("Not frozen - setting up pending teach request");
        pending_roi_x_ = roi_x;
        pending_roi_y_ = roi_y;
        pending_roi_w_ = roi_w;
        pending_roi_h_ = roi_h;
        teach_pending_.store(true);
        freeze_requested_.store(true);
        return true;  // Will execute when freeze completes
    }

    // Already frozen - execute immediately
    return executeTeaching(roi_x, roi_y, roi_w, roi_h);
}

void ColorTracker::executePendingTeach() {
    executeTeaching(pending_roi_x_, pending_roi_y_, pending_roi_w_, pending_roi_h_);
}

bool ColorTracker::executeTeaching(float roi_x, float roi_y, float roi_w, float roi_h) {
    LOGI("executeTeaching: ROI=(%.3f, %.3f, %.3f, %.3f)", roi_x, roi_y, roi_w, roi_h);

    if (!frontBuffer()) {
        LOGE("Frozen frame buffer not available");
        return false;
    }

    std::lock_guard<std::mutex> lock(teach_mutex_);

    // Save a copy of the original frozen frame for subsequent iterations
    size_t size = static_cast<size_t>(frozen_stride_) * frozen_height_;
    if (!ensureOriginalBuffer(size)) {
        return false;
    }
    // Use front buffer as the source (it has the clean frozen frame)
    memcpy(original_frame_, frontBuffer(), size);
    LOGI("Saved original frame for teaching (%zu bytes)", size);

    // Work on the back buffer
    uint8_t* work_buffer = backBuffer();
    memcpy(work_buffer, original_frame_, size);

    bool result = teacher_.start(work_buffer, frozen_width_, frozen_height_, frozen_stride_,
                                  roi_x, roi_y, roi_w, roi_h);

    LOGI("teacher_.start() returned %d, state=%d", result, static_cast<int>(teacher_.state()));

    if (result) {
        LOGI("Teaching started - calling first iteration");

        // Restore original frame before detection
        memcpy(work_buffer, original_frame_, size);

        bool iter_result = teacher_.iterate(work_buffer, frozen_width_, frozen_height_, frozen_stride_);
        LOGI("First iteration: result=%d, contours=%zu", iter_result, teacher_.contours().size());

        if (iter_result || !teacher_.contours().empty()) {
            teacher_.renderVisualization(work_buffer, frozen_width_, frozen_height_, frozen_stride_);
            LOGI("Visualization rendered to back buffer");
        }

        // Swap buffers atomically - front buffer now has the visualization
        swapBuffers();
        LOGI("Buffers swapped - visualization now visible");
    }

    return result;
}

bool ColorTracker::iterateTeaching() {
    LOGI("iterateTeaching called: frozen=%d", frozen_.load());

    if (!frozen_.load() || !frontBuffer()) {
        LOGW("Cannot iterate: not frozen or no frame buffer");
        return false;
    }

    std::lock_guard<std::mutex> lock(teach_mutex_);

    auto state = teacher_.state();
    LOGI("Teacher state=%d", static_cast<int>(state));

    if (state != tracking::TeachState::BOOTSTRAPPING &&
        state != tracking::TeachState::ITERATING) {
        LOGW("Cannot iterate: teaching not in progress (state=%d)", static_cast<int>(state));
        return false;
    }

    size_t size = static_cast<size_t>(frozen_stride_) * frozen_height_;

    // Work on the back buffer
    uint8_t* work_buffer = backBuffer();

    // Restore original frame before detection
    if (original_frame_) {
        memcpy(work_buffer, original_frame_, size);
        LOGI("Restored original frame to back buffer (%zu bytes)", size);
    }

    bool result = teacher_.iterate(work_buffer, frozen_width_, frozen_height_, frozen_stride_);
    LOGI("teacher_.iterate() returned %d, contours=%zu, bestContour=%p",
         result, teacher_.contours().size(), teacher_.bestContour());

    // Render visualization even if iterate returns false (still show any contours found)
    if (!teacher_.contours().empty()) {
        teacher_.renderVisualization(work_buffer, frozen_width_, frozen_height_, frozen_stride_);
        LOGI("Visualization rendered to back buffer");
    } else {
        LOGW("No contours to render");
    }

    // Swap buffers atomically - front buffer now has the new visualization
    swapBuffers();
    LOGI("Buffers swapped");

    return result;
}

void ColorTracker::acceptTeaching() {
    {
        std::lock_guard<std::mutex> lock(teach_mutex_);
        teacher_.accept();

        // Copy the accepted model for tracking
        model_ = teacher_.model();
    }

    tracking_enabled_.store(true);
    contours_.clear();

    LOGI("Tracking enabled: mean=(%.4f, %.4f), threshold_sq=%.2f",
         model_.mean_u, model_.mean_v, model_.threshold_sq);

    // Free the original buffer copy
    if (original_frame_) {
        FREE_MEM(original_frame_);
        original_frame_ = nullptr;
        original_frame_size_ = 0;
    }

    LOGI("Teaching accepted - model saved, tracking active");

    // Unfreeze automatically after accepting
    frozen_.store(false);
    LOGI("Auto-unfreezing after accept");
}

void ColorTracker::cancelTeaching() {
    {
        std::lock_guard<std::mutex> lock(teach_mutex_);
        teacher_.cancel();
    }

    // Free the original buffer copy
    if (original_frame_) {
        FREE_MEM(original_frame_);
        original_frame_ = nullptr;
        original_frame_size_ = 0;
    }

    LOGI("Teaching cancelled");
}

tracking::TeachState ColorTracker::getTeachState() const {
    return teacher_.state();
}

const tracking::ColorModel& ColorTracker::getColorModel() const {
    return model_;
}

//=============================================================================
// Tracking Visualization
//=============================================================================

void ColorTracker::renderTrackingOverlay(uint8_t* rgb, int w, int h, int stride) {
    if (!tracking_enabled_.load() || contours_.empty()) {
        return;
    }

    // Helper: draw semi-transparent tint over contour runs
    auto drawContourTinted = [&](const tracking::Contour& contour,
                                  uint8_t tintR, uint8_t tintG, uint8_t tintB,
                                  float alpha) {
        float invAlpha = 1.0f - alpha;

        for (const auto& run : contour.runs) {
            if (run.y < 0 || run.y >= h) continue;

            uint8_t* row = rgb + run.y * stride;
            int xStart = std::max(0, run.xStart);
            int xEnd = std::min(run.xEnd, w - 1);

            for (int x = xStart; x <= xEnd; x++) {
                int idx = x * 3;
                row[idx]     = static_cast<uint8_t>(row[idx]     * invAlpha + tintR * alpha);
                row[idx + 1] = static_cast<uint8_t>(row[idx + 1] * invAlpha + tintG * alpha);
                row[idx + 2] = static_cast<uint8_t>(row[idx + 2] * invAlpha + tintB * alpha);
            }
        }
    };

    // Helper: draw contour outline using convex hull
    auto drawContourOutline = [&](const tracking::Contour& contour,
                                   uint8_t r, uint8_t g, uint8_t b) {
        std::vector<tracking::Point> hull = contour.convexHull();
        if (hull.size() < 2) return;

        // Bresenham line drawing
        auto drawLine = [&](tracking::Point p0, tracking::Point p1) {
            int dx = std::abs(p1.x - p0.x);
            int dy = std::abs(p1.y - p0.y);
            int sx = (p0.x < p1.x) ? 1 : -1;
            int sy = (p0.y < p1.y) ? 1 : -1;
            int err = dx - dy;

            while (true) {
                if (p0.x >= 0 && p0.x < w && p0.y >= 0 && p0.y < h) {
                    int idx = p0.y * stride + p0.x * 3;
                    rgb[idx]     = r;
                    rgb[idx + 1] = g;
                    rgb[idx + 2] = b;
                }

                if (p0.x == p1.x && p0.y == p1.y) break;

                int e2 = 2 * err;
                if (e2 > -dy) { err -= dy; p0.x += sx; }
                if (e2 < dx)  { err += dx; p0.y += sy; }
            }
        };

        for (size_t i = 0; i < hull.size(); i++) {
            size_t j = (i + 1) % hull.size();
            drawLine(hull[i], hull[j]);
        }
    };

    // Helper: draw crosshair at point
    auto drawCross = [&](int cx, int cy, int size, uint8_t r, uint8_t g, uint8_t b) {
        for (int i = -size; i <= size; i++) {
            // Horizontal
            int x = cx + i;
            if (x >= 0 && x < w && cy >= 0 && cy < h) {
                int idx = cy * stride + x * 3;
                rgb[idx]     = r;
                rgb[idx + 1] = g;
                rgb[idx + 2] = b;
            }
            // Vertical
            int y = cy + i;
            if (cx >= 0 && cx < w && y >= 0 && y < h) {
                int idx = y * stride + cx * 3;
                rgb[idx]     = r;
                rgb[idx + 1] = g;
                rgb[idx + 2] = b;
            }
        }
    };

    // Find largest contour
    const tracking::Contour* largest = nullptr;
    int maxArea = 0;
    for (const auto& c : contours_) {
        if (c.area > maxArea) {
            maxArea = c.area;
            largest = &c;
        }
    }

    // Draw all non-primary contours in gray (semi-transparent)
    for (const auto& c : contours_) {
        if (&c != largest) {
            drawContourTinted(c, 128, 128, 128, 0.25f);  // Gray tint
            drawContourOutline(c, 100, 100, 100);        // Gray outline
        }
    }

    // Draw largest contour highlighted
    if (largest) {
        // Orange tint for primary tracking target
        drawContourTinted(*largest, 255, 165, 0, 0.35f);

        // Cyan outline
        drawContourOutline(*largest, 0, 255, 255);

        // Red crosshair at centroid
        tracking::Point center = largest->centroid();
        drawCross(center.x, center.y, 6, 255, 0, 0);
    }
}

void ColorTracker::renderTeachingOverlay(uint8_t* rgb, int w, int h, int stride) {
    std::lock_guard<std::mutex> lock(teach_mutex_);
    teacher_.renderVisualization(rgb, w, h, stride);
}
