// Morph targets, applied in bind space before skinning. Each model's deltas live in one RGBA32F texture: two texels
// per (target, vertex), position then normal, row-major in rows of the texture's width. A draw's active weights are
// (target, weight) pairs packed into draw records after the palettes, 16 per record: Model's columns hold pairs 0-7
// and Normal's 8-15, two per column (xy, zw).
layout(set = 2, binding = 0) uniform texture2D u_MorphDeltas;
layout(set = 2, binding = 1) uniform sampler u_MorphSampler;

vec4 MorphTexel(uint index)
{
	uint width = uint(textureSize(sampler2D(u_MorphDeltas, u_MorphSampler), 0).x);
	return texelFetch(sampler2D(u_MorphDeltas, u_MorphSampler), ivec2(index % width, index / width), 0);
}

vec2 MorphWeight(uint firstRecord, uint pair)
{
	DrawData record = u_Draws.Draws[firstRecord + pair / 16u];
	uint slot = pair % 16u;
	vec4 column = slot < 8u ? record.Model[slot / 2u] : record.Normal[(slot - 8u) / 2u];
	return (slot % 2u) == 0u ? column.xy : column.zw;
}

// gl_VertexIndex includes the draw's vertex offset; Morph.z already subtracts the submesh's base vertex.
void ApplyMorphTargets(DrawData draw, inout vec3 position, inout vec3 normal)
{
	for (uint pair = 0u; pair < draw.Morph.y; ++pair)
	{
		vec2 weight = MorphWeight(draw.Morph.x, pair);
		uint texel = draw.Morph.z + uint(weight.x) * draw.Morph.w + 2u * uint(gl_VertexIndex);
		position += weight.y * MorphTexel(texel).xyz;
		normal += weight.y * MorphTexel(texel + 1u).xyz;
	}
}
