#pragma once

#include <cstdint>
#include <vector>
#include "color_model.hpp"
#include "color_stats.hpp"
#include "contour.hpp"
#include "contour_detector.hpp"

namespace tracking {

// Teaching state machine
enum class TeachState {
    IDLE,           // No teaching in progress
    BOOTSTRAPPING,  // Collecting initial samples from ROI
    ITERATING,      // Running iterative refinement
    CONVERGED,      // Model has converged (area change below threshold)
    ACCEPTED        // User accepted the model
};

// Iterative color teaching with visualization
class ColorTeacher {
public:
    ColorTeacher();

    // Start teaching with a region of interest (normalized 0-1 coordinates)
    // Returns true if bootstrap succeeded
    bool start(const uint8_t* rgb_buffer, int width, int height, int stride,
               float roi_x, float roi_y, float roi_w, float roi_h);

    // Perform one iteration of refinement
    // Returns true if model is still valid, false if no contours found
    bool iterate(const uint8_t* rgb_buffer, int width, int height, int stride);

    // Accept current model
    void accept();

    // Cancel teaching
    void cancel();

    // Get current state
    TeachState state() const { return state_; }

    // Get current model (valid after bootstrap)
    const ColorModel& model() const { return model_; }

    // Get detected contours from last iteration
    const std::vector<Contour>& contours() const { return contours_; }

    // Get the best contour (closest to ROI center) from last iteration
    const Contour* bestContour() const { return bestContour_; }

    // Get iteration count
    int iteration() const { return iteration_; }

    // Check if converged
    bool isConverged() const { return state_ == TeachState::CONVERGED; }

    // Render visualization to RGB buffer
    // Highlights detected pixels and draws contour info
    void renderVisualization(uint8_t* rgb_buffer, int width, int height, int stride);

    // Configuration
    void setMinAreaFraction(float frac) { minAreaFraction_ = frac; }
    void setTeachThreshold(float t) { teachThreshold_ = t; }
    void setConvergenceTolerance(float t) { convergenceTolerance_ = t; }

private:
    TeachState state_;
    ColorModel model_;
    ColorStats stats_;
    ContourDetector detector_;
    std::vector<Contour> contours_;
    const Contour* bestContour_;

    // ROI in pixel coordinates
    int roiX_, roiY_, roiW_, roiH_;
    Point roiCenter_;

    // Iteration tracking
    int iteration_;
    int prevArea_;

    // Configuration
    float minAreaFraction_;     // Minimum contour area as fraction of image
    float teachThreshold_;      // Mahalanobis threshold during teaching
    float convergenceTolerance_; // Area change tolerance for convergence

    // Find contour closest to ROI center
    const Contour* findClosestContour(const Point& target) const;

    // Draw a filled contour with a tint color
    void drawContourTinted(uint8_t* buffer, int width, int height, int stride,
                           const Contour& contour,
                           uint8_t tintR, uint8_t tintG, uint8_t tintB,
                           float alpha);

    // Draw contour outline
    void drawContourOutline(uint8_t* buffer, int width, int height, int stride,
                            const Contour& contour,
                            uint8_t r, uint8_t g, uint8_t b);
};

} // namespace tracking
