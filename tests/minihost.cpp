// Minimal OpenFX host: loads the built PlateMask binary the way a real host does (dlopen /
// LoadLibrary, OfxGetPlugin, setHost, Load, Describe, DescribeInContext, CreateInstance, Render),
// feeds it the synthetic clip through temporal clip access, triggers detection + tracking through
// the plugin's request parameters, and checks the track and the rendered mask.
//   minihost <path-to-PlateMask.ofx binary> <output dir>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdarg>
#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <memory>
#include <chrono>
#include <fstream>
#include <sstream>
#include "ofxCore.h"
#include "ofxProperty.h"
#include "ofxImageEffect.h"
#include "ofxParam.h"
#include "ofxMemory.h"
#include "ofxMultiThread.h"
#include "ofxMessage.h"
#include "Synth.h"
#include "core/TrackStore.h"
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <direct.h>
#else
#include <dlfcn.h>
#include <sys/stat.h>
#endif

static int gFails = 0;
#define CHECK(c, ...) do { if (!(c)) { ++gFails; printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

// ------------------------------------------------------------------ property sets
struct PropVal {
  enum T { I, D, S, P } t = I;
  int i = 0; double d = 0; std::string s; void* p = nullptr;
};
struct PropSet {
  std::map<std::string, std::vector<PropVal>> m;
  std::vector<PropVal>& at(const char* k, int n) { auto& v = m[k]; if ((int)v.size() < n) v.resize(n); return v; }
  void setI(const char* k, int v, int idx = 0) { auto& a = at(k, idx + 1); a[idx].t = PropVal::I; a[idx].i = v; }
  void setD(const char* k, double v, int idx = 0) { auto& a = at(k, idx + 1); a[idx].t = PropVal::D; a[idx].d = v; }
  void setS(const char* k, const std::string& v, int idx = 0) { auto& a = at(k, idx + 1); a[idx].t = PropVal::S; a[idx].s = v; }
  void setP(const char* k, void* v, int idx = 0) { auto& a = at(k, idx + 1); a[idx].t = PropVal::P; a[idx].p = v; }
};
static PropSet* PS(OfxPropertySetHandle h) { return (PropSet*)h; }

static OfxStatus propSetPointer(OfxPropertySetHandle h, const char* k, int i, void* v) { if (!h) return kOfxStatErrBadHandle; PS(h)->setP(k, v, i); return kOfxStatOK; }
static OfxStatus propSetString(OfxPropertySetHandle h, const char* k, int i, const char* v) { if (!h) return kOfxStatErrBadHandle; PS(h)->setS(k, v ? v : "", i); return kOfxStatOK; }
static OfxStatus propSetDouble(OfxPropertySetHandle h, const char* k, int i, double v) { if (!h) return kOfxStatErrBadHandle; PS(h)->setD(k, v, i); return kOfxStatOK; }
static OfxStatus propSetInt(OfxPropertySetHandle h, const char* k, int i, int v) { if (!h) return kOfxStatErrBadHandle; PS(h)->setI(k, v, i); return kOfxStatOK; }
static OfxStatus propSetPointerN(OfxPropertySetHandle h, const char* k, int n, void* const* v) { for (int i = 0; i < n; ++i) propSetPointer(h, k, i, v[i]); return kOfxStatOK; }
static OfxStatus propSetStringN(OfxPropertySetHandle h, const char* k, int n, const char* const* v) { for (int i = 0; i < n; ++i) propSetString(h, k, i, v[i]); return kOfxStatOK; }
static OfxStatus propSetDoubleN(OfxPropertySetHandle h, const char* k, int n, const double* v) { for (int i = 0; i < n; ++i) propSetDouble(h, k, i, v[i]); return kOfxStatOK; }
static OfxStatus propSetIntN(OfxPropertySetHandle h, const char* k, int n, const int* v) { for (int i = 0; i < n; ++i) propSetInt(h, k, i, v[i]); return kOfxStatOK; }
static const PropVal* getVal(OfxPropertySetHandle h, const char* k, int i, OfxStatus& st) {
  if (!h) { st = kOfxStatErrBadHandle; return nullptr; }
  auto it = PS(h)->m.find(k);
  if (it == PS(h)->m.end()) { st = kOfxStatErrUnknown; return nullptr; }
  if (i < 0 || i >= (int)it->second.size()) { st = kOfxStatErrBadIndex; return nullptr; }
  st = kOfxStatOK;
  return &it->second[i];
}
static OfxStatus propGetPointer(OfxPropertySetHandle h, const char* k, int i, void** v) { OfxStatus st; const PropVal* p = getVal(h, k, i, st); if (p) *v = p->p; return st; }
static OfxStatus propGetString(OfxPropertySetHandle h, const char* k, int i, char** v) { OfxStatus st; const PropVal* p = getVal(h, k, i, st); if (p) *v = (char*)p->s.c_str(); return st; }
static OfxStatus propGetDouble(OfxPropertySetHandle h, const char* k, int i, double* v) { OfxStatus st; const PropVal* p = getVal(h, k, i, st); if (p) *v = p->t == PropVal::I ? p->i : p->d; return st; }
static OfxStatus propGetInt(OfxPropertySetHandle h, const char* k, int i, int* v) { OfxStatus st; const PropVal* p = getVal(h, k, i, st); if (p) *v = p->t == PropVal::D ? (int)p->d : p->i; return st; }
static OfxStatus propGetPointerN(OfxPropertySetHandle h, const char* k, int n, void** v) { for (int i = 0; i < n; ++i) { OfxStatus s = propGetPointer(h, k, i, &v[i]); if (s != kOfxStatOK) return s; } return kOfxStatOK; }
static OfxStatus propGetStringN(OfxPropertySetHandle h, const char* k, int n, char** v) { for (int i = 0; i < n; ++i) { OfxStatus s = propGetString(h, k, i, &v[i]); if (s != kOfxStatOK) return s; } return kOfxStatOK; }
static OfxStatus propGetDoubleN(OfxPropertySetHandle h, const char* k, int n, double* v) { for (int i = 0; i < n; ++i) { OfxStatus s = propGetDouble(h, k, i, &v[i]); if (s != kOfxStatOK) return s; } return kOfxStatOK; }
static OfxStatus propGetIntN(OfxPropertySetHandle h, const char* k, int n, int* v) { for (int i = 0; i < n; ++i) { OfxStatus s = propGetInt(h, k, i, &v[i]); if (s != kOfxStatOK) return s; } return kOfxStatOK; }
static OfxStatus propReset(OfxPropertySetHandle h, const char* k) { if (!h) return kOfxStatErrBadHandle; PS(h)->m.erase(k); return kOfxStatOK; }
static OfxStatus propGetDimension(OfxPropertySetHandle h, const char* k, int* n) { if (!h) return kOfxStatErrBadHandle; auto it = PS(h)->m.find(k); *n = it == PS(h)->m.end() ? 0 : (int)it->second.size(); return kOfxStatOK; }
static OfxPropertySuiteV1 gPropSuite = {propSetPointer, propSetString, propSetDouble, propSetInt, propSetPointerN, propSetStringN, propSetDoubleN, propSetIntN,
                                        propGetPointer, propGetString, propGetDouble, propGetInt, propGetPointerN, propGetStringN, propGetDoubleN, propGetIntN,
                                        propReset, propGetDimension};

