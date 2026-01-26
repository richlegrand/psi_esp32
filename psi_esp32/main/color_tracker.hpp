#pragma once

#include "frame.hpp"
#include "color_model.hpp"
#include "color_teacher.hpp"
#include "contour_detector.hpp"
#include <vector>
#include <atomic>
#include <mutex>

/**
 * ColorTracker - Handles all CV operations: freeze, teaching, and tracking.
 *
 * This class is designed to be used as a FrameCallback for VideoStreamer.
 * It manages:
 * - Freeze mode: Captures and holds a frame for region selection
 * - Teaching: Iterative color model learning from user-selected ROI
 * - Tracking: Real-time color detection and visualization
 *
 * Thread safety:
 * - onFrame() runs on capture task (Internal RAM stack)
 * - API methods (requestFreeze, startTeaching, etc.) called from HTTP handlers
 *   which may run on libdatachannel thread pool (PSRAM stack)
 * - Uses atomics and mutex for state shared between threads
 */
class ColorTracker {
public:
    ColorTracker();
    ~ColorTracker();

    /**
     * Main callback - called for each frame by VideoStreamer.
     *
     * Handles freeze mode, teaching, and tracking based on current state.
     * Returns the frame to encode (may be input or frozen frame).
     *
     * @param input Borrowed input frame (valid only during this call)
     * @return Frame to encode - either input (possibly modified) or frozen frame
     */
    Frame onFrame(const Frame& input);

    // ==================== Freeze Control ====================

    /**
     * Request freeze on next frame.
     * Video will freeze when captureLoop processes next frame.
     * Safe to call from any thread.
     */
    void requestFreeze();

    /**
     * Exit freeze mode and resume live video.
     * Cancels any ongoing teaching.
     * Safe to call from any thread.
     */
    void unfreeze();

    /**
     * Check if video is currently frozen.
     * Thread-safe (atomic).
     */
    bool isFrozen() const { return frozen_.load(); }

    // ==================== Teaching Control ====================

    /**
     * Start teaching with ROI (normalized 0-1 coordinates).
     * Must be frozen first.
     *
     * @param roi_x, roi_y Top-left corner (0-1)
     * @param roi_w, roi_h Size (0-1)
     * @return true if teaching started successfully
     */
    bool startTeaching(float roi_x, float roi_y, float roi_w, float roi_h);

    /**
     * Perform one teaching iteration.
     * Updates visualization and refines color model.
     *
     * @return true if iteration succeeded
     */
    bool iterateTeaching();

    /**
     * Accept current color model and enable tracking.
     * Automatically unfreezes after accepting.
     */
    void acceptTeaching();

    /**
     * Cancel teaching without accepting model.
     */
    void cancelTeaching();

    /**
     * Get current teaching state.
     */
    tracking::TeachState getTeachState() const;

    /**
     * Get current color model (valid after teaching accepted).
     */
    const tracking::ColorModel& getColorModel() const;

    // ==================== Tracking State ====================

    /**
     * Check if tracking is currently active.
     */
    bool isTrackingEnabled() const { return tracking_enabled_.load(); }

    /**
     * Disable tracking (stop color detection overlay).
     */
    void stopTracking() { tracking_enabled_.store(false); }

    /**
     * Get detected contours from current frame.
     * Note: Vector is updated during onFrame(), access with caution.
     */
    const std::vector<tracking::Contour>& getContours() const { return contours_; }

private:
    // ==================== Freeze State ====================
    std::atomic<bool> freeze_requested_{false};  // Start live, freeze on selection
    std::atomic<bool> frozen_{false};

    // Double-buffered frozen frames to avoid race condition
    // front_buffer_ is read by capture loop, back_buffer_ is written by teaching
    uint8_t* frozen_buffers_[2] = {nullptr, nullptr};
    std::atomic<int> front_buffer_idx_{0};  // Index of buffer being read
    size_t frozen_frame_size_ = 0;
    int frozen_width_ = 0;
    int frozen_height_ = 0;
    int frozen_stride_ = 0;

    // Helpers for double buffering
    uint8_t* frontBuffer() const { return frozen_buffers_[front_buffer_idx_.load()]; }
    uint8_t* backBuffer() const { return frozen_buffers_[1 - front_buffer_idx_.load()]; }
    void swapBuffers() { front_buffer_idx_.store(1 - front_buffer_idx_.load()); }

    // ==================== Teaching State ====================
    tracking::ColorTeacher teacher_;
    uint8_t* original_frame_ = nullptr;     // Copy for teaching iterations
    size_t original_frame_size_ = 0;
    std::atomic<bool> visualization_dirty_{false};
    std::mutex teach_mutex_;                // Protects teaching operations

    // Pending teach request (freeze + teach in one action)
    std::atomic<bool> teach_pending_{false};
    float pending_roi_x_ = 0, pending_roi_y_ = 0;
    float pending_roi_w_ = 0, pending_roi_h_ = 0;

    // ==================== Tracking State ====================
    std::atomic<bool> tracking_enabled_{false};
    tracking::ColorModel model_;
    tracking::ContourDetector detector_;
    std::vector<tracking::Contour> contours_;

    // ==================== Internal Helpers ====================

    /**
     * Allocate frozen frame buffer if needed.
     * @return true if buffer is ready
     */
    bool ensureFrozenBuffer(size_t size);

    /**
     * Allocate original frame buffer for teaching.
     * @return true if buffer is ready
     */
    bool ensureOriginalBuffer(size_t size);

    /**
     * Execute a pending teach request (called from onFrame after freeze).
     */
    void executePendingTeach();

    /**
     * Actually execute the teaching (shared by immediate and pending paths).
     */
    bool executeTeaching(float roi_x, float roi_y, float roi_w, float roi_h);

    /**
     * Render tracking visualization onto RGB buffer.
     * Draws contour overlays, outlines, and crosshairs.
     */
    void renderTrackingOverlay(uint8_t* rgb, int w, int h, int stride);

    /**
     * Render teaching visualization onto RGB buffer.
     * Delegates to ColorTeacher::renderVisualization.
     */
    void renderTeachingOverlay(uint8_t* rgb, int w, int h, int stride);
};
