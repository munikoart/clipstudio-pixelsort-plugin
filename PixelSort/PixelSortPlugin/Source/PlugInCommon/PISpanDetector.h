//! @file   PISpanDetector.h
//! @brief  Span detection for pixel sorting intervals
#pragma once

#include "PIPixelSort.h"
#include <cmath>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// ---------------------------------------------------------------------------
// Span struct
// ---------------------------------------------------------------------------

struct Span
{
	int start; // inclusive
	int end;   // exclusive
};

// ---------------------------------------------------------------------------
// Row accessor - abstracts horizontal vs vertical pixel access
// ---------------------------------------------------------------------------

struct RowAccessor
{
	BYTE*         imageBase;
	int           imagePixelBytes;
	int           imageRowBytes;
	int           rIdx, gIdx, bIdx;
	int           length;
	bool          vertical; // if true, iterate down a column instead of across a row

	// Stride between consecutive pixels in the iteration direction
	int pixelStride() const
	{
		return vertical ? imageRowBytes : imagePixelBytes;
	}

	BYTE* pixelAt(int i) const
	{
		return imageBase + i * pixelStride();
	}

	void getRGB(int i, BYTE& r, BYTE& g, BYTE& b) const
	{
		BYTE* p = pixelAt(i);
		r = p[rIdx];
		g = p[gIdx];
		b = p[bIdx];
	}

	void setRGB(int i, BYTE r, BYTE g, BYTE b) const
	{
		BYTE* p = pixelAt(i);
		p[rIdx] = r;
		p[gIdx] = g;
		p[bIdx] = b;
	}
};

// ---------------------------------------------------------------------------
// Threshold spans
// ---------------------------------------------------------------------------

inline void DetectSpansThreshold(
	const RowAccessor& row,
	float lowerNorm,
	float upperNorm,
	SortKey sortKey,
	std::vector<Span>& outSpans)
{
	outSpans.clear();
	int n = row.length;
	if (n <= 0) return;

	int spanStart = -1;
	for (int i = 0; i <= n; ++i)
	{
		bool inRange = false;
		if (i < n)
		{
			BYTE r, g, b;
			row.getRGB(i, r, g, b);
			float val = GetSortValueNorm(r, g, b, sortKey);
			inRange = (val >= lowerNorm && val <= upperNorm);
		}

		if (inRange && spanStart < 0)
		{
			spanStart = i;
		}
		else if (!inRange && spanStart >= 0)
		{
			Span s;
			s.start = spanStart;
			s.end = i;
			outSpans.push_back(s);
			spanStart = -1;
		}
	}
}

// ---------------------------------------------------------------------------
// Random spans
// ---------------------------------------------------------------------------

inline void DetectSpansRandom(
	int n,
	std::mt19937& rng,
	std::vector<Span>& outSpans)
{
	outSpans.clear();
	if (n <= 0) return;

	int maxLen = (std::max)(11, n / 4);
	int i = 0;
	while (i < n)
	{
		std::uniform_int_distribution<int> lenDist(10, maxLen);
		int length = lenDist(rng);
		int end = (std::min)(i + length, n);
		Span s;
		s.start = i;
		s.end = end;
		outSpans.push_back(s);

		std::uniform_int_distribution<int> gapDist(1, 20);
		int gap = gapDist(rng);
		i = end + gap;
	}
}

// ---------------------------------------------------------------------------
// Edge spans
// ---------------------------------------------------------------------------