// ------------------------------------------------------------------ params
struct Param {
  std::string name, type;
  PropSet props;
  std::vector<PropVal> value;
  int dims() const {
    if (type == kOfxParamTypeDouble2D) return 2;
    if (type == kOfxParamTypeDouble3D || type == kOfxParamTypeRGB) return 3;
    if (type == kOfxParamTypeRGBA) return 4;
    if (type == kOfxParamTypePushButton || type == kOfxParamTypeGroup || type == kOfxParamTypePage) return 0;
    return 1;
  }
  bool isInt() const { return type == kOfxParamTypeInteger || type == kOfxParamTypeBoolean || type == kOfxParamTypeChoice; }
  bool isString() const { return type == kOfxParamTypeString; }
  void initDefault() {
    value.assign(dims(), PropVal{});
    auto it = props.m.find(kOfxParamPropDefault);
    for (int i = 0; i < dims(); ++i)
      if (it != props.m.end() && i < (int)it->second.size()) value[i] = it->second[i];
  }
};
struct ParamSet {
  std::map<std::string, Param> params;
  std::vector<std::string> order;
  PropSet props;
};
static ParamSet* PSET(OfxParamSetHandle h) { return (ParamSet*)h; }
static Param* PRM(OfxParamHandle h) { return (Param*)h; }

