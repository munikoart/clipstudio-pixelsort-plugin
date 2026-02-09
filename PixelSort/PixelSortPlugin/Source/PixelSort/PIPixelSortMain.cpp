// PixelSort Filter Plugin for Clip Studio Paint
// Built using Triglav Plugin SDK
// Ports the pixel sorting algorithm from the Python PixelSorting project.

#include "TriglavPlugInSDK/TriglavPlugInSDK.h"
#include "PlugInCommon/PIPixelSort.h"
#include "PlugInCommon/PISpanDetector.h"
#include <vector>
#include <algorithm>
#include <random>
#include <cmath>

// ---------------------------------------------------------------------------
// Property item keys
// ---------------------------------------------------------------------------

static const int kItemKeyDirection      = 1;
static const int kItemKeySortKey        = 2;
static const int kItemKeyIntervalMode   = 3;
static const int kItemKeyLowerThreshold = 4;
static const int kItemKeyUpperThreshold = 5;
static const int kItemKeyReverse        = 6;
static const int kItemKeyJitter         = 7;
static const int kItemKeySpanMin        = 8;
static const int kItemKeySpanMax        = 9;
static const int kItemKeyAngle         = 10;
static const int kItemKeyFalloff       = 11;

// ---------------------------------------------------------------------------
// String resource IDs
// ---------------------------------------------------------------------------

static const int kStringIDFilterCategoryName        = 101;
static const int kStringIDFilterName                = 102;
static const int kStringIDItemCaptionDirection       = 103;
static const int kStringIDItemCaptionSortKey         = 104;
static const int kStringIDItemCaptionIntervalMode    = 105;
static const int kStringIDItemCaptionLowerThreshold  = 106;
static const int kStringIDItemCaptionUpperThreshold  = 107;
static const int kStringIDItemCaptionReverse         = 108;
static const int kStringIDItemCaptionJitter          = 109;
static const int kStringIDItemCaptionSpanMin         = 110;
static const int kStringIDItemCaptionSpanMax         = 111;
static const int kStringIDItemCaptionAngle           = 112;
static const int kStringIDItemCaptionFalloff         = 113;

// ---------------------------------------------------------------------------
// Filter info struct (persistent across calls)
// ---------------------------------------------------------------------------

struct PixelSortFilterInfo
{
	PixelSortParams params;
	TriglavPlugInPropertyService* pPropertyService;
	TriglavPlugInPropertyService2* pPropertyService2;
};

// ---------------------------------------------------------------------------
// Helper: read all property values from the property object
// ---------------------------------------------------------------------------

static void ReadAllProperties(
	PixelSortFilterInfo* pInfo,
	TriglavPlugInPropertyObject propertyObject)
{
	if (pInfo == NULL || pInfo->pPropertyService == NULL) return;

	TriglavPlugInPropertyService* ps = pInfo->pPropertyService;
	TriglavPlugInPropertyService2* ps2 = pInfo->pPropertyService2;
	TriglavPlugInInt val;

	// Enumeration properties (via propertyService2)
	if (ps2 != NULL)
	{
		ps2->getEnumerationValueProc(&val, propertyObject, kItemKeyDirection);
		pInfo->params.direction = static_cast<SortDirection>(val);

		ps2->getEnumerationValueProc(&val, propertyObject, kItemKeySortKey);
		pInfo->params.sortKey = static_cast<SortKey>(val);

		ps2->getEnumerationValueProc(&val, propertyObject, kItemKeyIntervalMode);
		pInfo->params.intervalMode = static_cast<IntervalMode>(val);
	}

	// Integer properties (via propertyService)
	ps->getIntegerValueProc(&val, propertyObject, kItemKeyLowerThreshold);
	pInfo->params.lowerThreshold = val;

	ps->getIntegerValueProc(&val, propertyObject, kItemKeyUpperThreshold);
	pInfo->params.upperThreshold = val;

	TriglavPlugInBool boolVal;
	ps->getBooleanValueProc(&boolVal, propertyObject, kItemKeyReverse);
	pInfo->params.reverse = (boolVal != 0);

	ps->getIntegerValueProc(&val, propertyObject, kItemKeyJitter);
	pInfo->params.jitter = val;

	ps->getIntegerValueProc(&val, propertyObject, kItemKeySpanMin);
	pInfo->params.spanMin = val;

	ps->getIntegerValueProc(&val, propertyObject, kItemKeySpanMax);
	pInfo->params.spanMax = val;

	ps->getIntegerValueProc(&val, propertyObject, kItemKeyAngle);
	pInfo->params.angle = val;

	ps->getIntegerValueProc(&val, propertyObject, kItemKeyFalloff);
	pInfo->params.falloff = val;

	ClampParams(pInfo->params);
}

// ---------------------------------------------------------------------------
// Property callback
// ---------------------------------------------------------------------------

static void TRIGLAV_PLUGIN_CALLBACK TriglavPlugInFilterPropertyCallBack(
	TriglavPlugInInt* result,
	TriglavPlugInPropertyObject propertyObject,
	const TriglavPlugInInt itemKey,
	const TriglavPlugInInt notify,
	TriglavPlugInPtr data)
{
	*result = kTriglavPlugInPropertyCallBackResultNoModify;

	PixelSortFilterInfo* pInfo = static_cast<PixelSortFilterInfo*>(data);
	if (pInfo == NULL || pInfo->pPropertyService == NULL) return;

	if (notify == kTriglavPlugInPropertyCallBackNotifyValueChanged)
	{
		// Re-read all properties when any value changes
		PixelSortParams oldParams = pInfo->params;
		ReadAllProperties(pInfo, propertyObject);

		// Check if anything actually changed
		if (memcmp(&oldParams, &pInfo->params, sizeof(PixelSortParams)) != 0)
		{
			*result = kTriglavPlugInPropertyCallBackResultModify;
		}
	}
}

