#include "Tracker.h"
#include <cmath>
#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace pm {
static const bool kDebug = getenv("PM_DEBUG") != nullptr;

const char* visStateName(VisState s) {
  switch (s) {
    case VisState::Seed: return "seed";
    case VisState::Confirmed: return "confirmed";
    case VisState::Partial: return "partial";
    case VisState::Occluded: return "occluded";
    case VisState::OutOfFrame: return "out_of_frame";
    case VisState::Lost: return "lost";
    case VisState::Manual: return "manual";
    case VisState::Inferred: return "inferred";
  }
  return "?";
}

void PlateTracker::sampleTemplate(const Gray& g, const Quad& qg, std::vector<float>& out) const {
  out.resize((size_t)tw_ * th_);
  double mean = 0;
  for (int j = 0; j < th_; ++j)
    for (int i = 0; i < tw_; ++i) {
      double u = (i + 0.5) / tw_, v = (j + 0.5) / th_;
      Pt p = qg[0] * ((1 - u) * (1 - v)) + qg[1] * (u * (1 - v)) + qg[2] * (u * v) + qg[3] * ((1 - u) * v);
      float s = g.bilinear(p.x, p.y);
      out[(size_t)j * tw_ + i] = s;
      mean += s;
    }
  mean /= out.size();
  double var = 0;
  for (float& s : out) { s -= (float)mean; var += s * s; }
  double sd = std::sqrt(var / out.size());
  float inv = sd > 1e-4 ? (float)(1.0 / sd) : 0.f;
  for (float& s : out) s *= inv;
}

double PlateTracker::ncc(const Gray& g, const Quad& qg) const {
  if (tmpl_.empty()) return 0;
  std::vector<float> cur;
  sampleTemplate(g, qg, cur);
  double dot = 0;
  for (size_t i = 0; i < cur.size(); ++i) dot += cur[i] * tmpl_[i];
  return dot / cur.size();
}

void PlateTracker::searchTemplate(const Gray& g, const Quad& qg, double range, Quad& best, double& bestNcc) const {
  double H = std::max(2.0, quadShortSide(qg));
  double step = std::max(1.0, H / 6.0);
  best = qg;
  bestNcc = -1;
  Pt c = quadCenter(qg);
  auto eval = [&](double dx, double dy) {
    Quad cand = quadTranslate(qg, {dx, dy});
    Pt cc = c + Pt{dx, dy};
    if (!g.inside(cc.x, cc.y, 1)) return;
    double n = ncc(g, cand);
    if (n > bestNcc) { bestNcc = n; best = cand; }
  };
  for (double dy = -range; dy <= range; dy += step)
    for (double dx = -range; dx <= range; dx += step) eval(dx, dy);
  // refine
  Pt bc = quadCenter(best) - c;
  for (double s = step / 2; s >= 0.5; s /= 2) {
    Pt base = bc;
    for (int dy = -1; dy <= 1; ++dy)
      for (int dx = -1; dx <= 1; ++dx)
        if (dx || dy) eval(base.x + dx * s, base.y + dy * s);
    bc = quadCenter(best) - c;
  }
}

double PlateTracker::outsideFraction(const GrayFrame& f, const Quad& qCanon) const {
  Quad qg = f.toGray(qCanon);
  Rect fr{0, 0, (double)f.g.w, (double)f.g.h};
  std::vector<Pt> poly(qg.begin(), qg.end());
  std::vector<Pt> frame{{fr.x1, fr.y1}, {fr.x2, fr.y1}, {fr.x2, fr.y2}, {fr.x1, fr.y2}};
  double a = quadArea(qg);
  if (a < 1e-6) return 1.0;
  double inside = polyArea(clipConvex(poly, frame));
  return 1.0 - std::clamp(inside / a, 0.0, 1.0);
}

