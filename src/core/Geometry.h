// Geometry primitives: points, quads, affine/similarity fits, convex polygon IoU.
#pragma once
#include <array>
#include <cmath>
#include <algorithm>
#include <vector>

namespace pm {

struct Pt {
  double x = 0, y = 0;
  Pt() = default;
  Pt(double x_, double y_) : x(x_), y(y_) {}
  Pt operator+(const Pt& o) const { return {x + o.x, y + o.y}; }
  Pt operator-(const Pt& o) const { return {x - o.x, y - o.y}; }
  Pt operator*(double s) const { return {x * s, y * s}; }
  Pt& operator+=(const Pt& o) { x += o.x; y += o.y; return *this; }
  double dot(const Pt& o) const { return x * o.x + y * o.y; }
  double cross(const Pt& o) const { return x * o.y - y * o.x; }
  double norm() const { return std::sqrt(x * x + y * y); }
};

// Corner order: c0 -> c1 runs along the plate's long axis, c0 -> c3 along the short axis.
using Quad = std::array<Pt, 4>;

struct Rect {
  double x1 = 0, y1 = 0, x2 = 0, y2 = 0;
  double w() const { return x2 - x1; }
  double h() const { return y2 - y1; }
  bool contains(const Pt& p) const { return p.x >= x1 && p.x < x2 && p.y >= y1 && p.y < y2; }
};

inline Pt quadCenter(const Quad& q) { return (q[0] + q[1] + q[2] + q[3]) * 0.25; }
inline double quadLongSide(const Quad& q) { return 0.5 * ((q[1] - q[0]).norm() + (q[2] - q[3]).norm()); }
inline double quadShortSide(const Quad& q) { return 0.5 * ((q[3] - q[0]).norm() + (q[2] - q[1]).norm()); }
inline double quadArea(const Quad& q) {
  double a = 0;
  for (int i = 0; i < 4; ++i) a += q[i].cross(q[(i + 1) % 4]);
  return std::fabs(a) * 0.5;
}
inline Rect quadBounds(const Quad& q) {
  Rect r{q[0].x, q[0].y, q[0].x, q[0].y};
  for (const Pt& p : q) {
    r.x1 = std::min(r.x1, p.x); r.y1 = std::min(r.y1, p.y);
    r.x2 = std::max(r.x2, p.x); r.y2 = std::max(r.y2, p.y);
  }
  return r;
}
inline Quad quadTranslate(const Quad& q, const Pt& d) { return {q[0] + d, q[1] + d, q[2] + d, q[3] + d}; }
inline Quad quadLerp(const Quad& a, const Quad& b, double t) {
  Quad r;
  for (int i = 0; i < 4; ++i) r[i] = a[i] * (1.0 - t) + b[i] * t;
  return r;
}
// Grow every corner away from the centre by `px` (pixels along the corner direction).
inline Quad quadGrow(const Quad& q, double px) {
  Pt c = quadCenter(q);
  Quad r;
  for (int i = 0; i < 4; ++i) {
    Pt d = q[i] - c;
    double n = d.norm();
    r[i] = n > 1e-9 ? q[i] + d * (px / n) : q[i];
  }
  return r;
}

struct Affine {
  double a = 1, b = 0, c = 0, d = 1, tx = 0, ty = 0;
  Pt apply(const Pt& p) const { return {a * p.x + b * p.y + tx, c * p.x + d * p.y + ty}; }
  Quad apply(const Quad& q) const { return {apply(q[0]), apply(q[1]), apply(q[2]), apply(q[3])}; }
};

// Solve A x = b (n x n) by Gaussian elimination with partial pivoting. Returns false if singular.
inline bool solveLinear(int n, std::vector<double> A, std::vector<double> b, std::vector<double>& x) {
  for (int col = 0; col < n; ++col) {
    int piv = col;
    for (int r = col + 1; r < n; ++r)
      if (std::fabs(A[r * n + col]) > std::fabs(A[piv * n + col])) piv = r;
    if (std::fabs(A[piv * n + col]) < 1e-12) return false;
    if (piv != col) {
      for (int k = 0; k < n; ++k) std::swap(A[col * n + k], A[piv * n + k]);
      std::swap(b[col], b[piv]);
    }
    for (int r = col + 1; r < n; ++r) {
      double f = A[r * n + col] / A[col * n + col];
      for (int k = col; k < n; ++k) A[r * n + k] -= f * A[col * n + k];
      b[r] -= f * b[col];
    }
  }
  x.assign(n, 0.0);
  for (int r = n - 1; r >= 0; --r) {
    double s = b[r];
    for (int k = r + 1; k < n; ++k) s -= A[r * n + k] * x[k];
    x[r] = s / A[r * n + r];
  }
  return true;
}

// Weighted least-squares similarity transform (scale, rotation, translation): dst ~ S(src).
inline bool fitSimilarity(const std::vector<Pt>& src, const std::vector<Pt>& dst, const std::vector<double>& w, Affine& out) {
  // unknowns p, q, tx, ty with x' = p x - q y + tx ; y' = q x + p y + ty
  std::vector<double> A(16, 0.0), b(4, 0.0), x;
  double wsum = 0;
  for (size_t i = 0; i < src.size(); ++i) {
    double wi = w[i];
    if (wi <= 0) continue;
    wsum += wi;
    double sx = src[i].x, sy = src[i].y, dx = dst[i].x, dy = dst[i].y;
    // rows of the design matrix for x': [sx, -sy, 1, 0], for y': [sy, sx, 0, 1]
    double r1[4] = {sx, -sy, 1, 0}, r2[4] = {sy, sx, 0, 1};
    for (int m = 0; m < 4; ++m) {
      for (int n = 0; n < 4; ++n) A[m * 4 + n] += wi * (r1[m] * r1[n] + r2[m] * r2[n]);
      b[m] += wi * (r1[m] * dx + r2[m] * dy);
    }
  }
  if (wsum < 2.0) return false;
  if (!solveLinear(4, A, b, x)) return false;
  out.a = x[0]; out.b = -x[1]; out.c = x[1]; out.d = x[0]; out.tx = x[2]; out.ty = x[3];
  return true;
}

// Weighted least-squares full affine (6 dof).
inline bool fitAffine(const std::vector<Pt>& src, const std::vector<Pt>& dst, const std::vector<double>& w, Affine& out) {
  std::vector<double> A(9, 0.0), bx(3, 0.0), by(3, 0.0), sx, sy;
  double wsum = 0;
  for (size_t i = 0; i < src.size(); ++i) {
    double wi = w[i];
    if (wi <= 0) continue;
    wsum += wi;
    double r[3] = {src[i].x, src[i].y, 1.0};
    for (int m = 0; m < 3; ++m) {
      for (int n = 0; n < 3; ++n) A[m * 3 + n] += wi * r[m] * r[n];
      bx[m] += wi * r[m] * dst[i].x;
      by[m] += wi * r[m] * dst[i].y;
    }
  }
  if (wsum < 3.0) return false;
  if (!solveLinear(3, A, bx, sx) || !solveLinear(3, A, by, sy)) return false;
  out.a = sx[0]; out.b = sx[1]; out.tx = sx[2];
  out.c = sy[0]; out.d = sy[1]; out.ty = sy[2];
  return true;
}

// Sutherland-Hodgman clip of convex polygon `subject` against convex polygon `clip`.
inline std::vector<Pt> clipConvex(std::vector<Pt> subject, const std::vector<Pt>& clip) {
  // ensure clip is counter-clockwise
  double area = 0;
  for (size_t i = 0; i < clip.size(); ++i) area += clip[i].cross(clip[(i + 1) % clip.size()]);
  std::vector<Pt> c = clip;
  if (area < 0) std::reverse(c.begin(), c.end());
  for (size_t i = 0; i < c.size() && !subject.empty(); ++i) {
    Pt a = c[i], b = c[(i + 1) % c.size()];
    std::vector<Pt> out;
    for (size_t j = 0; j < subject.size(); ++j) {
      Pt p = subject[j], q = subject[(j + 1) % subject.size()];
      double sp = (b - a).cross(p - a), sq = (b - a).cross(q - a);
      bool inP = sp >= 0, inQ = sq >= 0;
      if (inP) out.push_back(p);
      if (inP != inQ) {
        double t = sp / (sp - sq);
        out.push_back(p + (q - p) * t);
      }
    }
    subject = out;
  }
  return subject;
}
inline double polyArea(const std::vector<Pt>& p) {
  double a = 0;
  for (size_t i = 0; i < p.size(); ++i) a += p[i].cross(p[(i + 1) % p.size()]);
  return std::fabs(a) * 0.5;
}
inline double quadIoU(const Quad& a, const Quad& b) {
  std::vector<Pt> pa(a.begin(), a.end()), pb(b.begin(), b.end());
  double inter = polyArea(clipConvex(pa, pb));
  double uni = quadArea(a) + quadArea(b) - inter;
  return uni > 1e-9 ? inter / uni : 0.0;
}

// Signed distance from p to a convex quad: negative inside.
inline double quadSignedDistance(const Quad& q, const Pt& p) {
  double orient = 0;
  for (int i = 0; i < 4; ++i) orient += q[i].cross(q[(i + 1) % 4]);
  double sgn = orient >= 0 ? 1.0 : -1.0;
  bool inside = true;
  double best = 1e30;
  for (int i = 0; i < 4; ++i) {
    Pt a = q[i], b = q[(i + 1) % 4];
    Pt ab = b - a, ap = p - a;
    double len2 = ab.dot(ab);
    double t = len2 > 1e-12 ? std::clamp(ap.dot(ab) / len2, 0.0, 1.0) : 0.0;
    Pt proj = a + ab * t;
    best = std::min(best, (p - proj).norm());
    if (ab.cross(ap) * sgn < 0) inside = false;
  }
  return inside ? -best : best;
}

}  // namespace pm
