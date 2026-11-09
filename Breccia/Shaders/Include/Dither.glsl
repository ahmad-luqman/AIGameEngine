// Uniform zero-mean noise at one 8-bit quantization step. Applied only at final output conversion.
float OutputDither(vec2 pixel)
{
	return (fract(sin(dot(pixel, vec2(12.9898, 78.233))) * 43758.5453) - 0.5) / 255.0;
}