bool PlateTracker::trackContextFrom(const Pyramid& basePyr, const Quad& qg, const Quad& guessQ, const Pyramid& curPyr,
                                    const GrayFrame& cur, bool learn, const Affine* platePrev2Cur, Quad& predCanon,
                                    double& support, int& inliers) {
  support = 0; inliers = 0;
  if ((int)ctxW_.size() != kCtxNU * kCtxNV) { ctxW_.assign(kCtxNU * kCtxNV, 0.5); ctxLearned_ = 0; }
  const Gray& base = basePyr.levels.empty() ? cur.g : basePyr.levels[0];
  Pt u = qg[1] - qg[0], v = qg[3] - qg[0];  // plate axes in base gray pixels
  double Hg = std::max(2.0, quadShortSide(qg));
  // initial guess: similarity mapping the base quad onto the guessed current quad
  Affine Sguess;
  {
    std::vector<Pt> a(qg.begin(), qg.end()), b(guessQ.begin(), guessQ.end());
    std::vector<double> w(4, 1.0);
    if (!fitSimilarity(a, b, w, Sguess)) Sguess = Affine{};
  }
  std::vector<Pt> pts, init;
  std::vector<int> cell;
  int candidates = 0;
  for (int j = 0; j < kCtxNV; ++j)
    for (int i = 0; i < kCtxNU; ++i) {
      double a = -p_.ctxX + (1 + 2 * p_.ctxX) * (i + 0.5) / kCtxNU;
      double b = -p_.ctxY + (1 + 2 * p_.ctxY) * (j + 0.5) / kCtxNV;
      if (a > -0.15 && a < 1.15 && b > -0.3 && b < 1.3) continue;  // skip the plate itself
      Pt g = qg[0] + u * a + v * b;
      Pt gi = Sguess.apply(g);
      if (!base.inside(g.x, g.y, 10) || !cur.g.inside(gi.x, gi.y, 10)) continue;
      pts.push_back(g);
      init.push_back(gi);
      cell.push_back(j * kCtxNU + i);
      if (ctxW_[j * kCtxNU + i] >= 0.4) ++candidates;
    }
  if ((int)pts.size() < 6) return false;
  int halfWin = (int)std::clamp(Hg / 4.0, 4.0, 9.0);
  std::vector<LKResult> res;
  lkTrack(basePyr, curPyr, pts, res, halfWin, 30, &init);
  const double thr = std::max(1.0, 0.05 * Hg);
  const bool learned = ctxLearned_ >= 5;
  std::vector<double> w(pts.size(), 0.0);
  std::vector<Pt> dst(pts.size());
  int tracked = 0;
  for (size_t i = 0; i < pts.size(); ++i) {
    dst[i] = res[i].pt;
    if (res[i].ok && res[i].err < 0.12) { w[i] = learned ? ctxW_[cell[i]] : 1.0; ++tracked; }
  }
  if (learn && platePrev2Cur) {
    for (size_t i = 0; i < pts.size(); ++i) {
      if (!(res[i].ok && res[i].err < 0.12)) continue;
      double r = (platePrev2Cur->apply(pts[i]) - dst[i]).norm();
      double& cw = ctxW_[cell[i]];
      cw = 0.75 * cw + 0.25 * (r < thr ? 1.0 : 0.0);
    }
    ++ctxLearned_;
  }
  if (tracked < 6) return false;
  Affine S;
  if (!fitSimilarity(pts, dst, w, S)) return false;
  for (int it = 0; it < 4; ++it) {
    int inl = 0;
    for (size_t i = 0; i < pts.size(); ++i) {
      if (!(res[i].ok && res[i].err < 0.12)) { w[i] = 0; continue; }
      bool in = (S.apply(pts[i]) - dst[i]).norm() < thr;
      w[i] = in ? (learned ? std::max(0.05, ctxW_[cell[i]]) : 1.0) : 0.0;
      inl += (in && (!learned || ctxW_[cell[i]] >= 0.4)) ? 1 : 0;
    }
    if (inl < 4) return false;
    if (!fitSimilarity(pts, dst, w, S)) return false;
    inliers = inl;
  }
  double sc = std::sqrt(std::fabs(S.a * S.d - S.b * S.c));
  bool ok;
  if (learned) {
    support = candidates > 0 ? (double)inliers / candidates : 0.0;
    ok = inliers >= 6 && support >= 0.15;
  } else {
    support = (double)inliers / tracked;
    ok = inliers >= p_.ctxMinInliers && support >= p_.ctxMinSupport;
  }
  ok = ok && sc > 0.6 && sc < 1.6;
  if (kDebug) fprintf(stderr, "  [f%d] context%s pts=%zu tracked=%d cand=%d inliers=%d support=%.2f scale=%.3f ok=%d\n", cur.frame, learn ? "" : "(anchor)", pts.size(), tracked, candidates, inliers, support, sc, (int)ok);
  if (ok) predCanon = cur.toCanon(S.apply(qg));
  return ok;
}

