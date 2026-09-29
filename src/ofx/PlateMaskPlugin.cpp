// PlateMask: DaVinci Resolve / OpenFX licence-plate detection, tracking and destructive masking.
//
// Host-interaction model (Resolve keeps separate UI and render instances of an effect):
//   * Buttons only bump hidden "request" counters (+ the frame/plate they apply to). That is the only
//     thing the UI instance does; the work happens in render(), which is the only action that may
//     fetch frames (temporal clip access).
//   * Results (keyframed quads, states, template) live in a sidecar JSON keyed by a per-instance id
//     stored in a hidden param, so every instance sees them. The JSON is also mirrored into a hidden
//     string param whenever the host lets us set params, so projects stay self-contained.
#include "PlateMaskPlugin.h"
#include "Params.h"
#include "ofxsSupportPrivate.h"
#include "ofxDrawSuite.h"
#include "core/Detector.h"
#include "core/Tracker.h"
#include "core/Mask.h"
#include "core/Rng.h"
#include "core/Log.h"
#include "Monitor.h"
#include <cmath>
#include <functional>
#include <memory>
#include <vector>
#include <string>
#include <cstring>
#include <algorithm>

using namespace pmx;

namespace {


OfxRGBAColourF stateColour(pm::VisState s, bool active) {
  float a = active ? 1.0f : 0.6f;
  switch (s) {
    case pm::VisState::Seed:
    case pm::VisState::Manual: return {0.2f, 0.8f, 1.0f, a};
    case pm::VisState::Confirmed: return {0.2f, 1.0f, 0.3f, a};
    case pm::VisState::Partial: return {1.0f, 0.8f, 0.1f, a};
    case pm::VisState::Inferred: return {0.8f, 0.4f, 1.0f, a};
    case pm::VisState::Occluded: return {1.0f, 0.45f, 0.1f, a};
    case pm::VisState::OutOfFrame:
    case pm::VisState::Lost: return {1.0f, 0.2f, 0.2f, a};
  }
  return {1, 1, 1, a};
}

}  // namespace

// ============================================================================================ effect

PlateMaskPlugin::PlateMaskPlugin(OfxImageEffectHandle handle) : OFX::ImageEffect(handle) {
  pm::logf("createInstance: begin");
  dstClip_ = fetchClip(kOfxImageEffectOutputClipName);
  srcClip_ = fetchClip(kOfxImageEffectSimpleSourceClipName);

  plateIndex_ = fetchChoiceParam(pPlateIndex);
  seedPoint_ = fetchDouble2DParam(pSeedPoint);
  searchRadius_ = fetchDoubleParam(pSearchRadius);
  trackDirection_ = fetchChoiceParam(pTrackDirection);
  maxFrames_ = fetchIntParam(pMaxFrames);
  occlusionHold_ = fetchIntParam(pOcclusionHold);
  growPerFrame_ = fetchDoubleParam(pGrowPerFrame);
  gapInterp_ = fetchIntParam(pGapInterp);
  holdOutside_ = fetchBooleanParam(pHoldOutside);
  showOverlay_ = fetchBooleanParam(pShowOverlay);
  status_ = fetchStringParam(pStatus);
  notify_ = fetchBooleanParam(pNotify);
  expand_ = fetchDoubleParam(pExpand);
  feather_ = fetchDoubleParam(pFeather);
  invert_ = fetchBooleanParam(pInvert);
  maskOnly_ = fetchBooleanParam(pMaskOnly);
  maskToAlpha_ = fetchBooleanParam(pMaskToAlpha);
  obfMode_ = fetchChoiceParam(pObfMode);
  blurRadius_ = fetchDoubleParam(pBlurRadius);
  blockSize_ = fetchIntParam(pBlockSize);
  randomMode_ = fetchChoiceParam(pRandomMode);
  destructive_ = fetchBooleanParam(pDestructive);
  quantLevels_ = fetchIntParam(pQuantLevels);
  noiseAmount_ = fetchDoubleParam(pNoiseAmount);
  instanceId_ = fetchStringParam(pInstanceId);
  trackData_ = fetchStringParam(pTrackData);
  detectRequest_ = fetchIntParam(pDetectRequest);
  trackRequest_ = fetchIntParam(pTrackRequest);
  clearRequest_ = fetchIntParam(pClearRequest);
  stopRequest_ = fetchIntParam(pStopRequest);
  trackMode_ = fetchChoiceParam(pTrackMode);
  requestFrame_ = fetchIntParam(pRequestFrame);
  requestPlate_ = fetchIntParam(pRequestPlate);
  revision_ = fetchIntParam(pRevision);

  pm::logf("createInstance: params fetched");
  instanceId_->getValue(id_);
  if (id_.empty()) {
    id_ = pm::randomId();
    try { instanceId_->setValue(id_); } catch (...) { /* host refused; memory-only id */ }
  }
  sidecar_ = pm::sidecarPath(id_);
  pm::logf("createInstance: id=%s sidecar=%s", id_.c_str(), sidecar_.c_str());
  std::lock_guard<std::recursive_mutex> lk(mu_);
  if (pm::fileMTime(sidecar_) != 0) {
    loadStoreIfChangedLocked();
  } else {
    std::string data;
    trackData_->getValue(data);
    if (!data.empty() && store_.fromJson(data)) {
      pm::saveStore(sidecar_, store_);
      sidecarMTime_ = pm::fileMTime(sidecar_);
    }
  }
}

PlateMaskPlugin::~PlateMaskPlugin() { pm::logf("destroyInstance id=%s", id_.c_str()); }

void PlateMaskPlugin::loadStoreIfChangedLocked() {
  long long mt = pm::fileMTime(sidecar_);
  if (mt == 0 || mt == sidecarMTime_) return;
  pm::Store s;
  if (pm::loadStore(sidecar_, s)) {
    store_ = std::move(s);
    sidecarMTime_ = mt;
    paramFlushPending_ = true;  // mirror the other instance's results into our params when allowed
  }
}

void PlateMaskPlugin::saveStoreLocked() {
  if (pm::saveStore(sidecar_, store_)) sidecarMTime_ = pm::fileMTime(sidecar_);
  paramFlushPending_ = true;
}