inline void DetectSpansEdges(
	const RowAccessor& row,
	std::vector<Span>& outSpans,
	std::vector<float>& brightnessWork)
{
	outSpans.clear();
	int n = row.length;
	if (n <= 0) return;
	if (n == 1)
	{
		Span s; s.start = 0; s.end = 1;
		outSpans.push_back(s);
		return;
	}

	// Compute brightness for each pixel
	brightnessWork.resize(n);
	for (int i = 0; i < n; ++i)
	{
		BYTE r, g, b;
		row.getRGB(i, r, g, b);
		brightnessWork[i] = GetBrightnessNorm(r, g, b);
	}

	// Compute edge differences and statistics
	int nEdges = n - 1;
	double edgeSum = 0.0;
	double edgeSumSq = 0.0;

	// Reuse brightnessWork tail for edge values (nEdges < n)
	// We store edge values in a temporary section
	std::vector<float> edges(nEdges);
	for (int i = 0; i < nEdges; ++i)
	{
		edges[i] = fabsf(brightnessWork[i + 1] - brightnessWork[i]);
		edgeSum += edges[i];
		edgeSumSq += edges[i] * edges[i];
	}

	double mean = edgeSum / nEdges;
	double variance = (edgeSumSq / nEdges) - (mean * mean);
	if (variance < 0.0) variance = 0.0;
	double stddev = sqrt(variance);
	float threshold = static_cast<float>(mean + stddev);

	// Find edge positions (split points)
	int prevPos = 0;
	for (int i = 0; i < nEdges; ++i)
	{
		if (edges[i] > threshold)
		{
			int splitPos = i + 1;
			if (splitPos > prevPos)
			{
				Span s;
				s.start = prevPos;
				s.end = splitPos;
				outSpans.push_back(s);
			}
			prevPos = splitPos;
		}
	}
	// Last span
	if (n > prevPos)
	{
		Span s;
		s.start = prevPos;
		s.end = n;
		outSpans.push_back(s);
	}
}

// ---------------------------------------------------------------------------
// Wave spans
// ---------------------------------------------------------------------------

inline void DetectSpansWaves(
	int n,
	int rowIndex,
	std::vector<Span>& outSpans)
{
	outSpans.clear();
	if (n <= 0) return;

	int waveLen = (std::max)(10, n / 8);
	int i = 0;
	double phase = rowIndex * 0.1; // vary by row for visual interest
	while (i < n)
	{
		int length = static_cast<int>(waveLen * (0.5 + 0.5 * sin(phase)));
		length = (std::max)(2, length);
		int end = (std::min)(i + length, n);
		Span s;
		s.start = i;
		s.end = end;
		outSpans.push_back(s);
		i = end;
		phase += 0.5;
	}
}

// ---------------------------------------------------------------------------
// None - single span covering full row
// ---------------------------------------------------------------------------

inline void DetectSpansNone(int n, std::vector<Span>& outSpans)
{
	outSpans.clear();
	if (n <= 0) return;
	Span s;
	s.start = 0;
	s.end = n;
	outSpans.push_back(s);
}

// ---------------------------------------------------------------------------
// Dispatch + filter
// ---------------------------------------------------------------------------

inline void DetectSpans(
	const RowAccessor& row,
	const PixelSortParams& params,
	int rowIndex,
	std::mt19937& rng,
	std::vector<Span>& outSpans,
	std::vector<float>& brightnessWork)
{
	float lowerNorm = params.lowerThreshold / 255.0f;
	float upperNorm = params.upperThreshold / 255.0f;

	switch (params.intervalMode)
	{
	case kIntervalModeThreshold:
		DetectSpansThreshold(row, lowerNorm, upperNorm, params.sortKey, outSpans);
		break;
	case kIntervalModeRandom:
		DetectSpansRandom(row.length, rng, outSpans);
		break;
	case kIntervalModeEdges:
		DetectSpansEdges(row, outSpans, brightnessWork);
		break;
	case kIntervalModeWaves:
		DetectSpansWaves(row.length, rowIndex, outSpans);
		break;
	case kIntervalModeNone:
	default:
		DetectSpansNone(row.length, outSpans);
		break;
	}

	// Filter by span_min
	if (params.spanMin > 1)
	{
		int writeIdx = 0;
		for (int i = 0; i < static_cast<int>(outSpans.size()); ++i)
		{
			if (outSpans[i].end - outSpans[i].start >= params.spanMin)
				outSpans[writeIdx++] = outSpans[i];
		}
		outSpans.resize(writeIdx);
	}

	// Cap by span_max (split long spans)
	if (params.spanMax > 0)
	{
		std::vector<Span> capped;
		capped.reserve(outSpans.size());
		for (int i = 0; i < static_cast<int>(outSpans.size()); ++i)
		{
			int s = outSpans[i].start;
			int e = outSpans[i].end;
			while (s < e)
			{
				int end = (std::min)(s + params.spanMax, e);
				Span sp;
				sp.start = s;
				sp.end = end;
				capped.push_back(sp);
				s = end;
			}
		}
		outSpans.swap(capped);
	}
}
