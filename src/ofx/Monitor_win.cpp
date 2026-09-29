// Floating monitor window for Windows. Runs its own UI thread with its own message loop, so it
// keeps drawing no matter what the host's main thread is doing during the render that tracks.
#include "Monitor.h"
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <commctrl.h>
#include <atomic>
#include <mutex>
#include <cstring>
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

namespace {

const UINT WM_PM_SHOW = WM_APP + 1;
const UINT WM_PM_UPDATE = WM_APP + 2;
const UINT WM_PM_HIDE = WM_APP + 3;

struct Frame {
  std::vector<uint8_t> bgra;  // top-down, w*h*4
  int w = 0, h = 0;
  std::string text;
  double progress = 0;
};

std::atomic<bool> gCancelled{false};
std::atomic<bool> gDrawn{false};
std::atomic<int> gPending{0};
std::atomic<HWND> gWnd{nullptr};
HANDLE gThread = nullptr;
DWORD gThreadId = 0;
HWND gImage = nullptr, gLabel = nullptr, gBar = nullptr, gButton = nullptr;
std::mutex gFrameMu;
Frame gFrame;  // last frame, drawn on WM_PAINT
std::string gTitle;
HANDLE gReady = nullptr;

void layout(HWND hwnd) {
  RECT r;
  GetClientRect(hwnd, &r);
  int W = r.right - r.left, H = r.bottom - r.top;
  MoveWindow(gImage, 10, 10, W - 20, H - 70, TRUE);
  MoveWindow(gLabel, 10, H - 52, W - 140, 20, TRUE);
  MoveWindow(gBar, 10, H - 28, W - 140, 18, TRUE);
  MoveWindow(gButton, W - 120, H - 40, 110, 30, TRUE);
}

LRESULT CALLBACK imageProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  if (msg == WM_PAINT) {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd, &ps);
    RECT r;
    GetClientRect(hwnd, &r);
    HBRUSH bg = CreateSolidBrush(RGB(30, 30, 30));
    FillRect(dc, &r, bg);
    DeleteObject(bg);
    std::lock_guard<std::mutex> lk(gFrameMu);
    if (gFrame.w > 0 && gFrame.h > 0) {
      int cw = r.right - r.left, ch = r.bottom - r.top;
      double s = std::min((double)cw / gFrame.w, (double)ch / gFrame.h);
      int dw = std::max(1, (int)(gFrame.w * s)), dh = std::max(1, (int)(gFrame.h * s));
      int dx = (cw - dw) / 2, dy = (ch - dh) / 2;
      BITMAPINFO bi;
      memset(&bi, 0, sizeof bi);
      bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
      bi.bmiHeader.biWidth = gFrame.w;
      bi.bmiHeader.biHeight = -gFrame.h;  // top-down
      bi.bmiHeader.biPlanes = 1;
      bi.bmiHeader.biBitCount = 32;
      bi.bmiHeader.biCompression = BI_RGB;
      SetStretchBltMode(dc, HALFTONE);
      StretchDIBits(dc, dx, dy, dw, dh, 0, 0, gFrame.w, gFrame.h, gFrame.bgra.data(), &bi, DIB_RGB_COLORS, SRCCOPY);
    }
    EndPaint(hwnd, &ps);
    return 0;
  }
  return DefWindowProcA(hwnd, msg, wp, lp);
}

LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_PM_SHOW:
      gCancelled = false;
      SetWindowTextA(hwnd, gTitle.c_str());
      SetWindowTextA(gLabel, "Tracking...");
      SendMessageA(gBar, PBM_SETPOS, 0, 0);
      ShowWindow(hwnd, SW_SHOWNOACTIVATE);
      return 0;
    case WM_PM_UPDATE: {
      Frame* f = (Frame*)lp;
      {
        std::lock_guard<std::mutex> lk(gFrameMu);
        gFrame = std::move(*f);
      }
      delete f;
      SetWindowTextA(gLabel, gFrame.text.c_str());
      SendMessageA(gBar, PBM_SETPOS, (WPARAM)(gFrame.progress * 1000), 0);
      InvalidateRect(gImage, nullptr, FALSE);
      if (!IsWindowVisible(hwnd)) ShowWindow(hwnd, SW_SHOWNOACTIVATE);
      gDrawn = true;
      gPending--;
      return 0;
    }
    case WM_PM_HIDE:
      ShowWindow(hwnd, SW_HIDE);
      return 0;
    case WM_SIZE:
      layout(hwnd);
      return 0;
    case WM_COMMAND:
      if ((HWND)lp == gButton) {
        gCancelled = true;
        SetWindowTextA(gLabel, "Stopping after the current frame...");
      }
      return 0;
    case WM_CLOSE:
      gCancelled = true;
      ShowWindow(hwnd, SW_HIDE);
      return 0;
  }
  return DefWindowProcA(hwnd, msg, wp, lp);
}