void PlateMaskPlugin::flushStoreToParamLocked() {
  if (!paramFlushPending_) return;
  try {
    if (!anyLive()) {  // while live tracking runs the store changes every frame; mirror it when it settles
      std::string js = store_.toJson(), cur;
      trackData_->getValue(cur);
      if (cur != js) trackData_->setValue(js);  // may re-enter changedParam synchronously (recursive mutex)
    }
    std::string st;
    status_->getValue(st);
    if (st != store_.status) status_->setValue(store_.status);
    paramFlushPending_ = false;
  } catch (...) {
    // not an action that allows param writes; try again later
  }
}

pm::Pt PlateMaskPlugin::normToCanon(const pm::Pt& n) {
  OfxPointD off = getProjectOffset(), ext = getProjectExtent();
  return {off.x + n.x * ext.x, off.y + n.y * ext.y};
}

pm::Pt PlateMaskPlugin::canonToNorm(const pm::Pt& c) {
  OfxPointD off = getProjectOffset(), ext = getProjectExtent();
  return {ext.x > 0 ? (c.x - off.x) / ext.x : 0.0, ext.y > 0 ? (c.y - off.y) / ext.y : 0.0};
}

int PlateMaskPlugin::activePlate() {
  int v = 0;
  plateIndex_->getValue(v);
  return std::clamp(v, 0, pm::kMaxPlates - 1);
}

bool PlateMaskPlugin::overlayEnabled() {
  bool v = true;
  showOverlay_->getValue(v);
  return v;
}

void PlateMaskPlugin::getFramesNeeded(const OFX::FramesNeededArguments& args, OFX::FramesNeededSetter& frames) {
  // Only the frame being rendered is needed for a normal render; analysis fetches extra frames itself
  // through temporal clip access.
  OfxRangeD r{args.time, args.time};
  frames.setFramesNeeded(*srcClip_, r);
}

bool PlateMaskPlugin::isIdentity(const OFX::IsIdentityArguments& args, OFX::Clip*& identityClip, double& identityTime) {
  return false;
}

// Convert an OFX image into a gray working frame (decimated to <= ~1280 px wide) plus the mapping to
// canonical coordinates: canonical x = pixel x * PAR / renderScale.x.
bool PlateMaskPlugin::buildGrayFrame(OFX::Image* img, int frame, pm::GrayFrame& out) {
  if (!img) return false;
  const OfxRectI b = img->getBounds();
  const int w = b.x2 - b.x1, h = b.y2 - b.y1;
  if (w <= 0 || h <= 0) return false;
  const OfxPointD rs = img->getRenderScale();
  const double par = img->getPixelAspectRatio() > 0 ? img->getPixelAspectRatio() : 1.0;
  const int decim = std::max(1, (w + 1279) / 1280);
  const void* base = img->getPixelAddress(b.x1, b.y1);
  if (!base) return false;
  switch (img->getPixelDepth()) {
    case OFX::eBitDepthFloat: out.g = pm::grayFromRGBAf((const float*)base, img->getRowBytes(), w, h, decim); break;
    case OFX::eBitDepthUShort: out.g = pm::grayFromRGBA16((const uint16_t*)base, img->getRowBytes(), w, h, decim); break;
    case OFX::eBitDepthUByte: out.g = pm::grayFromRGBA8((const uint8_t*)base, img->getRowBytes(), w, h, decim); break;
    default: return false;
  }
  out.sx = rs.x / (par * decim);
  out.sy = rs.y / decim;
  out.ox = -(double)b.x1 / decim;
  out.oy = -(double)b.y1 / decim;
  out.frame = frame;
  return true;
}

void PlateMaskPlugin::runDetectLocked(int frame, int plate, pm::Pt seedCanon, double radiusFrac) {
  pm::logf("detect: frame=%d plate=%d seed=(%.1f,%.1f) radius=%.3f", frame, plate, seedCanon.x, seedCanon.y, radiusFrac);
  std::unique_ptr<OFX::Image> img(srcClip_->fetchImage(frame));
  pm::GrayFrame gf;
  if (!buildGrayFrame(img.get(), frame, gf)) return;
  pm::PlateTrack& pt = store_.plates[plate];
  pt.keys.clear();
  pt.templ.clear();
  pt.hasSeed = true;
  pt.seed = canonToNorm(seedCanon);  // stored normalised, like the param
  pt.seedFrame = frame;
  pm::Pt seedG = gf.toGray(seedCanon);
  double radius = std::max(16.0, radiusFrac * gf.g.w);
  pm::DetectParams dp;
  pm::Detection det;
  pm::Key key;
  key.st = pm::VisState::Seed;
  key.conf = 1;
  if (pm::detectAtSeed(gf.g, seedG, radius, dp, det)) {
    key.q = gf.toCanon(det.q);
  } else {
    // Nothing plausible near the click: give the operator a Swiss-proportioned box to adjust by hand.
    double hh = std::max(4.0, 0.02 * gf.g.w) / gf.sx, hw = hh * 3.75;
    key.q = {pm::Pt{seedCanon.x - hw, seedCanon.y - hh}, pm::Pt{seedCanon.x + hw, seedCanon.y - hh},
             pm::Pt{seedCanon.x + hw, seedCanon.y + hh}, pm::Pt{seedCanon.x - hw, seedCanon.y + hh}};
    key.conf = 0;
  }
  pm::logf("detect: quad=(%.1f,%.1f)(%.1f,%.1f)(%.1f,%.1f)(%.1f,%.1f) conf=%.2f gray=%dx%d", key.q[0].x, key.q[0].y, key.q[1].x, key.q[1].y, key.q[2].x, key.q[2].y, key.q[3].x, key.q[3].y, key.conf, gf.g.w, gf.g.h);
  pt.keys[frame] = key;
  char buf[256];
  if (key.conf > 0)
    snprintf(buf, sizeof buf, "Plate %d: detected on frame %d (%.0f x %.0f px). Check the box, then press Track.", plate + 1, frame,
             pm::quadLongSide(key.q), pm::quadShortSide(key.q));
  else
    snprintf(buf, sizeof buf, "Plate %d: nothing plate-like near the seed on frame %d. A default box was placed; drag its corners onto the plate, then press Track.", plate + 1, frame);
  store_.status = buf;
}

