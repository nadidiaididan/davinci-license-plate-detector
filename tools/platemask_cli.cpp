// Command-line harness: generate the synthetic clip, or run detection + tracking on PPM frames and
// write overlay frames. Used for visual verification outside Resolve.
//   platemask_cli synth <outdir>                      -> outdir/frame_%04d.ppm (+ truth.txt)
//   platemask_cli run <indir> <nframes> <seedFrame> <seedX> <seedY> <outdir> [radius]
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include <map>
#include "core/Detector.h"
#include "core/Tracker.h"
#include "core/Mask.h"
#include "core/Obfuscate.h"
#include "Synth.h"

using namespace pm;

static bool writePPM(const std::string& path, const std::vector<unsigned char>& rgb, int w, int h) {
  FILE* f = fopen(path.c_str(), "wb");
  if (!f) return false;
  fprintf(f, "P6\n%d %d\n255\n", w, h);
  fwrite(rgb.data(), 1, rgb.size(), f);
  fclose(f);
  return true;
}
static bool readPPM(const std::string& path, std::vector<unsigned char>& rgb, int& w, int& h) {
  FILE* f = fopen(path.c_str(), "rb");
  if (!f) return false;
  char magic[3] = {0};
  int maxv = 0;
  if (fscanf(f, "%2s %d %d %d", magic, &w, &h, &maxv) != 4 || std::string(magic) != "P6") { fclose(f); return false; }
  fgetc(f);
  rgb.resize((size_t)w * h * 3);
  size_t n = fread(rgb.data(), 1, rgb.size(), f);
  fclose(f);
  return n == rgb.size();
}
static void drawLine(std::vector<unsigned char>& rgb, int w, int h, Pt a, Pt b, unsigned char r, unsigned char g, unsigned char bl) {
  int n = (int)std::max(std::fabs(b.x - a.x), std::fabs(b.y - a.y)) + 1;
  for (int i = 0; i <= n; ++i) {
    double t = (double)i / n;
    int x = (int)std::lround(a.x + (b.x - a.x) * t), y = (int)std::lround(a.y + (b.y - a.y) * t);
    for (int dy = -1; dy <= 0; ++dy) for (int dx = -1; dx <= 0; ++dx) {
      int xx = x + dx, yy = y + dy;
      if (xx < 0 || yy < 0 || xx >= w || yy >= h) continue;
      unsigned char* p = &rgb[((size_t)yy * w + xx) * 3];
      p[0] = r; p[1] = g; p[2] = bl;
    }
  }
}
static void drawQuad(std::vector<unsigned char>& rgb, int w, int h, const Quad& q, unsigned char r, unsigned char g, unsigned char b) {
  for (int i = 0; i < 4; ++i) drawLine(rgb, w, h, q[i], q[(i + 1) % 4], r, g, b);
}

struct PPMProvider : FrameProvider {
  std::string dir; int n;
  bool fetch(int frame, GrayFrame& out) override {
    if (frame < 0 || frame >= n) return false;
    char buf[512]; snprintf(buf, sizeof buf, "%s/frame_%04d.ppm", dir.c_str(), frame);
    std::vector<unsigned char> rgb; int w, h;
    if (!readPPM(buf, rgb, w, h)) return false;
    out.g = Gray(w, h);
    for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
      const unsigned char* p = &rgb[((size_t)y * w + x) * 3];
      out.g.at(x, y) = (0.299f * p[0] + 0.587f * p[1] + 0.114f * p[2]) / 255.f;
    }
    out.frame = frame;
    return true;
  }
  void progress(double p) override { fprintf(stderr, "\r%3d%%", (int)(p * 100)); }
};

