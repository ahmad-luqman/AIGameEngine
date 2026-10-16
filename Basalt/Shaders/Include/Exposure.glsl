#ifndef BASALT_EXPOSURE_GLSL
#define BASALT_EXPOSURE_GLSL

// Shared histogram mapping and temporal-adaptation inputs.
const uint HistogramBins = 256u;
const uint HistogramGroupSize = 16u;
const float HistogramMinEV = -12.0;
const float HistogramEVSpan = 28.0;
const float HistogramIntervals = float(HistogramBins - 2u);
layout(push_constant) uniform ExposurePush
{
	uvec2 Size;
	float MinExposure;
	float MaxExposure;
	float Compensation;
	float Blend;
	uint Reset;
}
u_Push;

#endif