namespace {
struct ClipProvider : pm::FrameProvider {
  PlateMaskPlugin* fx;
  OFX::Clip* clip;
  std::function<bool(OFX::Image*, int, pm::GrayFrame&)> build;
  bool fetch(int frame, pm::GrayFrame& out) override {
    std::unique_ptr<OFX::Image> img;
    try { img.reset(clip->fetchImage(frame)); } catch (...) { return false; }
    return build(img.get(), frame, out);
  }
  // Resolve raises the OFX abort flag during batch renders, so cancellation comes from the host's
  // progress dialog (progressUpdate returns false when the user cancels) rather than abort().
  bool cancelled = false;
  int plate = 0;
  bool warnedMain = false;
  bool aborted() override { return cancelled || pmui::monitorCancelled(); }
  void progress(double p) override { if (!fx->progressUpdate(p)) cancelled = true; }
  void onFrame(const pm::GrayFrame& f, const pm::Key& k, int done, int tot) override {
    // Build a small RGB picture of the frame with the solved quad and push it to the monitor window.
    const int W = f.g.w, H = f.g.h;
    int step = std::max(1, (int)std::ceil(std::max(W, H) / 900.0));
    int w = W / step, h = H / step;
    if (w <= 0 || h <= 0) return;
    auto rgb = std::make_shared<std::vector<uint8_t>>((size_t)w * h * 3);
    for (int y = 0; y < h; ++y) {
      int sy = (h - 1 - y) * step;  // OFX rows are bottom-up; the window wants top row first
      for (int x = 0; x < w; ++x) {
        uint8_t v = (uint8_t)std::clamp(f.g.at(x * step, sy) * 255.f, 0.f, 255.f);
        uint8_t* p = &(*rgb)[((size_t)y * w + x) * 3];
        p[0] = p[1] = p[2] = v;
      }
    }
    pm::Quad qg = f.toGray(k.q);
    uint8_t col[3] = {60, 255, 80};
    switch (k.st) {
      case pm::VisState::Partial: col[0] = 255; col[1] = 200; col[2] = 30; break;
      case pm::VisState::Inferred: col[0] = 200; col[1] = 100; col[2] = 255; break;
      case pm::VisState::Occluded: col[0] = 255; col[1] = 120; col[2] = 30; break;
      case pm::VisState::OutOfFrame: case pm::VisState::Lost: col[0] = 255; col[1] = 50; col[2] = 50; break;
      case pm::VisState::Seed: case pm::VisState::Manual: col[0] = 50; col[1] = 200; col[2] = 255; break;
      default: break;
    }
    auto plot = [&](int x, int y) {
      for (int dy = -1; dy <= 1; ++dy)
        for (int dx = -1; dx <= 1; ++dx) {
          int xx = x + dx, yy = y + dy;
          if (xx < 0 || yy < 0 || xx >= w || yy >= h) continue;
          uint8_t* p = &(*rgb)[((size_t)yy * w + xx) * 3];
          p[0] = col[0]; p[1] = col[1]; p[2] = col[2];
        }
    };
    for (int i = 0; i < 4; ++i) {
      pm::Pt a = qg[i], b = qg[(i + 1) % 4];
      int n = (int)(std::max(std::fabs(b.x - a.x), std::fabs(b.y - a.y)) / step) + 1;
      for (int j = 0; j <= n; ++j) {
        double t = (double)j / std::max(1, n);
        double x = (a.x + (b.x - a.x) * t) / step, y = (a.y + (b.y - a.y) * t) / step;
        plot((int)std::lround(x), h - 1 - (int)std::lround(y));
      }
    }
    char text[200];
    snprintf(text, sizeof text, "Plate %d  -  frame %d  -  %s  -  %d of %d frames (%d%%)", plate + 1, f.frame,
             pm::visStateName(k.st), done, tot, tot > 0 ? (int)(100.0 * done / tot) : 0);
    pmui::monitorUpdate(rgb, w, h, text, tot > 0 ? (double)done / tot : 0.0);
    if (done == 12 && !pmui::monitorMainThreadResponsive() && !warnedMain) {
      warnedMain = true;
      pm::logf("monitor: main thread has not drawn after 12 frames (host UI blocked during render?)");
    }
  }
};
}  // namespace

void PlateMaskPlugin::runTrackLocked(int plate, const OFX::RenderArguments& args) {
  pm::PlateTrack& pt = store_.plates[plate];
  if (pt.seedFrame == INT_MIN) return;
  auto it = pt.keys.find(pt.seedFrame);
  if (it == pt.keys.end() || !pm::isAnchor(it->second.st)) return;
  pt.clearAuto();

  pm::RunOptions opt;
  opt.seedFrame = pt.seedFrame;
  OfxRangeD range = srcClip_->getFrameRange();
  opt.startFrame = (int)std::floor(range.min);
  opt.endFrame = (int)std::ceil(range.max);
  if (opt.endFrame < opt.startFrame) { opt.startFrame = pt.seedFrame - 100000; opt.endFrame = pt.seedFrame + 100000; }
  int dir = 0;
  trackDirection_->getValue(dir);
  opt.direction = dir == 0 ? 2 : (dir == 1 ? 0 : 1);
  maxFrames_->getValue(opt.maxFrames);
  occlusionHold_->getValue(opt.tp.maxAge);
  growPerFrame_->getValue(opt.tp.growPerFrame);
  gapInterp_->getValue(opt.tp.gapInterpMax);

  ClipProvider fp;
  fp.fx = this;
  fp.clip = srcClip_;
  fp.plate = plate;
  fp.build = [this](OFX::Image* img, int f, pm::GrayFrame& out) { return buildGrayFrame(img, f, out); };
  char title[96];
  snprintf(title, sizeof title, "PlateMask - tracking plate %d", plate + 1);
  pmui::monitorShow(title);
  pm::logf("track(batch): plate=%d seed=%d range=[%d,%d] dir=%d max=%d progressSuite=%p/%p messageSuite=%p", plate, pt.seedFrame, opt.startFrame, opt.endFrame, opt.direction, opt.maxFrames,
           (void*)OFX::Private::gProgressSuiteV1, (void*)OFX::Private::gProgressSuiteV2, (void*)OFX::Private::gMessageSuite);
  char msg[128];
  snprintf(msg, sizeof msg, "PlateMask: tracking plate %d...", plate + 1);
  progressStart(msg);
  bool ok = pm::runTracking(fp, opt, pt.keys, &pt.templ);
  progressEnd();
  pmui::monitorClose();
  pm::logf("track: monitor main-thread responsive=%d cancelled=%d", (int)pmui::monitorMainThreadResponsive(), (int)fp.aborted());
  pm::logf("track: done ok=%d keys=%zu", (int)ok, pt.keys.size());
  store_.status = summaryFor(plate, ok ? "tracked " : "cancelled - ");
  bool notify = true;
  notify_->getValue(notify);
  if (notify) {
    try { sendMessage(OFX::Message::eMessageMessage, "platemask.track", store_.status); } catch (...) {}
  }
}

