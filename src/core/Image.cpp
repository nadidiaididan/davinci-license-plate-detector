#include "Image.h"

namespace pm {

template <typename T>
static Gray grayFromRGBA(const T* base, int rowBytes, int w, int h, int decim, float scale) {
  decim = std::max(1, decim);
  int ow = w / decim, oh = h / decim;
  Gray g(ow, oh);
  const float inv = 1.0f / (decim * decim);
  for (int y = 0; y < oh; ++y) {
    for (int x = 0; x < ow; ++x) {
      float acc = 0;
      for (int dy = 0; dy < decim; ++dy) {
        const T* row = (const T*)((const char*)base + (size_t)(y * decim + dy) * rowBytes);
        for (int dx = 0; dx < decim; ++dx) {
          const T* px = row + (size_t)(x * decim + dx) * 4;
          acc += 0.299f * (float)px[0] + 0.587f * (float)px[1] + 0.114f * (float)px[2];
        }
      }
      g.at(x, y) = acc * inv * scale;
    }
  }
  return g;
}

Gray grayFromRGBAf(const float* base, int rowBytes, int w, int h, int decim) {
  return grayFromRGBA<float>(base, rowBytes, w, h, decim, 1.0f);
}
Gray grayFromRGBA8(const uint8_t* base, int rowBytes, int w, int h, int decim) {
  return grayFromRGBA<uint8_t>(base, rowBytes, w, h, decim, 1.0f / 255.0f);
}
Gray grayFromRGBA16(const uint16_t* base, int rowBytes, int w, int h, int decim) {
  return grayFromRGBA<uint16_t>(base, rowBytes, w, h, decim, 1.0f / 65535.0f);
}

Gray downsample2(const Gray& g) {
  static const float k[5] = {1 / 16.f, 4 / 16.f, 6 / 16.f, 4 / 16.f, 1 / 16.f};
  Gray tmp(g.w, g.h);
  for (int y = 0; y < g.h; ++y)
    for (int x = 0; x < g.w; ++x) {
      float s = 0;
      for (int i = -2; i <= 2; ++i) s += k[i + 2] * g.atClamped(x + i, y);
      tmp.at(x, y) = s;
    }
  int ow = std::max(1, g.w / 2), oh = std::max(1, g.h / 2);
  Gray out(ow, oh);
  for (int y = 0; y < oh; ++y)
    for (int x = 0; x < ow; ++x) {
      float s = 0;
      for (int i = -2; i <= 2; ++i) s += k[i + 2] * tmp.atClamped(2 * x, 2 * y + i);
      out.at(x, y) = s;
    }
  return out;
}

Gray boxBlur3(const Gray& g) {
  Gray out(g.w, g.h);
  for (int y = 0; y < g.h; ++y)
    for (int x = 0; x < g.w; ++x) {
      float s = 0;
      for (int dy = -1; dy <= 1; ++dy)
        for (int dx = -1; dx <= 1; ++dx) s += g.atClamped(x + dx, y + dy);
      out.at(x, y) = s / 9.f;
    }
  return out;
}

void gradients(const Gray& g, Gray& gx, Gray& gy) {
  gx = Gray(g.w, g.h);
  gy = Gray(g.w, g.h);
  for (int y = 0; y < g.h; ++y)
    for (int x = 0; x < g.w; ++x) {
      gx.at(x, y) = 0.5f * (g.atClamped(x + 1, y) - g.atClamped(x - 1, y));
      gy.at(x, y) = 0.5f * (g.atClamped(x, y + 1) - g.atClamped(x, y - 1));
    }
}

Gray cropGray(const Gray& g, int x1, int y1, int x2, int y2) {
  x1 = std::clamp(x1, 0, g.w); x2 = std::clamp(x2, 0, g.w);
  y1 = std::clamp(y1, 0, g.h); y2 = std::clamp(y2, 0, g.h);
  Gray out(std::max(0, x2 - x1), std::max(0, y2 - y1));
  for (int y = 0; y < out.h; ++y)
    for (int x = 0; x < out.w; ++x) out.at(x, y) = g.at(x1 + x, y1 + y);
  return out;
}

}  // namespace pm