// ---------------------------------------------------------------------------
// Sort a single line (row or column) of pixels
// ---------------------------------------------------------------------------

static void SortLine(
	const RowAccessor& row,
	const PixelSortParams& params,
	const BYTE* selectArea,       // NULL if no selection, otherwise 0-255 per pixel
	int selectPixelStride,        // stride between selection pixels
	int rowIndex,
	std::mt19937& rng,
	std::vector<Span>& spansWork,
	std::vector<float>& brightnessWork,
	std::vector<PixelData>& pixelsWork,
	std::vector<int>& includedIndices)
{
	int n = row.length;
	if (n <= 0) return;

	// Detect spans
	DetectSpans(row, params, rowIndex, rng, spansWork, brightnessWork);

	for (int si = 0; si < static_cast<int>(spansWork.size()); ++si)
	{
		int spanStart = spansWork[si].start;
		int spanEnd   = spansWork[si].end;
		int spanLen   = spanEnd - spanStart;
		if (spanLen < 2) continue;

		// Falloff: randomly skip this span
		if (params.falloff > 0)
		{
			std::uniform_int_distribution<int> falloffDist(0, 99);
			if (falloffDist(rng) < params.falloff) continue;
		}

		// Collect pixels in span, respecting selection mask
		pixelsWork.clear();
		includedIndices.clear();

		for (int i = 0; i < spanLen; ++i)
		{
			int pixelIdx = spanStart + i;
			if (pixelIdx >= n) break;

			// Check selection mask
			if (selectArea != NULL)
			{
				BYTE sel = selectArea[pixelIdx * selectPixelStride];
				if (sel == 0) continue; // not selected, skip
			}

			PixelData pd;
			row.getRGB(pixelIdx, pd.r, pd.g, pd.b);
			pd.sortValue = GetSortValue(pd.r, pd.g, pd.b, params.sortKey);
			pixelsWork.push_back(pd);
			includedIndices.push_back(i);
		}

		int count = static_cast<int>(pixelsWork.size());
		if (count < 2) continue;

		// Sort by sort value
		std::sort(pixelsWork.begin(), pixelsWork.end(),
			[](const PixelData& a, const PixelData& b) {
				return a.sortValue < b.sortValue;
			});

		// Reverse if requested
		if (params.reverse)
		{
			std::reverse(pixelsWork.begin(), pixelsWork.end());
		}

		// Apply jitter if requested
		if (params.jitter > 0 && count > 1)
		{
			for (int i = 0; i < count; ++i)
			{
				std::uniform_int_distribution<int> dist(-params.jitter, params.jitter);
				int offset = dist(rng);
				int j = (std::max)(0, (std::min)(count - 1, i + offset));
				std::swap(pixelsWork[i], pixelsWork[j]);
			}
		}

		// Write sorted pixels back
		if (selectArea != NULL)
		{
			// Write back only to included positions
			for (int i = 0; i < count; ++i)
			{
				int pixelIdx = spanStart + includedIndices[i];
				BYTE sel = selectArea[pixelIdx * selectPixelStride];
				if (sel == 255)
				{
					row.setRGB(pixelIdx, pixelsWork[i].r, pixelsWork[i].g, pixelsWork[i].b);
				}
				else if (sel > 0)
				{
					// Partial selection: blend original and sorted pixel
					BYTE origR, origG, origB;
					row.getRGB(pixelIdx, origR, origG, origB);
					int alpha = sel;
					BYTE finalR = static_cast<BYTE>(((pixelsWork[i].r - origR) * alpha / 255) + origR);
					BYTE finalG = static_cast<BYTE>(((pixelsWork[i].g - origG) * alpha / 255) + origG);
					BYTE finalB = static_cast<BYTE>(((pixelsWork[i].b - origB) * alpha / 255) + origB);
					row.setRGB(pixelIdx, finalR, finalG, finalB);
				}
			}
		}
		else
		{
			for (int i = 0; i < count; ++i)
			{
				int pixelIdx = spanStart + includedIndices[i];
				row.setRGB(pixelIdx, pixelsWork[i].r, pixelsWork[i].g, pixelsWork[i].b);
			}
		}
	}
}

// ---------------------------------------------------------------------------
// Plugin main entry point
// ---------------------------------------------------------------------------