bool PlateMaskPlugin::anyLive() const {
  for (const pm::PlateTrack& p : store_.plates) if (p.live) return true;
  return false;
}

pm::TrackerParams PlateMaskPlugin::trackerParams() {
  pm::TrackerParams tp;
  occlusionHold_->getValue(tp.maxAge);
  growPerFrame_->getValue(tp.growPerFrame);
  gapInterp_->getValue(tp.gapInterpMax);
  return tp;
}

std::string PlateMaskPlugin::summaryFor(int plate, const char* prefix) {
  const pm::PlateTrack& pt = store_.plates[plate];
  int n[8] = {0};
  int first = INT_MAX, last = INT_MIN;
  for (auto& kv : pt.keys) { n[std::clamp((int)kv.second.st, 0, 7)]++; first = std::min(first, kv.first); last = std::max(last, kv.first); }
  char buf[400];
  if (pt.keys.size() > 1)
    snprintf(buf, sizeof buf, "Plate %d: %s%zu frames (%d-%d): %d confirmed, %d partial, %d inferred from car body, %d occluded, %d out of frame, %d lost.",
             plate + 1, prefix, pt.keys.size(), first, last, n[1], n[2], n[7], n[3], n[4], n[5]);
  else
    snprintf(buf, sizeof buf, "Plate %d: %sno frames tracked yet.", plate + 1, prefix);
  return buf;
}

void PlateMaskPlugin::handleRequestsLocked(const OFX::RenderArguments& args) {
  int cr = 0, dr = 0, tr = 0, sr = 0, rf = 0, rp = 0;
  clearRequest_->getValue(cr);
  detectRequest_->getValue(dr);
  trackRequest_->getValue(tr);
  stopRequest_->getValue(sr);
  requestFrame_->getValue(rf);
  requestPlate_->getValue(rp);
  rp = std::clamp(rp, -1, pm::kMaxPlates - 1);
  if (cr <= store_.clearHandled && dr <= store_.detectHandled && tr <= store_.trackHandled && sr <= store_.stopHandled) return;
  // Another instance may have handled it meanwhile.
  loadStoreIfChangedLocked();
  bool changed = false;
  if (cr > store_.clearHandled) {
    if (rp < 0) for (auto& p : store_.plates) p = pm::PlateTrack{};
    else store_.plates[rp] = pm::PlateTrack{};
    store_.status = rp < 0 ? "All plates cleared." : "Plate " + std::to_string(rp + 1) + " cleared.";
    store_.clearHandled = cr;
    changed = true;
  }
  if (dr > store_.detectHandled) {
    double sx = 0, sy = 0;
    seedPoint_->getValue(sx, sy);
    double rad = 0.12;
    searchRadius_->getValue(rad);
    if (rp >= 0) runDetectLocked(rf, rp, normToCanon({sx, sy}), rad);
    store_.detectHandled = dr;
    changed = true;
  }
  if (sr > store_.stopHandled) {
    for (int i = 0; i < pm::kMaxPlates; ++i)
      if (store_.plates[i].live) { store_.plates[i].live = false; store_.status = summaryFor(i, "stopped - "); }
    store_.stopHandled = sr;
    changed = true;
  }
  if (tr > store_.trackHandled) {
    if (rp >= 0) {
      pm::PlateTrack& pt = store_.plates[rp];
      pt.live = false;
      if (pt.seedFrame != INT_MIN && pt.keys.count(pt.seedFrame)) runTrackLocked(rp, args);
      else store_.status = "Plate " + std::to_string(rp + 1) + ": nothing to track - press Detect license plate first.";
    }
    store_.trackHandled = tr;
    changed = true;
  }
  if (changed) saveStoreLocked();
}

