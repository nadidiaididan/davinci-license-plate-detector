// Seed-gated classical licence-plate detector (vertical stroke density -> blob -> oriented quad).
#pragma once
#include <vector>
#include "Image.h"

namespace pm {

struct DetectParams {
  double aspectMin = 1.6;          // w/h lower bound (motorcycle two-line plates are ~1.3; they are a secondary target)
  double aspectMax = 7.5;
  double preferAspect[2] = {3.75, 4.55};  // Swiss front 300x80, Swiss rear 500x110
  double minHeightPx = 6;
  double maxHeightFrac = 0.6;      // relative to ROI height
  double expandX = 0.06;           // text box -> plate: fraction of text width per side
  double expandY = 0.32;           // fraction of text height per side
  bool snapEdges = true;
};

struct Detection {
  Quad q;               // gray-pixel coordinates
  double score = 0;     // higher is better
  double aspect = 0;
};

// Detect the plate nearest `seed` (gray pixels) within `radius` pixels. Returns false if nothing plausible.
bool detectAtSeed(const Gray& g, const Pt& seed, double radius, const DetectParams& prm, Detection& out);

// Enumerate plausible plate candidates inside `roi` (gray pixels), best first, at most maxN.
std::vector<Detection> detectCandidates(const Gray& g, const Rect& roi, const DetectParams& prm, int maxN,
                                        const Pt* seed = nullptr);

}  // namespace pm
