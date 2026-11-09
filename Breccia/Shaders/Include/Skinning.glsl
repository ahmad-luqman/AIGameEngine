// Palettes are DrawData records in the existing draw buffer; Model holds each joint matrix.
// The offset/count belong to this draw, so different instances never share mutable pose data.
mat4 SkinMatrix(DrawData draw, uvec4 joints, vec4 weights)
{
	if (draw.Indices.w == 0u)
		return mat4(1.0);
	mat4 skin = mat4(0.0);
	for (int i = 0; i < 4; ++i)
		if (joints[i] < draw.Indices.w)
			skin += weights[i] * u_Draws.Draws[draw.Indices.z + joints[i]].Model;
	return skin;
}

// Normalize directions without squared-length underflow for very small rig units.
vec3 StableNormalize(vec3 value, vec3 fallback)
{
	float scale = max(abs(value.x), max(abs(value.y), abs(value.z)));
	return scale > 0.0 ? normalize(value / scale) : normalize(fallback);
}