bool PlateTracker::trackContext(const GrayFrame& prev, const Pyramid& curPyr, const GrayFrame& cur, const Quad& qCanon,
                                const Affine* platePrev2Cur, Quad& predCanon, double& support, int& inliers) {
  const Quad guess = cur.toGray(quadTranslate(qCanon, vel_));
  // Plate hidden: measure against the anchor image so the estimate does not drift.
  if (!platePrev2Cur && anchorValid_ && anchorFrame_ != cur.frame) {
    if (trackContextFrom(anchorPyr_, anchorGrayQ_, guess, curPyr, cur, false, nullptr, predCanon, support, inliers))
      return true;
  }
  return trackContextFrom(prevPyr_, prev.toGray(qCanon), guess, curPyr, cur, true, platePrev2Cur, predCanon, support, inliers);
}

void PlateTracker::init(const GrayFrame& f, const Quad& qCanon, VisState st) {
  q_ = qCanon;
  vel_ = {0, 0};
  st_ = st;
  unseen_ = 0;
  grow_ = 0;
  sampleTemplate(f.g, f.toGray(qCanon), tmpl_);
  prevPyr_ = buildPyramid(f.g, 4);
  prevPyrFrame_ = f.frame;
  ctxW_.assign(kCtxNU * kCtxNV, 0.5);
  ctxLearned_ = 0;
  anchorPyr_ = prevPyr_;
  anchorGrayQ_ = f.toGray(qCanon);
  anchorFrame_ = f.frame;
  anchorValid_ = true;
}

void PlateTracker::resetTo(const GrayFrame& f, const Quad& qCanon) {
  Pt oldC = quadCenter(q_);
  q_ = qCanon;
  if (isVisible(st_)) vel_ = (quadCenter(q_) - oldC) * 0.5;
  st_ = VisState::Manual;
  unseen_ = 0;
  grow_ = 0;
  std::vector<float> t;
  sampleTemplate(f.g, f.toGray(qCanon), t);
  if (tmpl_.empty()) tmpl_ = t;
  else for (size_t i = 0; i < t.size(); ++i) tmpl_[i] = 0.5f * tmpl_[i] + 0.5f * t[i];
  prevPyr_ = buildPyramid(f.g, 4);
  prevPyrFrame_ = f.frame;
  anchorPyr_ = prevPyr_;
  anchorGrayQ_ = f.toGray(qCanon);
  anchorFrame_ = f.frame;
  anchorValid_ = true;
}

void PlateTracker::resume(const GrayFrame& f, const Key& k, const std::vector<float>& templ) {
  q_ = k.q;
  st_ = k.st;
  vel_ = {0, 0};
  unseen_ = isVisible(k.st) ? 0 : 1;
  grow_ = k.grow;
  if ((int)templ.size() == tw_ * th_) tmpl_ = templ;
  else sampleTemplate(f.g, f.toGray(k.q), tmpl_);
  prevPyr_ = buildPyramid(f.g, 4);
  prevPyrFrame_ = f.frame;
  if ((int)ctxW_.size() != kCtxNU * kCtxNV) { ctxW_.assign(kCtxNU * kCtxNV, 0.5); ctxLearned_ = 0; }
  anchorValid_ = isVisible(k.st);
  if (anchorValid_) { anchorPyr_ = prevPyr_; anchorGrayQ_ = f.toGray(k.q); anchorFrame_ = f.frame; }
}

