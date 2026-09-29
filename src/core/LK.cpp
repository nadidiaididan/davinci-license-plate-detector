#include "LK.h"
#include <cmath>

namespace pm {

Pyramid buildPyramid(const Gray& g, int levels) {
  Pyramid p;
  p.levels.push_back(g);
  for (int i = 1; i < levels; ++i) {
    const Gray& prev = p.levels.back();
    if (prev.w < 16 || prev.h < 16) break;
    p.levels.push_back(downsample2(prev));
  }
  return p;
}

void lkTrack(const Pyramid& prev, const Pyramid& next, const std::vector<Pt>& pts, std::vector<LKResult>& out,
             int halfWin, int iters, const std::vector<Pt>* init) {
  out.assign(pts.size(), LKResult{});
  const int L = (int)std::min(prev.levels.size(), next.levels.size());
  if (L == 0) return;
  const int win = 2 * halfWin + 1;
  std::vector<float> Ix(win * win), Iy(win * win), Ip(win * win);

  for (size_t i = 0; i < pts.size(); ++i) {
    double scale = std::pow(2.0, L - 1);
    Pt p0 = pts[i] * (1.0 / scale);  // position in coarsest level
    Pt d{0, 0};                       // displacement estimate at current level
    if (init && i < init->size()) d = ((*init)[i] - pts[i]) * (1.0 / scale);
    bool ok = true;
    float lastErr = 1;
    for (int lv = L - 1; lv >= 0; --lv) {
      const Gray& A = prev.levels[lv];
      const Gray& B = next.levels[lv];
      if (lv != L - 1) { p0 = p0 * 2.0; d = d * 2.0; }
      if (!A.inside(p0.x, p0.y, halfWin + 1)) { ok = false; break; }
      // gradients & template in prev at p0
      double gxx = 0, gxy = 0, gyy = 0;
      int k = 0;
      for (int wy = -halfWin; wy <= halfWin; ++wy)
        for (int wx = -halfWin; wx <= halfWin; ++wx, ++k) {
          double x = p0.x + wx, y = p0.y + wy;
          float ix = 0.5f * (A.bilinear(x + 1, y) - A.bilinear(x - 1, y));
          float iy = 0.5f * (A.bilinear(x, y + 1) - A.bilinear(x, y - 1));
          Ix[k] = ix; Iy[k] = iy; Ip[k] = A.bilinear(x, y);
          gxx += ix * ix; gxy += ix * iy; gyy += iy * iy;
        }
      double det = gxx * gyy - gxy * gxy;
      double trace = gxx + gyy;
      double minEig = 0.5 * (trace - std::sqrt(std::max(0.0, trace * trace - 4 * det)));
      if (det < 1e-10 || minEig / (win * win) < 1e-5) { ok = false; break; }
      double inv00 = gyy / det, inv01 = -gxy / det, inv11 = gxx / det;
      for (int it = 0; it < iters; ++it) {
        Pt q = p0 + d;
        if (!B.inside(q.x, q.y, halfWin + 1)) { ok = false; break; }
        double bx = 0, by = 0, err = 0;
        k = 0;
        for (int wy = -halfWin; wy <= halfWin; ++wy)
          for (int wx = -halfWin; wx <= halfWin; ++wx, ++k) {
            float it_ = B.bilinear(q.x + wx, q.y + wy) - Ip[k];
            bx += it_ * Ix[k]; by += it_ * Iy[k];
            err += std::fabs(it_);
          }
        lastErr = (float)(err / (win * win));
        double dx = -(inv00 * bx + inv01 * by), dy = -(inv01 * bx + inv11 * by);
        d.x += dx; d.y += dy;
        if (dx * dx + dy * dy < 1e-4) break;
      }
      if (!ok) break;
    }
    out[i].ok = ok;
    out[i].pt = p0 + d;
    out[i].err = lastErr;
  }
}

}  // namespace pm
