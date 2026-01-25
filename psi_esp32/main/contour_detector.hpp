#pragma once

#include <vector>
#include <cstdint>
#include "contour.hpp"
#include "color_model.hpp"
#include "color_stats.hpp"

namespace tracking {

// Single-pass scanline contour detection
class ContourDetector {
public:
    ContourDetector(int minArea = 100);

    // Process a frame using Mahalanobis color model
    // Optionally collects UV statistics from detected run pixels
    // NOTE: buffer must be RGB format, 3 bytes per pixel
    void detect(const uint8_t* buffer, int width, int height, int stride,
                const ColorModel& model,
                std::vector<Contour>& contours,
                ColorStats* stats = nullptr);

    // Configuration
    void setMinArea(int minArea) { minArea_ = minArea; }
    int minArea() const { return minArea_; }

private:
    int minArea_;
    int nextContourId_;

    // Internal run with contour assignment
    struct InternalRun {
        int xStart;
        int xEnd;
        int contourId;

        int width() const { return xEnd - xStart + 1; }
    };

    // Active contour being built during detection
    struct ActiveContour {
        int id;
        int area;
        int startY;
        int lastY;
        int minX, maxX;
        std::vector<InternalRun> currentRuns;
        std::vector<InternalRun> prevRuns;
        std::vector<Run> allRuns;
    };

    std::vector<ActiveContour> activeContours_;

    void findRunsWithModel(const uint8_t* row, int width, int y,
                           const ColorModel& model, std::vector<InternalRun>& runs,
                           ColorStats* stats);

    void connectRuns(std::vector<InternalRun>& runs, int y);

    bool runsConnect(const InternalRun& a, const InternalRun& b);

    void closeInactiveContours(int currentY, std::vector<Contour>& output);

    void finalizeContour(ActiveContour& ac, std::vector<Contour>& output);
};

} // namespace tracking
