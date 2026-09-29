#pragma once
#include <mutex>
#include <string>
#include <memory>
#include "ofxsImageEffect.h"
#include "ofxsInteract.h"
#include "core/TrackStore.h"
#include "core/Obfuscate.h"

class PlateMaskPlugin : public OFX::ImageEffect {
 public:
  explicit PlateMaskPlugin(OfxImageEffectHandle handle);
  ~PlateMaskPlugin() override;

  void render(const OFX::RenderArguments& args) override;
  bool isIdentity(const OFX::IsIdentityArguments& args, OFX::Clip*& identityClip, double& identityTime) override;
  void changedParam(const OFX::InstanceChangedArgs& args, const std::string& paramName) override;
  void syncPrivateData() override;
  void getFramesNeeded(const OFX::FramesNeededArguments& args, OFX::FramesNeededSetter& frames) override;

  // ---- shared with the overlay
  std::recursive_mutex& storeMutex() { return mu_; }
  pm::Store& store() { return store_; }           // hold storeMutex()
  void loadStoreIfChangedLocked();                 // re-read the sidecar if another instance wrote it
  void saveStoreLocked();                          // write sidecar, mark param flush pending
  void flushStoreToParamLocked();                  // copy to the hidden string param (only from allowed actions)
  int activePlate();
  bool overlayEnabled();
  OFX::Double2DParam* seedParam() { return seedPoint_; }
  pm::Pt normToCanon(const pm::Pt& n);
  pm::Pt canonToNorm(const pm::Pt& c);

 private:
  struct GrayBuild;
  bool buildGrayFrame(OFX::Image* img, int frame, pm::GrayFrame& out);
  void handleRequestsLocked(const OFX::RenderArguments& args);
  void runDetectLocked(int frame, int plate, pm::Pt seedCanon, double radiusFrac);
  void runTrackLocked(int plate, const OFX::RenderArguments& args);
  pm::TrackerParams trackerParams();
  std::string summaryFor(int plate, const char* prefix);
  bool anyLive() const;

  OFX::Clip* dstClip_ = nullptr;
  OFX::Clip* srcClip_ = nullptr;

  OFX::ChoiceParam* plateIndex_ = nullptr;
  OFX::Double2DParam* seedPoint_ = nullptr;
  OFX::DoubleParam* searchRadius_ = nullptr;
  OFX::ChoiceParam* trackDirection_ = nullptr;
  OFX::IntParam* maxFrames_ = nullptr;
  OFX::IntParam* occlusionHold_ = nullptr;
  OFX::DoubleParam* growPerFrame_ = nullptr;
  OFX::IntParam* gapInterp_ = nullptr;
  OFX::BooleanParam* holdOutside_ = nullptr;
  OFX::BooleanParam* showOverlay_ = nullptr;
  OFX::StringParam* status_ = nullptr;
  OFX::BooleanParam* notify_ = nullptr;
  OFX::DoubleParam* expand_ = nullptr;
  OFX::DoubleParam* feather_ = nullptr;
  OFX::BooleanParam* invert_ = nullptr;
  OFX::BooleanParam* maskOnly_ = nullptr;
  OFX::BooleanParam* maskToAlpha_ = nullptr;
  OFX::ChoiceParam* obfMode_ = nullptr;
  OFX::DoubleParam* blurRadius_ = nullptr;
  OFX::IntParam* blockSize_ = nullptr;
  OFX::ChoiceParam* randomMode_ = nullptr;
  OFX::BooleanParam* destructive_ = nullptr;
  OFX::IntParam* quantLevels_ = nullptr;
  OFX::DoubleParam* noiseAmount_ = nullptr;
  OFX::StringParam* instanceId_ = nullptr;
  OFX::StringParam* trackData_ = nullptr;
  OFX::IntParam* detectRequest_ = nullptr;
  OFX::IntParam* trackRequest_ = nullptr;
  OFX::IntParam* clearRequest_ = nullptr;
  OFX::IntParam* stopRequest_ = nullptr;
  OFX::ChoiceParam* trackMode_ = nullptr;
  OFX::IntParam* requestFrame_ = nullptr;
  OFX::IntParam* requestPlate_ = nullptr;
  OFX::IntParam* revision_ = nullptr;

  std::recursive_mutex mu_;
  pm::Store store_;
  std::string id_;
  std::string sidecar_;
  long long sidecarMTime_ = 0;
  bool paramFlushPending_ = false;
};

class PlateOverlay : public OFX::OverlayInteract {
 public:
  PlateOverlay(OfxInteractHandle handle, OFX::ImageEffect* effect);
  bool draw(const OFX::DrawArgs& args) override;
  bool penDown(const OFX::PenArgs& args) override;
  bool penMotion(const OFX::PenArgs& args) override;
  bool penUp(const OFX::PenArgs& args) override;

 private:
  PlateMaskPlugin* fx_ = nullptr;
  int dragPlate_ = -1, dragCorner_ = -1;
};

class PlateOverlayDescriptor : public OFX::DefaultEffectOverlayDescriptor<PlateOverlayDescriptor, PlateOverlay> {};

class PlateMaskPluginFactory : public OFX::PluginFactoryHelper<PlateMaskPluginFactory> {
 public:
  PlateMaskPluginFactory();
  void describe(OFX::ImageEffectDescriptor& desc) override;
  void describeInContext(OFX::ImageEffectDescriptor& desc, OFX::ContextEnum context) override;
  OFX::ImageEffect* createInstance(OfxImageEffectHandle handle, OFX::ContextEnum context) override;
};
