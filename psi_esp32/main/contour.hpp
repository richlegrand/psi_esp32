#pragma once

#include <vector>
#include <map>
#include <cmath>
#include <algorithm>
#include <cfloat>
#include <cstdint>

namespace tracking {

// Simple point struct (replaces cv::Point)
struct Point {
    int x = 0;
    int y = 0;

    Point() = default;
    Point(int x_, int y_) : x(x_), y(y_) {}
};

// Simple rectangle struct (replaces cv::Rect)
struct Rect {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;

    Rect() = default;
    Rect(int x_, int y_, int w_, int h_) : x(x_), y(y_), width(w_), height(h_) {}
};

// A horizontal run of pixels
struct Run {
    int y;
    int xStart;
    int xEnd;

    int width() const { return xEnd - xStart + 1; }
    int centerX() const { return (xStart + xEnd) / 2; }
};

// A contour defined by its runs
struct Contour {
    std::vector<Run> runs;
    int area = 0;
    int minX = 0, maxX = 0, minY = 0, maxY = 0;

    Rect boundingBox() const {
        return Rect(minX, minY, maxX - minX + 1, maxY - minY + 1);
    }

    Point centroid() const {
        if (runs.empty()) return Point(0, 0);

        double sumX = 0, sumY = 0;
        int totalArea = 0;
        for (const auto& run : runs) {
            int w = run.width();
            sumX += run.centerX() * w;
            sumY += run.y * w;
            totalArea += w;
        }
        if (totalArea == 0) return Point(0, 0);
        return Point(
            static_cast<int>(sumX / totalArea),
            static_cast<int>(sumY / totalArea)
        );
    }

    // Cross product of vectors OA and OB (O is origin point)
    // Returns positive if counter-clockwise (left turn), negative if clockwise
    static int64_t cross(const Point& O, const Point& A, const Point& B) {
        return static_cast<int64_t>(A.x - O.x) * (B.y - O.y) -
               static_cast<int64_t>(A.y - O.y) * (B.x - O.x);
    }

    // Compute convex hull using Andrew's monotone chain algorithm
    // Optimized for run-based contours: O(n log n) due to sorting
    std::vector<Point> convexHull() const {
        if (runs.empty()) return {};

        // Build a map of y -> (minX, maxX) for each row
        // Using map handles unsorted runs and multiple runs per row
        std::map<int, std::pair<int, int>> rowExtents;

        for (const auto& run : runs) {
            auto it = rowExtents.find(run.y);
            if (it == rowExtents.end()) {
                rowExtents[run.y] = {run.xStart, run.xEnd};
            } else {
                it->second.first = std::min(it->second.first, run.xStart);
                it->second.second = std::max(it->second.second, run.xEnd);
            }
        }

        if (rowExtents.size() < 2) {
            // Single row - return the two corners
            if (rowExtents.size() == 1) {
                auto& ext = rowExtents.begin()->second;
                int y = rowExtents.begin()->first;
                return {Point(ext.first, y), Point(ext.second, y)};
            }
            return {};
        }

        // Extract sorted left and right edges
        std::vector<Point> leftEdges;
        std::vector<Point> rightEdges;
        leftEdges.reserve(rowExtents.size());
        rightEdges.reserve(rowExtents.size());

        for (const auto& [y, ext] : rowExtents) {
            leftEdges.emplace_back(ext.first, y);
            rightEdges.emplace_back(ext.second, y);
        }

        std::vector<Point> hull;

        // Build left side of hull: scan left edges top to bottom
        // Remove points that make right turns (not convex)
        for (const auto& p : leftEdges) {
            while (hull.size() >= 2 && cross(hull[hull.size()-2], hull[hull.size()-1], p) >= 0) {
                hull.pop_back();
            }
            hull.push_back(p);
        }

        // Build right side of hull: scan right edges bottom to top
        // Continue from where we left off
        size_t leftSize = hull.size();
        for (int i = static_cast<int>(rightEdges.size()) - 1; i >= 0; i--) {
            const auto& p = rightEdges[i];
            while (hull.size() > leftSize && cross(hull[hull.size()-2], hull[hull.size()-1], p) >= 0) {
                hull.pop_back();
            }
            hull.push_back(p);
        }

        return hull;
    }