static OfxStatus paramDefine(OfxParamSetHandle ps, const char* type, const char* name, OfxPropertySetHandle* props) {
  Param& p = PSET(ps)->params[name];
  p.name = name; p.type = type;
  p.props.setS(kOfxPropType, kOfxTypeParameter);
  p.props.setS(kOfxPropName, name);
  p.props.setS(kOfxParamPropType, type);
  p.props.setI(kOfxParamPropSecret, 0);
  p.props.setI(kOfxParamPropEnabled, 1);
  PSET(ps)->order.push_back(name);
  if (props) *props = (OfxPropertySetHandle)&p.props;
  return kOfxStatOK;
}
static OfxStatus paramGetHandle(OfxParamSetHandle ps, const char* name, OfxParamHandle* param, OfxPropertySetHandle* props) {
  auto it = PSET(ps)->params.find(name);
  if (it == PSET(ps)->params.end()) return kOfxStatErrUnknown;
  *param = (OfxParamHandle)&it->second;
  if (props) *props = (OfxPropertySetHandle)&it->second.props;
  return kOfxStatOK;
}
static OfxStatus paramSetGetPropertySet(OfxParamSetHandle ps, OfxPropertySetHandle* props) { *props = (OfxPropertySetHandle)&PSET(ps)->props; return kOfxStatOK; }
static OfxStatus paramGetPropertySet(OfxParamHandle p, OfxPropertySetHandle* props) { *props = (OfxPropertySetHandle)&PRM(p)->props; return kOfxStatOK; }
static OfxStatus getValueV(Param* p, va_list ap) {
  if (p->isString()) { char** out = va_arg(ap, char**); *out = (char*)p->value[0].s.c_str(); return kOfxStatOK; }
  if (p->isInt()) { int* out = va_arg(ap, int*); *out = p->value[0].t == PropVal::D ? (int)p->value[0].d : p->value[0].i; return kOfxStatOK; }
  for (int i = 0; i < p->dims(); ++i) { double* out = va_arg(ap, double*); *out = p->value[i].t == PropVal::I ? p->value[i].i : p->value[i].d; }
  return kOfxStatOK;
}
static OfxStatus paramGetValue(OfxParamHandle h, ...) { va_list ap; va_start(ap, h); OfxStatus s = getValueV(PRM(h), ap); va_end(ap); return s; }
static OfxStatus paramGetValueAtTime(OfxParamHandle h, OfxTime t, ...) { va_list ap; va_start(ap, t); OfxStatus s = getValueV(PRM(h), ap); va_end(ap); return s; }
static OfxStatus setValueV(Param* p, va_list ap) {
  if (p->isString()) { const char* v = va_arg(ap, const char*); p->value[0].t = PropVal::S; p->value[0].s = v ? v : ""; return kOfxStatOK; }
  if (p->isInt()) { int v = va_arg(ap, int); p->value[0].t = PropVal::I; p->value[0].i = v; return kOfxStatOK; }
  for (int i = 0; i < p->dims(); ++i) { double v = va_arg(ap, double); p->value[i].t = PropVal::D; p->value[i].d = v; }
  return kOfxStatOK;
}
static OfxStatus paramSetValue(OfxParamHandle h, ...) { va_list ap; va_start(ap, h); OfxStatus s = setValueV(PRM(h), ap); va_end(ap); return s; }
static OfxStatus paramSetValueAtTime(OfxParamHandle h, OfxTime t, ...) { va_list ap; va_start(ap, t); OfxStatus s = setValueV(PRM(h), ap); va_end(ap); return s; }
static OfxStatus paramGetNumKeys(OfxParamHandle, unsigned int* n) { *n = 0; return kOfxStatOK; }
static OfxStatus paramGetKeyTime(OfxParamHandle, unsigned int, OfxTime*) { return kOfxStatErrBadIndex; }
static OfxStatus paramGetKeyIndex(OfxParamHandle, OfxTime, int, int*) { return kOfxStatFailed; }
static OfxStatus paramDeleteKey(OfxParamHandle, OfxTime) { return kOfxStatOK; }
static OfxStatus paramDeleteAllKeys(OfxParamHandle) { return kOfxStatOK; }
static OfxStatus paramCopy(OfxParamHandle, OfxParamHandle, OfxTime, const OfxRangeD*) { return kOfxStatOK; }
static OfxStatus paramEditBegin(OfxParamSetHandle, const char*) { return kOfxStatOK; }
static OfxStatus paramEditEnd(OfxParamSetHandle) { return kOfxStatOK; }
static OfxStatus paramGetDerivative(OfxParamHandle, OfxTime, ...) { return kOfxStatErrUnsupported; }
static OfxStatus paramGetIntegral(OfxParamHandle, OfxTime, OfxTime, ...) { return kOfxStatErrUnsupported; }
static OfxParameterSuiteV1 gParamSuite = {paramDefine, paramGetHandle, paramSetGetPropertySet, paramGetPropertySet, paramGetValue, paramGetValueAtTime,
                                          paramGetDerivative, paramGetIntegral, paramSetValue, paramSetValueAtTime, paramGetNumKeys, paramGetKeyTime,
                                          paramGetKeyIndex, paramDeleteKey, paramDeleteAllKeys, paramCopy, paramEditBegin, paramEditEnd};

// ------------------------------------------------------------------ effect / clips / images
struct Clip { std::string name; PropSet props; };
struct Effect { PropSet props; ParamSet params; std::map<std::string, Clip> clips; };
struct Image { PropSet props; std::vector<float> data; };
static Effect* EFF(OfxImageEffectHandle h) { return (Effect*)h; }
static Clip* CLP(OfxImageClipHandle h) { return (Clip*)h; }
static std::vector<float>* gOutputBuffer = nullptr;  // owned by the driver during a render
static int gImagesAlive = 0;

