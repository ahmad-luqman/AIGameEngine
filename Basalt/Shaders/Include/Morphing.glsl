// Morph targets, applied in bind space before skinning. Each model's deltas live in one RGBA32F texture: two texels
// per (target, vertex), position then normal, row-major in rows of the texture's width. A draw's active weights are
// (target, weight) pairs packed into draw records alongside the palettes, ahead of the draw records, 16 per record:
// Model's columns hold pairs 0-7 and Normal's 8-15, two per column (xy, zw).
layout(set = 2, binding = 0) uniform texture2D u_MorphDeltas;
layout(set = 2, binding = 1) uniform sampler u_MorphSampler;

vec4 MorphTexel(uint index, uint width)
{
	return texelFetch(sampler2D(u_MorphDeltas, u_MorphSampler), ivec2(index % width, index / width), 0);
}

vec2 MorphWeight(uint firstRecord, uint pair)
{
	DrawData record = u_Draws.Draws[firstRecord + pair / 16u];
	uint slot = pair % 16u;
	vec4 column = slot < 8u ? record.Model[slot / 2u] : record.Normal[(slot - 8u) / 2u];
	return (slot % 2u) == 0u ? column.xy : column.zw;
}

// The position texel of one pair's target for this vertex. Morph.z already subtracts 2 x BaseVertex texels (unsigned
// wraparound), which gl_VertexIndex adds back.
uint MorphTexelIndex(DrawData draw, vec2 weight)
{
	return draw.Morph.z + uint(weight.x) * draw.Morph.w + 2u * uint(gl_VertexIndex);
}

void ApplyMorphTargets(DrawData draw, inout vec3 position, inout vec3 normal)
{
	uint width = uint(textureSize(sampler2D(u_MorphDeltas, u_MorphSampler), 0).x);
	for (uint pair = 0u; pair < draw.Morph.y; ++pair)
	{
		vec2 weight = MorphWeight(draw.Morph.x, pair);
		uint texel = MorphTexelIndex(draw, weight);
		position += weight.y * MorphTexel(texel, width).xyz;
		normal += weight.y * MorphTexel(texel + 1u, width).xyz;
	}
}

// Shadow passes need positions only.
void ApplyMorphPosition(DrawData draw, inout vec3 position)
{
	uint width = uint(textureSize(sampler2D(u_MorphDeltas, u_MorphSampler), 0).x);
	for (uint pair = 0u; pair < draw.Morph.y; ++pair)
	{
		vec2 weight = MorphWeight(draw.Morph.x, pair);
		position += weight.y * MorphTexel(MorphTexelIndex(draw, weight), width).xyz;
	}
}