Key PlateTracker::step(const GrayFrame& prev, const GrayFrame& cur) {
  if (prevPyrFrame_ != prev.frame) { prevPyr_ = buildPyramid(prev.g, 4); prevPyrFrame_ = prev.frame; }
  Pyramid curPyr = buildPyramid(cur.g, 4);

  const bool visibleBefore = isVisible(st_);
  const Pt oldC = quadCenter(q_);
  const double Hc = std::max(1.0, quadShortSide(q_));  // canonical
  const double scale = cur.scale();

  bool haveCand = false;
  Quad cand;
  double candNcc = -1, support = 0;
  Affine Splate;
  bool plateFit = false;

  // 1. Optical flow of the plate itself from the previous position (only meaningful if it was visible).
  if (visibleBefore) {
    Quad qg = prev.toGray(q_);
    std::vector<Pt> pts;
    const int nu = 7, nv = 3;
    for (int j = 0; j < nv; ++j)
      for (int i = 0; i < nu; ++i) {
        double u = 0.1 + 0.8 * (i + 0.5) / nu, v = 0.15 + 0.7 * (j + 0.5) / nv;
        pts.push_back(qg[0] * ((1 - u) * (1 - v)) + qg[1] * (u * (1 - v)) + qg[2] * (u * v) + qg[3] * ((1 - u) * v));
      }
    double Hg = std::max(2.0, quadShortSide(qg));
    int halfWin = (int)std::clamp(Hg / 4.0, 4.0, 12.0);
    std::vector<LKResult> res;
    lkTrack(prevPyr_, curPyr, pts, res, halfWin, 25);
    std::vector<double> w(pts.size(), 0.0);
    std::vector<Pt> dst(pts.size());
    int good = 0;
    for (size_t i = 0; i < pts.size(); ++i) {
      dst[i] = res[i].pt;
      if (res[i].ok && res[i].err < 0.12) { w[i] = 1; ++good; }
    }
    Affine S;
    bool fitOk = good >= 4 && fitSimilarity(pts, dst, w, S);
    double thr = std::max(1.5, 0.06 * Hg);
    for (int it = 0; it < 3 && fitOk; ++it) {
      int inl = 0;
      for (size_t i = 0; i < pts.size(); ++i) {
        if (!res[i].ok) { w[i] = 0; continue; }
        double r = (S.apply(pts[i]) - dst[i]).norm();
        w[i] = r < thr ? 1.0 : 0.0;
        inl += w[i] > 0;
      }
      if (inl < 4) { fitOk = false; break; }
      fitOk = fitSimilarity(pts, dst, w, S);
    }
    if (fitOk) {
      int inl = 0;
      for (double v : w) inl += v > 0;
      support = (double)inl / pts.size();
      double sc = std::sqrt(std::fabs(S.a * S.d - S.b * S.c));
      if (support >= p_.supportPartial && sc > 0.6 && sc < 1.6) {
        Quad cg = S.apply(qg);
        cand = cur.toCanon(cg);
        candNcc = ncc(cur.g, cur.toGray(cand));
        haveCand = true;
        Splate = S;
        plateFit = support >= p_.supportConfirm && candNcc >= p_.nccConfirm;
      }
    }
  }

  // 2. Car-body (context) motion: the best estimate of where the plate went whether or not the plate
  // itself can be seen. Learns which cells are the car while the plate is reliably tracked; falls
  // back to constant velocity when too little of the body is trackable.
  Quad ctxPred;
  double ctxSupport = 0;
  int ctxInliers = 0;
  const bool ctxOk = trackContext(prev, curPyr, cur, q_, plateFit ? &Splate : nullptr, ctxPred, ctxSupport, ctxInliers);
  const Quad pred = ctxOk ? ctxPred : quadTranslate(q_, vel_);
  if (!haveCand) cand = pred;

  // 3. Template search around the prediction (LK failed, or plate was hidden and may be back).
  if (!haveCand || candNcc < p_.nccPartial) {
    Quad pg = cur.toGray(pred);
    Pt pc = quadCenter(pg);
    double Hg = std::max(2.0, quadShortSide(pg));
    if (cur.g.inside(pc.x, pc.y, -Hg * 1.5)) {
      Quad best; double bn;
      searchTemplate(cur.g, pg, 2.0 * Hg, best, bn);
      double need = visibleBefore ? p_.nccPartial : p_.reacqNcc;
      if (bn >= need && bn > candNcc) {
        cand = cur.toCanon(best);
        candNcc = bn;
        support = 1.0;
        haveCand = true;
      }
    }
  }

  // 4. Re-detection: local around the prediction, or whole frame when out of frame / lost.
  if ((!haveCand || candNcc < p_.nccPartial) && !visibleBefore && (unseen_ % p_.redetectEvery) == 0) {
    Quad pg = cur.toGray(pred);
    Pt pc = quadCenter(pg);
    double Hg = std::max(2.0, quadShortSide(pg));
    Rect roi;
    bool inside = cur.g.inside(pc.x, pc.y, -Hg);
    bool wide = !inside || st_ == VisState::Lost || (st_ == VisState::OutOfFrame && !ctxOk) || (unseen_ > 12 && !ctxOk);
    if (!wide) roi = {pc.x - 6 * Hg * 3, pc.y - 6 * Hg, pc.x + 6 * Hg * 3, pc.y + 6 * Hg};
    else roi = {0, 0, (double)cur.g.w, (double)cur.g.h};
    DetectParams dp = p_.detect;
    dp.minHeightPx = std::max(dp.minHeightPx, Hg * 0.5);
    std::vector<Detection> cands = detectCandidates(cur.g, roi, dp, 8);
    double lng0 = quadLongSide(pg);
    if (kDebug) fprintf(stderr, "  [f%d] redetect wide=%d roi=(%.0f,%.0f,%.0f,%.0f) cands=%zu lng0=%.1f pc=(%.0f,%.0f)\n", cur.frame, (int)wide, roi.x1, roi.y1, roi.x2, roi.y2, cands.size(), lng0, pc.x, pc.y);
    for (const Detection& d : cands) {
      double lng = quadLongSide(d.q);
      if (kDebug) fprintf(stderr, "     cand c=(%.0f,%.0f) lng=%.1f score=%.2f\n", quadCenter(d.q).x, quadCenter(d.q).y, lng, d.score);
      if (lng < 0.5 * lng0 || lng > 2.0 * lng0) continue;
      // Score two hypotheses: the detector's own quad (follows the current scale/orientation) and the
      // last tracked shape centred on the detection. Each is aligned to the template before scoring.
      Quad shaped = quadTranslate(pg, quadCenter(d.q) - pc);
      Quad refA, refB; double nA, nB;
      searchTemplate(cur.g, d.q, 0.4 * Hg, refA, nA);
      searchTemplate(cur.g, shaped, 0.4 * Hg, refB, nB);
      Quad refined = nA >= nB ? refA : refB;
      double n = std::max(nA, nB);
      if (kDebug) fprintf(stderr, "     refined ncc=%.2f/%.2f at (%.0f,%.0f)\n", nA, nB, quadCenter(refined).x, quadCenter(refined).y);
      double need = d.score >= 0.7 ? std::min(p_.reacqNcc, p_.nccPartial + 0.1) : p_.reacqNcc;
      if (n > candNcc && n >= need) { candNcc = n; cand = cur.toCanon(refined); haveCand = true; support = 1.0; }
    }
  }

  Key out;
  bool seen = haveCand && candNcc >= p_.nccPartial && support >= p_.supportPartial;
  if (seen) {
    bool confirmed = candNcc >= p_.nccConfirm && support >= p_.supportConfirm;
    if (!confirmed && ctxOk) cand = quadLerp(cand, pred, 0.5);  // weak plate match: lean on the body
    Pt newC = quadCenter(cand);
    if (visibleBefore) vel_ = vel_ * 0.6 + (newC - oldC) * 0.4;
    else vel_ = (newC - oldC) * (1.0 / (unseen_ + 1));
    q_ = cand;
    st_ = confirmed ? VisState::Confirmed : VisState::Partial;
    unseen_ = 0;
    grow_ = 0;
    if (confirmed && candNcc > 0.75) {
      std::vector<float> t;
      sampleTemplate(cur.g, cur.toGray(cand), t);
      for (size_t i = 0; i < t.size(); ++i)
        tmpl_[i] = (float)((1 - p_.templateAlpha) * tmpl_[i] + p_.templateAlpha * t[i]);
    }
    if (confirmed && candNcc > 0.6) {
      anchorPyr_ = curPyr;
      anchorGrayQ_ = cur.toGray(cand);
      anchorFrame_ = cur.frame;
      anchorValid_ = true;
    }
  } else {
    q_ = pred;
    unseen_++;
    double outF = outsideFraction(cur, q_);
    if (ctxOk) {
      // Plate hidden but the car body is tracked: follow it, with modest fail-safe growth.
      vel_ = quadCenter(q_) - oldC;
      grow_ = std::min(0.5 * p_.growMax, grow_ + 0.5 * p_.growPerFrame);
      st_ = outF > 0.5 ? VisState::OutOfFrame : VisState::Inferred;
      unseen_ = std::min(unseen_, p_.maxAge / 2);  // body evidence keeps the track from going Lost
      candNcc = ctxSupport;
    } else {
      vel_ = vel_ * 0.92;
      grow_ = std::min(p_.growMax, grow_ + p_.growPerFrame);
      if (outF > 0.5) st_ = VisState::OutOfFrame;
      else if (unseen_ > p_.maxAge) st_ = VisState::Lost;
      else st_ = VisState::Occluded;
      candNcc = std::max(0.0, candNcc);
    }
    // keep the prediction from drifting to infinity while out of frame
    Pt c = quadCenter(q_);
    double W = cur.g.w / scale, H = cur.g.h / scale;
    double lim = 2.0 * Hc + 0.25 * std::max(W, H);
    Pt cl{std::clamp(c.x, -lim, W + lim), std::clamp(c.y, -lim, H + lim)};
    if (cl.x != c.x || cl.y != c.y) { q_ = quadTranslate(q_, cl - c); vel_ = {0, 0}; }
  }
  out.q = q_;
  out.st = st_;
  out.conf = candNcc;
  out.grow = grow_;
  prevPyr_ = std::move(curPyr);
  prevPyrFrame_ = cur.frame;
  return out;
}

