// Per-plate tracker: LK flow + similarity fit + template NCC + visibility state machine,
// and the sequence driver that runs it bidirectionally and fills gaps.
#pragma once
#include <map>
#include <vector>
#include <climits>
#include "Image.h"
#include "LK.h"
#include "Detector.h"

namespace pm {

// Inferred: the plate itself is hidden (bush, pole, frame edge) but the surrounding car body is
// tracked, so the position is estimated from the body's motion rather than coasted.
enum class VisState { Seed = 0, Confirmed = 1, Partial = 2, Occluded = 3, OutOfFrame = 4, Lost = 5, Manual = 6, Inferred = 7 };
inline bool isVisible(VisState s) {
  return s == VisState::Seed || s == VisState::Confirmed || s == VisState::Partial || s == VisState::Manual;
}
// Position backed by image evidence (plate or car body), as opposed to a pure prediction.
inline bool isEstimated(VisState s) { return isVisible(s) || s == VisState::Inferred; }
inline bool isAnchor(VisState s) { return s == VisState::Seed || s == VisState::Manual; }
const char* visStateName(VisState s);

struct Key {
  Quad q;                       // canonical coordinates
  VisState st = VisState::Confirmed;
  double conf = 0;              // template NCC (or 1 for anchors)
  double grow = 0;              // extra mask growth (canonical px) from uncertainty
};

struct TrackerParams {
  double supportConfirm = 0.5;
  double supportPartial = 0.25;
  double nccConfirm = 0.55;
  double nccPartial = 0.35;
  double reacqNcc = 0.6;
  int maxAge = 90;              // frames unseen before Occluded -> Lost
  int redetectEvery = 3;
  double growPerFrame = 1.5;    // canonical px per unseen frame
  double growMax = 40;
  double templateAlpha = 0.05;
  int gapInterpMax = 150;
  // Context (car body) tracking around the plate, in plate units: the region spans
  // [-ctxX, 1+ctxX] plate widths horizontally and [-ctxY, 1+ctxY] plate heights vertically.
  double ctxX = 1.2, ctxY = 2.5;
  int ctxMinInliers = 8;
  double ctxMinSupport = 0.35;
  DetectParams detect;
};

class PlateTracker {
 public:
  explicit PlateTracker(const TrackerParams& p) : p_(p) {}
  void init(const GrayFrame& f, const Quad& qCanon, VisState st = VisState::Seed);
  void resetTo(const GrayFrame& f, const Quad& qCanon);  // manual anchor
  // Continue a stored track from frame `f` whose key is `k` (template from the store).
  void resume(const GrayFrame& f, const Key& k, const std::vector<float>& templ);
  Key step(const GrayFrame& prev, const GrayFrame& cur);
  const Quad& quad() const { return q_; }
  VisState state() const { return st_; }
  const std::vector<float>& templ() const { return tmpl_; }
  void setTemplate(const std::vector<float>& t) { if ((int)t.size() == tw_ * th_) tmpl_ = t; }
  static const int kTW = 96, kTH = 24;

 private:
  void sampleTemplate(const Gray& g, const Quad& qg, std::vector<float>& out) const;
  double ncc(const Gray& g, const Quad& qg) const;
  void searchTemplate(const Gray& g, const Quad& qg, double range, Quad& best, double& bestNcc) const;
  double outsideFraction(const GrayFrame& f, const Quad& qCanon) const;
  // Track the car body around `qCanon` from prev to cur; on success `S` maps prev gray coords to cur.
  // `platePrev2Cur` (optional) is the plate's own motion this frame; when given, cells whose motion
  // agrees with it are learned as "car" cells so that later, with the plate hidden, only the car is fitted.
  // Returns the predicted plate quad (canonical) in `predCanon`. With the plate hidden it tracks the
  // body relative to the anchor image (last confirmed frame) so errors do not accumulate frame to frame.
  bool trackContext(const GrayFrame& prev, const Pyramid& curPyr, const GrayFrame& cur, const Quad& qCanon,
                    const Affine* platePrev2Cur, Quad& predCanon, double& support, int& inliers);
  bool trackContextFrom(const Pyramid& basePyr, const Quad& baseGrayQ, const Quad& guessGrayQ, const Pyramid& curPyr,
                        const GrayFrame& cur, bool learn, const Affine* platePrev2Cur, Quad& predCanon, double& support,
                        int& inliers);
  Pyramid anchorPyr_;
  Quad anchorGrayQ_{};
  int anchorFrame_ = INT_MIN;
  bool anchorValid_ = false;
  static const int kCtxNU = 12, kCtxNV = 9;
  std::vector<double> ctxW_;   // per-cell car membership (0..1), learned while the plate is visible
  int ctxLearned_ = 0;         // number of frames the membership has been updated

  TrackerParams p_;
  Quad q_{};
  Pt vel_{};
  VisState st_ = VisState::Seed;
  int unseen_ = 0;
  double grow_ = 0;
  std::vector<float> tmpl_;
  int tw_ = kTW, th_ = kTH;
  Pyramid prevPyr_;
  int prevPyrFrame_ = INT_MIN;
};

struct FrameProvider {
  virtual ~FrameProvider() {}
  virtual bool fetch(int frame, GrayFrame& out) = 0;
  virtual bool aborted() { return false; }
  virtual void progress(double /*0..1*/) {}
  // Called after each frame is solved (also for anchors), with the solved key.
  virtual void onFrame(const GrayFrame& /*f*/, const Key& /*k*/, int /*done*/, int /*total*/) {}
};

struct RunOptions {
  int seedFrame = 0;
  int startFrame = 0, endFrame = 0;   // inclusive clip range
  int direction = 2;                  // 0 forward, 1 backward, 2 both
  int maxFrames = 3000;               // per direction
  TrackerParams tp;
};

// keys must contain an anchor at seedFrame. Auto keys within the run range are replaced;
// anchors (Seed/Manual) are respected and re-anchor the tracker. Returns false if aborted/failed.
bool runTracking(FrameProvider& fp, const RunOptions& opt, std::map<int, Key>& keys, std::vector<float>* templOut);

// Replace predicted quads inside bounded occlusion/out-of-frame gaps by interpolation (RTS-style smoothing lite).
void fillGaps(std::map<int, Key>& keys, const TrackerParams& tp);

}  // namespace pm
