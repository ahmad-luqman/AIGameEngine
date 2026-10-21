#version 450

layout(set = 0, binding = 0) uniform texture2D u_Color;
layout(set = 0, binding = 1) uniform texture2D u_Depth;
layout(set = 0, binding = 2) uniform texture2D u_Motion;
layout(set = 0, binding = 3) uniform texture2D u_History;
layout(set = 0, binding = 4) uniform texture2D u_HistoryDepth;
layout(set = 0, binding = 5) uniform sampler u_Linear;
layout(set = 0, binding = 6) uniform sampler u_Point;
layout(push_constant) uniform TemporalPush
{
	mat4 SkyReprojection;
	uvec2 Size;
	uint Valid;
	uint Padding;
	vec2 JitterDelta;
} u_Push;

layout(location = 0) out vec4 o_Color;
layout(location = 1) out float o_Depth;

vec3 Current(ivec2 pixel)
{
	return texelFetch(sampler2D(u_Color, u_Point), clamp(pixel, ivec2(0), ivec2(u_Push.Size) - 1), 0).rgb;
}

void main()
{
	ivec2 pixel = ivec2(gl_FragCoord.xy);
	vec2 uv = gl_FragCoord.xy / vec2(u_Push.Size);
	vec3 current = Current(pixel);
	float depth = texelFetch(sampler2D(u_Depth, u_Point), pixel, 0).r;
	vec4 motion = texelFetch(sampler2D(u_Motion, u_Point), pixel, 0);
	// Dilate foreground motion into silhouette pixels. These pixels need coverage history even when the
	// current jittered sample just misses the surface.
	float motionDepth = depth;
	ivec2 motionPixel = pixel;
	for (int y = -1; y <= 1; y++)
		for (int x = -1; x <= 1; x++)
		{
			ivec2 candidate = clamp(pixel + ivec2(x, y), ivec2(0), ivec2(u_Push.Size) - 1);
			float candidateDepth = texelFetch(sampler2D(u_Depth, u_Point), candidate, 0).r;
			if (candidateDepth < motionDepth)
			{
				motionDepth = candidateDepth;
				motionPixel = candidate;
			}
		}
	if (motionPixel != pixel)
	{
		motion = texelFetch(sampler2D(u_Motion, u_Point), motionPixel, 0);
		motion.xy += vec2(pixel - motionPixel) / vec2(u_Push.Size);
	}
	if (motionDepth == 1.0)
	{
		// Infinite sky: reproject camera rotation, with translation removed on the CPU.
		vec4 previous = u_Push.SkyReprojection * vec4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 1.0, 1.0);
		motion = vec4(previous.xy / previous.w * vec2(0.5, -0.5) + 0.5, 1.0, previous.w > 0.0 ? 1.0 : 0.0);
	}
	// Color history lives on a stable pixel grid; rasterization jitter is not surface motion.
	motion.xy += u_Push.JitterDelta;
	vec2 halfPixel = 0.5 / vec2(u_Push.Size);
	bool valid = u_Push.Valid != 0u && motion.w > 0.5 && motion.z >= 0.0 && motion.z <= 1.0 &&
		all(greaterThanEqual(motion.xy, halfPixel)) && all(lessThanEqual(motion.xy, 1.0 - halfPixel));
	float tolerance = clamp(2.0 * (abs(dFdx(motion.z)) + abs(dFdy(motion.z))), 0.00002, 0.001);
	vec3 result = current;
	if (valid)
	{
		ivec2 previousPixel = ivec2(motion.xy * vec2(u_Push.Size));
		// Check the neighborhood that the jittered depth samples cover. A silhouette can change coverage
		// without a disocclusion; a depth found nowhere nearby still rejects history.
		bool depthMatches = false;
		for (int y = -1; y <= 1; y++)
			for (int x = -1; x <= 1; x++)
			{
				ivec2 tap = clamp(previousPixel + ivec2(x, y), ivec2(0), ivec2(u_Push.Size) - 1);
				float previousDepth = texelFetch(sampler2D(u_HistoryDepth, u_Point), tap, 0).r;
				depthMatches = depthMatches || (abs(previousDepth - motion.z) <= tolerance && (motionDepth == 1.0) == (previousDepth == 1.0));
			}
		if (depthMatches)
		{
			vec3 low = current, high = current;
			for (int y = -1; y <= 1; y++)
				for (int x = -1; x <= 1; x++)
				{
					vec3 sampleColor = Current(pixel + ivec2(x, y));
					low = min(low, sampleColor);
					high = max(high, sampleColor);
				}
			vec2 targetUV = motion.xy * vec2(u_Push.Size) / vec2(textureSize(sampler2D(u_History, u_Linear), 0));
			vec3 history = clamp(texture(sampler2D(u_History, u_Linear), targetUV).rgb, low, high);
			float speed = length((uv - motion.xy) * vec2(u_Push.Size));
			result = mix(current, history, mix(0.9, 0.5, clamp(speed / 16.0, 0.0, 1.0)));
		}
	}
	o_Color = vec4(result, 1.0);
	o_Depth = depth;
}
