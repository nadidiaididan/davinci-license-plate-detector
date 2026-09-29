// Floating monitor window shown while tracking runs (macOS: AppKit; other platforms: no-op).
// Called from the render thread; all UI work is dispatched to the main thread.
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace pmui {
void monitorShow(const std::string& title);
// rgb: w*h*3 bytes, top row first. text: status line. progress: 0..1.
void monitorUpdate(std::shared_ptr<std::vector<uint8_t>> rgb, int w, int h, const std::string& text, double progress);
void monitorClose();
bool monitorCancelled();
bool monitorMainThreadResponsive();  // true once the main thread has drawn at least one update
}  // namespace pmui
