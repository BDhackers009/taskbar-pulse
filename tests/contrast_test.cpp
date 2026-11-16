// Checks the WCAG color math used by the menu and Settings surfaces.
// Built and run by build.ps1; it has no Windows dependencies beyond the types.

#include "../src/theme_color.h"
#include <cmath>
#include <cstdio>

namespace {

int g_checks = 0;
int g_failures = 0;

void Check(bool ok, const char* what) {
  ++g_checks;
  if (!ok) {
    ++g_failures;
    std::printf("FAIL: %s\n", what);
  }
}

D2D1_COLOR_F C(float r, float g, float b, float a = 1.0f) {
  return D2D1_COLOR_F{r, g, b, a};
}

bool Near(float a, float b, float tolerance = 0.02f) {
  return std::fabs(a - b) <= tolerance;
}

}  // namespace

int main() {
  using namespace themecolor;

  const D2D1_COLOR_F black = C(0, 0, 0);
  const D2D1_COLOR_F white = C(1, 1, 1);

  Check(Near(Contrast(black, white), 21.0f, 0.01f), "black and white are 21:1");
  Check(Near(Contrast(white, black), 21.0f, 0.01f), "contrast is symmetric");
  Check(Near(Contrast(C(0.5f, 0.5f, 0.5f), black), 5.28f, 0.02f), "#808080 on black is 5.28:1");
  Check(Near(Contrast(C(0.4667f, 0.4667f, 0.4667f), white), 4.48f, 0.03f),
        "#777777 on white is 4.48:1");
  Check(Near(Contrast(C(0.07f, 0.07f, 0.07f), white), 18.73f, 0.05f), "#121212 on white is 18.73:1");

  // Composite: half white over black is middle grey; fully opaque ignores the base.
  const D2D1_COLOR_F half = Composite(C(1, 1, 1, 0.5f), black);
  Check(Near(half.r, 0.5f) && Near(half.g, 0.5f) && Near(half.b, 0.5f) && Near(half.a, 1.0f),
        "50% white over black is middle grey");
  const D2D1_COLOR_F opaque = Composite(C(0.2f, 0.4f, 0.6f, 1.0f), white);
  Check(Near(opaque.r, 0.2f) && Near(opaque.g, 0.4f) && Near(opaque.b, 0.6f),
        "opaque foreground replaces the base");

  Check(Near(Chroma(C(0.5f, 0.5f, 0.5f)), 0.0f), "grey has no chroma");
  Check(Chroma(C(0.0f, 0.47f, 0.83f)) > 0.5f, "blue is chromatic");

  // A color that already passes must not move.
  const D2D1_COLOR_F untouched = EnsureContrast(white, C(0.115f, 0.115f, 0.125f), 4.6f);
  Check(Near(untouched.r, 1.0f) && Near(untouched.g, 1.0f) && Near(untouched.b, 1.0f),
        "a passing color is returned untouched");

  // Sweep the accent cube against the real panel, hover and track surfaces.
  const D2D1_COLOR_F surfaces[] = {
      C(0.133f, 0.133f, 0.133f),  // dark panel over white
      C(0.198f, 0.198f, 0.198f),  // dark hover row
      C(0.245f, 0.245f, 0.245f),  // dark track
      C(0.965f, 0.965f, 0.970f),  // light panel over black
      C(0.912f, 0.912f, 0.917f),  // light hover row
      C(0.840f, 0.840f, 0.845f),  // light track
  };
  const float levels[] = {0.0f, 0.2f, 0.4f, 0.6f, 0.8f, 1.0f};
  const float targets[] = {3.2f, 4.6f};
  int swept = 0;
  for (float r : levels) {
    for (float g : levels) {
      for (float b : levels) {
        const D2D1_COLOR_F accent = C(r, g, b);
        for (const D2D1_COLOR_F& surface : surfaces) {
          for (float target : targets) {
            const D2D1_COLOR_F out = EnsureContrast(accent, surface, target);
            ++swept;
            if (Contrast(out, surface) < target - 0.001f) {
              char msg[160];
              std::snprintf(msg, sizeof(msg),
                            "accent %.2f/%.2f/%.2f on %.2f/%.2f/%.2f missed %.1f:1 (got %.3f)",
                            r, g, b, surface.r, surface.g, surface.b, target,
                            Contrast(out, surface));
              Check(false, msg);
            }
          }
        }
      }
    }
  }
  Check(swept == 216 * 6 * 2, "all accent/surface/target combinations were swept");

  // The policy the app actually uses: composite the real panel and widget-track
  // surfaces, then require 4.5:1 for text and 3:1 for fills on every accent and
  // both themes.
  struct Palette {
    bool light;
    D2D1_COLOR_F text, menuBg, hover, fillSurface;
  };
  const Palette palettes[] = {
      {false, C(1, 1, 1, 0.95f), C(0.115f, 0.115f, 0.125f, 0.98f), C(1, 1, 1, 0.075f),
       C(0.281f, 0.281f, 0.281f)},
      {true, C(0.07f, 0.07f, 0.07f, 0.95f), C(0.975f, 0.975f, 0.98f, 0.99f),
       C(0, 0, 0, 0.055f), C(0.792f, 0.792f, 0.792f)},
  };
  int policy = 0;
  for (float r : levels) {
    for (float g : levels) {
      for (float b : levels) {
        const D2D1_COLOR_F accent = C(r, g, b);
        for (const Palette& p : palettes) {
          const D2D1_COLOR_F panel = Composite(p.menuBg, p.light ? black : white);
          const D2D1_COLOR_F hover = Composite(p.hover, panel);
          const AccentRamp ramp =
              AccentRampFor(accent, p.light, p.text, p.menuBg, p.hover, p.fillSurface);
          const D2D1_COLOR_F text = Composite(ramp.text, hover);
          ++policy;
          if (Contrast(text, hover) < 4.5f - 0.001f ||
              Contrast(ramp.fill, p.fillSurface) < 3.2f - 0.001f) {
            char msg[160];
            std::snprintf(msg, sizeof(msg),
                          "policy accent %.2f/%.2f/%.2f light=%d: text %.3f, fill %.3f", r, g, b,
                          p.light ? 1 : 0, Contrast(text, hover),
                          Contrast(ramp.fill, p.fillSurface));
            Check(false, msg);
          }
        }
      }
    }
  }
  Check(policy == 216 * 2, "the accent policy was swept for both themes");

  // Alpha-zero and mid-grey colors must not produce NaN ratios, and an
  // unreachable target must return the pole that gets closest.
  const D2D1_COLOR_F weird = C(0.0f, 0.0f, 0.0f, 0.0f);
  Check(std::isfinite(Contrast(weird, white)), "zero-alpha color still yields a finite ratio");
  const D2D1_COLOR_F mid = C(0.46f, 0.46f, 0.46f);
  const D2D1_COLOR_F best = EnsureContrast(C(0.5f, 0.5f, 0.5f), mid, 4.6f);
  const float cw = Contrast(white, mid);
  const float cb = Contrast(black, mid);
  Check(std::isfinite(best.r) && Near(Contrast(best, mid), cw > cb ? cw : cb, 0.01f),
        "an unreachable target returns the closest pole");

  std::printf("contrast tests: %d checks, %d sweep + %d accent-policy combinations, %d failures\n",
              g_checks, swept, policy, g_failures);
  return g_failures == 0 ? 0 : 1;
}

