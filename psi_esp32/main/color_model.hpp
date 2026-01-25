#pragma once

#include <cstdint>
#include <cmath>

namespace tracking {

// Color model for Mahalanobis-based detection in opponent colorspace (u, v)
// u = (R - G) / (R + G + B)      -- red-green axis
// v = (2B - R - G) / (R + G + B) -- blue-yellow axis (orthogonal to u)
struct ColorModel {
    // Mean in UV space
    float mean_u = 0.0f;
    float mean_v = 0.0f;

    // Covariance matrix elements
    float cov_uu = 0.01f;  // variance of u
    float cov_uv = 0.0f;   // covariance
    float cov_vv = 0.01f;  // variance of v

    // Mahalanobis coefficients (derived from inverse covariance)
    // dist_sq = a * du^2 + b * du * dv + c * dv^2
    float a = 100.0f;
    float b = 0.0f;
    float c = 100.0f;

    // Detection threshold (Mahalanobis distance squared)
    float threshold_sq = 2.0f;

    // Pixel filtering parameters
    float min_sat = 0.03f;       // Minimum saturation: max(|u|, |v|)
    uint8_t max_channel = 255;   // Maximum channel value (filter overexposed)

    // Compute a, b, c from covariance matrix
    // Uses 2x2 matrix inversion with regularization
    void updateCoefficients() {
        // Add small regularization to avoid singularity
        float reg = 0.001f;
        float uu = cov_uu + reg;
        float uv = cov_uv;
        float vv = cov_vv + reg;

        // Determinant of covariance matrix
        float det = uu * vv - uv * uv;
        if (det < 1e-10f) {
            // Near-singular, use identity
            a = 1.0f;
            b = 0.0f;
            c = 1.0f;
            return;
        }

        // Inverse covariance matrix
        float inv_det = 1.0f / det;
        float inv_uu = vv * inv_det;
        float inv_uv = -uv * inv_det;
        float inv_vv = uu * inv_det;

        // Mahalanobis coefficients
        a = inv_uu;
        b = 2.0f * inv_uv;
        c = inv_vv;
    }

    // Convert RGB pixel to UV colorspace
    // Returns false if pixel should be filtered out (low saturation or overexposed)
    bool pixelToUV(uint8_t r_val, uint8_t g_val, uint8_t b_val, float& u, float& v) const {
        int r = r_val, g = g_val, b = b_val;
        float sum = static_cast<float>(r + g + b);
        if (sum < 1) {
            return false;
        }

        u = (r - g) / sum;
        v = (2 * b - r - g) / sum;
        return true;
    }

    // Compute Mahalanobis distance squared from mean
    float mahalDistSq(float u, float v) const {
        float du = u - mean_u;
        float dv = v - mean_v;
        return a * du * du + b * du * dv + c * dv * dv;
    }

    // Test if an RGB pixel matches this color model
    bool test(uint8_t r_val, uint8_t g_val, uint8_t b_val) const {
        float u, v;
        if (!pixelToUV(r_val, g_val, b_val, u, v)) {
            return false;
        }
        float du = u - mean_u;
        float dv = v - mean_v;
        return (a * du * du + b * du * dv + c * dv * dv) < threshold_sq;
    }
};

} // namespace tracking
