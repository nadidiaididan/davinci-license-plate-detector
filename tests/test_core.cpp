#include <cstdio>
#include <cstdlib>
#include <map>
#include "core/Detector.h"
#include "core/Tracker.h"
#include "core/TrackStore.h"
#include "core/Mask.h"
#include "core/Obfuscate.h"
#include "Synth.h"

using namespace pm;
static int fails = 0;
#define CHECK(cond, ...) do { if (!(cond)) { ++fails; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

struct SynthProvider : FrameProvider {
  std::map<int, synth::Truth> truth;
  int fetched = 0;
  bool fetch(int frame, GrayFrame& out) override {
    if (frame < 0 || frame >= synth::FRAMES) return false;
    synth::Truth t;
    synth::render(frame, out.g, t);
    out.frame = frame;
    truth[frame] = t;
    ++fetched;
    return true;
  }
};

int main() {
  // ---- detection at the seed frame
  GrayFrame f0; synth::Truth t0;
  synth::render(10, f0.g, t0);
  Pt seed = quadCenter(t0.q) + Pt{5, -3};
  DetectParams dp;
  Detection det;
  bool ok = detectAtSeed(f0.g, seed, 140, dp, det);
  CHECK(ok, "no detection at seed");
  double iou0 = ok ? quadIoU(det.q, t0.q) : 0;
  printf("detect: iou=%.3f aspect=%.2f score=%.2f\n", iou0, det.aspect, det.score);
  CHECK(iou0 > 0.6, "seed detection IoU %.3f", iou0);
  // seed on the distractor must not pick the plate
  Detection dd;
  bool okd = detectAtSeed(f0.g, {740, 120}, 140, dp, dd);
  if (okd) CHECK(quadIoU(dd.q, t0.q) < 0.1, "distractor click returned the plate");

  // ---- tracking through occlusion, exit and return
  SynthProvider sp;
  std::map<int, Key> keys;
  keys[10] = Key{det.q, VisState::Seed, 1.0, 0};
  RunOptions opt;
  opt.seedFrame = 10; opt.startFrame = 0; opt.endFrame = synth::FRAMES - 1; opt.direction = 2;
  bool ran = runTracking(sp, opt, keys, nullptr);
  CHECK(ran, "runTracking failed");
  int visN = 0, visGood = 0, visConf = 0, occN = 0, occState = 0, occNear = 0, retN = 0, retGood = 0, oofN = 0, oofState = 0;
  int firstReturnGood = -1;
  int bushN = 0, bushInferred = 0, bushNear = 0;
  for (int f = 0; f < synth::FRAMES; ++f) {
    if (!keys.count(f)) { CHECK(false, "missing key at %d", f); continue; }
    const Key& k = keys[f];
    const synth::Truth& t = sp.truth[f];
    double iou = quadIoU(k.q, t.q);
    if (f % 10 == 0 || (f >= 38 && f <= 62) || (f >= 88 && f <= 130) || t.bush)
      printf("f=%3d st=%-12s ncc=%.2f grow=%4.1f iou=%.2f gt[occ=%d part=%d oof=%d]\n", f, visStateName(k.st), k.conf, k.grow, iou, t.occluded, t.partial, t.outOfFrame);
    bool fullyVisible = !t.occluded && !t.partial && !t.outOfFrame && quadBounds(t.q).x1 > 0 && quadBounds(t.q).x2 < synth::W;
    if (fullyVisible && f < 85) { ++visN; if (iou > 0.5) ++visGood; if (isVisible(k.st)) ++visConf; }
    if (t.occluded) { ++occN; if (!isVisible(k.st)) ++occState; if ((quadCenter(k.q) - quadCenter(t.q)).norm() < 1.5 * synth::PH) ++occNear; }
    if (t.outOfFrame) { ++oofN; if (k.st == VisState::OutOfFrame || k.st == VisState::Lost) ++oofState; }
    if (fullyVisible && f >= 128 && f < 140) { ++retN; if (iou > 0.5) { ++retGood; if (firstReturnGood < 0) firstReturnGood = f; } }
    if (t.bush) { ++bushN; if (k.st == VisState::Inferred) ++bushInferred; if ((quadCenter(k.q) - quadCenter(t.q)).norm() < 0.6 * synth::PH) ++bushNear; }
  }
  printf("visible: iou>0.5 %d/%d, visible-state %d/%d\n", visGood, visN, visConf, visN);
  printf("occluded: hidden-state %d/%d, near %d/%d\n", occState, occN, occNear, occN);
  printf("out-of-frame: state %d/%d\n", oofState, oofN);
  printf("returned: iou>0.5 %d/%d (first good frame %d)\n", retGood, retN, firstReturnGood);
  printf("bush (plate hidden, car visible): inferred-state %d/%d, centre within 0.6H %d/%d\n", bushInferred, bushN, bushNear, bushN);
  CHECK(bushN > 0 && bushInferred >= bushN * 0.8, "bush: inferred from car body");
  CHECK(bushN > 0 && bushNear >= bushN * 0.8, "bush: estimated position accuracy");
  CHECK(visN > 0 && visGood >= visN * 0.9, "visible tracking accuracy");
  CHECK(visN > 0 && visConf >= visN * 0.85, "visible state labelling");
  CHECK(occN > 0 && occState >= occN * 0.7, "occlusion state labelling");
  CHECK(occN > 0 && occNear >= occN * 0.8, "occlusion prediction stays near (sweeping occluder)");
  CHECK(occN > 0 && occNear >= occN * 0.8, "occlusion prediction stays near");
  CHECK(oofN > 0 && oofState >= oofN * 0.6, "out-of-frame labelling");
  CHECK(retN > 0 && retGood >= retN * 0.75, "re-acquisition after return");

  // ---- store round trip
  Store st;
  st.plates[2].hasSeed = true; st.plates[2].seed = seed; st.plates[2].seedFrame = 10; st.plates[2].keys = keys;
  st.trackHandled = 3;
  std::string js = st.toJson();
  Store st2;
  CHECK(st2.fromJson(js), "store parse");
  CHECK(st2.plates[2].keys.size() == keys.size(), "store keys %zu vs %zu", st2.plates[2].keys.size(), keys.size());
  CHECK(st2.trackHandled == 3, "store counters");
  Key kk;
  CHECK(st2.plates[2].keyAt(50, false, kk) && quadIoU(kk.q, keys[50].q) > 0.99, "keyAt exact");
  CHECK(!st2.plates[2].keyAt(-5, false, kk) && st2.plates[2].keyAt(-5, true, kk), "keyAt hold");
  std::string path = sidecarPath("test_" + randomId());
  CHECK(saveStore(path, st), "save sidecar");
  Store st3;
  CHECK(loadStore(path, st3) && st3.plates[2].keys.size() == keys.size(), "load sidecar");
  std::remove(path.c_str());

  // ---- matte + obfuscation
  const int W = 200, H = 120;
  std::vector<float> src((size_t)W * H * 4), dst1, dst2;
  for (int y = 0; y < H; ++y)
    for (int x = 0; x < W; ++x) {
      float* p = &src[((size_t)y * W + x) * 4];
      p[0] = (x % 7) / 7.f; p[1] = (y % 5) / 5.f; p[2] = ((x + y) % 11) / 11.f; p[3] = 1;
    }
  Quad q{Pt{40, 40}, Pt{160, 44}, Pt{158, 80}, Pt{38, 76}};
  std::vector<float> alpha((size_t)W * H, 0.f);
  RectI reg = unionQuadAlpha(alpha, W, H, q, 4, 6);
  CHECK(!reg.empty(), "matte region");
  CHECK(alpha[(size_t)60 * W + 100] > 0.99f, "matte inside = 1 (%.3f)", alpha[(size_t)60 * W + 100]);
  CHECK(alpha[(size_t)10 * W + 10] < 0.01f, "matte outside = 0");
  CHECK(alpha[(size_t)42 * W + 34] > 0.05f && alpha[(size_t)42 * W + 34] < 0.95f, "feather band partial");
  ImageView sv{src.data(), W * 16, W, H};
  ObfParams op; op.mode = ObfParams::Pixelize; op.blockSize = 12; op.destructive = true;
  Rng rng;
  dst1 = src; dst2 = src;
  ImageView d1{dst1.data(), W * 16, W, H}, d2{dst2.data(), W * 16, W, H};
  applyObfuscation(sv, d1, reg, alpha, op, rng);
  applyObfuscation(sv, d2, reg, alpha, op, rng);
  double diffSrc = 0, diffRuns = 0; int n = 0;
  for (int y = reg.y1; y < reg.y2; ++y)
    for (int x = reg.x1; x < reg.x2; ++x) {
      if (alpha[(size_t)y * W + x] < 0.99f) continue;
      const float* a = d1.px(x, y); const float* b = d2.px(x, y); const float* s = sv.px(x, y);
      for (int k = 0; k < 3; ++k) { diffSrc += std::fabs(a[k] - s[k]); diffRuns += std::fabs(a[k] - b[k]); }
      ++n;
    }
  printf("obfuscation: mean|out-src|=%.3f mean|run1-run2|=%.3f over %d px\n", diffSrc / (3 * n), diffRuns / (3 * n), n);
  CHECK(diffSrc / (3 * n) > 0.05, "pixelize changed the plate");
  CHECK(diffRuns / (3 * n) > 0.005, "destructive output is frame-unique (two renders differ)");
  CHECK(std::fabs(d1.px(5, 5)[0] - sv.px(5, 5)[0]) < 1e-6, "outside the matte untouched");
  op.mode = ObfParams::Blur; op.blurRadius = 20;
  dst1 = src; applyObfuscation(sv, d1, reg, alpha, op, rng);
  op.mode = ObfParams::Randomize; op.randomMode = 0;
  dst1 = src; applyObfuscation(sv, d1, reg, alpha, op, rng);
  op.randomMode = 2; dst1 = src; applyObfuscation(sv, d1, reg, alpha, op, rng);
  dst1 = src; writeMatte(d1, reg, alpha, W, true);
  CHECK(d1.px(100, 60)[0] > 0.99f && d1.px(100, 60)[3] > 0.99f, "mask-only output");

  printf(fails ? "\n%d FAILURES\n" : "\nALL TESTS PASSED\n", fails);
  return fails ? 1 : 0;
}