void TRIGLAV_PLUGIN_API TriglavPluginCall(
	TriglavPlugInInt* result,
	TriglavPlugInPtr* data,
	TriglavPlugInInt selector,
	TriglavPlugInServer* pluginServer,
	TriglavPlugInPtr reserved)
{
	*result = kTriglavPlugInCallResultFailed;

	try
	{
		if (pluginServer == NULL)
		{
			PixelSortLog("[PixelSort] ERROR: pluginServer is NULL\n");
			return;
		}

		// =================================================================
		// MODULE INITIALIZE
		// =================================================================
		if (selector == kTriglavPlugInSelectorModuleInitialize)
		{
			PixelSortLog("[PixelSort] ModuleInitialize\n");

			TriglavPlugInModuleInitializeRecord* pModuleInitializeRecord = (*pluginServer).recordSuite.moduleInitializeRecord;
			TriglavPlugInStringService* pStringService = (*pluginServer).serviceSuite.stringService;
			if (pModuleInitializeRecord == NULL || pStringService == NULL)
			{
				PixelSortLog("[PixelSort] ERROR: NULL record/service in ModuleInitialize\n");
				return;
			}

			TriglavPlugInInt hostVersion;
			(*pModuleInitializeRecord).getHostVersionProc(&hostVersion, (*pluginServer).hostObject);
			if (hostVersion < kTriglavPlugInNeedHostVersion)
			{
				PixelSortLog("[PixelSort] ERROR: host version %d < needed %d\n", hostVersion, kTriglavPlugInNeedHostVersion);
				return;
			}

			TriglavPlugInStringObject moduleID = NULL;
			const char* moduleIDString = "B7F3A1D4-92C6-4E8B-A5D1-7C3F0E9B2A68";
			(*pStringService).createWithAsciiStringProc(&moduleID, moduleIDString, static_cast<TriglavPlugInInt>(::strlen(moduleIDString)));
			(*pModuleInitializeRecord).setModuleIDProc((*pluginServer).hostObject, moduleID);
			(*pModuleInitializeRecord).setModuleKindProc((*pluginServer).hostObject, kTriglavPlugInModuleSwitchKindFilter);
			(*pStringService).releaseProc(moduleID);

			PixelSortFilterInfo* pInfo = new PixelSortFilterInfo;
			pInfo->params = MakeDefaultParams();
			pInfo->pPropertyService = NULL;
			pInfo->pPropertyService2 = NULL;
			*data = pInfo;
			*result = kTriglavPlugInCallResultSuccess;
		}
		// =================================================================
		// MODULE TERMINATE
		// =================================================================
		else if (selector == kTriglavPlugInSelectorModuleTerminate)
		{
			PixelSortLog("[PixelSort] ModuleTerminate\n");
			PixelSortFilterInfo* pInfo = static_cast<PixelSortFilterInfo*>(*data);
			delete pInfo;
			*data = NULL;
			*result = kTriglavPlugInCallResultSuccess;
		}
		// =================================================================
		// FILTER INITIALIZE
		// =================================================================
		else if (selector == kTriglavPlugInSelectorFilterInitialize)
		{
			PixelSortLog("[PixelSort] FilterInitialize\n");

			TriglavPlugInRecordSuite*     pRecordSuite    = &(*pluginServer).recordSuite;
			TriglavPlugInHostObject       hostObject       = (*pluginServer).hostObject;
			TriglavPlugInStringService*   pStringService   = (*pluginServer).serviceSuite.stringService;
			TriglavPlugInPropertyService* pPropertyService = (*pluginServer).serviceSuite.propertyService;
			TriglavPlugInPropertyService2* pPropertyService2 = (*pluginServer).serviceSuite.propertyService2;

			if (TriglavPlugInGetFilterInitializeRecord(pRecordSuite) == NULL ||
				pStringService == NULL || pPropertyService == NULL)
			{
				PixelSortLog("[PixelSort] ERROR: NULL service in FilterInitialize\n");
				return;
			}

			if (pPropertyService2 == NULL)
			{
				PixelSortLog("[PixelSort] WARNING: propertyService2 is NULL, enumerations unavailable\n");
			}

			// Filter category and name
			TriglavPlugInStringObject filterCategoryName = NULL;
			TriglavPlugInStringObject filterName = NULL;
			(*pStringService).createWithStringIDProc(&filterCategoryName, kStringIDFilterCategoryName, hostObject);
			(*pStringService).createWithStringIDProc(&filterName, kStringIDFilterName, hostObject);
			TriglavPlugInFilterInitializeSetFilterCategoryName(pRecordSuite, hostObject, filterCategoryName, 'p');
			TriglavPlugInFilterInitializeSetFilterName(pRecordSuite, hostObject, filterName, 's');
			(*pStringService).releaseProc(filterCategoryName);
			(*pStringService).releaseProc(filterName);

			// Preview
			TriglavPlugInFilterInitializeSetCanPreview(pRecordSuite, hostObject, true);

			// Target: RGB layers only
			TriglavPlugInInt target[] = { kTriglavPlugInFilterTargetKindRasterLayerRGBAlpha };
			TriglavPlugInFilterInitializeSetTargetKinds(pRecordSuite, hostObject, target, 1);

			// Create property object
			TriglavPlugInPropertyObject propertyObject;
			(*pPropertyService).createProc(&propertyObject);

			// --- Direction (Enumeration: Horizontal, Vertical) ---
			{
				TriglavPlugInStringObject caption = NULL;
				(*pStringService).createWithStringIDProc(&caption, kStringIDItemCaptionDirection, hostObject);
				(*pPropertyService).addItemProc(propertyObject, kItemKeyDirection,
					kTriglavPlugInPropertyValueTypeEnumeration,
					kTriglavPlugInPropertyValueKindDefault,
					kTriglavPlugInPropertyInputKindDefault, caption, 'd');
				(*pStringService).releaseProc(caption);

				if (pPropertyService2 != NULL)
				{
					TriglavPlugInStringObject s = NULL;
					(*pStringService).createWithAsciiStringProc(&s, "Horizontal", 10);
					(*pPropertyService2).addEnumerationItemProc(propertyObject, kItemKeyDirection, 0, s, 'h');
					(*pStringService).releaseProc(s);

					(*pStringService).createWithAsciiStringProc(&s, "Vertical", 8);
					(*pPropertyService2).addEnumerationItemProc(propertyObject, kItemKeyDirection, 1, s, 'v');
					(*pStringService).releaseProc(s);

					(*pPropertyService2).setEnumerationValueProc(propertyObject, kItemKeyDirection, 0);
					(*pPropertyService2).setEnumerationDefaultValueProc(propertyObject, kItemKeyDirection, 0);
				}
			}

			// --- Sort Key (Enumeration) ---
			{
				TriglavPlugInStringObject caption = NULL;
				(*pStringService).createWithStringIDProc(&caption, kStringIDItemCaptionSortKey, hostObject);
				(*pPropertyService).addItemProc(propertyObject, kItemKeySortKey,
					kTriglavPlugInPropertyValueTypeEnumeration,
					kTriglavPlugInPropertyValueKindDefault,
					kTriglavPlugInPropertyInputKindDefault, caption, 'k');
				(*pStringService).releaseProc(caption);

				if (pPropertyService2 != NULL)
				{
					static const char* sortKeyNames[] = {
						"Brightness", "Hue", "Saturation", "Intensity",
						"Minimum", "Red", "Green", "Blue"
					};
					static const char sortKeyAccess[] = { 'b', 'h', 's', 'i', 'm', 'r', 'g', 'u' };
					for (int i = 0; i < 8; ++i)
					{
						TriglavPlugInStringObject s = NULL;
						(*pStringService).createWithAsciiStringProc(&s, sortKeyNames[i],
							static_cast<TriglavPlugInInt>(::strlen(sortKeyNames[i])));
						(*pPropertyService2).addEnumerationItemProc(propertyObject, kItemKeySortKey, i, s, sortKeyAccess[i]);
						(*pStringService).releaseProc(s);
					}
					(*pPropertyService2).setEnumerationValueProc(propertyObject, kItemKeySortKey, 0);
					(*pPropertyService2).setEnumerationDefaultValueProc(propertyObject, kItemKeySortKey, 0);
				}
			}

			// --- Interval Mode (Enumeration) ---
			{
				TriglavPlugInStringObject caption = NULL;
				(*pStringService).createWithStringIDProc(&caption, kStringIDItemCaptionIntervalMode, hostObject);
				(*pPropertyService).addItemProc(propertyObject, kItemKeyIntervalMode,
					kTriglavPlugInPropertyValueTypeEnumeration,
					kTriglavPlugInPropertyValueKindDefault,
					kTriglavPlugInPropertyInputKindDefault, caption, 'm');
				(*pStringService).releaseProc(caption);

				if (pPropertyService2 != NULL)
				{
					static const char* modeNames[] = {
						"Threshold", "Random", "Edges", "Waves", "None"
					};
					static const char modeAccess[] = { 't', 'r', 'e', 'w', 'n' };
					for (int i = 0; i < 5; ++i)
					{
						TriglavPlugInStringObject s = NULL;
						(*pStringService).createWithAsciiStringProc(&s, modeNames[i],
							static_cast<TriglavPlugInInt>(::strlen(modeNames[i])));
						(*pPropertyService2).addEnumerationItemProc(propertyObject, kItemKeyIntervalMode, i, s, modeAccess[i]);
						(*pStringService).releaseProc(s);
					}
					(*pPropertyService2).setEnumerationValueProc(propertyObject, kItemKeyIntervalMode, 0);
					(*pPropertyService2).setEnumerationDefaultValueProc(propertyObject, kItemKeyIntervalMode, 0);
				}
			}

			// --- Lower Threshold (Integer 0-255, default 64) ---
			{
				TriglavPlugInStringObject caption = NULL;
				(*pStringService).createWithStringIDProc(&caption, kStringIDItemCaptionLowerThreshold, hostObject);
				(*pPropertyService).addItemProc(propertyObject, kItemKeyLowerThreshold,
					kTriglavPlugInPropertyValueTypeInteger,
					kTriglavPlugInPropertyValueKindDefault,
					kTriglavPlugInPropertyInputKindDefault, caption, 'l');
				(*pStringService).releaseProc(caption);
				(*pPropertyService).setIntegerValueProc(propertyObject, kItemKeyLowerThreshold, 64);
				(*pPropertyService).setIntegerDefaultValueProc(propertyObject, kItemKeyLowerThreshold, 64);
				(*pPropertyService).setIntegerMinValueProc(propertyObject, kItemKeyLowerThreshold, 0);
				(*pPropertyService).setIntegerMaxValueProc(propertyObject, kItemKeyLowerThreshold, 255);
			}

			// --- Upper Threshold (Integer 0-255, default 204) ---
			{
				TriglavPlugInStringObject caption = NULL;
				(*pStringService).createWithStringIDProc(&caption, kStringIDItemCaptionUpperThreshold, hostObject);
				(*pPropertyService).addItemProc(propertyObject, kItemKeyUpperThreshold,
					kTriglavPlugInPropertyValueTypeInteger,
					kTriglavPlugInPropertyValueKindDefault,
					kTriglavPlugInPropertyInputKindDefault, caption, 'u');
				(*pStringService).releaseProc(caption);
				(*pPropertyService).setIntegerValueProc(propertyObject, kItemKeyUpperThreshold, 204);
				(*pPropertyService).setIntegerDefaultValueProc(propertyObject, kItemKeyUpperThreshold, 204);
				(*pPropertyService).setIntegerMinValueProc(propertyObject, kItemKeyUpperThreshold, 0);
				(*pPropertyService).setIntegerMaxValueProc(propertyObject, kItemKeyUpperThreshold, 255);
			}

			// --- Reverse (Boolean, default false) ---
			{
				TriglavPlugInStringObject caption = NULL;
				(*pStringService).createWithStringIDProc(&caption, kStringIDItemCaptionReverse, hostObject);
				(*pPropertyService).addItemProc(propertyObject, kItemKeyReverse,
					kTriglavPlugInPropertyValueTypeBoolean,
					kTriglavPlugInPropertyValueKindDefault,
					kTriglavPlugInPropertyInputKindDefault, caption, 'r');
				(*pStringService).releaseProc(caption);
				(*pPropertyService).setBooleanValueProc(propertyObject, kItemKeyReverse, kTriglavPlugInBoolFalse);
				(*pPropertyService).setBooleanDefaultValueProc(propertyObject, kItemKeyReverse, kTriglavPlugInBoolFalse);
			}

			// --- Jitter (Integer 0-100, default 0) ---
			{
				TriglavPlugInStringObject caption = NULL;
				(*pStringService).createWithStringIDProc(&caption, kStringIDItemCaptionJitter, hostObject);
				(*pPropertyService).addItemProc(propertyObject, kItemKeyJitter,
					kTriglavPlugInPropertyValueTypeInteger,
					kTriglavPlugInPropertyValueKindDefault,
					kTriglavPlugInPropertyInputKindDefault, caption, 'j');
				(*pStringService).releaseProc(caption);
				(*pPropertyService).setIntegerValueProc(propertyObject, kItemKeyJitter, 0);
				(*pPropertyService).setIntegerDefaultValueProc(propertyObject, kItemKeyJitter, 0);
				(*pPropertyService).setIntegerMinValueProc(propertyObject, kItemKeyJitter, 0);
				(*pPropertyService).setIntegerMaxValueProc(propertyObject, kItemKeyJitter, 100);
			}

			// --- Span Min (Integer 1-10000, default 1) ---
			{
				TriglavPlugInStringObject caption = NULL;
				(*pStringService).createWithStringIDProc(&caption, kStringIDItemCaptionSpanMin, hostObject);
				(*pPropertyService).addItemProc(propertyObject, kItemKeySpanMin,
					kTriglavPlugInPropertyValueTypeInteger,
					kTriglavPlugInPropertyValueKindDefault,
					kTriglavPlugInPropertyInputKindDefault, caption, 'n');
				(*pStringService).releaseProc(caption);
				(*pPropertyService).setIntegerValueProc(propertyObject, kItemKeySpanMin, 1);
				(*pPropertyService).setIntegerDefaultValueProc(propertyObject, kItemKeySpanMin, 1);
				(*pPropertyService).setIntegerMinValueProc(propertyObject, kItemKeySpanMin, 1);
				(*pPropertyService).setIntegerMaxValueProc(propertyObject, kItemKeySpanMin, 10000);
			}

			// --- Span Max (Integer 0-10000, default 0 = unlimited) ---
			{
				TriglavPlugInStringObject caption = NULL;
				(*pStringService).createWithStringIDProc(&caption, kStringIDItemCaptionSpanMax, hostObject);
				(*pPropertyService).addItemProc(propertyObject, kItemKeySpanMax,
					kTriglavPlugInPropertyValueTypeInteger,
					kTriglavPlugInPropertyValueKindDefault,
					kTriglavPlugInPropertyInputKindDefault, caption, 'x');
				(*pStringService).releaseProc(caption);
				(*pPropertyService).setIntegerValueProc(propertyObject, kItemKeySpanMax, 0);
				(*pPropertyService).setIntegerDefaultValueProc(propertyObject, kItemKeySpanMax, 0);
				(*pPropertyService).setIntegerMinValueProc(propertyObject, kItemKeySpanMax, 0);
				(*pPropertyService).setIntegerMaxValueProc(propertyObject, kItemKeySpanMax, 10000);
			}

			// --- Angle (Integer 0-359, default 0, only applies when Direction=Horizontal) ---
			{
				TriglavPlugInStringObject caption = NULL;
				(*pStringService).createWithStringIDProc(&caption, kStringIDItemCaptionAngle, hostObject);
				(*pPropertyService).addItemProc(propertyObject, kItemKeyAngle,
					kTriglavPlugInPropertyValueTypeInteger,
					kTriglavPlugInPropertyValueKindDefault,
					kTriglavPlugInPropertyInputKindDefault, caption, 'a');
				(*pStringService).releaseProc(caption);
				(*pPropertyService).setIntegerValueProc(propertyObject, kItemKeyAngle, 0);
				(*pPropertyService).setIntegerDefaultValueProc(propertyObject, kItemKeyAngle, 0);
				(*pPropertyService).setIntegerMinValueProc(propertyObject, kItemKeyAngle, 0);
				(*pPropertyService).setIntegerMaxValueProc(propertyObject, kItemKeyAngle, 359);
			}

			// --- Falloff (Integer 0-100%, default 0) ---
			{
				TriglavPlugInStringObject caption = NULL;
				(*pStringService).createWithStringIDProc(&caption, kStringIDItemCaptionFalloff, hostObject);
				(*pPropertyService).addItemProc(propertyObject, kItemKeyFalloff,
					kTriglavPlugInPropertyValueTypeInteger,
					kTriglavPlugInPropertyValueKindDefault,
					kTriglavPlugInPropertyInputKindDefault, caption, 'f');
				(*pStringService).releaseProc(caption);
				(*pPropertyService).setIntegerValueProc(propertyObject, kItemKeyFalloff, 0);
				(*pPropertyService).setIntegerDefaultValueProc(propertyObject, kItemKeyFalloff, 0);
				(*pPropertyService).setIntegerMinValueProc(propertyObject, kItemKeyFalloff, 0);
				(*pPropertyService).setIntegerMaxValueProc(propertyObject, kItemKeyFalloff, 100);
			}

			// Set property and callback
			TriglavPlugInFilterInitializeSetProperty(pRecordSuite, hostObject, propertyObject);
			TriglavPlugInFilterInitializeSetPropertyCallBack(pRecordSuite, hostObject, TriglavPlugInFilterPropertyCallBack, *data);

			(*pPropertyService).releaseProc(propertyObject);
			*result = kTriglavPlugInCallResultSuccess;
		}
		// =================================================================
		// FILTER TERMINATE
		// =================================================================
		else if (selector == kTriglavPlugInSelectorFilterTerminate)
		{
			PixelSortLog("[PixelSort] FilterTerminate\n");
			*result = kTriglavPlugInCallResultSuccess;
		}
		// =================================================================
		// FILTER RUN
		// =================================================================
		else if (selector == kTriglavPlugInSelectorFilterRun)
		{
			PixelSortLog("[PixelSort] FilterRun\n");

			TriglavPlugInRecordSuite*      pRecordSuite      = &(*pluginServer).recordSuite;
			TriglavPlugInOffscreenService* pOffscreenService = (*pluginServer).serviceSuite.offscreenService;
			TriglavPlugInPropertyService*  pPropertyService  = (*pluginServer).serviceSuite.propertyService;
			TriglavPlugInPropertyService2* pPropertyService2 = (*pluginServer).serviceSuite.propertyService2;

			if (TriglavPlugInGetFilterRunRecord(pRecordSuite) == NULL ||
				pOffscreenService == NULL || pPropertyService == NULL)
			{
				PixelSortLog("[PixelSort] ERROR: NULL service in FilterRun\n");
				return;
			}

			TriglavPlugInPropertyObject propertyObject;
			TriglavPlugInFilterRunGetProperty(pRecordSuite, &propertyObject, (*pluginServer).hostObject);

			TriglavPlugInOffscreenObject sourceOffscreenObject;
			TriglavPlugInFilterRunGetSourceOffscreen(pRecordSuite, &sourceOffscreenObject, (*pluginServer).hostObject);

			TriglavPlugInOffscreenObject destinationOffscreenObject;
			TriglavPlugInFilterRunGetDestinationOffscreen(pRecordSuite, &destinationOffscreenObject, (*pluginServer).hostObject);

			TriglavPlugInRect selectAreaRect;
			TriglavPlugInFilterRunGetSelectAreaRect(pRecordSuite, &selectAreaRect, (*pluginServer).hostObject);

			TriglavPlugInOffscreenObject selectAreaOffscreenObject;
			TriglavPlugInFilterRunGetSelectAreaOffscreen(pRecordSuite, &selectAreaOffscreenObject, (*pluginServer).hostObject);

			TriglavPlugInInt rIdx, gIdx, bIdx;
			(*pOffscreenService).getRGBChannelIndexProc(&rIdx, &gIdx, &bIdx, destinationOffscreenObject);

			TriglavPlugInInt blockRectCount;
			(*pOffscreenService).getBlockRectCountProc(&blockRectCount, destinationOffscreenObject, &selectAreaRect);

			std::vector<TriglavPlugInRect> blockRects(blockRectCount);
			for (TriglavPlugInInt i = 0; i < blockRectCount; ++i)
			{
				(*pOffscreenService).getBlockRectProc(&blockRects[i], i, destinationOffscreenObject, &selectAreaRect);
			}

			TriglavPlugInFilterRunSetProgressTotal(pRecordSuite, (*pluginServer).hostObject, blockRectCount);

			PixelSortFilterInfo* pInfo = static_cast<PixelSortFilterInfo*>(*data);
			pInfo->pPropertyService = pPropertyService;
			pInfo->pPropertyService2 = pPropertyService2;
			pInfo->params = MakeDefaultParams();

			// Reusable work buffers
			std::vector<Span>      spansWork;
			std::vector<float>     brightnessWork;
			std::vector<PixelData> pixelsWork;
			std::vector<int>       includedIndices;

			bool restart = true;
			PixelSortParams currentParams = MakeDefaultParams();
			std::mt19937 rng(42); // fixed seed for deterministic preview

			TriglavPlugInInt blockIndex = 0;
			while (true)
			{
				if (restart)
				{
					restart = false;

					TriglavPlugInInt processResult;
					TriglavPlugInFilterRunProcess(pRecordSuite, &processResult, (*pluginServer).hostObject, kTriglavPlugInFilterRunProcessStateStart);
					if (processResult == kTriglavPlugInFilterRunProcessResultExit) break;

					blockIndex = 0;
					ReadAllProperties(pInfo, propertyObject);
					currentParams = pInfo->params;
					rng.seed(42); // re-seed for consistent preview

					PixelSortLog("[PixelSort] Params: dir=%d key=%d mode=%d lo=%d hi=%d rev=%d jit=%d smin=%d smax=%d ang=%d fall=%d\n",
						currentParams.direction, currentParams.sortKey, currentParams.intervalMode,
						currentParams.lowerThreshold, currentParams.upperThreshold,
						currentParams.reverse ? 1 : 0, currentParams.jitter,
						currentParams.spanMin, currentParams.spanMax, currentParams.angle,
						currentParams.falloff);

					// ----------------------------------------------------------
					// Full-image processing (avoids block fragmentation)
					// ----------------------------------------------------------
					{
						int fullW = selectAreaRect.right - selectAreaRect.left;
						int fullH = selectAreaRect.bottom - selectAreaRect.top;

						if (fullW > 0 && fullH > 0)
						{
							PixelSortLog("[PixelSort] Full-image: %dx%d ang=%d\n", fullW, fullH, currentParams.angle);

							// Gather full image from destination blocks (RGB)
							std::vector<BYTE> fullImage(fullW * fullH * 3, 0);
							std::vector<BYTE> origImage;
							std::vector<BYTE> fullSelect;
							bool hasSelection = (selectAreaOffscreenObject != NULL);

							for (TriglavPlugInInt bi = 0; bi < blockRectCount; ++bi)
							{
								TriglavPlugInRect br = blockRects[bi];
								TriglavPlugInPoint bpos; bpos.x = br.left; bpos.y = br.top;
								TriglavPlugInRect tmpR;
								TriglavPlugInPtr imgAddr; TriglavPlugInInt imgRB, imgPB;
								(*pOffscreenService).getBlockImageProc(&imgAddr, &imgRB, &imgPB, &tmpR, destinationOffscreenObject, &bpos);
								if (imgAddr != NULL)
								{
									int bw = br.right - br.left, bh = br.bottom - br.top;
									for (int y = 0; y < bh; ++y)
									{
										BYTE* sr = static_cast<BYTE*>(imgAddr) + y * imgRB;
										int fy = (br.top - selectAreaRect.top) + y;
										for (int x = 0; x < bw; ++x)
										{
											int fx = (br.left - selectAreaRect.left) + x;
											BYTE* sp = sr + x * imgPB;
											int idx = (fy * fullW + fx) * 3;
											fullImage[idx + 0] = sp[rIdx];
											fullImage[idx + 1] = sp[gIdx];
											fullImage[idx + 2] = sp[bIdx];
										}
									}
								}
								if (hasSelection)
								{
									TriglavPlugInPtr selAddr; TriglavPlugInInt selRB, selPB;
									(*pOffscreenService).getBlockSelectAreaProc(&selAddr, &selRB, &selPB, &tmpR, selectAreaOffscreenObject, &bpos);
									if (selAddr != NULL)
									{
										fullSelect.resize(fullW * fullH, 0);
										int bw = br.right - br.left, bh = br.bottom - br.top;
										for (int y = 0; y < bh; ++y)
										{
											BYTE* sr = static_cast<BYTE*>(selAddr) + y * selRB;
											int fy = (br.top - selectAreaRect.top) + y;
											for (int x = 0; x < bw; ++x)
											{
												int fx = (br.left - selectAreaRect.left) + x;
												fullSelect[fy * fullW + fx] = sr[x * selPB];
											}
										}
									}
								}
							}

							// Determine sort buffer and dimensions
							bool useAngle = (currentParams.angle != 0 && currentParams.direction == kSortDirectionHorizontal);
							BYTE* sortBuf = fullImage.data();
							int sortW = fullW, sortH = fullH;
							std::vector<BYTE> rotImage;
							double cosA = 1.0, sinA = 0.0;
							double cx = 0, cy = 0, rcx = 0, rcy = 0;

							if (useAngle)
							{
								double rad = currentParams.angle * M_PI / 180.0;
								cosA = cos(rad); sinA = sin(rad);
								sortW = (int)ceil(fabs(fullW * cosA) + fabs(fullH * sinA));
								sortH = (int)ceil(fabs(fullW * sinA) + fabs(fullH * cosA));
								if (sortW < 1) sortW = 1;
								if (sortH < 1) sortH = 1;

								rotImage.resize(sortW * sortH * 3, 0);
								cx = (fullW - 1) / 2.0; cy = (fullH - 1) / 2.0;
								rcx = (sortW - 1) / 2.0; rcy = (sortH - 1) / 2.0;

								for (int ry = 0; ry < sortH; ++ry)
								{
									for (int rx = 0; rx < sortW; ++rx)
									{
										double dx = rx - rcx, dy = ry - rcy;
										int sx = (int)(dx * cosA - dy * sinA + cx + 0.5);
										int sy = (int)(dx * sinA + dy * cosA + cy + 0.5);
										if (sx >= 0 && sx < fullW && sy >= 0 && sy < fullH)
										{
											int si = (sy * fullW + sx) * 3;
											int di = (ry * sortW + rx) * 3;
											rotImage[di]     = fullImage[si];
											rotImage[di + 1] = fullImage[si + 1];
											rotImage[di + 2] = fullImage[si + 2];
										}
									}
								}
								sortBuf = rotImage.data();
							}

							// Save original for selection blending
							if (hasSelection && !fullSelect.empty())
								origImage = fullImage;

							// Sort rows or columns on the sort buffer
							if (currentParams.direction == kSortDirectionHorizontal)
							{
								for (int y = 0; y < sortH; ++y)
								{
									RowAccessor row;
									row.imageBase = sortBuf + y * sortW * 3;
									row.imagePixelBytes = 3;
									row.imageRowBytes = sortW * 3;
									row.rIdx = 0; row.gIdx = 1; row.bIdx = 2;
									row.length = sortW;
									row.vertical = false;

									const BYTE* selRow = NULL;
									int selStride = 0;
									if (!useAngle && !fullSelect.empty())
									{
										selRow = fullSelect.data() + y * fullW;
										selStride = 1;
									}

									SortLine(row, currentParams, selRow, selStride, y, rng,
										spansWork, brightnessWork, pixelsWork, includedIndices);
								}
							}
							else // VERTICAL
							{
								for (int x = 0; x < sortW; ++x)
								{
									RowAccessor col;
									col.imageBase = sortBuf + x * 3;
									col.imagePixelBytes = 3;
									col.imageRowBytes = sortW * 3;
									col.rIdx = 0; col.gIdx = 1; col.bIdx = 2;
									col.length = sortH;
									col.vertical = true;

									const BYTE* selCol = NULL;
									int selStride = 0;
									if (!fullSelect.empty())
									{
										selCol = fullSelect.data() + x;
										selStride = fullW;
									}

									SortLine(col, currentParams, selCol, selStride, x, rng,
										spansWork, brightnessWork, pixelsWork, includedIndices);
								}
							}

							// Unrotate if needed
							if (useAngle)
							{
								std::fill(fullImage.begin(), fullImage.end(), (BYTE)0);
								for (int fy = 0; fy < fullH; ++fy)
								{
									for (int fx = 0; fx < fullW; ++fx)
									{
										double dx = fx - cx, dy = fy - cy;
										int rx = (int)(dx * cosA + dy * sinA + rcx + 0.5);
										int ry = (int)(-dx * sinA + dy * cosA + rcy + 0.5);
										if (rx >= 0 && rx < sortW && ry >= 0 && ry < sortH)
										{
											int si = (ry * sortW + rx) * 3;
											int di = (fy * fullW + fx) * 3;
											fullImage[di]     = rotImage[si];
											fullImage[di + 1] = rotImage[si + 1];
											fullImage[di + 2] = rotImage[si + 2];
										}
									}
								}
							}

							// Apply selection blending (for angle mode, blend after unrotate)
							if (useAngle && hasSelection && !fullSelect.empty())
							{
								for (int i = 0; i < fullW * fullH; ++i)
								{
									BYTE sel = fullSelect[i];
									if (sel == 0)
									{
										fullImage[i * 3 + 0] = origImage[i * 3 + 0];
										fullImage[i * 3 + 1] = origImage[i * 3 + 1];
										fullImage[i * 3 + 2] = origImage[i * 3 + 2];
									}
									else if (sel < 255)
									{
										for (int c = 0; c < 3; ++c)
										{
											int idx = i * 3 + c;
											fullImage[idx] = static_cast<BYTE>(
												((fullImage[idx] - origImage[idx]) * sel / 255) + origImage[idx]);
										}
									}
								}
							}

							// Write back to destination blocks
							for (TriglavPlugInInt bi = 0; bi < blockRectCount; ++bi)
							{
								TriglavPlugInRect br = blockRects[bi];
								TriglavPlugInPoint bpos; bpos.x = br.left; bpos.y = br.top;
								TriglavPlugInRect tmpR;
								TriglavPlugInPtr imgAddr; TriglavPlugInInt imgRB, imgPB;
								(*pOffscreenService).getBlockImageProc(&imgAddr, &imgRB, &imgPB, &tmpR, destinationOffscreenObject, &bpos);
								if (imgAddr != NULL)
								{
									int bw = br.right - br.left, bh = br.bottom - br.top;
									for (int y = 0; y < bh; ++y)
									{
										BYTE* dr = static_cast<BYTE*>(imgAddr) + y * imgRB;
										int fy = (br.top - selectAreaRect.top) + y;
										for (int x = 0; x < bw; ++x)
										{
											int fx = (br.left - selectAreaRect.left) + x;
											int idx = (fy * fullW + fx) * 3;
											BYTE* dp = dr + x * imgPB;
											dp[rIdx] = fullImage[idx + 0];
											dp[gIdx] = fullImage[idx + 1];
											dp[bIdx] = fullImage[idx + 2];
										}
									}
								}
								TriglavPlugInFilterRunUpdateDestinationOffscreenRect(pRecordSuite, (*pluginServer).hostObject, &br);
							}
						}
						blockIndex = blockRectCount;
					}
				}

				if (blockIndex < blockRectCount)
				{
					// Fallback: should not reach here with full-image processing
					++blockIndex;
				}

				TriglavPlugInInt processResult;
				if (blockIndex < blockRectCount)
				{
					TriglavPlugInFilterRunProcess(pRecordSuite, &processResult, (*pluginServer).hostObject, kTriglavPlugInFilterRunProcessStateContinue);
				}
				else
				{
					TriglavPlugInFilterRunSetProgressDone(pRecordSuite, (*pluginServer).hostObject, blockIndex);
					TriglavPlugInFilterRunProcess(pRecordSuite, &processResult, (*pluginServer).hostObject, kTriglavPlugInFilterRunProcessStateEnd);
				}

				if (processResult == kTriglavPlugInFilterRunProcessResultRestart)
				{
					restart = true;
				}
				else if (processResult == kTriglavPlugInFilterRunProcessResultExit)
				{
					break;
				}
			}
			*result = kTriglavPlugInCallResultSuccess;
		}
	}
	catch (...)
	{
		PixelSortLog("[PixelSort] EXCEPTION caught in TriglavPluginCall, selector=%d\n", selector);
		*result = kTriglavPlugInCallResultFailed;
	}
	return;
}