int main(int argc, char** argv) {
  if (argc >= 3 && std::string(argv[1]) == "synth") {
    std::string out = argv[2];
    FILE* tf = fopen((out + "/truth.txt").c_str(), "w");
    for (int f = 0; f < synth::FRAMES; ++f) {
      Gray g; synth::Truth t;
      synth::render(f, g, t);
      std::vector<unsigned char> rgb((size_t)g.w * g.h * 3);
      for (size_t i = 0; i < g.p.size(); ++i) { unsigned char v = (unsigned char)std::clamp(g.p[i] * 255.f, 0.f, 255.f); rgb[i * 3] = rgb[i * 3 + 1] = rgb[i * 3 + 2] = v; }
      char buf[512]; snprintf(buf, sizeof buf, "%s/frame_%04d.ppm", out.c_str(), f);
      writePPM(buf, rgb, g.w, g.h);
      if (tf) fprintf(tf, "%d %.1f %.1f %.1f %.1f %.1f %.1f %.1f %.1f %d %d %d\n", f, t.q[0].x, t.q[0].y, t.q[1].x, t.q[1].y, t.q[2].x, t.q[2].y, t.q[3].x, t.q[3].y, t.occluded, t.partial, t.outOfFrame);
    }
    if (tf) fclose(tf);
    printf("wrote %d frames to %s\n", synth::FRAMES, out.c_str());
    return 0;
  }
  if (argc >= 8 && std::string(argv[1]) == "run") {
    PPMProvider fp; fp.dir = argv[2]; fp.n = atoi(argv[3]);
    int seedFrame = atoi(argv[4]);
    Pt seed{atof(argv[5]), atof(argv[6])};
    std::string out = argv[7];
    double radius = argc > 8 ? atof(argv[8]) : 140;
    GrayFrame f0;
    if (!fp.fetch(seedFrame, f0)) { fprintf(stderr, "cannot read seed frame\n"); return 1; }
    DetectParams dp; Detection det;
    if (!detectAtSeed(f0.g, seed, radius, dp, det)) { fprintf(stderr, "no plate found near seed\n"); return 1; }
    printf("detected: score=%.2f aspect=%.2f quad=(%.1f,%.1f)(%.1f,%.1f)(%.1f,%.1f)(%.1f,%.1f)\n", det.score, det.aspect,
           det.q[0].x, det.q[0].y, det.q[1].x, det.q[1].y, det.q[2].x, det.q[2].y, det.q[3].x, det.q[3].y);
    std::map<int, Key> keys;
    keys[seedFrame] = Key{det.q, VisState::Seed, 1, 0};
    RunOptions opt; opt.seedFrame = seedFrame; opt.startFrame = 0; opt.endFrame = fp.n - 1;
    runTracking(fp, opt, keys, nullptr);
    fprintf(stderr, "\n");
    Rng rng;
    for (int f = 0; f < fp.n; ++f) {
      char buf[512]; snprintf(buf, sizeof buf, "%s/frame_%04d.ppm", fp.dir.c_str(), f);
      std::vector<unsigned char> rgb; int w, h;
      if (!readPPM(buf, rgb, w, h)) break;
      auto it = keys.find(f);
      if (it != keys.end()) {
        const Key& k = it->second;
        // obfuscate through the matte, then draw the outline
        std::vector<float> src((size_t)w * h * 4), dst;
        for (size_t i = 0; i < (size_t)w * h; ++i) { src[i * 4] = rgb[i * 3] / 255.f; src[i * 4 + 1] = rgb[i * 3 + 1] / 255.f; src[i * 4 + 2] = rgb[i * 3 + 2] / 255.f; src[i * 4 + 3] = 1; }
        dst = src;
        std::vector<float> alpha((size_t)w * h, 0.f);
        RectI reg = unionQuadAlpha(alpha, w, h, k.q, 4 + k.grow, 4);
        ImageView sv{src.data(), w * 16, w, h}, dv{dst.data(), w * 16, w, h};
        ObfParams op; op.mode = ObfParams::Pixelize; op.blockSize = 10;
        applyObfuscation(sv, dv, reg, alpha, op, rng);
        for (size_t i = 0; i < (size_t)w * h; ++i) for (int c = 0; c < 3; ++c) rgb[i * 3 + c] = (unsigned char)std::clamp(dst[i * 4 + c] * 255.f, 0.f, 255.f);
        unsigned char r = 0, g = 255, b = 0;
        if (k.st == VisState::Partial) { r = 255; g = 200; b = 0; }
        else if (!isVisible(k.st)) { r = 255; g = 40; b = 40; }
        drawQuad(rgb, w, h, k.q, r, g, b);
        char txt[64]; snprintf(txt, sizeof txt, "%s", visStateName(k.st));
        (void)txt;
      }
      snprintf(buf, sizeof buf, "%s/out_%04d.ppm", out.c_str(), f);
      writePPM(buf, rgb, w, h);
    }
    printf("wrote overlays to %s\n", out.c_str());
    return 0;
  }
  fprintf(stderr, "usage: platemask_cli synth <outdir> | run <indir> <nframes> <seedFrame> <seedX> <seedY> <outdir> [radius]\n");
  return 1;
}
