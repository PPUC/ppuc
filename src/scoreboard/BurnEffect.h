#pragma once

#include <cstdint>
#include <vector>

#include "ScoreboardModel.h"

namespace scoreboard
{

// Turns the shape of a letter into a letter burnt into wood.
//
// Takes coverage (0..255 per pixel, as a font renderer leaves it) and returns
// RGBA to be blended over the wood: a dark, slightly uneven core where the iron
// touched, and a scorch that fades out around it and darkens the grain without
// hiding it. With `glow`, a brand fresh from the fire: a hot core and an orange
// heat around it.
//
// Done once per glyph and size when the scoreboard loads, never per frame --
// the blur is the only expensive part, and a score is then drawn by copying a
// few finished glyphs.
//
// The output is `Pad(style, size)` pixels bigger on every side than the input,
// so the scorch is not cut off at the glyph's box.
int BurnPad(const Style& style, int fontSize);
void Burn(const uint8_t* coverage, int width, int height, const Style& style, int fontSize, std::vector<uint8_t>* rgba,
          int* outWidth, int* outHeight);

// The shadow of a glyph, for a style with `shadow` above 0: the letter's shape,
// blurred and in shadowColor, the same size as Burn's output so it lines up
// with it. Drawn first, ShadowOffset() pixels down and right of the letter, so
// one letter's shadow never falls on the letter before it.
bool HasShadow(const Style& style);
int ShadowOffset(const Style& style, int fontSize);
void BurnShadow(const uint8_t* coverage, int width, int height, const Style& style, int fontSize,
                std::vector<uint8_t>* rgba, int* outWidth, int* outHeight);

// Area-averaged downscale of coverage, for the logo: a plain resample would
// drop the thin lines of its lettering.
std::vector<uint8_t> ScaleCoverage(const uint8_t* src, int srcW, int srcH, int dstW, int dstH);

// A box blur run three times, which is close enough to a Gaussian for a scorch
// and costs the same for any radius. In place, `width` x `height`.
void BlurCoverage(std::vector<float>* values, int width, int height, int radius);

}  // namespace scoreboard
