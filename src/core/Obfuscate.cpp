#include "Obfuscate.h"
#include <cmath>
#include <algorithm>
#include <thread>
#include <functional>

namespace pm {

namespace {

void parallelRows(int y1, int y2, const std::function<void(int, int)>& fn) {
  int n = y2 - y1;
  if (n <= 0) return;
  int threads = (int)std::min<unsigned>(std::max(1u, std::thread::hardware_concurrency()), 16u);
  if (n < 64 || threads <= 1) { fn(y1, y2); return; }
  std::vector<std::thread> pool;
  int chunk = (n + threads - 1) / threads;
  for (int t = 0; t < threads; ++t) {
    int a = y1 + t * chunk, b = std::min(y2, a + chunk);
    if (a >= b) break;
    pool.emplace_back(fn, a, b);
  }
  for (auto& th : pool) th.join();
}

struct Buf {  // RGBA float buffer over a rect of the image
  RectI r;
  std::vector<float> v;
  Buf(const RectI& rr) : r(rr), v((size_t)std::max(0, rr.w()) * std::max(0, rr.h()) * 4, 0.f) {}
  float* px(int x, int y) { return &v[((size_t)(y - r.y1) * r.w() + (x - r.x1)) * 4]; }
  const float* px(int x, int y) const { return &v[((size_t)(y - r.y1) * r.w() + (x - r.x1)) * 4]; }
};

void copyFrom(const ImageView& src, Buf& b) {
  for (int y = b.r.y1; y < b.r.y2; ++y)
    for (int x = b.r.x1; x < b.r.x2; ++x) {
      const float* s = src.px(x, y);
      float* d = b.px(x, y);
      d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = s[3];
    }
}

// Three-pass box blur approximating a Gaussian of sigma. Reads clamp to the buffer edge.
void boxBlur(Buf& b, double sigma) {
  if (sigma < 0.3 || b.r.empty()) return;
  int wbox = (int)std::sqrt(12.0 * sigma * sigma / 3.0 + 1.0);
  if (wbox % 2 == 0) ++wbox;
  int rad = (wbox - 1) / 2;
  if (rad < 1) return;
  int W = b.r.w(), H = b.r.h();
  std::vector<float> tmp(b.v.size());
  for (int pass = 0; pass < 3; ++pass) {
    // horizontal
    parallelRows(0, H, [&](int ya, int yb) {
      for (int y = ya; y < yb; ++y) {
        const float* row = &b.v[(size_t)y * W * 4];
        float* out = &tmp[(size_t)y * W * 4];
        for (int c = 0; c < 3; ++c) {
          double acc = 0;
          for (int k = -rad; k <= rad; ++k) acc += row[(size_t)std::clamp(k, 0, W - 1) * 4 + c];
          for (int x = 0; x < W; ++x) {
            out[(size_t)x * 4 + c] = (float)(acc / wbox);
            int xo = std::clamp(x - rad, 0, W - 1), xi = std::clamp(x + rad + 1, 0, W - 1);
            acc += row[(size_t)xi * 4 + c] - row[(size_t)xo * 4 + c];
          }
        }
        for (int x = 0; x < W; ++x) out[(size_t)x * 4 + 3] = row[(size_t)x * 4 + 3];
      }
    });
    // vertical (columns split across threads by x)
    parallelRows(0, W, [&](int xa, int xb) {
      for (int x = xa; x < xb; ++x)
        for (int c = 0; c < 3; ++c) {
          double acc = 0;
          for (int k = -rad; k <= rad; ++k) acc += tmp[((size_t)std::clamp(k, 0, H - 1) * W + x) * 4 + c];
          for (int y = 0; y < H; ++y) {
            b.v[((size_t)y * W + x) * 4 + c] = (float)(acc / wbox);
            int yo = std::clamp(y - rad, 0, H - 1), yi = std::clamp(y + rad + 1, 0, H - 1);
            acc += tmp[((size_t)yi * W + x) * 4 + c] - tmp[((size_t)yo * W + x) * 4 + c];
          }
        }
    });
  }
}

// Mosaic with grid phase (ox,oy). Block means are computed from the source view so that blocks at the
// region border still average real pixels.
void pixelize(const ImageView& src, Buf& b, int B, int ox, int oy) {
  B = std::max(2, B);
  int bx0 = (int)std::floor((double)(b.r.x1 - ox) / B), bx1 = (int)std::floor((double)(b.r.x2 - 1 - ox) / B);
  int by0 = (int)std::floor((double)(b.r.y1 - oy) / B), by1 = (int)std::floor((double)(b.r.y2 - 1 - oy) / B);
  parallelRows(by0, by1 + 1, [&](int ba, int bb) {
    for (int by = ba; by < bb; ++by)
      for (int bx = bx0; bx <= bx1; ++bx) {
        int x1 = std::max(0, bx * B + ox), x2 = std::min(src.w, bx * B + ox + B);
        int y1 = std::max(0, by * B + oy), y2 = std::min(src.h, by * B + oy + B);
        if (x2 <= x1 || y2 <= y1) continue;
        double acc[3] = {0, 0, 0};
        int n = 0;
        for (int y = y1; y < y2; ++y)
          for (int x = x1; x < x2; ++x) {
            const float* s = src.px(x, y);
            acc[0] += s[0]; acc[1] += s[1]; acc[2] += s[2];
            ++n;
          }
        float m[3] = {(float)(acc[0] / n), (float)(acc[1] / n), (float)(acc[2] / n)};
        int wx1 = std::max(x1, b.r.x1), wx2 = std::min(x2, b.r.x2);
        int wy1 = std::max(y1, b.r.y1), wy2 = std::min(y2, b.r.y2);
        for (int y = wy1; y < wy2; ++y)
          for (int x = wx1; x < wx2; ++x) {
            float* d = b.px(x, y);
            d[0] = m[0]; d[1] = m[1]; d[2] = m[2];
          }
      }
  });
}

void randomize(Buf& b, int B, int mode, Rng& rng) {
  B = std::max(2, B);
  int W = b.r.w(), H = b.r.h();
  for (int by = 0; by < H; by += B)
    for (int bx = 0; bx < W; bx += B) {
      int x2 = std::min(W, bx + B), y2 = std::min(H, by + B);
      int n = (x2 - bx) * (y2 - by);
      if (n <= 0) continue;
      if (mode == 0) {  // Fisher-Yates shuffle of pixel values inside the block
        for (int i = n - 1; i > 0; --i) {
          int j = rng.below(i + 1);
          int xi = bx + i % (x2 - bx), yi = by + i / (x2 - bx);
          int xj = bx + j % (x2 - bx), yj = by + j / (x2 - bx);
          float* a = &b.v[((size_t)yi * W + xi) * 4];
          float* c = &b.v[((size_t)yj * W + xj) * 4];
          for (int k = 0; k < 3; ++k) std::swap(a[k], c[k]);
        }
      } else {
        double m[3] = {0, 0, 0};
        for (int y = by; y < y2; ++y)
          for (int x = bx; x < x2; ++x) {
            const float* s = &b.v[((size_t)y * W + x) * 4];
            m[0] += s[0]; m[1] += s[1]; m[2] += s[2];
          }
        for (double& v : m) v /= n;
        if (mode == 1) {  // noise around the block mean
          for (int y = by; y < y2; ++y)
            for (int x = bx; x < x2; ++x) {
              float* d = &b.v[((size_t)y * W + x) * 4];
              double u = rng.uniform() - 0.5;
              for (int k = 0; k < 3; ++k) d[k] = (float)std::clamp(m[k] + u * 0.6, 0.0, 1.0);
            }
        } else {  // random flat colour near the block mean luminance
          double lum = 0.299 * m[0] + 0.587 * m[1] + 0.114 * m[2];
          float col[3];
          for (int k = 0; k < 3; ++k) col[k] = (float)std::clamp(lum * (0.5 + rng.uniform()), 0.0, 1.0);
          for (int y = by; y < y2; ++y)
            for (int x = bx; x < x2; ++x) {
              float* d = &b.v[((size_t)y * W + x) * 4];
              d[0] = col[0]; d[1] = col[1]; d[2] = col[2];
            }
        }
      }
    }
}

// Quantise with random dither, then add uniform noise. Destroys the low bits that deconvolution or
// multi-frame averaging would need.
void destroy(Buf& b, int levels, double noiseAmp, Rng& rng) {
  levels = std::max(2, levels);
  for (size_t i = 0; i < b.v.size(); i += 4)
    for (int k = 0; k < 3; ++k) {
      double v = std::clamp((double)b.v[i + k], 0.0, 1.0);
      double q = std::floor(v * (levels - 1) + rng.uniform()) / (levels - 1);
      q += (rng.uniform() * 2 - 1) * noiseAmp;
      b.v[i + k] = (float)std::clamp(q, 0.0, 1.0);
    }
}

}  // namespace

void applyObfuscation(const ImageView& src, const ImageView& dst, const RectI& region, const std::vector<float>& alpha,
                      const ObfParams& p, Rng& rng) {
  RectI reg = intersectRect(region, {0, 0, src.w, src.h});
  if (reg.empty()) return;
  const int W = src.w;
  if (p.mode == ObfParams::None) {
    if (p.writeAlpha)
      for (int y = reg.y1; y < reg.y2; ++y)
        for (int x = reg.x1; x < reg.x2; ++x) dst.px(x, y)[3] = alpha[(size_t)y * W + x];
    return;
  }
  int B = std::max(2, p.blockSize);
  if (p.mode == ObfParams::Blur) {
    double sigma = std::max(0.5, p.blurRadius * 0.5);
    int pad = (int)std::ceil(3 * sigma) + 2;
    RectI padded = intersectRect({reg.x1 - pad, reg.y1 - pad, reg.x2 + pad, reg.y2 + pad}, {0, 0, src.w, src.h});
    Buf big(padded);
    copyFrom(src, big);
    boxBlur(big, sigma);
    Buf b(reg);
    for (int y = reg.y1; y < reg.y2; ++y)
      for (int x = reg.x1; x < reg.x2; ++x) {
        const float* s = big.px(x, y);
        float* d = b.px(x, y);
        d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = s[3];
      }
    if (p.destructive) {
      // A plain blur is a linear operator and can be partially inverted; break it with a jittered mosaic.
      ImageView bv{b.v.data(), b.r.w() * 4 * (int)sizeof(float), b.r.w(), b.r.h()};
      Buf b2(RectI{0, 0, b.r.w(), b.r.h()});
      int B2 = std::max(6, (int)(p.blurRadius / 3));
      pixelize(bv, b2, B2, rng.below(B2), rng.below(B2));
      b.v.swap(b2.v);
      destroy(b, p.quantLevels, p.noiseAmp, rng);
    }
    for (int y = reg.y1; y < reg.y2; ++y)
      for (int x = reg.x1; x < reg.x2; ++x) {
        float a = alpha[(size_t)y * W + x];
        if (a <= 0) { if (p.writeAlpha) dst.px(x, y)[3] = 0; continue; }
        const float* s = src.px(x, y);
        const float* o = b.px(x, y);
        float* d = dst.px(x, y);
        for (int k = 0; k < 3; ++k) d[k] = s[k] * (1 - a) + o[k] * a;
        if (p.writeAlpha) d[3] = a;
      }
    return;
  }
  Buf b(reg);
  if (p.mode == ObfParams::Pixelize) {
    int ox = p.destructive ? rng.below(B) : 0, oy = p.destructive ? rng.below(B) : 0;
    pixelize(src, b, B, ox, oy);
  } else {
    copyFrom(src, b);
    randomize(b, B, p.randomMode, rng);
  }
  if (p.destructive) destroy(b, p.quantLevels, p.noiseAmp, rng);
  for (int y = reg.y1; y < reg.y2; ++y)
    for (int x = reg.x1; x < reg.x2; ++x) {
      float a = alpha[(size_t)y * W + x];
      if (a <= 0) { if (p.writeAlpha) dst.px(x, y)[3] = 0; continue; }
      const float* s = src.px(x, y);
      const float* o = b.px(x, y);
      float* d = dst.px(x, y);
      for (int k = 0; k < 3; ++k) d[k] = s[k] * (1 - a) + o[k] * a;
      if (p.writeAlpha) d[3] = a;
    }
}

void writeMatte(const ImageView& dst, const RectI& region, const std::vector<float>& alpha, int W, bool alphaToo) {
  RectI reg = intersectRect(region, {0, 0, dst.w, dst.h});
  for (int y = reg.y1; y < reg.y2; ++y)
    for (int x = reg.x1; x < reg.x2; ++x) {
      float a = alpha[(size_t)y * W + x];
      float* d = dst.px(x, y);
      d[0] = d[1] = d[2] = a;
      d[3] = alphaToo ? a : 1.0f;
    }
}

}  // namespace pm