static void fillImageProps(PropSet& p, int W, int H, void* data, const std::string& id) {
  p.setS(kOfxPropType, kOfxTypeImage);
  p.setS(kOfxImageEffectPropPixelDepth, kOfxBitDepthFloat);
  p.setS(kOfxImageEffectPropComponents, kOfxImageComponentRGBA);
  p.setS(kOfxImageEffectPropPreMultiplication, kOfxImageOpaque);
  p.setD(kOfxImageEffectPropRenderScale, 1, 0); p.setD(kOfxImageEffectPropRenderScale, 1, 1);
  p.setD(kOfxImagePropPixelAspectRatio, 1);
  p.setP(kOfxImagePropData, data);
  int b[4] = {0, 0, W, H};
  for (int i = 0; i < 4; ++i) { p.setI(kOfxImagePropBounds, b[i], i); p.setI(kOfxImagePropRegionOfDefinition, b[i], i); }
  p.setI(kOfxImagePropRowBytes, W * 4 * (int)sizeof(float));
  p.setS(kOfxImagePropField, kOfxImageFieldNone);
  p.setS(kOfxImagePropUniqueIdentifier, id);
}
static OfxStatus effGetPropertySet(OfxImageEffectHandle h, OfxPropertySetHandle* p) { *p = (OfxPropertySetHandle)&EFF(h)->props; return kOfxStatOK; }
static OfxStatus effGetParamSet(OfxImageEffectHandle h, OfxParamSetHandle* p) { *p = (OfxParamSetHandle)&EFF(h)->params; return kOfxStatOK; }
static OfxStatus clipDefine(OfxImageEffectHandle h, const char* name, OfxPropertySetHandle* props) {
  Clip& c = EFF(h)->clips[name];
  c.name = name;
  c.props.setS(kOfxPropType, kOfxTypeClip);
  c.props.setS(kOfxPropName, name);
  if (props) *props = (OfxPropertySetHandle)&c.props;
  return kOfxStatOK;
}
static OfxStatus clipGetHandle(OfxImageEffectHandle h, const char* name, OfxImageClipHandle* clip, OfxPropertySetHandle* props) {
  auto it = EFF(h)->clips.find(name);
  if (it == EFF(h)->clips.end()) return kOfxStatErrUnknown;
  *clip = (OfxImageClipHandle)&it->second;
  if (props) *props = (OfxPropertySetHandle)&it->second.props;
  return kOfxStatOK;
}
static OfxStatus clipGetPropertySet(OfxImageClipHandle h, OfxPropertySetHandle* p) { *p = (OfxPropertySetHandle)&CLP(h)->props; return kOfxStatOK; }
static OfxStatus clipGetImage(OfxImageClipHandle h, OfxTime t, const OfxRectD*, OfxPropertySetHandle* out) {
  Clip* c = CLP(h);
  const int W = synth::W, H = synth::H;
  if (c->name == kOfxImageEffectOutputClipName) {
    if (!gOutputBuffer) return kOfxStatFailed;
    Image* img = new Image;
    fillImageProps(img->props, W, H, gOutputBuffer->data(), "out");
    ++gImagesAlive;
    *out = (OfxPropertySetHandle)&img->props;
    return kOfxStatOK;
  }
  int f = (int)t;
  if (f < 0 || f >= synth::FRAMES) return kOfxStatFailed;  // out of range: black/transparent for the plugin
  Image* img = new Image;
  pm::Gray g; synth::Truth tr;
  synth::render(f, g, tr);
  img->data.resize((size_t)W * H * 4);
  for (int y = 0; y < H; ++y)
    for (int x = 0; x < W; ++x) {
      float v = g.at(x, H - 1 - y);  // OFX rows are bottom-up; keep the picture upright
      float* p = &img->data[((size_t)y * W + x) * 4];
      p[0] = p[1] = p[2] = v; p[3] = 1.f;
    }
  fillImageProps(img->props, W, H, img->data.data(), "src" + std::to_string(f));
  ++gImagesAlive;
  *out = (OfxPropertySetHandle)&img->props;
  return kOfxStatOK;
}
static OfxStatus clipReleaseImage(OfxPropertySetHandle h) {
  // props is the first member of Image
  Image* img = (Image*)h;
  delete img;
  --gImagesAlive;
  return kOfxStatOK;
}
static OfxStatus clipGetRegionOfDefinition(OfxImageClipHandle, OfxTime, OfxRectD* rod) { rod->x1 = 0; rod->y1 = 0; rod->x2 = synth::W; rod->y2 = synth::H; return kOfxStatOK; }
static int effAbort(OfxImageEffectHandle) { return 0; }
static OfxStatus imageMemoryAlloc(OfxImageEffectHandle, size_t n, OfxImageMemoryHandle* h) { *h = (OfxImageMemoryHandle)malloc(n); return *h ? kOfxStatOK : kOfxStatErrMemory; }
static OfxStatus imageMemoryFree(OfxImageMemoryHandle h) { free(h); return kOfxStatOK; }
static OfxStatus imageMemoryLock(OfxImageMemoryHandle h, void** p) { *p = h; return kOfxStatOK; }
static OfxStatus imageMemoryUnlock(OfxImageMemoryHandle) { return kOfxStatOK; }
static OfxImageEffectSuiteV1 gEffectSuite = {effGetPropertySet, effGetParamSet, clipDefine, clipGetHandle, clipGetPropertySet, clipGetImage, clipReleaseImage,
                                             clipGetRegionOfDefinition, effAbort, imageMemoryAlloc, imageMemoryFree, imageMemoryLock, imageMemoryUnlock};

