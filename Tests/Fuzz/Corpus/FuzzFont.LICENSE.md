# FuzzFont seed fonts

The seeds come from Dear ImGui's `misc/fonts` and are licensed under the SIL Open Font
License 1.1 (full text: `Tests/Data/FeatureTest/Assets/Fonts/Karla-OFL.txt`, https://openfontlicense.org).

- `FuzzFont/Karla-Regular.ttf`: Copyright (c) 2011, Jonathan Pinhorn, with Reserved Font Names "Karla" and
  "Karla Tamil". Simple glyphs only.
- `FuzzFont/Cousine-Regular.ttf`: by Steve Matteson, digitized data copyright (c) 2010 Google Corporation.
  Includes composite glyphs, so the fuzzer starts from both glyph kinds.
- `FuzzFont/regression-blank-period.ttf`: Karla-Regular.ttf as changed by the fuzzer (its '.' draws
  nothing); a regression input, also read by the unit tests.
