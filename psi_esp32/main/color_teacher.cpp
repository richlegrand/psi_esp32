#include "color_teacher.hpp"
#include <cmath>
#include <climits>
#include <algorithm>

#ifdef ESP_PLATFORM
#include "esp_log.h"
static const char* TAG = "ColorTeacher";
#define LOGI(fmt, ...) ESP_LOGI(TAG, fmt, ##__VA_ARGS__)
#else
#include <cstdio>
#define LOGI(fmt, ...) printf(fmt "\n", ##__VA_ARGS__)
#endif

namespace tracking {

ColorTeacher::ColorTeacher()
    : state_(TeachState::IDLE)
    , detector_(100)
    , bestContour_(nullptr)
    , roiX_(0), roiY_(0), roiW_(0), roiH_(0)
    , iteration_(0)
    , prevArea_(0)
    , minAreaFraction_(0.001f)   // 0.1% of image
    , teachThreshold_(3.5f)      // Larger threshold during teaching
    , convergenceTolerance_(0.05f) // 5% area change
{
}

bool ColorTeacher::start(const uint8_t* rgb_buffer, int width, int height, int stride,
                          float roi_x, float roi_y, float roi_w, float roi_h) {
    // Convert normalized coordinates to pixels
    roiX_ = static_cast<int>(roi_x * width);
    roiY_ = static_cast<int>(roi_y * height);
    roiW_ = static_cast<int>(roi_w * width);
    roiH_ = static_cast<int>(roi_h * height);

    // Clamp to image bounds
    roiX_ = std::max(0, std::min(roiX_, width - 1));
    roiY_ = std::max(0, std::min(roiY_, height - 1));
    roiW_ = std::max(1, std::min(roiW_, width - roiX_));
    roiH_ = std::max(1, std::min(roiH_, height - roiY_));

    roiCenter_ = Point(roiX_ + roiW_ / 2, roiY_ + roiH_ / 2);

    LOGI("Starting teach: ROI=(%d,%d) %dx%d, center=(%d,%d)",
         roiX_, roiY_, roiW_, roiH_, roiCenter_.x, roiCenter_.y);

    // Set minimum area for detector
    int minAreaPixels = static_cast<int>(minAreaFraction_ * width * height);
    detector_.setMinArea(minAreaPixels);

    // Bootstrap: collect initial stats from ROI
    stats_.reset();
    ColorModel bootstrapModel;  // Default model for initial extraction
    bootstrapModel.min_sat = 0.03f;
    bootstrapModel.max_channel = 255;

    // Extract UV samples from ROI
    stats_.addROI(rgb_buffer, stride, roiX_, roiY_, roiW_, roiH_, bootstrapModel);

    if (stats_.count < 10) {
        LOGI("Not enough pixels in ROI (%lld)", (long long)stats_.count);
        state_ = TeachState::IDLE;
        return false;
    }

    // Compute initial model from ROI
    model_ = stats_.computeModel(teachThreshold_);

    LOGI("Bootstrap model: mean=(%.4f, %.4f), samples=%lld",
         model_.mean_u, model_.mean_v, (long long)stats_.count);

    state_ = TeachState::BOOTSTRAPPING;
    iteration_ = 0;
    prevArea_ = 0;
    bestContour_ = nullptr;
    contours_.clear();

    return true;
}

const Contour* ColorTeacher::findClosestContour(const Point& target) const {
    if (contours_.empty()) return nullptr;

    const Contour* closest = nullptr;
    int minDistSq = INT_MAX;

    for (const auto& c : contours_) {
        Point center = c.centroid();
        int dx = center.x - target.x;
        int dy = center.y - target.y;
        int distSq = dx * dx + dy * dy;
        if (distSq < minDistSq) {
            minDistSq = distSq;
            closest = &c;
        }
    }
    return closest;
}

bool ColorTeacher::iterate(const uint8_t* rgb_buffer, int width, int height, int stride) {
    if (state_ != TeachState::BOOTSTRAPPING && state_ != TeachState::ITERATING) {
        return false;
    }

    state_ = TeachState::ITERATING;
    iteration_++;

    // Detect contours using current model
    detector_.detect(rgb_buffer, width, height, stride, model_, contours_);

    // Find contour closest to ROI center
    bestContour_ = findClosestContour(roiCenter_);

    if (!bestContour_) {
        LOGI("Iteration %d: No contours found", iteration_);
        return false;
    }

    // Collect stats from the best contour's runs
    stats_.reset();
    stats_.addContour(rgb_buffer, stride, *bestContour_, model_);

    int area = bestContour_->area;
    int nContours = static_cast<int>(contours_.size());

    // Compute area change
    float change = (prevArea_ > 0)
        ? std::abs(area - prevArea_) / static_cast<float>(prevArea_)
        : 1.0f;
    bool converged = (prevArea_ > 0) && (change < convergenceTolerance_);

    LOGI("Iteration %d: %d contours, area=%d, change=%.3f, mean=(%.4f, %.4f)",
         iteration_, nContours, area, change, model_.mean_u, model_.mean_v);

    if (converged) {
        state_ = TeachState::CONVERGED;
        LOGI("Converged at iteration %d", iteration_);
    }

    // Update model from collected stats
    if (stats_.count >= 10) {
        model_ = stats_.computeModel(teachThreshold_);
    }

    prevArea_ = area;
    return true;
}

void ColorTeacher::accept() {
    if (state_ == TeachState::ITERATING || state_ == TeachState::CONVERGED) {
        state_ = TeachState::ACCEPTED;
        LOGI("Model accepted: mean=(%.4f, %.4f)", model_.mean_u, model_.mean_v);
    }
}

void ColorTeacher::cancel() {
    state_ = TeachState::IDLE;
    contours_.clear();
    bestContour_ = nullptr;
    iteration_ = 0;
    LOGI("Teaching cancelled");
}

void ColorTeacher::drawContourTinted(uint8_t* buffer, int width, int height, int stride,
                                      const Contour& contour,
                                      uint8_t tintR, uint8_t tintG, uint8_t tintB,
                                      float alpha) {
    float invAlpha = 1.0f - alpha;

    for (const auto& run : contour.runs) {
        if (run.y < 0 || run.y >= height) continue;

        uint8_t* row = buffer + run.y * stride;
        int xStart = std::max(0, run.xStart);
        int xEnd = std::min(run.xEnd, width - 1);

        for (int x = xStart; x <= xEnd; x++) {
            int idx = x * 3;
            row[idx]     = static_cast<uint8_t>(row[idx]     * invAlpha + tintR * alpha);
            row[idx + 1] = static_cast<uint8_t>(row[idx + 1] * invAlpha + tintG * alpha);
            row[idx + 2] = static_cast<uint8_t>(row[idx + 2] * invAlpha + tintB * alpha);
        }
    }
}

void ColorTeacher::drawContourOutline(uint8_t* buffer, int width, int height, int stride,
                                       const Contour& contour,
                                       uint8_t r, uint8_t g, uint8_t b) {
    // Draw convex hull outline
    std::vector<Point> hull = contour.convexHull();
    if (hull.size() < 2) return;

    // Simple line drawing (Bresenham)
    auto drawLine = [&](Point p0, Point p1) {
        int dx = std::abs(p1.x - p0.x);
        int dy = std::abs(p1.y - p0.y);
        int sx = (p0.x < p1.x) ? 1 : -1;
        int sy = (p0.y < p1.y) ? 1 : -1;
        int err = dx - dy;

        while (true) {
            if (p0.x >= 0 && p0.x < width && p0.y >= 0 && p0.y < height) {
                int idx = p0.y * stride + p0.x * 3;
                buffer[idx]     = r;
                buffer[idx + 1] = g;
                buffer[idx + 2] = b;
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
}

void ColorTeacher::renderVisualization(uint8_t* rgb_buffer, int width, int height, int stride) {
    if (contours_.empty()) return;

    // Draw all contours in gray (semi-transparent)
    for (const auto& c : contours_) {
        drawContourTinted(rgb_buffer, width, height, stride, c,
                          128, 128, 128, 0.3f);
    }

    // Draw all contour outlines in gray
    for (const auto& c : contours_) {
        drawContourOutline(rgb_buffer, width, height, stride, c,
                           100, 100, 100);
    }

    // Draw best contour in yellow (brighter)
    if (bestContour_) {
        drawContourTinted(rgb_buffer, width, height, stride, *bestContour_,
                          255, 255, 0, 0.4f);

        // Draw best contour outline in cyan
        drawContourOutline(rgb_buffer, width, height, stride, *bestContour_,
                           0, 255, 255);
    }

    // Draw ROI center marker (blue cross)
    auto drawCross = [&](int cx, int cy, int size, uint8_t r, uint8_t g, uint8_t b) {
        for (int i = -size; i <= size; i++) {
            // Horizontal
            int x = cx + i;
            if (x >= 0 && x < width && cy >= 0 && cy < height) {
                int idx = cy * stride + x * 3;
                rgb_buffer[idx] = r;
                rgb_buffer[idx + 1] = g;
                rgb_buffer[idx + 2] = b;
            }
            // Vertical
            int y = cy + i;
            if (cx >= 0 && cx < width && y >= 0 && y < height) {
                int idx = y * stride + cx * 3;
                rgb_buffer[idx] = r;
                rgb_buffer[idx + 1] = g;
                rgb_buffer[idx + 2] = b;
            }
        }
    };

    // Blue cross at ROI center
    drawCross(roiCenter_.x, roiCenter_.y, 8, 0, 0, 255);

    // Red dot at best contour centroid
    if (bestContour_) {
        Point center = bestContour_->centroid();
        drawCross(center.x, center.y, 4, 255, 0, 0);
    }
}

} // namespace tracking
