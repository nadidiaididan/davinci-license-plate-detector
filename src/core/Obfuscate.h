// Destructive obfuscation operators applied through a matte.
#pragma once
#include <vector>
#include "Mask.h"
#include "Rng.h"

namespace pm {

struct ObfParams {
  enum Mode { None = 0, Blur = 1, Pixelize = 2, Randomize = 3 };
  int mode = Pixelize;
  double blurRadius = 24;     // pixels
  int blockSize = 24;         // pixels
  int randomMode = 0;         // 0 shuffle within block, 1 noise around block mean, 2 random flat blocks
  bool destructive = true;    // force quantise + dither + noise + jittered grid (non-invertible)
  int quantLevels = 12;
  double noiseAmp = 0.04;
  bool writeAlpha = false;    // write matte into the alpha channel
};

// Interleaved float RGBA view, pixel (x,y) at row y. Coordinates are 0-based inside the view.
struct ImageView {
  float* data = nullptr;
  int rowBytes = 0;
  int w = 0, h = 0;
  float* px(int x, int y) const { return (float*)((char*)data + (size_t)y * rowBytes) + (size_t)x * 4; }
};

// dst must already contain a copy of src. alpha is W*H (W=src.w, H=src.h). region limits the work.
void applyObfuscation(const ImageView& src, const ImageView& dst, const RectI& region, const std::vector<float>& alpha,
                      const ObfParams& p, Rng& rng);

// Write the matte itself (white on black) into dst over `region`.
void writeMatte(const ImageView& dst, const RectI& region, const std::vector<float>& alpha, int W, bool alphaToo);

}  // namespace pm
