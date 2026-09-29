#include "Monitor.h"
#ifdef __APPLE__
#import <Cocoa/Cocoa.h>
#include <atomic>
#include <cstring>

namespace {
std::atomic<bool> gCancelled{false};
std::atomic<int> gPending{0};
std::atomic<bool> gDrawn{false};
}

@interface PMMonitor : NSObject
@property(strong) NSPanel* panel;
@property(strong) NSImageView* imageView;
@property(strong) NSTextField* label;
@property(strong) NSProgressIndicator* bar;
@property(strong) NSButton* stopButton;
- (void)ensureWindow;
- (void)stop:(id)sender;
@end

@implementation PMMonitor
- (void)ensureWindow {
  if (self.panel) return;
  NSRect frame = NSMakeRect(0, 0, 760, 600);
  NSPanel* p = [[NSPanel alloc] initWithContentRect:frame
                                          styleMask:(NSWindowStyleMaskTitled | NSWindowStyleMaskUtilityWindow |
                                                     NSWindowStyleMaskNonactivatingPanel | NSWindowStyleMaskResizable)
                                            backing:NSBackingStoreBuffered
                                              defer:NO];
  p.title = @"PlateMask";
  p.level = NSFloatingWindowLevel;
  p.releasedWhenClosed = NO;
  p.hidesOnDeactivate = NO;
  p.floatingPanel = YES;
  NSView* content = p.contentView;
  NSImageView* iv = [[NSImageView alloc] initWithFrame:NSMakeRect(10, 56, 740, 534)];
  iv.imageScaling = NSImageScaleProportionallyUpOrDown;
  iv.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
  [content addSubview:iv];
  NSTextField* lbl = [[NSTextField alloc] initWithFrame:NSMakeRect(10, 30, 620, 20)];
  lbl.editable = NO; lbl.bezeled = NO; lbl.drawsBackground = NO; lbl.selectable = NO;
  lbl.stringValue = @"Tracking...";
  lbl.autoresizingMask = NSViewWidthSizable | NSViewMaxYMargin;
  [content addSubview:lbl];
  NSProgressIndicator* bar = [[NSProgressIndicator alloc] initWithFrame:NSMakeRect(10, 10, 620, 16)];
  bar.style = NSProgressIndicatorStyleBar;
  bar.indeterminate = NO; bar.minValue = 0; bar.maxValue = 1; bar.doubleValue = 0;
  bar.autoresizingMask = NSViewWidthSizable | NSViewMaxYMargin;
  [content addSubview:bar];
  NSButton* btn = [[NSButton alloc] initWithFrame:NSMakeRect(640, 8, 110, 32)];
  btn.title = @"Stop";
  btn.bezelStyle = NSBezelStyleRounded;
  btn.target = self;
  btn.action = @selector(stop:);
  btn.autoresizingMask = NSViewMinXMargin | NSViewMaxYMargin;
  [content addSubview:btn];
  self.panel = p; self.imageView = iv; self.label = lbl; self.bar = bar; self.stopButton = btn;
  [p center];
}
- (void)stop:(id)sender {
  gCancelled = true;
  self.label.stringValue = @"Stopping after the current frame...";
}
@end

static PMMonitor* shared() {
  static PMMonitor* m = nil;
  if (!m) m = [[PMMonitor alloc] init];
  return m;
}

namespace pmui {

void monitorShow(const std::string& title) {
  gCancelled = false;
  gDrawn = false;
  gPending = 0;
  NSString* t = [NSString stringWithUTF8String:title.c_str()];
  dispatch_async(dispatch_get_main_queue(), ^{
    PMMonitor* m = shared();
    [m ensureWindow];
    m.panel.title = t;
    m.label.stringValue = @"Tracking...";
    m.bar.doubleValue = 0;
    [m.panel orderFront:nil];
  });
}

void monitorUpdate(std::shared_ptr<std::vector<uint8_t>> rgb, int w, int h, const std::string& text, double progress) {
  if (gPending.load() > 1) return;  // main thread is behind: drop this frame rather than queue it
  gPending++;
  NSString* t = [NSString stringWithUTF8String:text.c_str()];
  dispatch_async(dispatch_get_main_queue(), ^{
    PMMonitor* m = shared();
    [m ensureWindow];
    if (rgb && w > 0 && h > 0 && rgb->size() >= (size_t)w * h * 3) {
      NSBitmapImageRep* rep = [[NSBitmapImageRep alloc] initWithBitmapDataPlanes:NULL pixelsWide:w pixelsHigh:h
          bitsPerSample:8 samplesPerPixel:3 hasAlpha:NO isPlanar:NO colorSpaceName:NSDeviceRGBColorSpace
          bytesPerRow:w * 3 bitsPerPixel:24];
      if (rep) {
        memcpy(rep.bitmapData, rgb->data(), (size_t)w * h * 3);
        NSImage* img = [[NSImage alloc] initWithSize:NSMakeSize(w, h)];
        [img addRepresentation:rep];
        m.imageView.image = img;
      }
    }
    m.label.stringValue = t;
    m.bar.doubleValue = progress;
    if (!m.panel.visible) [m.panel orderFront:nil];
    gDrawn = true;
    gPending--;
  });
}

void monitorClose() {
  dispatch_async(dispatch_get_main_queue(), ^{
    PMMonitor* m = shared();
    if (m.panel) [m.panel orderOut:nil];
  });
}

bool monitorCancelled() { return gCancelled.load(); }
bool monitorMainThreadResponsive() { return gDrawn.load(); }

}  // namespace pmui
#else
namespace pmui {
void monitorShow(const std::string&) {}
void monitorUpdate(std::shared_ptr<std::vector<uint8_t>>, int, int, const std::string&, double) {}
void monitorClose() {}
bool monitorCancelled() { return false; }
bool monitorMainThreadResponsive() { return true; }
}  // namespace pmui
#endif