DWORD WINAPI uiThread(LPVOID) {
  INITCOMMONCONTROLSEX icc{sizeof(INITCOMMONCONTROLSEX), ICC_PROGRESS_CLASS};
  InitCommonControlsEx(&icc);
  HINSTANCE inst = GetModuleHandleA(nullptr);
  WNDCLASSA wc{};
  wc.lpfnWndProc = wndProc;
  wc.hInstance = inst;
  wc.lpszClassName = "PlateMaskMonitor";
  wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
  wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
  RegisterClassA(&wc);
  WNDCLASSA ic{};
  ic.lpfnWndProc = imageProc;
  ic.hInstance = inst;
  ic.lpszClassName = "PlateMaskMonitorImage";
  ic.hCursor = LoadCursor(nullptr, IDC_ARROW);
  RegisterClassA(&ic);
  HWND hwnd = CreateWindowExA(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, "PlateMaskMonitor", "PlateMask",
                              WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX, CW_USEDEFAULT, CW_USEDEFAULT, 780, 640,
                              nullptr, nullptr, inst, nullptr);
  if (!hwnd) { SetEvent(gReady); return 1; }
  gImage = CreateWindowExA(0, "PlateMaskMonitorImage", "", WS_CHILD | WS_VISIBLE, 0, 0, 10, 10, hwnd, nullptr, inst, nullptr);
  gLabel = CreateWindowExA(0, "STATIC", "Tracking...", WS_CHILD | WS_VISIBLE | SS_LEFT, 0, 0, 10, 10, hwnd, nullptr, inst, nullptr);
  gBar = CreateWindowExA(0, PROGRESS_CLASSA, "", WS_CHILD | WS_VISIBLE, 0, 0, 10, 10, hwnd, nullptr, inst, nullptr);
  SendMessageA(gBar, PBM_SETRANGE32, 0, 1000);
  gButton = CreateWindowExA(0, "BUTTON", "Stop", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 10, 10, hwnd, nullptr, inst, nullptr);
  HFONT font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
  SendMessageA(gLabel, WM_SETFONT, (WPARAM)font, TRUE);
  SendMessageA(gButton, WM_SETFONT, (WPARAM)font, TRUE);
  layout(hwnd);
  gWnd = hwnd;
  SetEvent(gReady);
  MSG m;
  while (GetMessageA(&m, nullptr, 0, 0) > 0) {
    TranslateMessage(&m);
    DispatchMessageA(&m);
  }
  gWnd = nullptr;
  return 0;
}

bool ensureThread() {
  if (gWnd.load()) return true;
  if (!gThread) {
    gReady = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    gThread = CreateThread(nullptr, 0, uiThread, nullptr, 0, &gThreadId);
    if (!gThread) return false;
  }
  WaitForSingleObject(gReady, 3000);
  return gWnd.load() != nullptr;
}

}  // namespace

namespace pmui {

void monitorShow(const std::string& title) {
  gCancelled = false;
  gDrawn = false;
  gPending = 0;
  gTitle = title;
  if (!ensureThread()) return;
  PostMessageA(gWnd.load(), WM_PM_SHOW, 0, 0);
}

void monitorUpdate(std::shared_ptr<std::vector<uint8_t>> rgb, int w, int h, const std::string& text, double progress) {
  if (!gWnd.load()) return;
  if (gPending.load() > 1) return;  // UI thread is behind: drop this frame
  Frame* f = new Frame;
  f->w = w; f->h = h; f->text = text; f->progress = progress;
  if (rgb && w > 0 && h > 0 && rgb->size() >= (size_t)w * h * 3) {
    f->bgra.resize((size_t)w * h * 4);
    const uint8_t* s = rgb->data();
    uint8_t* d = f->bgra.data();
    for (size_t i = 0, n = (size_t)w * h; i < n; ++i) { d[4 * i] = s[3 * i + 2]; d[4 * i + 1] = s[3 * i + 1]; d[4 * i + 2] = s[3 * i]; d[4 * i + 3] = 255; }
  } else {
    f->w = f->h = 0;
  }
  gPending++;
  if (!PostMessageA(gWnd.load(), WM_PM_UPDATE, 0, (LPARAM)f)) { delete f; gPending--; }
}

void monitorClose() {
  if (gWnd.load()) PostMessageA(gWnd.load(), WM_PM_HIDE, 0, 0);
}

bool monitorCancelled() { return gCancelled.load(); }
bool monitorMainThreadResponsive() { return gDrawn.load(); }

}  // namespace pmui
#endif
