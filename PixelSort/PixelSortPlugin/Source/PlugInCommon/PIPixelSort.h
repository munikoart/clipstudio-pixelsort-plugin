//! @file   PIPixelSort.h
//! @brief  Pixel sort enums, sort key functions, logging, and shared types
#pragma once

#include <windows.h>
#include <cstdio>
#include <cstdarg>
#include <cmath>
#include <algorithm>
#include <vector>
#include <random>

typedef unsigned char BYTE;

// ---------------------------------------------------------------------------
// Diagnostics
// ---------------------------------------------------------------------------

inline void PixelSortLog(const char* fmt, ...)
{
	char buf[512];
	va_list args;
	va_start(args, fmt);
	vsnprintf(buf, sizeof(buf), fmt, args);
	va_end(args);
	OutputDebugStringA(buf);
}

// ---------------------------------------------------------------------------
// Enums
// ---------------------------------------------------------------------------

enum SortDirection
{
	kSortDirectionHorizontal = 0,
	kSortDirectionVertical   = 1
};

enum SortKey
{
	kSortKeyBrightness = 0,
	kSortKeyHue        = 1,
	kSortKeySaturation = 2,
	kSortKeyIntensity  = 3,
	kSortKeyMinimum    = 4,
	kSortKeyRed        = 5,
	kSortKeyGreen      = 6,
	kSortKeyBlue       = 7
};

enum IntervalMode
{
	kIntervalModeThreshold = 0,
	kIntervalModeRandom    = 1,
	kIntervalModeEdges     = 2,
	kIntervalModeWaves     = 3,
	kIntervalModeNone      = 4
};

// ---------------------------------------------------------------------------
// Parameter struct
// ---------------------------------------------------------------------------

struct PixelSortParams
{
	SortDirection direction;
	SortKey       sortKey;
	IntervalMode  intervalMode;
	int           lowerThreshold; // 0-255
	int           upperThreshold; // 0-255
	bool          reverse;
	int           jitter;         // 0-100
	int           spanMin;        // 1-10000
	int           spanMax;        // 0-10000, 0 = unlimited
	int           angle;          // 0-359 degrees (only applies when direction=Horizontal)
	int           falloff;        // 0-100 percent chance to skip sorting a span
};

inline PixelSortParams MakeDefaultParams()
{
	PixelSortParams p;
	p.direction      = kSortDirectionHorizontal;
	p.sortKey        = kSortKeyBrightness;
	p.intervalMode   = kIntervalModeThreshold;
	p.lowerThreshold = 64;
	p.upperThreshold = 204;
	p.reverse        = false;
	p.jitter         = 0;
	p.spanMin        = 1;
	p.spanMax        = 0;
	p.angle          = 0;
	p.falloff        = 0;
	return p;
}

inline void ClampParams(PixelSortParams& p)
{
	if (p.direction < 0 || p.direction > 1)
		p.direction = kSortDirectionHorizontal;
	if (p.sortKey < 0 || p.sortKey > 7)
		p.sortKey = kSortKeyBrightness;
	if (p.intervalMode < 0 || p.intervalMode > 4)
		p.intervalMode = kIntervalModeThreshold;

	p.lowerThreshold = (std::max)(0, (std::min)(255, p.lowerThreshold));
	p.upperThreshold = (std::max)(0, (std::min)(255, p.upperThreshold));
	if (p.upperThreshold < p.lowerThreshold)
		p.upperThreshold = p.lowerThreshold;

	p.jitter  = (std::max)(0, (std::min)(100, p.jitter));
	p.spanMin = (std::max)(1, (std::min)(10000, p.spanMin));
	p.spanMax = (std::max)(0, (std::min)(10000, p.spanMax));
	if (p.spanMax > 0 && p.spanMax < p.spanMin)
		p.spanMax = p.spanMin;

	p.angle = ((p.angle % 360) + 360) % 360;
	p.falloff = (std::max)(0, (std::min)(100, p.falloff));
}

// ---------------------------------------------------------------------------
// Pixel data for sorting
// ---------------------------------------------------------------------------

struct PixelData
{
	BYTE  r, g, b;
	float sortValue;
};

// ---------------------------------------------------------------------------
// Sort key functions
// ---------------------------------------------------------------------------

inline float GetBrightness(BYTE r, BYTE g, BYTE b)
{
	return 0.299f * r + 0.587f * g + 0.114f * b;
}

inline float GetIntensity(BYTE r, BYTE g, BYTE b)
{
	return (r + g + b) / 3.0f;
}

inline float GetMinimum(BYTE r, BYTE g, BYTE b)
{
	return static_cast<float>((std::min)({r, g, b}));
}

inline float GetHue(BYTE rv, BYTE gv, BYTE bv)
{
	float r = static_cast<float>(rv);
	float g = static_cast<float>(gv);
	float b = static_cast<float>(bv);

	float maxC = (std::max)({r, g, b});
	float minC = (std::min)({r, g, b});
	float delta = maxC - minC;

	if (delta <= 0.0f)
		return 0.0f;

	float hue = 0.0f;
	if (maxC == r)
		hue = 60.0f * fmodf((g - b) / delta, 6.0f);
	else if (maxC == g)
		hue = 60.0f * (((b - r) / delta) + 2.0f);
	else
		hue = 60.0f * (((r - g) / delta) + 4.0f);

	if (hue < 0.0f)
		hue += 360.0f;
	return hue;
}

inline float GetSaturation(BYTE rv, BYTE gv, BYTE bv)
{
	float r = static_cast<float>(rv);
	float g = static_cast<float>(gv);
	float b = static_cast<float>(bv);

	float maxC = (std::max)({r, g, b});
	if (maxC <= 0.0f)
		return 0.0f;

	float minC = (std::min)({r, g, b});
	return (maxC - minC) / maxC;
}

inline float GetSortValue(BYTE r, BYTE g, BYTE b, SortKey key)
{
	switch (key)
	{
	case kSortKeyBrightness: return GetBrightness(r, g, b);
	case kSortKeyHue:        return GetHue(r, g, b);
	case kSortKeySaturation: return GetSaturation(r, g, b);
	case kSortKeyIntensity:  return GetIntensity(r, g, b);
	case kSortKeyMinimum:    return GetMinimum(r, g, b);
	case kSortKeyRed:        return static_cast<float>(r);
	case kSortKeyGreen:      return static_cast<float>(g);
	case kSortKeyBlue:       return static_cast<float>(b);
	default:                 return GetBrightness(r, g, b);
	}
}

// Brightness normalized to 0.0-1.0
inline float GetBrightnessNorm(BYTE r, BYTE g, BYTE b)
{
	return (0.299f * r + 0.587f * g + 0.114f * b) / 255.0f;
}

// Sort value normalized to 0.0-1.0 (for threshold span detection)
inline float GetSortValueNorm(BYTE r, BYTE g, BYTE b, SortKey key)
{
	switch (key)
	{
	case kSortKeyBrightness: return GetBrightness(r, g, b) / 255.0f;
	case kSortKeyHue:        return GetHue(r, g, b) / 360.0f;
	case kSortKeySaturation: return GetSaturation(r, g, b); // already 0-1
	case kSortKeyIntensity:  return GetIntensity(r, g, b) / 255.0f;
	case kSortKeyMinimum:    return GetMinimum(r, g, b) / 255.0f;
	case kSortKeyRed:        return r / 255.0f;
	case kSortKeyGreen:      return g / 255.0f;
	case kSortKeyBlue:       return b / 255.0f;
	default:                 return GetBrightness(r, g, b) / 255.0f;
	}
}
