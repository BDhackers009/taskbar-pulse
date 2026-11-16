#pragma once

#include <algorithm>
#include <cmath>
#include <d2d1.h>

// WCAG 2.2 contrast helpers for colors drawn over a known surface. D2D1 colors
// carry straight alpha, so translucent surfaces must be composited first.
namespace themecolor {

inline float Linearize(float c) {
  return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

inline float Luminance(const D2D1_COLOR_F& c) {
  return 0.2126f * Linearize(c.r) + 0.7152f * Linearize(c.g) + 0.0722f * Linearize(c.b);
}

inline D2D1_COLOR_F Composite(const D2D1_COLOR_F& top, const D2D1_COLOR_F& bottom) {
  const float a = top.a;
  return D2D1_COLOR_F{top.r * a + bottom.r * (1.0f - a),
                      top.g * a + bottom.g * (1.0f - a),
                      top.b * a + bottom.b * (1.0f - a), 1.0f};
}

inline float Contrast(const D2D1_COLOR_F& a, const D2D1_COLOR_F& b) {
  float hi = Luminance(a);
  float lo = Luminance(b);
  if (hi < lo) std::swap(hi, lo);
  return (hi + 0.05f) / (lo + 0.05f);
}

inline D2D1_COLOR_F Mix(const D2D1_COLOR_F& from, const D2D1_COLOR_F& to, float t) {
  return D2D1_COLOR_F{from.r + (to.r - from.r) * t, from.g + (to.g - from.g) * t,
                      from.b + (to.b - from.b) * t, 1.0f};
}

inline float Chroma(const D2D1_COLOR_F& c) {
  const float hi = (std::max)((std::max)(c.r, c.g), c.b);
  const float lo = (std::min)((std::min)(c.r, c.g), c.b);
  return hi - lo;
}

// Move a color toward white or black, whichever has more headroom, until it
// reaches `target` against `background`. The first passing mix wins, so a color
// that already passes is untouched and a tinted accent keeps its hue. If even
// the better pole cannot reach the target (a mid-gray surface), the pole is
// returned; the app only calls this with near-black or near-white surfaces.
inline D2D1_COLOR_F EnsureContrast(const D2D1_COLOR_F& color, const D2D1_COLOR_F& background,
                                   float target) {
  if (Contrast(color, background) >= target) return color;
  const D2D1_COLOR_F white{1.0f, 1.0f, 1.0f, 1.0f};
  const D2D1_COLOR_F black{0.0f, 0.0f, 0.0f, 1.0f};
  const D2D1_COLOR_F& pole =
      Contrast(white, background) >= Contrast(black, background) ? white : black;
  float lo = 0.0f;
  float hi = 1.0f;
  for (int i = 0; i < 20; ++i) {
    const float mid = (lo + hi) * 0.5f;
    if (Contrast(Mix(color, pole, mid), background) >= target)
      hi = mid;
    else
      lo = mid;
  }
  return Mix(color, pole, hi);
}

struct AccentRamp {
  D2D1_COLOR_F text;
  D2D1_COLOR_F fill;
};

// Derives the two accent values the UI draws: text and check marks at 4.5:1 on
// a hovered panel, and fills at 3:1 on the widget track. The small margins
// (4.6 and 3.2) leave headroom for antialiased edges. A nearly gray accent
// (black, white, gray) falls back to the normal text color, because tinting it
// only produces a washed-out gray.
inline AccentRamp AccentRampFor(const D2D1_COLOR_F& accent, bool light,
                                const D2D1_COLOR_F& text, const D2D1_COLOR_F& menuBg,
                                const D2D1_COLOR_F& menuHover,
                                const D2D1_COLOR_F& fillSurface) {
  const D2D1_COLOR_F white{1.0f, 1.0f, 1.0f, 1.0f};
  const D2D1_COLOR_F black{0.0f, 0.0f, 0.0f, 1.0f};
  const D2D1_COLOR_F panel = Composite(menuBg, light ? black : white);
  const D2D1_COLOR_F hover = Composite(menuHover, panel);
  AccentRamp ramp;
  ramp.text = Chroma(accent) < 0.06f ? text : EnsureContrast(accent, hover, 4.6f);
  ramp.fill = EnsureContrast(accent, fillSurface, 3.2f);
  return ramp;
}

}  // namespace themecolor
