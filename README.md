# PlateMask — licence-plate detection, tracking and destructive masking for DaVinci Resolve

An OpenFX plugin for DaVinci Resolve Studio on macOS (Apple silicon + Intel universal binary),
Windows (x64) and Linux (x64). Verified end to end inside Resolve 21.0 on macOS; the Windows and
Linux builds are produced and tested on real machines by CI (see *What was verified*). Click a licence plate in the viewer, press **Detect license plate**, check the
box, press **Track license plate**. The plate is followed through the clip — including partial and
full occlusion, leaving the frame and coming back — and destroyed with an irreversible blur, mosaic
or pixel randomiser. Swiss plate formats (300×80 front, 500×110 rear) are the priority aspect
ratios; up to 8 plates per effect instance; the matte can be exported to drive other effects.

The design decomposition (epistemologies, axioms, fractals, factors) is in
[`spec/decomposition.json`](spec/decomposition.json).

## Install (release downloads)

Grab the zip for your platform from the GitHub release page. Resolve **Studio** is required on
every platform (the free edition does not load third-party OpenFX plugins).

- **macOS**: run `install.sh` (asks for your password and clears the download-quarantine flag), or
  open the `.pkg`. Installs to `/Library/OFX/Plugins`.
- **Windows**: right-click `install.bat` → *Run as administrator*. Installs to
  `C:\Program Files\Common Files\OFX\Plugins`. The DLL is self-contained (static C runtime).
- **Linux**: `sudo ./install.sh`. Installs to `/usr/OFX/Plugins`.

Restart Resolve afterwards. The effect is under *OpenFX › Filters › PlateMask › License Plate Mask*.

## Build from source

