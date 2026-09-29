// Quad matte rasterisation with analytic expand/contract and feather (signed-distance based).
#pragma once
#include <vector>
#include "Geometry.h"

namespace pm {

struct RectI {
  int x1 = 0, y1 = 0, x2 = 0, y2 = 0;
  int w() const { return x2 - x1; }
  int h() const { return y2 - y1; }
  bool empty() const { return x2 <= x1 || y2 <= y1; }
};
inline RectI intersectRect(const RectI& a, const RectI& b) {
  return {std::max(a.x1, b.x1), std::max(a.y1, b.y1), std::min(a.x2, b.x2), std::min(a.y2, b.y2)};
}
inline RectI unionRect(const RectI& a, const RectI& b) {
  if (a.empty()) return b;
  if (b.empty()) return a;
  return {std::min(a.x1, b.x1), std::min(a.y1, b.y1), std::max(a.x2, b.x2), std::max(a.y2, b.y2)};
}

// Max-composite the quad's alpha into `alpha` (W*H, pixel (x,y) at y*W+x). Quad in pixel coordinates.
// expand may be negative (contract). feather is the transition width in pixels. Returns the touched rect.
RectI unionQuadAlpha(std::vector<float>& alpha, int W, int H, const Quad& qPix, double expand, double feather);

}  // namespace pm
