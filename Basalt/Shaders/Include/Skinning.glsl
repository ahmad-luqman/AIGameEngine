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
