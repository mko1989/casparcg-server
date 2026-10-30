/*
 * Copyright (c) 2026 CasparCG contributors
 *
 * This file is part of CasparCG (www.casparcg.com).
 *
 * CasparCG is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * CasparCG is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with CasparCG. If not, see <http://www.gnu.org/licenses/>.
 */

// Screen consumer slices: N (source rectangle -> destination quad) mappings of the channel frame onto the
// window canvas. Pure geometry, no GL calls, so the same code can serve another renderer later.

#pragma once

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace caspar { namespace screen {

constexpr std::size_t max_slices = 64;

struct slice
{
    double src[4]     = {0, 0, 0, 0};             // x y w h in channel pixels
    double dst[8]     = {0, 0, 0, 0, 0, 0, 0, 0}; // ul ur lr ll corners (x y each) in window-canvas pixels
    float  opacity    = 1.0f;                     // 0..1
    float  feather[4] = {0, 0, 0, 0};             // l t r b in canvas pixels; edges named by dst corners:
                                                  // l = ul-ll, t = ul-ur, r = ur-lr, b = ll-lr
    double clip[4]    = {0, 0, 0, 0};             // x y w h in canvas pixels; w or h <= 0 = no clip

    bool has_clip() const { return clip[2] > 0.0 && clip[3] > 0.0; }
};

// One vertex of a slice triangle. Texture and quad-local coordinates are pre-multiplied by q; the
// fragment shader divides by q (perspective-correct mapping of the rectangle onto the quad).
struct slice_vertex
{
    float x, y;       // NDC
    float u, v, r, q; // texture (u*q, v*q, 0, q)
    float lu, lv;     // quad-local (lu*q, lv*q), 0..1 across the quad
    float opacity;
    float feather[4]; // l t r b in quad-local units (fraction of the quad's mean width/height)
    float src_clamp[4]; // u0 v0 u1 v1: the source rect inset by half a texel (no bleed from neighbours)
};

inline double slice_cross(double ax, double ay, double bx, double by) { return ax * by - ay * bx; }

// Checks a slice and clamps its source rectangle to the channel raster. Accepts convex quads of either
// orientation (a flipped slice arrives with the opposite winding); rejects bow-ties, concave and
// degenerate quads. Returns false with a reason in `error`.
inline bool validate_slice(slice& s, double raster_w, double raster_h, std::string& error)
{
    for (double v : s.src)
        if (!std::isfinite(v)) {
            error = "non-finite source value";
            return false;
        }
    for (double v : s.dst)
        if (!std::isfinite(v)) {
            error = "non-finite destination value";
            return false;
        }
    for (double v : s.clip)
        if (!std::isfinite(v)) {
            error = "non-finite clip value";
            return false;
        }
    if (!std::isfinite(s.opacity)) {
        error = "non-finite opacity";
        return false;
    }
    for (float f : s.feather)
        if (!std::isfinite(f)) {
            error = "non-finite feather";
            return false;
        }

    if (s.src[2] <= 0.0 || s.src[3] <= 0.0) {
        error = "source width/height must be > 0";
        return false;
    }

    // Clamp the source rectangle to the raster.
    double x0 = std::clamp(s.src[0], 0.0, raster_w);
    double y0 = std::clamp(s.src[1], 0.0, raster_h);
    double x1 = std::clamp(s.src[0] + s.src[2], 0.0, raster_w);
    double y1 = std::clamp(s.src[1] + s.src[3], 0.0, raster_h);
    if (x1 - x0 <= 0.0 || y1 - y0 <= 0.0) {
        error = "source rectangle lies outside the channel raster";
        return false;
    }
    s.src[0] = x0;
    s.src[1] = y0;
    s.src[2] = x1 - x0;
    s.src[3] = y1 - y0;

    // Convexity: the four turns must share one sign, none (near) zero.
    double max_len2 = 0.0;
    for (int i = 0; i < 4; ++i) {
        int    j  = (i + 1) % 4;
        double dx = s.dst[j * 2] - s.dst[i * 2];
        double dy = s.dst[j * 2 + 1] - s.dst[i * 2 + 1];
        max_len2  = std::max(max_len2, dx * dx + dy * dy);
    }
    const double eps  = 1e-9 * std::max(1.0, max_len2);
    int          sign = 0;
    for (int i = 0; i < 4; ++i) {
        int    j     = (i + 1) % 4;
        int    k     = (i + 2) % 4;
        double cross = slice_cross(s.dst[j * 2] - s.dst[i * 2],
                                   s.dst[j * 2 + 1] - s.dst[i * 2 + 1],
                                   s.dst[k * 2] - s.dst[j * 2],
                                   s.dst[k * 2 + 1] - s.dst[j * 2 + 1]);
        if (std::abs(cross) <= eps) {
            error = "degenerate destination quad";
            return false;
        }
        int this_sign = cross > 0 ? 1 : -1;
        if (sign == 0)
            sign = this_sign;
        else if (sign != this_sign) {
            error = "destination quad is not convex (bow-tie or concave)";
            return false;
        }
    }
    double area2 = 0.0;
    for (int i = 0; i < 4; ++i) {
        int j = (i + 1) % 4;
        area2 += s.dst[i * 2] * s.dst[j * 2 + 1] - s.dst[j * 2] * s.dst[i * 2 + 1];
    }
    if (std::abs(area2) * 0.5 < 0.25) {
        error = "destination quad area < 0.25 px";
        return false;
    }

    s.opacity = std::clamp(s.opacity, 0.0f, 1.0f);
    for (auto& f : s.feather)
        f = std::max(f, 0.0f);
    return true;
}

inline bool validate_slices(std::vector<slice>& slices, double raster_w, double raster_h, std::string& error)
{
    if (slices.size() > max_slices) {
        error = "more than " + std::to_string(max_slices) + " slices";
        return false;
    }
    for (std::size_t n = 0; n < slices.size(); ++n) {
        std::string e;
        if (!validate_slice(slices[n], raster_w, raster_h, e)) {
            error = "slice " + std::to_string(n) + ": " + e;
            return false;
        }
    }
    return true;
}

inline bool slices_need_blend(const std::vector<slice>& slices)
{
    for (auto& s : slices) {
        if (s.opacity < 1.0f)
            return true;
        for (float f : s.feather)
            if (f > 0.0f)
                return true;
    }
    return false;
}

// Per-corner q for a convex quad (ul ur lr ll), from the diagonal intersection — the same formula as
// accelerator/ogl/util/transforms.cpp fill_texture_q_for_quad. Falls back to q = 1.
inline void slice_corner_q(const double* p, double q[4])
{
    q[0] = q[1] = q[2] = q[3] = 1.0;

    double s1_x  = p[4] - p[0]; // ul -> lr
    double s1_y  = p[5] - p[1];
    double s2_x  = p[6] - p[2]; // ur -> ll
    double s2_y  = p[7] - p[3];
    double denom = -s2_x * s1_y + s1_x * s2_y;
    if (denom == 0.0)
        return;

    double s = (-s1_y * (p[0] - p[2]) + s1_x * (p[1] - p[3])) / denom;
    double t = (s2_x * (p[1] - p[3]) - s2_y * (p[0] - p[2])) / denom;
    if (!(s >= 0 && s <= 1 && t >= 0 && t <= 1))
        return;

    double ix = p[0] + t * s1_x;
    double iy = p[1] + t * s1_y;

    double d_ul = std::hypot(p[0] - ix, p[1] - iy);
    double d_ur = std::hypot(p[2] - ix, p[3] - iy);
    double d_lr = std::hypot(p[4] - ix, p[5] - iy);
    double d_ll = std::hypot(p[6] - ix, p[7] - iy);
    if (d_ul <= 0.0 || d_ur <= 0.0 || d_lr <= 0.0 || d_ll <= 0.0)
        return;

    q[0] = (d_ul + d_lr) / d_lr;
    q[1] = (d_ur + d_ll) / d_ll;
    q[2] = (d_lr + d_ul) / d_ul;
    q[3] = (d_ll + d_ur) / d_ur;
}

// 6 vertices (two triangles: ul ur lr, ul lr ll) per slice, in slice order (= z order).
inline std::vector<slice_vertex> build_slice_vertices(const std::vector<slice>& slices,
                                                      double                    canvas_w,
                                                      double                    canvas_h,
                                                      double                    raster_w,
                                                      double                    raster_h)
{
    std::vector<slice_vertex> out;
    out.reserve(slices.size() * 6);

    for (auto& s : slices) {
        double q[4];
        slice_corner_q(s.dst, q);

        const double u0 = s.src[0] / raster_w;
        const double v0 = s.src[1] / raster_h;
        const double u1 = (s.src[0] + s.src[2]) / raster_w;
        const double v1 = (s.src[1] + s.src[3]) / raster_h;

        const double uv[4][2] = {{u0, v0}, {u1, v0}, {u1, v1}, {u0, v1}};
        const double hu       = 0.5 / raster_w;
        const double hv       = 0.5 / raster_h;
        const float  clamp_uv[4] = {static_cast<float>(std::min(u0 + hu, (u0 + u1) * 0.5)),
                                    static_cast<float>(std::min(v0 + hv, (v0 + v1) * 0.5)),
                                    static_cast<float>(std::max(u1 - hu, (u0 + u1) * 0.5)),
                                    static_cast<float>(std::max(v1 - hv, (v0 + v1) * 0.5))};
        const double lc[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};

        auto len = [&](int a, int b) { return std::hypot(s.dst[b * 2] - s.dst[a * 2], s.dst[b * 2 + 1] - s.dst[a * 2 + 1]); };
        const double w = std::max(1e-6, 0.5 * (len(0, 1) + len(3, 2)));
        const double h = std::max(1e-6, 0.5 * (len(0, 3) + len(1, 2)));
        const float  fl = static_cast<float>(s.feather[0] / w);
        const float  ft = static_cast<float>(s.feather[1] / h);
        const float  fr = static_cast<float>(s.feather[2] / w);
        const float  fb = static_cast<float>(s.feather[3] / h);

        slice_vertex corner[4];
        for (int c = 0; c < 4; ++c) {
            auto& v      = corner[c];
            v.x          = static_cast<float>(s.dst[c * 2] / canvas_w * 2.0 - 1.0);
            v.y          = static_cast<float>(1.0 - s.dst[c * 2 + 1] / canvas_h * 2.0);
            v.u          = static_cast<float>(uv[c][0] * q[c]);
            v.v          = static_cast<float>(uv[c][1] * q[c]);
            v.r          = 0.0f;
            v.q          = static_cast<float>(q[c]);
            v.lu         = static_cast<float>(lc[c][0] * q[c]);
            v.lv         = static_cast<float>(lc[c][1] * q[c]);
            v.opacity    = s.opacity;
            v.feather[0] = fl;
            v.feather[1] = ft;
            v.feather[2] = fr;
            v.feather[3] = fb;
            for (int k = 0; k < 4; ++k)
                v.src_clamp[k] = clamp_uv[k];
        }

        for (int c : {0, 1, 2, 0, 2, 3})
            out.push_back(corner[c]);
    }
    return out;
}

}} // namespace caspar::screen