// ------------------------------------------------------------------ memory / threads / messages
static OfxStatus memAlloc(void*, size_t n, void** p) { *p = malloc(n); return *p ? kOfxStatOK : kOfxStatErrMemory; }
static OfxStatus memFree(void* p) { free(p); return kOfxStatOK; }
static OfxMemorySuiteV1 gMemSuite = {memAlloc, memFree};
static OfxStatus mtRun(OfxThreadFunctionV1 f, unsigned int n, void* arg) { for (unsigned i = 0; i < n; ++i) f(i, n, arg); return kOfxStatOK; }
static OfxStatus mtNumCPUs(unsigned int* n) { *n = 1; return kOfxStatOK; }
static OfxStatus mtIndex(unsigned int* i) { *i = 0; return kOfxStatOK; }
static int mtIsSpawned(void) { return 0; }
static OfxStatus mxCreate(OfxMutexHandle* h, int) { *h = (OfxMutexHandle) new std::recursive_mutex; return kOfxStatOK; }
static OfxStatus mxDestroy(const OfxMutexHandle h) { delete (std::recursive_mutex*)h; return kOfxStatOK; }
static OfxStatus mxLock(const OfxMutexHandle h) { ((std::recursive_mutex*)h)->lock(); return kOfxStatOK; }
static OfxStatus mxUnlock(const OfxMutexHandle h) { ((std::recursive_mutex*)h)->unlock(); return kOfxStatOK; }
static OfxStatus mxTryLock(const OfxMutexHandle h) { return ((std::recursive_mutex*)h)->try_lock() ? kOfxStatOK : kOfxStatFailed; }
static OfxMultiThreadSuiteV1 gThreadSuite = {mtRun, mtNumCPUs, mtIndex, mtIsSpawned, mxCreate, mxDestroy, mxLock, mxUnlock, mxTryLock};
static OfxStatus msgMessage(void*, const char* type, const char* id, const char* fmt, ...) {
  printf("  [plugin message %s/%s] ", type ? type : "?", id ? id : "");
  va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap); printf("\n");
  return kOfxStatReplyYes;
}
static OfxMessageSuiteV1 gMsgSuite = {msgMessage};

static const void* fetchSuite(OfxPropertySetHandle, const char* name, int version) {
  std::string n = name;
  if (n == kOfxPropertySuite && version == 1) return &gPropSuite;
  if (n == kOfxImageEffectSuite && version == 1) return &gEffectSuite;
  if (n == kOfxParameterSuite && version == 1) return &gParamSuite;
  if (n == kOfxMemorySuite && version == 1) return &gMemSuite;
  if (n == kOfxMultiThreadSuite && version == 1) return &gThreadSuite;
  if (n == kOfxMessageSuite && version == 1) return &gMsgSuite;
  return nullptr;
}

