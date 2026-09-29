#include "Mask.h"
#include <cmath>
#include <algorithm>

namespace pm {

RectI unionQuadAlpha(std::vector<float>& alpha, int W, int H, const Quad& q, double expand, double feather) {
  double f = std::max(feather, 1.0);
  Rect b = quadBounds(q);
  double m = std::max(0.0, expand) + f + 2;
  RectI r{(int)std::floor(b.x1 - m), (int)std::floor(b.y1 - m), (int)std::ceil(b.x2 + m) + 1, (int)std::ceil(b.y2 + m) + 1};
  r = intersectRect(r, {0, 0, W, H});
  if (r.empty()) return {0, 0, 0, 0};
  for (int y = r.y1; y < r.y2; ++y)
    for (int x = r.x1; x < r.x2; ++x) {
      double d = quadSignedDistance(q, {x + 0.5, y + 0.5});
      double a = std::clamp((expand - d) / f + 0.5, 0.0, 1.0);
      float& dst = alpha[(size_t)y * W + x];
      if (a > dst) dst = (float)a;
    }
  return r;
}

}  // namespace pm