void PlateMaskPlugin::render(const OFX::RenderArguments& args) {
  pm::logv("render t=%.1f rs=%.2f win=(%d,%d,%d,%d)", args.time, args.renderScale.x, args.renderWindow.x1, args.renderWindow.y1, args.renderWindow.x2, args.renderWindow.y2);
  std::unique_ptr<OFX::Image> dst(dstClip_->fetchImage(args.time));
  std::unique_ptr<OFX::Image> src(srcClip_->fetchImage(args.time));
  if (!dst || !src) OFX::throwSuiteStatusException(kOfxStatFailed);
  if (dst->getPixelDepth() != OFX::eBitDepthFloat || src->getPixelDepth() != OFX::eBitDepthFloat ||
      dst->getPixelComponents() != OFX::ePixelComponentRGBA || src->getPixelComponents() != OFX::ePixelComponentRGBA)
    OFX::throwSuiteStatusException(kOfxStatErrUnsupported);

  const OfxRectI db = dst->getBounds();
  const OfxRectI sb = src->getBounds();
  const int W = db.x2 - db.x1, H = db.y2 - db.y1;
  if (W <= 0 || H <= 0) return;

  // Copy source -> destination over the destination bounds.
  for (int y = db.y1; y < db.y2; ++y) {
    float* d = (float*)dst->getPixelAddress(db.x1, y);
    if (!d) continue;
    if (y >= sb.y1 && y < sb.y2) {
      for (int x = db.x1; x < db.x2; ++x) {
        float* dp = d + (size_t)(x - db.x1) * 4;
        const float* sp = (x >= sb.x1 && x < sb.x2) ? (const float*)src->getPixelAddress(x, y) : nullptr;
        if (sp) { dp[0] = sp[0]; dp[1] = sp[1]; dp[2] = sp[2]; dp[3] = sp[3]; }
        else { dp[0] = dp[1] = dp[2] = dp[3] = 0; }
      }
    } else {
      std::memset(d, 0, (size_t)W * 4 * sizeof(float));
    }
  }
  // A source view with the same geometry as the destination (built from the copy we just made).
  pm::ImageView dv{(float*)dst->getPixelAddress(db.x1, db.y1), dst->getRowBytes(), W, H};
  std::vector<float> srcCopy((size_t)W * H * 4);
  for (int y = 0; y < H; ++y) std::memcpy(&srcCopy[(size_t)y * W * 4], dv.px(0, y), (size_t)W * 4 * sizeof(float));
  pm::ImageView sv{srcCopy.data(), W * 4 * (int)sizeof(float), W, H};

  std::lock_guard<std::recursive_mutex> lk(mu_);
  loadStoreIfChangedLocked();
  handleRequestsLocked(args);

  // ---- matte
  const double par = dst->getPixelAspectRatio() > 0 ? dst->getPixelAspectRatio() : 1.0;
  const OfxPointD rs = args.renderScale;
  const int frame = (int)std::lround(args.time);
  bool hold = false, invert = false, maskOnly = false, maskToAlpha = false;
  holdOutside_->getValue(hold);
  invert_->getValue(invert);
  maskOnly_->getValue(maskOnly);
  maskToAlpha_->getValue(maskToAlpha);
  double expand = 0, feather = 0;
  expand_->getValueAtTime(args.time, expand);
  feather_->getValueAtTime(args.time, feather);

  std::vector<float> alpha((size_t)W * H, 0.f);
  pm::RectI region{0, 0, 0, 0};
  for (const pm::PlateTrack& pt : store_.plates) {
    pm::Key k;
    if (!pt.keyAt(frame, hold, k)) continue;
    pm::Quad qp;
    for (int i = 0; i < 4; ++i) qp[i] = {k.q[i].x * rs.x / par - db.x1, k.q[i].y * rs.y - db.y1};
    pm::RectI r = pm::unionQuadAlpha(alpha, W, H, qp, (expand + k.grow) * rs.x, feather * rs.x);
    region = pm::unionRect(region, r);
  }
  if (invert) {
    for (float& a : alpha) a = 1.f - a;
    region = {0, 0, W, H};
  }

  if (maskOnly) {
    pm::writeMatte(dv, {0, 0, W, H}, alpha, W, true);
    return;
  }
  if (maskToAlpha)
    for (int y = 0; y < H; ++y)
      for (int x = 0; x < W; ++x) dv.px(x, y)[3] = 0.f;
  if (region.empty()) return;

  pm::ObfParams op;
  int mode = 0, rmode = 0;
  obfMode_->getValue(mode);
  randomMode_->getValue(rmode);
  op.mode = mode == 0 ? pm::ObfParams::Pixelize : (mode == 1 ? pm::ObfParams::Blur : (mode == 2 ? pm::ObfParams::Randomize : pm::ObfParams::None));
  blurRadius_->getValueAtTime(args.time, op.blurRadius);
  op.blurRadius *= rs.x;
  blockSize_->getValueAtTime(args.time, op.blockSize);
  op.blockSize = std::max(2, (int)std::lround(op.blockSize * rs.x));
  op.randomMode = rmode;
  destructive_->getValue(op.destructive);
  quantLevels_->getValue(op.quantLevels);
  noiseAmount_->getValue(op.noiseAmp);
  op.writeAlpha = maskToAlpha;
  pm::Rng rng;
  pm::applyObfuscation(sv, dv, region, alpha, op, rng);
}

void PlateMaskPlugin::changedParam(const OFX::InstanceChangedArgs& args, const std::string& paramName) {
  pm::logv("changedParam %s t=%.1f reason=%d", paramName.c_str(), args.time, (int)args.reason);
  std::lock_guard<std::recursive_mutex> lk(mu_);
  auto bump = [&](OFX::IntParam* counter, int plate) {
    int v = 0;
    counter->getValue(v);
    counter->setValue(v + 1);
    requestFrame_->setValue((int)std::lround(args.time));
    requestPlate_->setValue(plate);
    int rev = 0;
    revision_->getValue(rev);
    revision_->setValue(rev + 1);  // invalidates the host's render cache for every frame
  };
  auto say = [&](const std::string& m) { try { status_->setValue(m); } catch (...) {} };
  if (paramName == pDetect) { say("Detecting plate " + std::to_string(activePlate() + 1) + "..."); bump(detectRequest_, activePlate()); }
  else if (paramName == pTrack) {
    say("Tracking plate " + std::to_string(activePlate() + 1) + "... watch the PlateMask window (it shows each frame as it is solved; Stop ends early).");
    bump(trackRequest_, activePlate());
  }
  else if (paramName == pClearPlate) bump(clearRequest_, activePlate());
  else if (paramName == pClearAll) bump(clearRequest_, -1);
  else if (paramName == pPlateIndex) {
    // show the selected plate's seed in the seed control
    loadStoreIfChangedLocked();
    const pm::PlateTrack& pt = store_.plates[activePlate()];
    if (pt.hasSeed) seedPoint_->setValue(pt.seed.x, pt.seed.y);
  }
  loadStoreIfChangedLocked();
  flushStoreToParamLocked();
}

void PlateMaskPlugin::syncPrivateData() {
  std::lock_guard<std::recursive_mutex> lk(mu_);
  loadStoreIfChangedLocked();
  paramFlushPending_ = true;
  flushStoreToParamLocked();
}

// ============================================================================================ overlay

PlateOverlay::PlateOverlay(OfxInteractHandle handle, OFX::ImageEffect* effect)
    : OFX::OverlayInteract(handle), fx_(dynamic_cast<PlateMaskPlugin*>(effect)) {}