    // Squared distance between two bounding boxes (0 if overlapping)
    static int boundingBoxDistanceSq(const Rect& a, const Rect& b) {
        // Gap in x direction (0 if overlapping)
        int dx = std::max(0, std::max(a.x - (b.x + b.width), b.x - (a.x + a.width)));
        // Gap in y direction (0 if overlapping)
        int dy = std::max(0, std::max(a.y - (b.y + b.height), b.y - (a.y + a.height)));
        return dx * dx + dy * dy;
    }

    // Distance from point P to line segment AB (parametric projection)
    static float pointToSegmentDistance(const Point& P, const Point& A, const Point& B) {
        float ABx = static_cast<float>(B.x - A.x);
        float ABy = static_cast<float>(B.y - A.y);
        float APx = static_cast<float>(P.x - A.x);
        float APy = static_cast<float>(P.y - A.y);

        float lenSq = ABx * ABx + ABy * ABy;
        if (lenSq == 0.0f) {
            // A and B are the same point
            return std::sqrt(APx * APx + APy * APy);
        }

        // Project P onto line AB, clamped to segment
        float t = (APx * ABx + APy * ABy) / lenSq;
        t = std::max(0.0f, std::min(1.0f, t));

        // Closest point on segment
        float closestX = A.x + t * ABx;
        float closestY = A.y + t * ABy;

        float dx = P.x - closestX;
        float dy = P.y - closestY;
        return std::sqrt(dx * dx + dy * dy);
    }

    // Fast intersection test: check if any hull vertex is in the BB intersection region
    static bool hullsIntersectFast(const std::vector<Point>& hullA, const Rect& bbA,
                                    const std::vector<Point>& hullB, const Rect& bbB) {
        // Compute BB intersection
        int left = std::max(bbA.x, bbB.x);
        int top = std::max(bbA.y, bbB.y);
        int right = std::min(bbA.x + bbA.width, bbB.x + bbB.width);
        int bottom = std::min(bbA.y + bbA.height, bbB.y + bbB.height);

        if (left >= right || top >= bottom) return false;  // No BB overlap

        // Check if any vertex is in the intersection region
        for (const auto& p : hullA) {
            if (p.x >= left && p.x <= right && p.y >= top && p.y <= bottom)
                return true;
        }
        for (const auto& p : hullB) {
            if (p.x >= left && p.x <= right && p.y >= top && p.y <= bottom)
                return true;
        }
        return false;
    }

    // Minimum distance between two convex hulls (0 if intersecting)
    static float hullDistance(const std::vector<Point>& hullA, const Rect& bbA,
                               const std::vector<Point>& hullB, const Rect& bbB) {
        if (hullA.empty() || hullB.empty()) return FLT_MAX;

        // Fast intersection check
        if (hullsIntersectFast(hullA, bbA, hullB, bbB)) return 0.0f;

        float minDist = FLT_MAX;

        // Check all vertices of A against all edges of B
        for (const auto& p : hullA) {
            for (size_t i = 0; i < hullB.size(); i++) {
                size_t j = (i + 1) % hullB.size();
                minDist = std::min(minDist, pointToSegmentDistance(p, hullB[i], hullB[j]));
            }
        }

        // Check all vertices of B against all edges of A
        for (const auto& p : hullB) {
            for (size_t i = 0; i < hullA.size(); i++) {
                size_t j = (i + 1) % hullA.size();
                minDist = std::min(minDist, pointToSegmentDistance(p, hullA[i], hullA[j]));
            }
        }

        return minDist;
    }

    // Distance to another contour (using convex hulls)
    float distanceTo(const Contour& other) const {
        return hullDistance(convexHull(), boundingBox(), other.convexHull(), other.boundingBox());
    }
};

} // namespace tracking
