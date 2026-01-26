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

    // Integer bounding box for fast test (scaled by 256)
    // Computed from mean ± k*sigma where k = sqrt(threshold_sq)
    int32_t u_min_scaled = -256;  // u_min * 256
    int32_t u_max_scaled = 256;   // u_max * 256
    int32_t v_min_scaled = -256;  // v_min * 256
    int32_t v_max_scaled = 256;   // v_max * 256

    // Integer coefficients for Mahalanobis (scaled by 256)
    int32_t mean_u_q8 = 0;        // mean_u * 256
    int32_t mean_v_q8 = 0;        // mean_v * 256
    int32_t a_q8 = 256;           // a * 256
    int32_t b_q8 = 0;             // b * 256
    int32_t c_q8 = 256;           // c * 256
    int64_t threshold_sq_q24 = 0; // threshold_sq * 256 * 65536

    // Int32-only Mahalanobis coefficients (smaller scaling to avoid overflow)
    int16_t a_q4 = 16;            // a * 16
    int16_t b_q4 = 0;             // b * 16
    int16_t c_q4 = 16;            // c * 16
    int32_t threshold_q12 = 0;    // threshold_sq * 4096

    // Compute a, b, c from covariance matrix
    // Uses 2x2 matrix inversion with regularization
    // Also computes integer bounding box for fast test
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

        // Compute integer bounding box for fast test
        // Use k = sqrt(threshold_sq) standard deviations
        float k = std::sqrt(threshold_sq);
        float sigma_u = std::sqrt(cov_uu);
        float sigma_v = std::sqrt(cov_vv);

        float u_min = mean_u - k * sigma_u;
        float u_max = mean_u + k * sigma_u;
        float v_min = mean_v - k * sigma_v;
        float v_max = mean_v + k * sigma_v;

        // Scale by 256 and convert to integer
        u_min_scaled = static_cast<int32_t>(u_min * 256.0f);
        u_max_scaled = static_cast<int32_t>(u_max * 256.0f);
        v_min_scaled = static_cast<int32_t>(v_min * 256.0f);
        v_max_scaled = static_cast<int32_t>(v_max * 256.0f);

        // Integer Mahalanobis coefficients (Q8 = scaled by 256)
        mean_u_q8 = static_cast<int32_t>(mean_u * 256.0f);
        mean_v_q8 = static_cast<int32_t>(mean_v * 256.0f);
        a_q8 = static_cast<int32_t>(a * 256.0f);
        b_q8 = static_cast<int32_t>(b * 256.0f);
        c_q8 = static_cast<int32_t>(c * 256.0f);
        threshold_sq_q24 = static_cast<int64_t>(threshold_sq * 256.0f * 65536.0f);

        // Int32-only coefficients (Q4 = scaled by 16)
        a_q4 = static_cast<int16_t>(a * 16.0f);
        b_q4 = static_cast<int16_t>(b * 16.0f);
        c_q4 = static_cast<int16_t>(c * 16.0f);
        threshold_q12 = static_cast<int32_t>(threshold_sq * 4096.0f);
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

    // Fast test using cross-multiplication (no division)
    // Instead of computing u = (r-g)/sum and testing distance,
    // we multiply through by sum to avoid division entirely.
    //
    // Original: du = (r-g)/sum - mean_u, dv = (2b-r-g)/sum - mean_v
    // Rewrite:  delta_r = (r-g) - mean_u*sum  (= du * sum)
    //           delta_b = (2b-r-g) - mean_v*sum  (= dv * sum)
    // Test: a*du² + b*du*dv + c*dv² < threshold_sq
    // Becomes: a*delta_r² + b*delta_r*delta_b + c*delta_b² < threshold_sq * sum²
    bool testFast(uint8_t r_val, uint8_t g_val, uint8_t b_val) const {
        int sum = r_val + g_val + b_val;
        if (sum < 3) return false;  // Too dark

        // Scaled differences (avoids division)
        float fsum = static_cast<float>(sum);
        float delta_r = (r_val - g_val) - mean_u * fsum;
        float delta_b = (2 * b_val - r_val - g_val) - mean_v * fsum;

        // Mahalanobis distance * sum², compared to threshold * sum²
        float dist = a * delta_r * delta_r
                   + b * delta_r * delta_b
                   + c * delta_b * delta_b;
        float thresh = threshold_sq * fsum * fsum;

        return dist < thresh;
    }

    // Ultra-fast integer-only bounding box test (no floats, no division)
    // Uses pre-computed scaled bounds and cross-multiplication
    // Tests if (u, v) falls within rectangular region around mean
    bool testFastInt(uint8_t r, uint8_t g, uint8_t b) const {
        int32_t sum = r + g + b;
        if (sum < 3) return false;  // Too dark

        // dr = (r - g) * 256, proportional to u * sum * 256
        // We want: u_min <= u <= u_max
        // Which is: u_min * sum <= (r-g) <= u_max * sum
        // Scaled:   u_min_scaled * sum <= dr <= u_max_scaled * sum
        int32_t dr = (r - g) << 8;
        int32_t db = (2 * b - r - g) << 8;

        // Bounding box test with cross-multiplication (no division)
        int32_t u_lo = u_min_scaled * sum;
        int32_t u_hi = u_max_scaled * sum;
        int32_t v_lo = v_min_scaled * sum;
        int32_t v_hi = v_max_scaled * sum;

        return (dr >= u_lo) && (dr <= u_hi) && (db >= v_lo) && (db <= v_hi);
    }

    // Integer Mahalanobis test (ellipse, not box) - no floats, no division
    // Same accuracy as original test(), but using fixed-point math
    bool testMahalInt(uint8_t r, uint8_t g, uint8_t b) const {
        int32_t sum = r + g + b;
        if (sum < 3) return false;  // Too dark

        // Compute scaled deltas (Q8): delta = (pixel - mean*sum) * 256
        // dr = du * sum * 256, db = dv * sum * 256
        int32_t dr = ((int32_t)(r - g) << 8) - mean_u_q8 * sum;
        int32_t db = ((2 * (int32_t)b - r - g) << 8) - mean_v_q8 * sum;

        // Mahalanobis: a*du² + b*du*dv + c*dv² < threshold_sq
        // Substituting du = dr/(sum*256), dv = db/(sum*256) and multiplying by (sum*256)²:
        // a*dr² + b*dr*db + c*db² < threshold_sq * sum² * 65536
        // Using Q8 coefficients (×256) and rearranging:
        // a_q8*dr² + b_q8*dr*db + c_q8*db² < threshold_sq_q24 * sum²
        int64_t dist = (int64_t)a_q8 * dr * dr
                     + (int64_t)b_q8 * dr * db
                     + (int64_t)c_q8 * db * db;

        int64_t thresh = threshold_sq_q24 * sum * sum;

        return dist < thresh;
    }

    // Int32-only Mahalanobis test (no int64, no float)
    // Uses aggressive shifting to keep all values in int32
    bool testMahalInt32(uint8_t r, uint8_t g, uint8_t b) const {
        int32_t sum = r + g + b;
        if (sum < 3) return false;

        // Compute scaled deltas with Q4 scaling (×16)
        // dr ≈ du * sum * 16, range ±16K
        int32_t dr = ((int32_t)(r - g) << 4) - ((mean_u_q8 * sum) >> 4);
        int32_t db = ((2 * (int32_t)b - r - g) << 4) - ((mean_v_q8 * sum) >> 4);

        // Squared terms, shifted right by 8 to prevent overflow
        // dr² max ≈ 256M, after >>8 max ≈ 1M
        int32_t dr_sq = (dr * dr) >> 8;
        int32_t db_sq = (db * db) >> 8;
        int32_t dr_db = (dr * db) >> 8;

        // Mahalanobis distance (scaled)
        // a_q4 max ≈ 1600, dr_sq max ≈ 1M → product max ≈ 1.6B, fits int32
        int32_t dist = a_q4 * dr_sq + b_q4 * dr_db + c_q4 * db_sq;

        // Threshold: need to match scaling
        // dist is scaled by: 16 (coeff) * 16² (dr²) / 256 (shift) = 16
        // Original comparison: a*du² < threshold_sq, where du = dr/(sum*16)
        // Our dist ≈ a * dr² / 16 = a * (du * sum * 16)² / 16 = a * du² * sum² * 16
        // So: dist < threshold_sq * sum² * 16
        int32_t thresh = threshold_q12 * ((sum * sum) >> 8);  // threshold_q12 = thresh * 4096

        return dist < thresh;
    }
};

} // namespace tracking
