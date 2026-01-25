#include "contour_detector.hpp"
#include <algorithm>
#include <climits>

namespace tracking {

ContourDetector::ContourDetector(int minArea)
    : minArea_(minArea), nextContourId_(0) {
}

void ContourDetector::findRunsWithModel(const uint8_t* row, int width, int y,
                                         const ColorModel& model,
                                         std::vector<InternalRun>& runs,
                                         ColorStats* stats) {
    int x = 0;
    while (x < width) {
        // Find start of run
        while (x < width) {
            // RGB format: R at offset 0, G at 1, B at 2
            uint8_t r = row[x * 3];
            uint8_t g = row[x * 3 + 1];
            uint8_t b = row[x * 3 + 2];
            if (model.test(r, g, b)) break;
            x++;
        }

        if (x >= width) break;

        int xStart = x;

        // Find end of run
        while (x < width) {
            uint8_t r = row[x * 3];
            uint8_t g = row[x * 3 + 1];
            uint8_t b = row[x * 3 + 2];
            if (!model.test(r, g, b)) break;
            x++;
        }

        int xEnd = x - 1;
        runs.push_back({xStart, xEnd, -1});

        // Collect statistics from run pixels if requested
        if (stats) {
            stats->addRun(row, xStart, xEnd, model);
        }
    }
}

void ContourDetector::detect(const uint8_t* buffer, int width, int height, int stride,
                              const ColorModel& model,
                              std::vector<Contour>& contours,
                              ColorStats* stats) {
    contours.clear();
    activeContours_.clear();
    nextContourId_ = 0;

    if (stats) {
        stats->reset();
    }

    std::vector<InternalRun> runs;

    for (int y = 0; y < height; y++) {
        const uint8_t* row = buffer + y * stride;

        // 1. Find runs of matching pixels in this row
        runs.clear();
        findRunsWithModel(row, width, y, model, runs, stats);

        // 2. Connect runs to active contours
        connectRuns(runs, y);

        // 3. Close contours that have no runs this row
        closeInactiveContours(y, contours);

        // 4. Update prevRuns for next iteration
        for (auto& ac : activeContours_) {
            ac.prevRuns = std::move(ac.currentRuns);
            ac.currentRuns.clear();
        }
    }

    // Close all remaining active contours
    for (auto& ac : activeContours_) {
        finalizeContour(ac, contours);
    }
    activeContours_.clear();
}

bool ContourDetector::runsConnect(const InternalRun& a, const InternalRun& b) {
    // Runs connect if they overlap (share at least one x coordinate)
    return !(a.xEnd < b.xStart || b.xEnd < a.xStart);
}

void ContourDetector::connectRuns(std::vector<InternalRun>& runs, int y) {
    for (auto& run : runs) {
        std::vector<int> connectedIds;

        // Check against all active contours
        for (auto& ac : activeContours_) {
            bool connected = false;

            // Check previous row's runs
            // Note: lastY == y means we already added runs this row, but prevRuns
            // still contains row y-1 (swap happens after all runs processed)
            if (ac.lastY == y - 1 || ac.lastY == y) {
                for (const auto& prevRun : ac.prevRuns) {
                    if (runsConnect(run, prevRun)) {
                        connected = true;
                        break;
                    }
                }
            }

            if (connected) {
                connectedIds.push_back(ac.id);
            }
        }

        int targetId;
        if (connectedIds.empty()) {
            // New contour
            ActiveContour ac;
            ac.id = nextContourId_++;
            ac.area = 0;
            ac.startY = y;
            ac.lastY = y;
            ac.minX = INT_MAX;
            ac.maxX = 0;
            activeContours_.push_back(std::move(ac));
            targetId = activeContours_.back().id;
        } else {
            targetId = connectedIds[0];

            // Merge other connected contours into the target
            for (size_t i = 1; i < connectedIds.size(); i++) {
                int mergeId = connectedIds[i];

                ActiveContour* target = nullptr;
                ActiveContour* source = nullptr;
                for (auto& ac : activeContours_) {
                    if (ac.id == targetId) target = &ac;
                    if (ac.id == mergeId) source = &ac;
                }

                if (target && source && target != source) {
                    target->area += source->area;
                    target->startY = std::min(target->startY, source->startY);
                    target->minX = std::min(target->minX, source->minX);
                    target->maxX = std::max(target->maxX, source->maxX);

                    for (auto& r : source->prevRuns) {
                        target->prevRuns.push_back(r);
                    }
                    for (auto& r : source->currentRuns) {
                        target->currentRuns.push_back(r);
                    }
                    for (auto& r : source->allRuns) {
                        target->allRuns.push_back(r);
                    }

                    source->id = -1;
                }
            }

            activeContours_.erase(
                std::remove_if(activeContours_.begin(), activeContours_.end(),
                    [](const ActiveContour& ac) { return ac.id == -1; }),
                activeContours_.end());
        }

        // Add run to target contour
        run.contourId = targetId;
        for (auto& ac : activeContours_) {
            if (ac.id == targetId) {
                ac.currentRuns.push_back(run);
                ac.area += run.width();
                ac.lastY = y;
                ac.minX = std::min(ac.minX, run.xStart);
                ac.maxX = std::max(ac.maxX, run.xEnd);
                ac.allRuns.push_back({y, run.xStart, run.xEnd});
                break;
            }
        }
    }
}

void ContourDetector::closeInactiveContours(int currentY, std::vector<Contour>& output) {
    auto it = activeContours_.begin();
    while (it != activeContours_.end()) {
        if (it->lastY < currentY) {
            finalizeContour(*it, output);
            it = activeContours_.erase(it);
        } else {
            ++it;
        }
    }
}

void ContourDetector::finalizeContour(ActiveContour& ac, std::vector<Contour>& output) {
    if (ac.area < minArea_) return;

    Contour c;
    c.area = ac.area;
    c.runs = std::move(ac.allRuns);
    c.minX = ac.minX;
    c.maxX = ac.maxX;
    c.minY = ac.startY;
    c.maxY = ac.lastY;

    output.push_back(std::move(c));
}

} // namespace tracking