// ------------------------------------------------------------------ driver
static void setHostProps(PropSet& h) {
  h.setS(kOfxPropType, kOfxTypeImageEffectHost);
  h.setS(kOfxPropName, "ch.platemask.minihost");
  h.setS(kOfxPropLabel, "PlateMask mini host");
  h.setI(kOfxPropAPIVersion, 1, 0); h.setI(kOfxPropAPIVersion, 4, 1);
  h.setI(kOfxPropVersion, 1, 0); h.setI(kOfxPropVersion, 0, 1); h.setI(kOfxPropVersion, 0, 2);
  h.setS(kOfxPropVersionLabel, "1.0");
  h.setI(kOfxImageEffectHostPropIsBackground, 0);
  h.setI(kOfxImageEffectPropSupportsOverlays, 0);
  h.setI(kOfxImageEffectPropSupportsMultiResolution, 0);
  h.setI(kOfxImageEffectPropSupportsTiles, 0);
  h.setI(kOfxImageEffectPropTemporalClipAccess, 1);
  h.setS(kOfxImageEffectPropSupportedComponents, kOfxImageComponentRGBA, 0);
  h.setS(kOfxImageEffectPropSupportedContexts, kOfxImageEffectContextFilter, 0);
  h.setS(kOfxImageEffectPropSupportedContexts, kOfxImageEffectContextGeneral, 1);
  h.setI(kOfxImageEffectPropSupportsMultipleClipDepths, 0);
  h.setI(kOfxImageEffectPropSupportsMultipleClipPARs, 0);
  h.setI(kOfxImageEffectPropSetableFrameRate, 0);
  h.setI(kOfxImageEffectPropSetableFielding, 0);
  h.setI(kOfxImageEffectInstancePropSequentialRender, 0);
  h.setI(kOfxParamHostPropSupportsStringAnimation, 0);
  h.setI(kOfxParamHostPropSupportsCustomInteract, 0);
  h.setI(kOfxParamHostPropSupportsChoiceAnimation, 0);
  h.setI(kOfxParamHostPropSupportsBooleanAnimation, 0);
  h.setI(kOfxParamHostPropSupportsCustomAnimation, 0);
  h.setI(kOfxParamHostPropMaxParameters, -1);
  h.setI(kOfxParamHostPropMaxPages, 0);
  h.setI(kOfxParamHostPropPageRowColumnCount, 0, 0); h.setI(kOfxParamHostPropPageRowColumnCount, 0, 1);
  h.setS(kOfxImageEffectPropSupportedPixelDepths, kOfxBitDepthFloat, 0);
  h.setS(kOfxImageEffectHostPropNativeOrigin, kOfxHostNativeOriginBottomLeft);
  h.setI(kOfxImageEffectPropRenderQualityDraft, 0);
}
static void setInstanceProps(Effect& e, const char* context) {
  e.props.setS(kOfxPropType, kOfxTypeImageEffectInstance);
  e.props.setS(kOfxImageEffectPropContext, context);
  e.props.setD(kOfxImageEffectPropProjectSize, synth::W, 0); e.props.setD(kOfxImageEffectPropProjectSize, synth::H, 1);
  e.props.setD(kOfxImageEffectPropProjectOffset, 0, 0); e.props.setD(kOfxImageEffectPropProjectOffset, 0, 1);
  e.props.setD(kOfxImageEffectPropProjectExtent, synth::W, 0); e.props.setD(kOfxImageEffectPropProjectExtent, synth::H, 1);
  e.props.setD(kOfxImageEffectPropProjectPixelAspectRatio, 1);
  e.props.setD(kOfxImageEffectInstancePropEffectDuration, synth::FRAMES);
  e.props.setD(kOfxImageEffectPropFrameRate, 24);
  e.props.setI(kOfxPropIsInteractive, 0);
  e.props.setI(kOfxImageEffectInstancePropSequentialRender, 0);
  e.props.setI(kOfxImageEffectPropSupportsTiles, 0);
}
static void setClipInstanceProps(Clip& c) {
  c.props.setS(kOfxImageEffectPropPixelDepth, kOfxBitDepthFloat);
  c.props.setS(kOfxImageEffectPropComponents, kOfxImageComponentRGBA);
  c.props.setS(kOfxImageClipPropUnmappedPixelDepth, kOfxBitDepthFloat);
  c.props.setS(kOfxImageClipPropUnmappedComponents, kOfxImageComponentRGBA);
  c.props.setS(kOfxImageEffectPropPreMultiplication, kOfxImageOpaque);
  c.props.setD(kOfxImagePropPixelAspectRatio, 1);
  c.props.setD(kOfxImageEffectPropFrameRate, 24);
  c.props.setD(kOfxImageEffectPropFrameRange, 0, 0); c.props.setD(kOfxImageEffectPropFrameRange, synth::FRAMES, 1);  // Resolve style: end exclusive
  c.props.setS(kOfxImageClipPropFieldOrder, kOfxImageFieldNone);
  c.props.setI(kOfxImageClipPropConnected, 1);
  c.props.setD(kOfxImageEffectPropUnmappedFrameRange, 0, 0); c.props.setD(kOfxImageEffectPropUnmappedFrameRange, synth::FRAMES, 1);
  c.props.setD(kOfxImageEffectPropUnmappedFrameRate, 24);
  c.props.setI(kOfxImageClipPropContinuousSamples, 0);
}
static bool setParam(Effect& e, const char* name, double a, double b = 0) {
  auto it = e.params.params.find(name);
  if (it == e.params.params.end()) { printf("  (no param %s)\n", name); return false; }
  Param& p = it->second;
  if (p.isInt()) { p.value[0].t = PropVal::I; p.value[0].i = (int)a; }
  else { p.value[0].t = PropVal::D; p.value[0].d = a; if (p.dims() > 1) { p.value[1].t = PropVal::D; p.value[1].d = b; } }
  return true;
}
static std::string getStringParam(Effect& e, const char* name) {
  auto it = e.params.params.find(name);
  return it == e.params.params.end() ? "" : it->second.value[0].s;
}
static bool writePPM(const std::string& path, const std::vector<float>& rgba, int W, int H) {
  FILE* f = fopen(path.c_str(), "wb");
  if (!f) return false;
  fprintf(f, "P6\n%d %d\n255\n", W, H);
  for (int y = H - 1; y >= 0; --y)
    for (int x = 0; x < W; ++x) {
      const float* p = &rgba[((size_t)y * W + x) * 4];
      for (int c = 0; c < 3; ++c) { float v = p[c] < 0 ? 0 : (p[c] > 1 ? 1 : p[c]); fputc((int)(v * 255 + 0.5f), f); }
    }
  fclose(f);
  return true;
}