No dependencies beyond a C++17 compiler and CMake; the OpenFX headers and C++ Support library are
vendored under `third_party/openfx`.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release   # Windows: add -A x64 (Visual Studio 2022)
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
```

`ctest` runs the core algorithm tests and the **mini host** (`tests/minihost.cpp`), a minimal
OpenFX host that loads the built plugin binary exactly as Resolve would, runs describe / instance
creation / detection / tracking / render on the synthetic clip and checks the result. The bundle
lands in `build/PlateMask.ofx.bundle` with the platform sub-folder Resolve expects (`MacOS`,
`Win64`, `Linux-x86-64`).

On macOS the plain `Makefile` still works: `make`, `make test`, `make cli`, `sudo make install`,
`make dist` (zip + pkg). For development without installing, Resolve honours the OFX search path:
`scripts/run_resolve_dev.sh` launches it with the build folder on `OFX_PLUGIN_PATH`.

## Usage in Resolve

The effect is listed as **PlateMask → License Plate Mask** in the OpenFX library (Edit page
inspector, Color page node, Fusion page node).

1. Park on a frame where the plate is clearly visible.
2. With the effect selected and the viewer in OpenFX overlay mode, **click on the plate**. This sets
   the *Seed point* (stored normalised 0..1; you can also type it).
3. Press **Detect license plate**. A box appears around the plate. Drag its corner handles if it
   needs adjusting (that frame becomes a manual anchor).
4. Press **Track license plate**. A floating **PlateMask window** opens and shows every frame as it
   is solved, with the plate outline colour-coded by state, a progress bar and a **Stop** button
   (frames tracked so far are kept). Tracking runs from the detection frame forward to the end and
   then backward to the start (see *Tracking → Direction*) inside one render call, so Resolve's own
   viewer waits until it is done; the window is the live view. Outlines are colour-coded: green
   confirmed, yellow partial, purple inferred from the car body, orange occluded (coasting), red out
   of frame / lost, cyan anchor. In the viewer overlay a dashed outline shows the extra coverage
   added while the plate is hidden.
   The **Status** field in the Plate group reports the result (frames tracked, how many confirmed,
   inferred, occluded, out of frame), and *Notify when tracking finishes* posts it as a message.
5. Repeat with another *Active plate* slot for more plates. Corner drags on any frame add manual
   anchors that re-anchor the tracker on the next track run.

**Mask**: *Expand / contract* (morphology on the quad matte), *Feather*, *Invert*,
*Mask only* (white-on-black matte + alpha, for use as an external matte / Fusion mask) and
*Write mask to alpha* (keep the picture, matte in alpha).

**Obfuscation**: *Pixelize*, *Blur*, *Randomize pixels* (shuffle / noise / blocks) or *None*.
The **Destructive lock** (on by default) forces quantisation, random dithering, additive noise and a
per-frame jittered mosaic grid so the output cannot be de-pixelated, deconvolved or recovered by
averaging frames. The random source is reseeded from the OS every render and never stored, so the
same frame renders differently every time by design. Turning the lock off gives a reversible plain
blur/mosaic — only do that when anonymisation is not the goal.

### Fusion page notes

The node's *Seed point* is a normalised Fusion point (0..1). All parameters are scriptable; the
`scripts/verify_resolve.py` and `scripts/verify_phase2.py` scripts show how to drive detection and
tracking without the GUI through the hidden request inputs (`detectRequest`, `trackRequest`,
`requestFrame`, `requestPlate`).

## How it works

* **Detection** (`src/core/Detector.cpp`): seed-gated classical detector — vertical-stroke energy,
  morphological closing into a text blob, PCA-oriented rectangle, aspect-ratio prior (Swiss ratios
  weighted), edge snapping to the plate border.
* **Tracking** (`src/core/Tracker.cpp`): pyramidal Lucas–Kanade flow on a grid inside the quad,
  robust similarity fit, normalised cross-correlation against a running plate template, and a
  visibility state machine (confirmed / partial / inferred / occluded / out-of-frame / lost).
* **Context (car-body) estimation**: a grid of points around the plate (about 1.2 plate widths to
  each side and 2.5 plate heights above/below) is tracked every frame. While the plate is reliably
  visible, cells whose motion agrees with the plate's are learned as "car" cells. When the plate is
  hidden (bush, pole, sign, frame edge) but the car is still visible, only those cells are fitted,
  measured against the last confirmed frame (an anchor image) so the estimate does not drift, and
  the plate is placed where the body says it must be — state **inferred** (purple outline), with
  slower fail-safe growth. If too little of the body is visible the tracker coasts on damped
  constant velocity (occluded / out-of-frame). Re-acquisition uses template search around the
  prediction and re-detection; bounded pure-prediction gaps are interpolated after the pass.
* **Mask** (`src/core/Mask.cpp`): signed-distance rasterisation of the quad, so expand/contract and
  feather are analytic.
* **Obfuscation** (`src/core/Obfuscate.cpp`): blur / mosaic / randomiser plus the destructive stage.
* **Host integration** (`src/ofx/PlateMaskPlugin.cpp`): Resolve keeps separate UI and render
  instances, so buttons only bump hidden request counters; the work runs in `render()` using
  temporal clip access. The tracking loop pushes each solved frame (with the quad drawn) to a
  floating AppKit panel (`src/ofx/Monitor.mm`) through the main queue, the same approach plugins
  such as Neat Video use for their own UI in Resolve. Resolve's timeline suite cannot move the
  playhead on the Edit page (`gotoTime` returns unsupported there), so the viewer itself cannot be
  driven. Results live in a sidecar JSON
  (`~/Library/Application Support/PlateMask/tracks/<instance-id>.json`) shared by all instances and
  are mirrored into a hidden string parameter so projects stay self-contained. Overlays use the
  OFX 1.5 Draw Suite (Resolve provides no OpenGL context).

Diagnostics: the plugin always logs events (describe, instance creation, detection, tracking,
overlay) to `~/Library/Logs/PlateMask.log` (macOS), `%APPDATA%\PlateMask.log` (Windows) or
`~/.platemask.log` (Linux); per-frame render lines are added when `PLATEMASK_LOG=1` is set.

## What was verified

* `make test`: detection IoU, tracking through a sweeping full occlusion, out-of-frame exit and
  return with re-acquisition, a static bush hiding the plate while the car stays visible (inferred
  position within 0.6 plate heights), store round trip, matte and destructive obfuscation.
* Resolve 21.0.0 (macOS 26, M4 Max): plugin loads, instance creation on the Fusion page,
  MediaIn → PlateMask → MediaOut, detection + bidirectional tracking through temporal clip access
  (190 frames: confirmed / partial / inferred / occluded / out-of-frame states), overlay draw called
  with a valid Draw Suite context, the monitor window updated from the render thread while tracking,
  Deliver-page render with the mosaic following the plate. On the Edit page with real footage:
  overlay draw, click-to-seed, detection and tracking confirmed through the plugin log.
* Windows x64 and Linux x64: built by GitHub Actions on real Windows (MSVC, Visual Studio 2022)
  and Ubuntu runners; the core tests and the mini host pass there, i.e. the actual `PlateMask.ofx`
  DLL / shared object exports the OpenFX entry points, describes, instantiates, tracks the whole
  synthetic clip through temporal clip access and renders the mask. On Windows the monitor window
  runs on its own UI thread (Win32). What has **not** been done yet is loading these builds inside
  Resolve Studio on Windows or Linux; the host-facing code is identical to the macOS build that
  was verified in Resolve, so please report the plugin log (`%APPDATA%\PlateMask.log` on Windows,
  `~/.platemask.log` on Linux) if anything differs.
* Not exercised by automation (needs a mouse): clicking in the viewer and dragging corner handles
  on the Edit/Color pages. The code path is the standard OFX interact pen actions.

## Limitations / next steps

* The detector is classical (no neural network). It needs a click near the plate and reasonably
  legible characters (≥ ~10 px high). A learned detector can be plugged in behind
  `detectAtSeed`/`detectCandidates`.
* Tracking is planar-similarity (no full perspective update per frame); strong perspective changes
  are handled by manual anchors.
* Float RGBA only (what Resolve delivers to OpenFX).
* Linux has no monitor window yet (tracking feedback there is the Status field and the host
  progress dialog).
