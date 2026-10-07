#version 450

// Fullscreen triangle from gl_VertexIndex; no vertex buffer.
// Clip space is +Y up (nvrhi flips the Vulkan viewport); texture coordinates start at the top-left.
layout(location = 0) out vec2 v_TexCoord;

void main()
{
	v_TexCoord = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
	gl_Position = vec4(v_TexCoord.x * 2.0 - 1.0, 1.0 - v_TexCoord.y * 2.0, 0.0, 1.0);
}
