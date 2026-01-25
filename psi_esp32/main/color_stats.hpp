#pragma once

#include <cstdint>
#include <cmath>
#include "color_model.hpp"
#include "contour.hpp"

namespace tracking {

// Online statistics accumulator for UV color samples
// Computes mean and covariance incrementally from run pixels
struct ColorStats {
    // Accumulators for online mean/covariance computation
    int64_t count = 0;
    double sum_u = 0.0;
    double sum_v = 0.0;
    double sum_uu = 0.0;
    double sum_uv = 0.0;
    double sum_vv = 0.0;

    // Reset all accumulators
    void reset() {
        count = 0;
        sum_u = sum_v = 0.0;
        sum_uu = sum_uv = sum_vv = 0.0;
    }

    // Add a single UV sample
    void addSample(float u, float v) {
        count++;
        sum_u += u;
        sum_v += v;
        sum_uu += u * u;
        sum_uv += u * v;
        sum_vv += v * v;
    }

    // Add all valid pixels from a horizontal run
    // Uses model's pixelToUV for filtering (saturation, overexposure)
    // NOTE: Expects RGB pixel format (R at offset 0, G at 1, B at 2)
    void addRun(const uint8_t* row, int xStart, int xEnd, const ColorModel& model) {
        for (int x = xStart; x <= xEnd; x++) {
            uint8_t r = row[x * 3];
            uint8_t g = row[x * 3 + 1];
            uint8_t b = row[x * 3 + 2];

            float u, v;
            if (model.pixelToUV(r, g, b, u, v)) {
                addSample(u, v);
            }
        }
    }

    // Add all valid pixels from a rectangular ROI
    // frame must be RGB, 3 channels
    void addROI(const uint8_t* data, int stride, int x, int y, int w, int h,
                const ColorModel& model) {
        for (int row = 0; row < h; row++) {
            const uint8_t* rowPtr = data + (y + row) * stride;
            addRun(rowPtr, x, x + w - 1, model);
        }
    }

    // Add all valid pixels from a Contour's runs
    void addContour(const uint8_t* data, int stride, const Contour& contour,
                    const ColorModel& model) {
        for (const auto& run : contour.runs) {
            const uint8_t* rowPtr = data + run.y * stride;
            addRun(rowPtr, run.xStart, run.xEnd, model);
        }
    }

    // Compute mean UV
    bool getMean(float& mean_u, float& mean_v) const {
        if (count < 1) return false;
        mean_u = static_cast<float>(sum_u / count);
        mean_v = static_cast<float>(sum_v / count);
        return true;
    }

    // Compute covariance matrix elements
    bool getCovariance(float& cov_uu, float& cov_uv, float& cov_vv) const {
        if (count < 2) return false;

        double mean_u = sum_u / count;
        double mean_v = sum_v / count;

        // Covariance: E[XY] - E[X]E[Y], with Bessel's correction
        double n = static_cast<double>(count);
        cov_uu = static_cast<float>((sum_uu - n * mean_u * mean_u) / (n - 1));
        cov_uv = static_cast<float>((sum_uv - n * mean_u * mean_v) / (n - 1));
        cov_vv = static_cast<float>((sum_vv - n * mean_v * mean_v) / (n - 1));

        return true;
    }

    // Create a ColorModel from accumulated statistics
    ColorModel computeModel(float threshold_sq = 2.0f) const {
        ColorModel model;
        model.threshold_sq = threshold_sq;

        if (count < 10) {
            // Not enough samples, return default model
            return model;
        }

        // Set mean
        getMean(model.mean_u, model.mean_v);

        // Set covariance
        getCovariance(model.cov_uu, model.cov_uv, model.cov_vv);

        // Compute Mahalanobis coefficients
        model.updateCoefficients();

        return model;
    }

    // Merge another ColorStats into this one
    void merge(const ColorStats& other) {
        count += other.count;
        sum_u += other.sum_u;
        sum_v += other.sum_v;
        sum_uu += other.sum_uu;
        sum_uv += other.sum_uv;
        sum_vv += other.sum_vv;
    }
};

} // namespace tracking