bool PlateOverlay::draw(const OFX::DrawArgs& args) {
  OfxDrawSuiteV1* ds = OFX::Private::gDrawSuite;
  static bool once = false;
  if (!once) { once = true; pm::logf("overlay draw: drawSuite=%p ctx=%p pixelScale=%.4f t=%.1f", (void*)ds, (void*)args.context, args.pixelScale.x, args.time); }
  if (!fx_ || !ds || !args.context || !fx_->overlayEnabled()) return false;
  const int frame = (int)std::lround(args.time);
  const int active = fx_->activePlate();
  const double ps = std::max(args.pixelScale.x, 1e-6);  // canonical units per screen pixel
  std::lock_guard<std::recursive_mutex> lk(fx_->storeMutex());
  fx_->loadStoreIfChangedLocked();
  fx_->flushStoreToParamLocked();
  const pm::Store& st = fx_->store();
  ds->setLineWidth(args.context, 1.5f);
  ds->setLineStipple(args.context, kOfxDrawLineStipplePatternSolid);
  for (int i = 0; i < pm::kMaxPlates; ++i) {
    const pm::PlateTrack& pt = st.plates[i];
    pm::Key k;
    bool have = pt.keyAt(frame, false, k);
    bool held = false;
    if (!have && pt.keyAt(frame, true, k)) { have = true; held = true; }
    if (!have) continue;
    OfxRGBAColourF c = stateColour(k.st, i == active);
    if (held) c.a *= 0.4f;
    ds->setColour(args.context, &c);
    ds->setLineStipple(args.context, held ? kOfxDrawLineStipplePatternDot : kOfxDrawLineStipplePatternSolid);
    OfxPointD pts[4];
    for (int j = 0; j < 4; ++j) pts[j] = {k.q[j].x, k.q[j].y};
    ds->draw(args.context, kOfxDrawPrimitiveLineLoop, pts, 4);
    if (k.grow > 0) {
      pm::Quad g = pm::quadGrow(k.q, k.grow);
      OfxPointD gp[4];
      for (int j = 0; j < 4; ++j) gp[j] = {g[j].x, g[j].y};
      ds->setLineStipple(args.context, kOfxDrawLineStipplePatternDash);
      ds->draw(args.context, kOfxDrawPrimitiveLineLoop, gp, 4);
      ds->setLineStipple(args.context, kOfxDrawLineStipplePatternSolid);
    }
    if (i == active) {
      double hs = 4.0 * ps;
      for (int j = 0; j < 4; ++j) {
        OfxPointD h[2] = {{k.q[j].x - hs, k.q[j].y - hs}, {k.q[j].x + hs, k.q[j].y + hs}};
        ds->draw(args.context, kOfxDrawPrimitiveRectangle, h, 2);
      }
    }
    char label[64];
    snprintf(label, sizeof label, "P%d %s%s", i + 1, pm::visStateName(k.st), held ? " (hold)" : "");
    pm::Rect b = pm::quadBounds(k.q);
    OfxPointD tp{b.x1, b.y2 + 6 * ps};
    ds->drawText(args.context, label, &tp, kOfxDrawTextAlignmentLeft | kOfxDrawTextAlignmentBottom);
  }
  // seed crosshair of the active plate
  double sx = 0, sy = 0;
  fx_->seedParam()->getValue(sx, sy);
  pm::Pt sc_ = fx_->normToCanon({sx, sy});
  sx = sc_.x; sy = sc_.y;
  OfxRGBAColourF sc{1, 1, 1, 0.9f};
  ds->setColour(args.context, &sc);
  double r = 7 * ps;
  OfxPointD cross[4] = {{sx - r, sy}, {sx + r, sy}, {sx, sy - r}, {sx, sy + r}};
  ds->draw(args.context, kOfxDrawPrimitiveLines, cross, 4);
  return true;
}

bool PlateOverlay::penDown(const OFX::PenArgs& args) {
  pm::logf("penDown (%.1f,%.1f) t=%.1f", args.penPosition.x, args.penPosition.y, args.time);
  if (!fx_ || !fx_->overlayEnabled()) return false;
  const int frame = (int)std::lround(args.time);
  const int active = fx_->activePlate();
  const double ps = std::max(args.pixelScale.x, 1e-6);
  pm::Pt p{args.penPosition.x, args.penPosition.y};
  {
    std::lock_guard<std::recursive_mutex> lk(fx_->storeMutex());
    fx_->loadStoreIfChangedLocked();
    const pm::PlateTrack& pt = fx_->store().plates[active];
    pm::Key k;
    if (pt.keyAt(frame, false, k)) {
      for (int j = 0; j < 4; ++j)
        if ((k.q[j] - p).norm() < 10 * ps) { dragPlate_ = active; dragCorner_ = j; return true; }
    }
  }
  // Plain click: this is the seed for the active plate (stored normalised to the project size).
  pm::Pt n = fx_->canonToNorm(p);
  fx_->seedParam()->setValue(n.x, n.y);
  requestRedraw();
  return true;
}

bool PlateOverlay::penMotion(const OFX::PenArgs& args) {
  if (dragPlate_ < 0 || !fx_) return false;
  const int frame = (int)std::lround(args.time);
  pm::Pt p{args.penPosition.x, args.penPosition.y};
  std::lock_guard<std::recursive_mutex> lk(fx_->storeMutex());
  fx_->loadStoreIfChangedLocked();
  pm::PlateTrack& pt = fx_->store().plates[dragPlate_];
  pm::Key k;
  if (!pt.keyAt(frame, false, k)) return true;
  k.q[dragCorner_] = p;
  if (!pm::isAnchor(k.st)) k.st = pm::VisState::Manual;
  k.conf = 1;
  k.grow = 0;
  pt.keys[frame] = k;
  fx_->saveStoreLocked();
  requestRedraw();
  return true;
}

bool PlateOverlay::penUp(const OFX::PenArgs& args) {
  if (dragPlate_ < 0 || !fx_) return false;
  dragPlate_ = dragCorner_ = -1;
  std::lock_guard<std::recursive_mutex> lk(fx_->storeMutex());
  fx_->flushStoreToParamLocked();
  // Force a re-render so the edited matte shows up.
  try {
    OFX::IntParam* rev = fx_->fetchIntParam(pRevision);
    int v = 0; rev->getValue(v); rev->setValue(v + 1);
  } catch (...) {}
  return true;
}

// ============================================================================================ factory

PlateMaskPluginFactory::PlateMaskPluginFactory()
    : OFX::PluginFactoryHelper<PlateMaskPluginFactory>(kPluginIdentifier, kVersionMajor, kVersionMinor) {}

