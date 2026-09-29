// Single-channel float images and the mapping between canonical (host) coordinates and image pixels.
#pragma once
#include <vector>
#include <cmath>
#include <algorithm>
#include <cstdint>
#include "Geometry.h"

namespace pm {

struct Gray {
  int w = 0, h = 0;
  std::vector<float> p;
  Gray() = default;
  Gray(int w_, int h_) : w(w_), h(h_), p((size_t)w_ * h_, 0.0f) {}
  bool empty() const { return w <= 0 || h <= 0; }
  float& at(int x, int y) { return p[(size_t)y * w + x]; }
  float at(int x, int y) const { return p[(size_t)y * w + x]; }
  float atClamped(int x, int y) const {
    x = std::clamp(x, 0, w - 1); y = std::clamp(y, 0, h - 1);
    return p[(size_t)y * w + x];
  }
  float bilinear(double x, double y) const {
    if (w == 0 || h == 0) return 0.f;
    x = std::clamp(x, 0.0, (double)w - 1.0001);
    y = std::clamp(y, 0.0, (double)h - 1.0001);
    int x0 = (int)x, y0 = (int)y;
    double fx = x - x0, fy = y - y0;
    int x1 = std::min(x0 + 1, w - 1), y1 = std::min(y0 + 1, h - 1);
    double v0 = at(x0, y0) * (1 - fx) + at(x1, y0) * fx;
    double v1 = at(x0, y1) * (1 - fx) + at(x1, y1) * fx;
    return (float)(v0 * (1 - fy) + v1 * fy);
  }
  bool inside(double x, double y, double margin = 0) const {
    return x >= margin && y >= margin && x < w - margin && y < h - margin;
  }
};

// A gray frame plus the mapping canonical -> gray pixel: gx = cx * sx + ox ; gy = cy * sy + oy.
struct GrayFrame {
  Gray g;
  double sx = 1, sy = 1, ox = 0, oy = 0;
  int frame = 0;
  Pt toGray(const Pt& c) const { return {c.x * sx + ox, c.y * sy + oy}; }
  Pt toCanon(const Pt& p) const { return {(p.x - ox) / sx, (p.y - oy) / sy}; }
  Quad toGray(const Quad& q) const { return {toGray(q[0]), toGray(q[1]), toGray(q[2]), toGray(q[3])}; }
  Quad toCanon(const Quad& q) const { return {toCanon(q[0]), toCanon(q[1]), toCanon(q[2]), toCanon(q[3])}; }
  // One canonical unit expressed in gray pixels (uses x scale).
  double scale() const { return sx; }
};

// Build a gray image from interleaved float RGBA rows. `decim` takes every decim-th pixel (box averaged).
Gray grayFromRGBAf(const float* base, int rowBytes, int w, int h, int decim);
// Same from 8-bit and 16-bit RGBA.
Gray grayFromRGBA8(const uint8_t* base, int rowBytes, int w, int h, int decim);
Gray grayFromRGBA16(const uint16_t* base, int rowBytes, int w, int h, int decim);

Gray downsample2(const Gray& g);          // 5-tap Gaussian then decimate by 2
Gray boxBlur3(const Gray& g);             // 3x3 box
void gradients(const Gray& g, Gray& gx, Gray& gy);  // central differences
Gray cropGray(const Gray& g, int x1, int y1, int x2, int y2);

}  // namespace pm
