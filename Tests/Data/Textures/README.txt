Synthetic fixtures authored for Basalt, no external image content.
8x8 RGBA (180,40,70,110) solid PNG, encoded with Basis Universal v1_60:
  basisu -ktx2 -mipmap -file solid.png -output_file ETC1S.ktx2
  basisu -ktx2 -uastc -mipmap -file solid.png -output_file UASTC.ktx2
UASTC uses the default Zstandard supercompression. Both contain four mip levels.

ZstdRegression.ktx2 is a minimized FuzzTexture input (also in Tests/Fuzz/Corpus/FuzzTexture) that the
Zstandard 1.4.9 bundled with Basis mishandled. With the pinned libzstd it must fail cleanly with
"Basis transcode failed".
