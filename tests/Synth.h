// Deterministic synthetic street scene: a moving plate with characters, a passing occluder,
// an exit through the left frame edge and a return, plus a static distractor sign.
#pragma once
#include <cmath>
#include "core/Image.h"

namespace synth {

struct Truth {
  pm::Quad q;          // plate quad in pixels
  bool occluded = false;    // fully covered
  bool bush = false;        // covered by the static bush (car body still visible)
  bool partial = false;     // partially covered
  bool outOfFrame = false;  // centre outside frame
};

constexpr int W = 960, H = 540, FRAMES = 190;
constexpr double PW = 120, PH = 32;

inline double plateX(int f) {
  auto base = [](double t) { return 480 + 150 * std::sin(0.06 * t); };
  if (f <= 85) return base(f);
  if (f <= 105) return base(85) - (f - 85) * 30;
  if (f <= 125) return base(85) - 600 + (f - 105) * 30;
  return base(85 + (f - 125));
}
inline double plateY(int f) { return 270 + 40 * std::sin(0.03 * f); }
inline double plateScale(int f) { return 1.0 + 0.12 * std::sin(0.04 * f); }
inline double plateAngle(int f) { return 0.08 * std::sin(0.05 * f); }

inline uint32_t hash2(int x, int y) {
  uint32_t h = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u;
  h = (h ^ (h >> 13)) * 1274126177u;
  return h ^ (h >> 16);
}
inline float noise(int x, int y) { return (hash2(x, y) & 1023) / 1023.0f; }

inline void render(int f, pm::Gray& g, Truth& t) {
  g = pm::Gray(W, H);
  double cx = plateX(f), cy = plateY(f), s = plateScale(f), ang = plateAngle(f);
  double hw = PW * s / 2, hh = PH * s / 2;
  pm::Pt u{std::cos(ang), std::sin(ang)}, v{-std::sin(ang), std::cos(ang)};
  pm::Pt c{cx, cy};
  t.q[0] = c - u * hw - v * hh; t.q[1] = c + u * hw - v * hh; t.q[2] = c + u * hw + v * hh; t.q[3] = c - u * hw + v * hh;
  t.outOfFrame = cx < 0 || cx >= W;
  // occluder: a pole/pedestrian sweeping across the plate between frames 40 and 60
  bool occ = f >= 38 && f <= 62;
  double ox = cx + (f - 50) * 12, ow = 180, oh = 200;
  t.occluded = occ && std::fabs(ox - cx) < (ow / 2 - hw);
  t.partial = occ && !t.occluded && std::fabs(ox - cx) < (ow / 2 + hw);
  // static "bush": a textured band that hides the plate (and only the plate's height) on the right
  const double bx1 = 540, bx2 = 760, by1 = 205, by2 = 270;
  {
    pm::Rect pb = pm::quadBounds(t.q);
    bool coversY = pb.y1 > by1 - 4 && pb.y2 < by2 + 4;
    bool insideX = pb.x1 > bx1 && pb.x2 < bx2;
    bool touchX = pb.x2 > bx1 && pb.x1 < bx2;
    t.bush = f >= 140 && insideX && coversY;
    if (f >= 140 && touchX && !t.bush) t.partial = true;
    if (t.bush) t.occluded = true;
  }

  for (int y = 0; y < H; ++y)
    for (int x = 0; x < W; ++x) {
      float bg = 0.45f + 0.15f * std::sin(x / 70.0) * std::cos(y / 90.0) + 0.06f * noise(x, y);
      // car body
      double dx = x - cx, dy = y - cy;
      double pu = dx * u.x + dy * u.y, pv = dx * v.x + dy * v.y;
      if (std::fabs(pu) < hw * 2.2 && std::fabs(pv) < hh * 3.5) bg = 0.22f + 0.05f * noise(x + 7, y + 3);
      // plate
      if (std::fabs(pu) < hw && std::fabs(pv) < hh) {
        double un = (pu + hw) / (2 * hw), vn = (pv + hh) / (2 * hh);
        float val = 0.92f;
        if (un < 0.03 || un > 0.97 || vn < 0.06 || vn > 0.94) val = 0.3f;  // frame
        else {
          for (int k = 0; k < 7; ++k) {
            double u0 = 0.08 + 0.125 * k;
            if (un >= u0 && un < u0 + 0.045 && vn > 0.22 && vn < 0.78) val = 0.08f;
            if ((k % 2 == 0) && un >= u0 && un < u0 + 0.09 && ((vn > 0.22 && vn < 0.30) || (vn > 0.70 && vn < 0.78))) val = 0.08f;
            if ((k % 3 == 1) && un >= u0 + 0.05 && un < u0 + 0.09 && vn > 0.22 && vn < 0.78) val = 0.08f;
          }
        }
        bg = val;
      }
      // distractor sign (static): white box with bars, wrong aspect ratio
      if (x > 700 && x < 780 && y > 80 && y < 160) {
        bg = 0.9f;
        if (((x - 700) / 10) % 2 == 0 && y > 95 && y < 145) bg = 0.1f;
      }
      // occluder
      if (occ && std::fabs(x - ox) < ow / 2 && std::fabs(y - cy) < oh / 2) bg = 0.5f + 0.08f * noise(x * 3, y * 3);
      // static bush
      if (f >= 140 && x >= bx1 && x < bx2 && y >= by1 && y < by2) bg = 0.35f + 0.25f * noise(x * 5 + 11, y * 5 + 7);
      g.at(x, y) = bg;
    }
}

}  // namespace synth
