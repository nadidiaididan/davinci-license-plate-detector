#include "Detector.h"
#include <cmath>
#include <queue>
#include <algorithm>

namespace pm {

namespace {

struct Blob {
  std::vector<int> pix;  // indices into ROI
  int minx = 1 << 30, miny = 1 << 30, maxx = -1, maxy = -1;
};

double aspectScore(double aspect, const DetectParams& prm) {
  if (aspect < prm.aspectMin || aspect > prm.aspectMax) return 0;
  double best = 1e9;
  for (double pa : prm.preferAspect) best = std::min(best, std::fabs(std::log(aspect / pa)));
  return std::max(0.0, 1.0 - best * 1.2);  // 1 at preferred, decays with log ratio
}

// PCA-oriented rectangle around blob pixels (in ROI coords). Percentile-trimmed extents.
Quad orientedRect(const Blob& b, int roiW, double& outLong, double& outShort, Pt& outU, Pt& outV) {
  double mx = 0, my = 0;
  for (int idx : b.pix) { mx += idx % roiW; my += idx / roiW; }
  mx /= b.pix.size(); my /= b.pix.size();
  double cxx = 0, cxy = 0, cyy = 0;
  for (int idx : b.pix) {
    double dx = idx % roiW - mx, dy = idx / roiW - my;
    cxx += dx * dx; cxy += dx * dy; cyy += dy * dy;
  }
  // Principal axis: plates are much wider than tall; the major axis is the long side. Bias towards
  // horizontal when the covariance is near-isotropic (text blobs of one plate are spread horizontally).
  double theta = 0.5 * std::atan2(2 * cxy, cxx - cyy);
  Pt u{std::cos(theta), std::sin(theta)};
  if (u.x < 0) u = u * -1.0;
  Pt v{-u.y, u.x};
  if (v.y < 0) v = v * -1.0;
  std::vector<double> pu, pv;
  pu.reserve(b.pix.size()); pv.reserve(b.pix.size());
  for (int idx : b.pix) {
    Pt d{idx % roiW - mx, idx / roiW - my};
    pu.push_back(d.dot(u)); pv.push_back(d.dot(v));
  }
  std::sort(pu.begin(), pu.end()); std::sort(pv.begin(), pv.end());
  size_t n = pu.size();
  size_t lo = (size_t)(n * 0.01), hi = std::min(n - 1, (size_t)(n * 0.99));
  double u1 = pu[lo], u2 = pu[hi], v1 = pv[lo], v2 = pv[hi];
  outLong = u2 - u1 + 1; outShort = v2 - v1 + 1;
  outU = u; outV = v;
  Pt c{mx, my};
  Quad q;
  q[0] = c + u * u1 + v * v1;
  q[1] = c + u * u2 + v * v1;
  q[2] = c + u * u2 + v * v2;
  q[3] = c + u * u1 + v * v2;
  return q;
}

// Move one side of the quad outward along its normal by searching for the strongest bright->dark
// transition. side: 0 = c0c1 (low v), 1 = c1c2 (high u), 2 = c2c3 (high v), 3 = c3c0 (low u).
void snapSide(const Gray& g, Quad& q, int side, double minOff, double maxOff, double defaultOff) {
  int a = side, b = (side + 1) % 4;
  Pt edge = q[b] - q[a];
  double len = edge.norm();
  if (len < 2) { return; }
  Pt t = edge * (1.0 / len);
  Pt n{t.y, -t.x};  // outward normal for CCW-ordered quads; verify with centre
  Pt c = quadCenter(q);
  if ((q[a] - c).dot(n) < 0) n = n * -1.0;
  int steps = std::max(3, (int)maxOff);
  int samples = std::max(3, (int)(len / 2));
  std::vector<double> prof(steps + 1, 0.0);
  for (int s = 0; s <= steps; ++s) {
    double off = maxOff * s / steps;
    double acc = 0;
    for (int k = 0; k < samples; ++k) {
      Pt p = q[a] + t * (len * (k + 0.5) / samples) + n * off;
      acc += g.bilinear(p.x, p.y);
    }
    prof[s] = acc / samples;
  }
  // Strongest negative gradient beyond minOff (plate background is bright; surroundings darker).
  double bestDrop = 0; int bestS = -1;
  for (int s = 1; s < steps; ++s) {
    double off = maxOff * s / steps;
    if (off < minOff) continue;
    double drop = prof[s - 1] - prof[s + 1];
    if (drop > bestDrop) { bestDrop = drop; bestS = s; }
  }
  double off = defaultOff;
  if (bestS >= 0 && bestDrop > 0.12) off = maxOff * bestS / steps;
  q[a] = q[a] + n * off;
  q[b] = q[b] + n * off;
}

}  // namespace

std::vector<Detection> detectCandidates(const Gray& g, const Rect& roi, const DetectParams& prm, int maxN,
                                        const Pt* seed) {
  std::vector<Detection> result;
  int x1 = std::clamp((int)std::floor(roi.x1), 0, g.w), x2 = std::clamp((int)std::ceil(roi.x2), 0, g.w);
  int y1 = std::clamp((int)std::floor(roi.y1), 0, g.h), y2 = std::clamp((int)std::ceil(roi.y2), 0, g.h);
  int W = x2 - x1, H = y2 - y1;
  if (W < 12 || H < 8) return result;

  Gray crop = boxBlur3(cropGray(g, x1, y1, x2, y2));
  // vertical stroke energy
  std::vector<float> e((size_t)W * H, 0.f);
  double mean = 0, sq = 0;
  for (int y = 0; y < H; ++y)
    for (int x = 0; x < W; ++x) {
      float v = std::fabs(crop.atClamped(x + 1, y) - crop.atClamped(x - 1, y));
      e[(size_t)y * W + x] = v;
      mean += v; sq += v * v;
    }
  mean /= (double)W * H;
  double sd = std::sqrt(std::max(0.0, sq / ((double)W * H) - mean * mean));
  float thr = (float)std::max(0.05, mean + 1.5 * sd);
  std::vector<uint8_t> bin((size_t)W * H, 0);
  for (size_t i = 0; i < e.size(); ++i) bin[i] = e[i] > thr ? 1 : 0;

  // Horizontal closing: dilate then erode with a horizontal kernel (characters -> word blob).
  auto closeH = [&](int kw) {
    std::vector<uint8_t> tmp(bin.size(), 0);
    for (int y = 0; y < H; ++y)
      for (int x = 0; x < W; ++x) {
        uint8_t v = 0;
        for (int k = -kw; k <= kw && !v; ++k) {
          int xx = x + k;
          if (xx >= 0 && xx < W && bin[(size_t)y * W + xx]) v = 1;
        }
        tmp[(size_t)y * W + x] = v;
      }
    for (int y = 0; y < H; ++y)
      for (int x = 0; x < W; ++x) {
        uint8_t v = 1;
        for (int k = -kw; k <= kw && v; ++k) {
          int xx = x + k;
          if (xx < 0 || xx >= W || !tmp[(size_t)y * W + xx]) v = 0;
        }
        bin[(size_t)y * W + x] = v;
      }
  };
  auto closeV = [&](int kh) {
    std::vector<uint8_t> tmp(bin.size(), 0);
    for (int y = 0; y < H; ++y)
      for (int x = 0; x < W; ++x) {
        uint8_t v = 0;
        for (int k = -kh; k <= kh && !v; ++k) {
          int yy = y + k;
          if (yy >= 0 && yy < H && bin[(size_t)yy * W + x]) v = 1;
        }
        tmp[(size_t)y * W + x] = v;
      }
    for (int y = 0; y < H; ++y)
      for (int x = 0; x < W; ++x) {
        uint8_t v = 1;
        for (int k = -kh; k <= kh && v; ++k) {
          int yy = y + k;
          if (yy < 0 || yy >= H || !tmp[(size_t)yy * W + x]) v = 0;
        }
        bin[(size_t)y * W + x] = v;
      }
  };
  // Try two closing scales so that both small and large plates form a single blob.
  std::vector<int> scales = {std::max(2, W / 60), std::max(3, W / 25)};
  std::vector<Detection> all;
  for (int kw : scales) {
    std::vector<uint8_t> saved = bin;
    closeH(kw);
    closeV(std::max(1, kw / 4));
    // connected components
    std::vector<int> label((size_t)W * H, -1);
    std::vector<Blob> blobs;
    for (int y = 0; y < H; ++y)
      for (int x = 0; x < W; ++x) {
        size_t i0 = (size_t)y * W + x;
        if (!bin[i0] || label[i0] >= 0) continue;
        int id = (int)blobs.size();
        blobs.push_back(Blob{});
        Blob& b = blobs.back();
        std::vector<int> stack{(int)i0};
        label[i0] = id;
        while (!stack.empty()) {
          int i = stack.back(); stack.pop_back();
          int px = i % W, py = i / W;
          b.pix.push_back(i);
          b.minx = std::min(b.minx, px); b.maxx = std::max(b.maxx, px);
          b.miny = std::min(b.miny, py); b.maxy = std::max(b.maxy, py);
          const int nx[4] = {px - 1, px + 1, px, px}, ny[4] = {py, py, py - 1, py + 1};
          for (int k = 0; k < 4; ++k) {
            if (nx[k] < 0 || nx[k] >= W || ny[k] < 0 || ny[k] >= H) continue;
            size_t j = (size_t)ny[k] * W + nx[k];
            if (bin[j] && label[j] < 0) { label[j] = id; stack.push_back((int)j); }
          }
        }
      }
    for (const Blob& b : blobs) {
      int bw = b.maxx - b.minx + 1, bh = b.maxy - b.miny + 1;
      if (bh < prm.minHeightPx || bh > prm.maxHeightFrac * H || bw < 2 * prm.minHeightPx) continue;
      if ((int)b.pix.size() < 20) continue;
      double lng, shrt; Pt u, v;
      Quad q = orientedRect(b, W, lng, shrt, u, v);
      if (shrt < prm.minHeightPx) continue;
      double aspect = lng / shrt;
      double as = aspectScore(aspect, prm);
      if (as <= 0) continue;
      double fill = (double)b.pix.size() / (lng * shrt);
      double fillScore = fill < 0.15 ? fill / 0.15 : (fill > 0.9 ? 0.5 : 1.0);
      // edge density inside the text box (mean stroke energy)
      double dens = 0;
      for (int idx : b.pix) dens += e[idx];
      dens /= b.pix.size();
      double densScore = std::min(1.0, dens / (thr * 2.0));
      double score = as * (0.5 + 0.5 * fillScore) * (0.5 + 0.5 * densScore);
      if (seed) {
        Pt s{seed->x - x1, seed->y - y1};
        Pt c = quadCenter(q);
        Pt d = s - c;
        double du = std::fabs(d.dot(u)) / (lng * 0.5 + shrt), dv = std::fabs(d.dot(v)) / (shrt * 1.2);
        double dist = std::sqrt(du * du + dv * dv);
        if (dist > 1.6) continue;  // too far from the click
        score *= std::max(0.1, 1.0 - 0.5 * dist);
      }
      // text box -> plate box
      Pt c = quadCenter(q);
      double ex = lng * prm.expandX, ey = shrt * prm.expandY;
      Quad pq;
      pq[0] = q[0] - u * ex - v * ey; pq[1] = q[1] + u * ex - v * ey;
      pq[2] = q[2] + u * ex + v * ey; pq[3] = q[3] - u * ex + v * ey;
      if (prm.snapEdges) {
        Quad snapped = q;  // start from the text box and search outward
        snapSide(crop, snapped, 0, ey * 0.5, ey * 2.2, ey);
        snapSide(crop, snapped, 2, ey * 0.5, ey * 2.2, ey);
        snapSide(crop, snapped, 1, ex * 0.4, ex * 3.0, ex);
        snapSide(crop, snapped, 3, ex * 0.4, ex * 3.0, ex);
        pq = snapped;
      }
      (void)c;
      Detection d;
      for (int i = 0; i < 4; ++i) d.q[i] = {pq[i].x + x1, pq[i].y + y1};
      d.score = score;
      d.aspect = quadLongSide(d.q) / std::max(1e-6, quadShortSide(d.q));
      all.push_back(d);
    }
    bin = saved;
  }
  std::sort(all.begin(), all.end(), [](const Detection& a, const Detection& b) { return a.score > b.score; });
  // non-maximum suppression by IoU
  for (const Detection& d : all) {
    bool dup = false;
    for (const Detection& r : result)
      if (quadIoU(d.q, r.q) > 0.4) { dup = true; break; }
    if (!dup) result.push_back(d);
    if ((int)result.size() >= maxN) break;
  }
  return result;
}

bool detectAtSeed(const Gray& g, const Pt& seed, double radius, const DetectParams& prm, Detection& out) {
  Rect roi{seed.x - radius, seed.y - radius * 0.7, seed.x + radius, seed.y + radius * 0.7};
  std::vector<Detection> c = detectCandidates(g, roi, prm, 3, &seed);
  if (c.empty()) return false;
  out = c[0];
  return true;
}

}  // namespace pm