void PlateMaskPluginFactory::describe(OFX::ImageEffectDescriptor& desc) {
  pm::logf("describe: host=%s overlays=%d progressSuite=%p/%p messageSuite=%p/%p timeLineSuite=%p", OFX::getImageEffectHostDescription() ? OFX::getImageEffectHostDescription()->hostName.c_str() : "?", OFX::getImageEffectHostDescription() ? (int)OFX::getImageEffectHostDescription()->supportsOverlays : -1,
           (void*)OFX::Private::gProgressSuiteV1, (void*)OFX::Private::gProgressSuiteV2, (void*)OFX::Private::gMessageSuite, (void*)OFX::Private::gMessageSuiteV2, (void*)OFX::Private::gTimeLineSuite);
  desc.setLabels(kPluginName, kPluginName, kPluginName);
  desc.setPluginGrouping(kPluginGrouping);
  desc.setPluginDescription(
      "Detects a licence plate at the clicked seed point, tracks it through occlusion and out-of-frame "
      "excursions, and masks it with a destructive (non-invertible) blur, mosaic or pixel randomiser. "
      "Swiss plate formats are prioritised. Up to 8 plates per instance. The matte can be exported "
      "(Mask Only / alpha) to drive other effects.");
  desc.addSupportedContext(OFX::eContextFilter);
  desc.addSupportedContext(OFX::eContextGeneral);
  desc.addSupportedBitDepth(OFX::eBitDepthFloat);
  desc.setSingleInstance(false);
  desc.setHostFrameThreading(false);
  desc.setSupportsMultiResolution(false);
  desc.setSupportsTiles(false);
  desc.setTemporalClipAccess(true);
  desc.setRenderTwiceAlways(false);
  desc.setSupportsMultipleClipPARs(false);
  desc.setRenderThreadSafety(OFX::eRenderInstanceSafe);
  desc.setOverlayInteractDescriptor(new PlateOverlayDescriptor);
}

