#pragma once
// Parameter script names shared by the effect and the overlay.
namespace pmx {
constexpr const char* kPluginIdentifier = "ch.platemask.PlateMask";
constexpr const char* kPluginName = "License Plate Mask";
constexpr const char* kPluginGrouping = "PlateMask";
constexpr int kVersionMajor = 1, kVersionMinor = 0;

constexpr const char* pPlateIndex = "plateIndex";
constexpr const char* pSeedPoint = "seedPoint";
constexpr const char* pDetect = "detectPlate";
constexpr const char* pTrack = "trackPlate";
constexpr const char* pStop = "stopTracking";
constexpr const char* pTrackMode = "trackingMode";
constexpr const char* pClearPlate = "clearPlate";
constexpr const char* pClearAll = "clearAll";
constexpr const char* pSearchRadius = "searchRadius";
constexpr const char* pTrackDirection = "trackDirection";
constexpr const char* pMaxFrames = "maxFrames";
constexpr const char* pOcclusionHold = "occlusionHold";
constexpr const char* pGrowPerFrame = "growPerFrame";
constexpr const char* pGapInterp = "gapInterp";
constexpr const char* pHoldOutside = "holdOutside";
constexpr const char* pShowOverlay = "showOverlay";
constexpr const char* pStatus = "status";
constexpr const char* pNotify = "notifyWhenDone";
constexpr const char* pExpand = "expand";
constexpr const char* pFeather = "feather";
constexpr const char* pInvert = "invertMask";
constexpr const char* pMaskOnly = "maskOnly";
constexpr const char* pMaskToAlpha = "maskToAlpha";
constexpr const char* pObfMode = "obfuscation";
constexpr const char* pBlurRadius = "blurRadius";
constexpr const char* pBlockSize = "blockSize";
constexpr const char* pRandomMode = "randomMode";
constexpr const char* pDestructive = "destructiveLock";
constexpr const char* pQuantLevels = "quantLevels";
constexpr const char* pNoiseAmount = "noiseAmount";
// hidden
constexpr const char* pInstanceId = "instanceId";
constexpr const char* pTrackData = "trackData";
constexpr const char* pDetectRequest = "detectRequest";
constexpr const char* pTrackRequest = "trackRequest";
constexpr const char* pClearRequest = "clearRequest";
constexpr const char* pStopRequest = "stopRequest";
constexpr const char* pRequestFrame = "requestFrame";
constexpr const char* pRequestPlate = "requestPlate";
constexpr const char* pRevision = "revision";
}  // namespace pmx
