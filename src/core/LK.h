// Pyramidal Lucas-Kanade sparse optical flow.
#pragma once
#include <vector>
#include "Image.h"

namespace pm {

struct Pyramid {
  std::vector<Gray> levels;  // level 0 = full res
};
Pyramid buildPyramid(const Gray& g, int levels);

struct LKResult {
  Pt pt;          // tracked position in the next image (level-0 pixels)
  bool ok = false;
  float err = 1;  // mean absolute residual over the window (0..1 intensities)
};

// Track `pts` from prev to next. halfWin: half window size (pixels), iters: max iterations per level.
// `init` (optional) gives an initial guess of each point's position in `next` (level-0 pixels).
void lkTrack(const Pyramid& prev, const Pyramid& next, const std::vector<Pt>& pts, std::vector<LKResult>& out,
             int halfWin = 7, int iters = 25, const std::vector<Pt>* init = nullptr);

}  // namespace pm