void PlateMaskPluginFactory::describeInContext(OFX::ImageEffectDescriptor& desc, OFX::ContextEnum ctx) {
  pm::logf("describeInContext ctx=%d", (int)ctx);
  OFX::ClipDescriptor* srcClip = desc.defineClip(kOfxImageEffectSimpleSourceClipName);
  srcClip->addSupportedComponent(OFX::ePixelComponentRGBA);
  srcClip->setTemporalClipAccess(true);
  srcClip->setSupportsTiles(false);
  srcClip->setIsMask(false);
  OFX::ClipDescriptor* dstClip = desc.defineClip(kOfxImageEffectOutputClipName);
  dstClip->addSupportedComponent(OFX::ePixelComponentRGBA);
  dstClip->setSupportsTiles(false);

  OFX::PageParamDescriptor* page = desc.definePageParam("Controls");

  auto group = [&](const char* name, const char* label, bool open) {
    OFX::GroupParamDescriptor* g = desc.defineGroupParam(name);
    g->setLabels(label, label, label);
    g->setOpen(open);
    page->addChild(*g);
    return g;
  };
  auto dbl = [&](const char* name, const char* label, const char* hint, double def, double mn, double mx,
                 OFX::GroupParamDescriptor* parent, bool animates = true) {
    OFX::DoubleParamDescriptor* p = desc.defineDoubleParam(name);
    p->setLabels(label, label, label); p->setScriptName(name); p->setHint(hint);
    p->setDefault(def); p->setRange(mn, mx); p->setDisplayRange(mn, mx); p->setIncrement(0.5);
    p->setAnimates(animates);
    if (parent) p->setParent(*parent);
    page->addChild(*p);
    return p;
  };
  auto integer = [&](const char* name, const char* label, const char* hint, int def, int mn, int mx,
                     OFX::GroupParamDescriptor* parent, bool animates = false) {
    OFX::IntParamDescriptor* p = desc.defineIntParam(name);
    p->setLabels(label, label, label); p->setScriptName(name); p->setHint(hint);
    p->setDefault(def); p->setRange(mn, mx); p->setDisplayRange(mn, mx);
    p->setAnimates(animates);
    if (parent) p->setParent(*parent);
    page->addChild(*p);
    return p;
  };
  auto boolean = [&](const char* name, const char* label, const char* hint, bool def, OFX::GroupParamDescriptor* parent) {
    OFX::BooleanParamDescriptor* p = desc.defineBooleanParam(name);
    p->setLabels(label, label, label); p->setScriptName(name); p->setHint(hint);
    p->setDefault(def); p->setAnimates(false);
    if (parent) p->setParent(*parent);
    page->addChild(*p);
    return p;
  };
  auto button = [&](const char* name, const char* label, const char* hint, OFX::GroupParamDescriptor* parent) {
    OFX::PushButtonParamDescriptor* p = desc.definePushButtonParam(name);
    p->setLabels(label, label, label); p->setScriptName(name); p->setHint(hint);
    if (parent) p->setParent(*parent);
    page->addChild(*p);
    return p;
  };
  auto choice = [&](const char* name, const char* label, const char* hint, std::initializer_list<const char*> opts, int def,
                    OFX::GroupParamDescriptor* parent) {
    OFX::ChoiceParamDescriptor* p = desc.defineChoiceParam(name);
    p->setLabels(label, label, label); p->setScriptName(name); p->setHint(hint);
    for (const char* o : opts) p->appendOption(o);
    p->setDefault(def); p->setAnimates(false);
    if (parent) p->setParent(*parent);
    page->addChild(*p);
    return p;
  };
  auto hiddenInt = [&](const char* name) {
    OFX::IntParamDescriptor* p = desc.defineIntParam(name);
    p->setLabels(name, name, name); p->setScriptName(name);
    p->setDefault(0); p->setRange(-1, 1 << 30); p->setDisplayRange(-1, 1 << 30);
    p->setAnimates(false); p->setIsSecret(true); p->setEvaluateOnChange(true);
    page->addChild(*p);
    return p;
  };

  // ---- Plate
  OFX::GroupParamDescriptor* gPlate = group("grpPlate", "Plate", true);
  choice(pPlateIndex, "Active plate", "Which of the 8 plate slots the seed point and buttons apply to.",
         {"Plate 1", "Plate 2", "Plate 3", "Plate 4", "Plate 5", "Plate 6", "Plate 7", "Plate 8"}, 0, gPlate);
  {
    OFX::Double2DParamDescriptor* p = desc.defineDouble2DParam(pSeedPoint);
    p->setLabels("Seed point", "Seed point", "Seed point");
    p->setScriptName(pSeedPoint);
    p->setHint("Click on the licence plate in the viewer (or type the position, 0..1 of the frame). Detection searches around this point.");
    p->setDoubleType(OFX::eDoubleTypePlain);
    p->setDimensionLabels("X", "Y");
    p->setDefault(0.5, 0.5);
    p->setRange(-1, -1, 2, 2);
    p->setDisplayRange(0, 0, 1, 1);
    p->setIncrement(0.001);
    p->setAnimates(false);
    p->setUseHostNativeOverlayHandle(false);
    p->setParent(*gPlate);
    page->addChild(*p);
  }
  button(pDetect, "Detect license plate", "Find the plate around the seed point on the current frame. Adjust the corners in the viewer if needed.", gPlate);
  button(pTrack, "Track license plate", "Track the detected plate through the clip (both directions from the detection frame), handling occlusion, partial occlusion and out-of-frame excursions. A PlateMask window shows each frame as it is solved, with a Stop button.", gPlate);
  choice(pTrackMode, "Tracking mode", "(unused)", {"Auto", "Batch"}, 0, gPlate)->setIsSecret(true);
  button(pStop, "Stop tracking", "(unused)", gPlate)->setIsSecret(true);
  button(pClearPlate, "Clear this plate", "Remove the active plate's detection and track.", gPlate);
  button(pClearAll, "Clear all plates", "Remove every plate.", gPlate);
  boolean(pShowOverlay, "Show overlay", "Draw plate outlines, state labels and corner handles in the viewer.", true, gPlate);
  {
    OFX::StringParamDescriptor* p = desc.defineStringParam(pStatus);
    p->setLabels("Status", "Status", "Status");
    p->setScriptName(pStatus);
    p->setHint("What the plugin did last (detection result, tracking summary).");
    p->setStringType(OFX::eStringTypeMultiLine);
    p->setDefault("Click the plate in the viewer, then press Detect license plate.");
    p->setAnimates(false);
    p->setEvaluateOnChange(false);
    p->setEnabled(false);
    p->setParent(*gPlate);
    page->addChild(*p);
  }
  boolean(pNotify, "Notify when tracking finishes", "Show a message box with the tracking summary when a track completes.", true, gPlate);

  // ---- Tracking
  OFX::GroupParamDescriptor* gTrack = group("grpTracking", "Tracking", false);
  dbl(pSearchRadius, "Search radius", "Detection search radius around the seed, as a fraction of frame width.", 0.12, 0.02, 0.5, gTrack, false);
  choice(pTrackDirection, "Direction", "Tracking direction from the detection frame.", {"Both", "Forward", "Backward"}, 0, gTrack);
  integer(pMaxFrames, "Max frames", "Maximum frames to track per direction.", 3000, 1, 100000, gTrack);
  integer(pOcclusionHold, "Occlusion hold (frames)", "How long a hidden plate is coasted before it is marked lost. The mask keeps covering the predicted position either way.", 90, 1, 5000, gTrack);
  dbl(pGrowPerFrame, "Uncertainty growth (px/frame)", "Extra mask growth per unseen frame while the plate is occluded or out of frame (fail-safe over-coverage).", 1.5, 0, 20, gTrack, false);
  integer(pGapInterp, "Interpolate gaps up to (frames)", "Occlusion gaps shorter than this are bridged by interpolating between the last and next confirmed positions.", 150, 0, 5000, gTrack);
  boolean(pHoldOutside, "Hold mask outside tracked range", "Keep masking with the first/last tracked position before/after the tracked frames.", false, gTrack);

  // ---- Mask
  OFX::GroupParamDescriptor* gMask = group("grpMask", "Mask", true);
  dbl(pExpand, "Expand / contract", "Grow (positive) or shrink (negative) the matte, in pixels.", 4, -64, 128, gMask);
  dbl(pFeather, "Feather", "Soft edge width in pixels.", 6, 0, 128, gMask);
  boolean(pInvert, "Invert mask", "Obfuscate everything except the plates.", false, gMask);
  boolean(pMaskOnly, "Mask only (matte output)", "Output the matte as white-on-black (and in alpha) to drive other effects.", false, gMask);
  boolean(pMaskToAlpha, "Write mask to alpha", "Keep the picture but put the matte in the alpha channel.", false, gMask);

  // ---- Obfuscation
  OFX::GroupParamDescriptor* gObf = group("grpObf", "Obfuscation", true);
  choice(pObfMode, "Mode", "How the plate is destroyed.", {"Pixelize", "Blur", "Randomize pixels", "None (mask only)"}, 0, gObf);
  dbl(pBlurRadius, "Blur radius", "Blur radius in pixels.", 24, 0, 256, gObf);
  integer(pBlockSize, "Block size", "Mosaic / randomiser block size in pixels.", 24, 2, 256, gObf, true);
  choice(pRandomMode, "Randomize style", "Shuffle: permute pixels inside each block. Noise: noise around each block's mean. Blocks: random flat colour per block.", {"Shuffle", "Noise", "Blocks"}, 0, gObf);
  boolean(pDestructive, "Destructive lock (irreversible)", "Forces quantisation, dithering, noise and a per-frame jittered grid so the output cannot be de-pixelated, deconvolved or averaged back. Keep this on for anonymisation.", true, gObf);
  integer(pQuantLevels, "Quantisation levels", "Colour levels kept per channel under the destructive lock.", 12, 2, 64, gObf);
  dbl(pNoiseAmount, "Noise amount", "Random noise amplitude added under the destructive lock.", 0.04, 0, 0.5, gObf, false);

  // ---- hidden state
  {
    OFX::StringParamDescriptor* p = desc.defineStringParam(pInstanceId);
    p->setLabels(pInstanceId, pInstanceId, pInstanceId); p->setScriptName(pInstanceId);
    p->setDefault(""); p->setAnimates(false); p->setIsSecret(true); p->setEvaluateOnChange(false);
    page->addChild(*p);
  }
  {
    OFX::StringParamDescriptor* p = desc.defineStringParam(pTrackData);
    p->setLabels(pTrackData, pTrackData, pTrackData); p->setScriptName(pTrackData);
    p->setStringType(OFX::eStringTypeMultiLine);
    p->setDefault(""); p->setAnimates(false); p->setIsSecret(true); p->setEvaluateOnChange(false);
    page->addChild(*p);
  }
  hiddenInt(pDetectRequest);
  hiddenInt(pTrackRequest);
  hiddenInt(pClearRequest);
  hiddenInt(pStopRequest);
  hiddenInt(pRequestFrame);
  hiddenInt(pRequestPlate);
  hiddenInt(pRevision);
}

OFX::ImageEffect* PlateMaskPluginFactory::createInstance(OfxImageEffectHandle handle, OFX::ContextEnum) {
  return new PlateMaskPlugin(handle);
}

void OFX::Plugin::getPluginIDs(OFX::PluginFactoryArray& arr) {
  static PlateMaskPluginFactory factory;
  arr.push_back(&factory);
}