bool runTracking(FrameProvider& fp, const RunOptions& opt, std::map<int, Key>& keys, std::vector<float>* templOut) {
  auto anchorIt = keys.find(opt.seedFrame);
  if (anchorIt == keys.end() || !isAnchor(anchorIt->second.st)) return false;
  int total = 0;
  if (opt.direction != 1) total += std::min(opt.maxFrames, std::max(0, opt.endFrame - opt.seedFrame));
  if (opt.direction != 0) total += std::min(opt.maxFrames, std::max(0, opt.seedFrame - opt.startFrame));
  int done = 0;

  auto runDir = [&](int dir) -> bool {
    GrayFrame prev;
    if (!fp.fetch(opt.seedFrame, prev)) return false;
    PlateTracker tr(opt.tp);
    tr.init(prev, keys[opt.seedFrame].q, keys[opt.seedFrame].st);
    int f = opt.seedFrame;
    for (int n = 0; n < opt.maxFrames; ++n) {
      f += dir;
      if (f < opt.startFrame || f > opt.endFrame) break;
      if (fp.aborted()) return false;
      GrayFrame cur;
      if (!fp.fetch(f, cur)) break;
      auto it = keys.find(f);
      if (it != keys.end() && isAnchor(it->second.st)) {
        tr.resetTo(cur, it->second.q);
      } else {
        keys[f] = tr.step(prev, cur);
      }
      ++done;
      fp.onFrame(cur, keys[f], done, total);
      prev = std::move(cur);
      if (total > 0) fp.progress((double)done / total);
    }
    if (templOut) *templOut = tr.templ();
    return true;
  };
  if (opt.direction != 1 && !runDir(+1)) return false;
  if (opt.direction != 0 && !runDir(-1)) return false;
  fillGaps(keys, opt.tp);
  return true;
}

void fillGaps(std::map<int, Key>& keys, const TrackerParams& tp) {
  if (keys.size() < 3) return;
  std::vector<int> frames;
  for (auto& kv : keys) frames.push_back(kv.first);
  size_t i = 0;
  while (i < frames.size()) {
    if (isEstimated(keys[frames[i]].st)) { ++i; continue; }
    size_t j = i;
    while (j < frames.size() && !isEstimated(keys[frames[j]].st)) ++j;
    // gap = [i, j)
    bool boundedL = i > 0 && frames[i - 1] == frames[i] - 1;
    bool boundedR = j < frames.size() && frames[j] == frames[j - 1] + 1;
    int len = (int)(j - i);
    if (boundedL && boundedR && len <= tp.gapInterpMax) {
      const Key& A = keys[frames[i - 1]];
      const Key& B = keys[frames[j]];
      for (size_t k = i; k < j; ++k) {
        double t = (double)(k - i + 1) / (len + 1);
        Key& K = keys[frames[k]];
        K.q = quadLerp(A.q, B.q, t);
        int distToVisible = (int)std::min(k - i + 1, j - k);
        K.grow = std::min(tp.growMax, tp.growPerFrame * distToVisible);
      }
    }
    i = j;
  }
}

}  // namespace pm