int main(int argc, char** argv) {
  setvbuf(stdout, nullptr, _IONBF, 0);
  if (argc < 3) { fprintf(stderr, "usage: minihost <PlateMask.ofx binary> <outdir>\n"); return 2; }
  std::string lib = argv[1], outdir = argv[2];
#ifdef _WIN32
  _mkdir(outdir.c_str());
  HMODULE mod = LoadLibraryA(lib.c_str());
  if (!mod) { fprintf(stderr, "LoadLibrary failed: %lu\n", GetLastError()); return 1; }
  auto getNum = (int (*)(void))GetProcAddress(mod, "OfxGetNumberOfPlugins");
  auto getPlugin = (OfxPlugin* (*)(int))GetProcAddress(mod, "OfxGetPlugin");
#else
  mkdir(outdir.c_str(), 0755);
  void* mod = dlopen(lib.c_str(), RTLD_NOW | RTLD_LOCAL);
  if (!mod) { fprintf(stderr, "dlopen failed: %s\n", dlerror()); return 1; }
  auto getNum = (int (*)(void))dlsym(mod, "OfxGetNumberOfPlugins");
  auto getPlugin = (OfxPlugin* (*)(int))dlsym(mod, "OfxGetPlugin");
#endif
  CHECK(getNum && getPlugin, "OfxGetNumberOfPlugins / OfxGetPlugin not exported");
  if (!getNum || !getPlugin) return 1;
  int n = getNum();
  CHECK(n == 1, "expected 1 plugin, got %d", n);
  OfxPlugin* plugin = getPlugin(0);
  CHECK(plugin && std::string(plugin->pluginApi) == kOfxImageEffectPluginApi, "plugin api");
  CHECK(plugin && std::string(plugin->pluginIdentifier) == "ch.platemask.PlateMask", "identifier %s", plugin ? plugin->pluginIdentifier : "?");
  printf("plugin: %s v%d.%d api %s v%d\n", plugin->pluginIdentifier, plugin->pluginVersionMajor, plugin->pluginVersionMinor, plugin->pluginApi, plugin->apiVersion);

  PropSet hostProps;
  setHostProps(hostProps);
  OfxHost host{(OfxPropertySetHandle)&hostProps, fetchSuite};
  plugin->setHost(&host);
  OfxStatus st = plugin->mainEntry(kOfxActionLoad, nullptr, nullptr, nullptr);
  CHECK(st == kOfxStatOK || st == kOfxStatReplyDefault, "load -> %d", st);

  Effect desc;
  desc.props.setS(kOfxPropType, kOfxTypeImageEffect);
  st = plugin->mainEntry(kOfxActionDescribe, &desc, nullptr, nullptr);
  CHECK(st == kOfxStatOK || st == kOfxStatReplyDefault, "describe -> %d", st);
  char* label = nullptr;
  propGetString((OfxPropertySetHandle)&desc.props, kOfxPropLabel, 0, &label);
  printf("describe: label='%s'\n", label ? label : "");
  int ctxCount = 0; propGetDimension((OfxPropertySetHandle)&desc.props, kOfxImageEffectPropSupportedContexts, &ctxCount);
  CHECK(ctxCount >= 1, "supported contexts");
  int temporal = 0; propGetInt((OfxPropertySetHandle)&desc.props, kOfxImageEffectPropTemporalClipAccess, 0, &temporal);
  CHECK(temporal == 1, "temporal clip access declared");

  Effect ctxDesc;
  ctxDesc.props = desc.props;
  ctxDesc.props.setS(kOfxImageEffectPropContext, kOfxImageEffectContextFilter);
  PropSet inArgs;
  inArgs.setS(kOfxImageEffectPropContext, kOfxImageEffectContextFilter);
  st = plugin->mainEntry(kOfxImageEffectActionDescribeInContext, &ctxDesc, (OfxPropertySetHandle)&inArgs, nullptr);
  CHECK(st == kOfxStatOK || st == kOfxStatReplyDefault, "describeInContext -> %d", st);
  CHECK(ctxDesc.clips.count(kOfxImageEffectSimpleSourceClipName) && ctxDesc.clips.count(kOfxImageEffectOutputClipName), "clips defined");
  printf("describeInContext: %zu params, %zu clips\n", ctxDesc.params.params.size(), ctxDesc.clips.size());
  for (const char* p : {"seedPoint", "detectPlate", "trackPlate", "detectRequest", "trackRequest", "requestFrame", "requestPlate", "status", "expand", "feather", "obfuscation", "blockSize"})
    CHECK(ctxDesc.params.params.count(p), "param %s defined", p);

  // ---- instance
  Effect inst;
  inst.props = ctxDesc.props;
  setInstanceProps(inst, kOfxImageEffectContextFilter);
  inst.params = ctxDesc.params;
  for (auto& kv : inst.params.params) kv.second.initDefault();
  inst.clips = ctxDesc.clips;
  for (auto& kv : inst.clips) setClipInstanceProps(kv.second);
  st = plugin->mainEntry(kOfxActionCreateInstance, &inst, nullptr, nullptr);
  CHECK(st == kOfxStatOK || st == kOfxStatReplyDefault, "createInstance -> %d", st);
  std::string id = getStringParam(inst, "instanceId");
  CHECK(id.size() == 32, "instance id set by plugin (%s)", id.c_str());
  printf("instance id %s\n", id.c_str());

  // ---- request detection + tracking like the UI does (seed on the plate at frame 10)
  setParam(inst, "seedPoint", 565.0 / synth::W, 270.0 / synth::H);
  setParam(inst, "requestPlate", 0);
  setParam(inst, "requestFrame", 10);
  setParam(inst, "detectRequest", 1);
  setParam(inst, "trackRequest", 1);
  setParam(inst, "notifyWhenDone", 1);
  setParam(inst, "blockSize", 10);
  setParam(inst, "expand", 4);

  auto render = [&](int frame, std::vector<float>& out) -> OfxStatus {
    out.assign((size_t)synth::W * synth::H * 4, 0.f);
    gOutputBuffer = &out;
    PropSet ra;
    ra.setD(kOfxPropTime, frame);
    ra.setD(kOfxImageEffectPropRenderScale, 1, 0); ra.setD(kOfxImageEffectPropRenderScale, 1, 1);
    ra.setI(kOfxImageEffectPropRenderWindow, 0, 0); ra.setI(kOfxImageEffectPropRenderWindow, 0, 1);
    ra.setI(kOfxImageEffectPropRenderWindow, synth::W, 2); ra.setI(kOfxImageEffectPropRenderWindow, synth::H, 3);
    ra.setS(kOfxImageEffectPropFieldToRender, kOfxImageFieldNone);
    ra.setI(kOfxPropIsInteractive, 0);
    ra.setI(kOfxImageEffectPropSequentialRenderStatus, 0);
    ra.setI(kOfxImageEffectPropInteractiveRenderStatus, 0);
    ra.setI(kOfxImageEffectPropRenderQualityDraft, 0);
    OfxStatus s = plugin->mainEntry(kOfxImageEffectActionRender, &inst, (OfxPropertySetHandle)&ra, nullptr);
    gOutputBuffer = nullptr;
    return s;
  };

  std::vector<float> out;
  auto t0 = std::chrono::steady_clock::now();
  st = render(0, out);
  double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  CHECK(st == kOfxStatOK || st == kOfxStatReplyDefault, "render(0) with detect+track -> %d", st);
  printf("render(0) incl. detection + tracking: %.2f s, images alive after: %d\n", secs, gImagesAlive);
  CHECK(gImagesAlive == 0, "all fetched images released (%d alive)", gImagesAlive);

  // ---- inspect the track through the sidecar the plugin wrote
  pm::Store store;
  std::string sidecar = pm::sidecarPath(id);
  CHECK(pm::loadStore(sidecar, store), "sidecar readable at %s", sidecar.c_str());
  const pm::PlateTrack& pt = store.plates[0];
  int cnt[8] = {0};
  for (auto& kv : pt.keys) cnt[std::min(7, std::max(0, (int)kv.second.st))]++;
  printf("track: %zu keys (seed %d) confirmed=%d partial=%d inferred=%d occluded=%d out=%d lost=%d\n", pt.keys.size(), pt.seedFrame,
         cnt[1], cnt[2], cnt[7], cnt[3], cnt[4], cnt[5]);
  printf("status: %s\n", store.status.c_str());
  CHECK(pt.seedFrame == 10, "seed frame");
  CHECK(pt.keys.size() >= (size_t)synth::FRAMES - 1, "tracked the whole clip (%zu keys)", pt.keys.size());
  CHECK(cnt[1] + cnt[2] >= 100, "most frames confirmed/partial");
  CHECK(cnt[7] >= 5, "inferred frames present (car-body estimation)");
  CHECK(cnt[4] >= 5, "out-of-frame frames present");
  pm::Key k10;
  CHECK(pt.keyAt(10, false, k10), "key at seed frame");
  synth::Truth tr10; pm::Gray g10; synth::render(10, g10, tr10);
  // synth truth is y-down; the plugin works in canonical y-up on the upright picture
  pm::Quad truthUp;
  for (int i = 0; i < 4; ++i) truthUp[i] = {tr10.q[i].x, synth::H - tr10.q[i].y};
  double iou = pm::quadIoU(k10.q, truthUp);
  printf("seed-frame IoU vs truth: %.3f\n", iou);
  CHECK(iou > 0.5, "detected box overlaps the plate (IoU %.3f)", iou);

  // ---- the rendered frames must be modified inside the plate and untouched outside
  for (int f : {10, 60, 170}) {
    st = render(f, out);
    CHECK(st == kOfxStatOK || st == kOfxStatReplyDefault, "render(%d) -> %d", f, st);
    synth::Truth tr; pm::Gray g; synth::render(f, g, tr);
    pm::Key k; if (!pt.keyAt(f, false, k)) continue;
    pm::Rect b = pm::quadBounds(k.q);
    double inDiff = 0, outDiff = 0; int inN = 0, outN = 0;
    for (int y = 0; y < synth::H; ++y)
      for (int x = 0; x < synth::W; ++x) {
        float src = g.at(x, synth::H - 1 - y);
        float dst = out[((size_t)y * synth::W + x) * 4];
        bool inside = pm::quadSignedDistance(k.q, {x + 0.5, y + 0.5}) < -2;
        bool farOut = x < b.x1 - 40 || x > b.x2 + 40 || y < b.y1 - 40 || y > b.y2 + 40;
        if (inside) { inDiff += std::fabs(dst - src); ++inN; }
        else if (farOut) { outDiff += std::fabs(dst - src); ++outN; }
      }
    printf("frame %3d [%s]: mean|out-src| inside=%.3f outside=%.5f\n", f, pm::visStateName(k.st), inN ? inDiff / inN : 0, outN ? outDiff / outN : 0);
    if (pm::isEstimated(k.st) && inN > 0 && (k.st != pm::VisState::Inferred)) CHECK(inDiff / inN > 0.03, "frame %d: plate region obfuscated", f);
    CHECK(outN > 0 && outDiff / outN < 1e-6, "frame %d: pixels outside the mask untouched", f);
    writePPM(outdir + "/minihost_f" + std::to_string(f) + ".ppm", out, synth::W, synth::H);
  }

  st = plugin->mainEntry(kOfxActionDestroyInstance, &inst, nullptr, nullptr);
  CHECK(st == kOfxStatOK || st == kOfxStatReplyDefault, "destroyInstance -> %d", st);
  st = plugin->mainEntry(kOfxActionUnload, nullptr, nullptr, nullptr);
  CHECK(st == kOfxStatOK || st == kOfxStatReplyDefault, "unload -> %d", st);
  std::remove(sidecar.c_str());
  printf(gFails ? "\nMINIHOST: %d FAILURES\n" : "\nMINIHOST: ALL CHECKS PASSED\n", gFails);
  return gFails ? 1 : 0;
}
