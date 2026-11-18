// Pins the taskbar placement arithmetic without Windows or Direct2D.
// Built and run by build.ps1.

#include "../src/taskbar_anchor.h"
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

anchor::Input Make(anchor::Box bar, anchor::Box tray, bool horizontal, int widgetW,
                   int contentH, int gapPx, bool borderless = false, int insetPx = 0) {
  anchor::Input in;
  in.bar = bar;
  in.tray = tray;
  in.hasTray = true;
  in.horizontal = horizontal;
  in.widgetW = widgetW;
  in.contentH = contentH;
  in.gapPx = gapPx;
  in.borderless = borderless;
  in.insetPx = insetPx;
  return in;
}

void Expect(const char* name, const anchor::Input& in, int x, int y, int h, int offsetY,
            bool right = false) {
  const anchor::Outcome out = anchor::Compute(in);
  if (!out.result) {
    Check(false, name);
    return;
  }
  const anchor::Result& r = *out.result;
  Check(r.x == x && r.y == y && r.h == h && r.offsetY == offsetY &&
            r.widgetRightOfTray == right,
        name);
}

void ExpectNoAnchor(const char* name, const anchor::Input& in) {
  const anchor::Outcome out = anchor::Compute(in);
  Check(!out.result && out.reason == anchor::Reason::NoAnchor, name);
}

}  // namespace

int main() {
  using anchor::Box;

  Expect("LTR horizontal", Make(Box{0, 0, 1920, 48}, Box{1800, 0, 1920, 48}, true, 228, 32, 6),
         1566, 8, 32, 0);
  Expect("RTL horizontal", Make(Box{0, 0, 1920, 48}, Box{0, 0, 120, 48}, true, 228, 32, 6), 126,
         8, 32, 0, true);
  Expect("mirror invariant LTR",
         Make(Box{0, 0, 1000, 48}, Box{800, 0, 1000, 48}, true, 100, 32, 6), 694, 8, 32, 0);
  Expect("mirror invariant RTL",
         Make(Box{0, 0, 1000, 48}, Box{0, 0, 200, 48}, true, 100, 32, 6), 206, 8, 32, 0, true);
  Expect("tray exactly at bar centre",
         Make(Box{0, 0, 1000, 48}, Box{400, 0, 600, 48}, true, 100, 32, 6), 294, 8, 32, 0);
  Expect("tray before centre", Make(Box{0, 0, 1000, 48}, Box{300, 0, 499, 48}, true, 100, 32, 6),
         505, 8, 32, 0, true);
  Expect("tray after centre", Make(Box{0, 0, 1000, 48}, Box{501, 0, 700, 48}, true, 100, 32, 6),
         395, 8, 32, 0);
  Expect("top-docked bar",
         Make(Box{0, -48, 1920, 0}, Box{1800, -48, 1920, 0}, true, 228, 32, 6), 1566, -40, 32, 0);
  Expect("negative-origin bar",
         Make(Box{-1920, 0, 0, 48}, Box{-120, 0, 0, 48}, true, 228, 32, 6), -354, 8, 32, 0);

  // Known 1.1.4 clipping: the max clamp wins on a 30 px bar.
  Expect("small bar clips", Make(Box{0, 0, 1920, 30}, Box{1800, 0, 1920, 30}, true, 228, 32, 6),
         1566, -4, 32, 0);

  // Known 1.1.4 clamp inversion: max wins when the widget is wider than the bar.
  Expect("widget wider than bar",
         Make(Box{0, 0, 200, 48}, Box{100, 0, 200, 48}, true, 300, 32, 6), -104, 8, 32, 0);
  Expect("widget exactly fits",
         Make(Box{0, 0, 200, 48}, Box{100, 0, 200, 48}, true, 192, 32, 0), 4, 8, 32, 0);

  Expect("borderless", Make(Box{0, 0, 1920, 48}, Box{1800, 0, 1920, 48}, true, 228, 32, 6, true, 8),
         1566, 4, 40, 4);
  Expect("120 dpi values",
         Make(Box{0, 0, 2400, 60}, Box{2250, 0, 2400, 60}, true, 285, 40, 8), 1957, 10, 40, 0);
  Expect("144 dpi values",
         Make(Box{0, 0, 2880, 72}, Box{2700, 0, 2880, 72}, true, 342, 48, 9), 2349, 12, 48, 0);
  Expect("192 dpi values",
         Make(Box{0, 0, 3840, 96}, Box{3600, 0, 3840, 96}, true, 456, 64, 12), 3132, 16, 64, 0);

  Expect("gap 40", Make(Box{0, 0, 1920, 48}, Box{1800, 0, 1920, 48}, true, 228, 32, 40), 1532, 8,
         32, 0);
  Expect("gap 4000 hits min clamp",
         Make(Box{0, 0, 1920, 48}, Box{1800, 0, 1920, 48}, true, 228, 32, 4000), 4, 8, 32, 0);

  Expect("negative gap clamped",
         Make(Box{0, 0, 1920, 48}, Box{1800, 0, 1920, 48}, true, 228, 32, -100), 1572, 8, 32, 0);

  {
    anchor::Input noTray =
        Make(Box{0, 0, 1920, 48}, Box{1800, 0, 1920, 48}, true, 228, 32, 6);
    noTray.hasTray = false;
    ExpectNoAnchor("no tray", noTray);
    ExpectNoAnchor("zero-width tray",
                   Make(Box{0, 0, 1920, 48}, Box{1800, 0, 1800, 48}, true, 228, 32, 6));
    ExpectNoAnchor("zero-height tray",
                   Make(Box{0, 0, 1920, 48}, Box{1800, 0, 1920, 0}, true, 228, 32, 6));
    ExpectNoAnchor("tray outside bar",
                   Make(Box{0, 0, 1000, 48}, Box{900, 0, 1100, 48}, true, 228, 32, 6));
    Expect("tray wider than half accepted",
           Make(Box{0, 0, 1000, 48}, Box{0, 0, 700, 48}, true, 100, 32, 6), 706, 8, 32, 0,
           true);
  }

  {
    anchor::Input in =
        Make(Box{0, 0, 1000, 48}, Box{800, 0, 1000, 48}, true, 100, 32, 6);
    in.feeds[0] = Box{600, 0, 800, 48};
    in.feedsCount = 1;
    Expect("feeds LTR", in, 494, 8, 32, 0);
    in.feeds[1] = Box{700, 0, 800, 48};
    in.feedsCount = 2;
    Expect("feeds LTR inboard", in, 594, 8, 32, 0);
    in.feeds[0] = Box{0, 0, 40, 48};
    in.feeds[1] = Box{};
    in.feedsCount = 1;
    Expect("feeds LTR fallback", in, 694, 8, 32, 0);
  }
  {
    anchor::Input in =
        Make(Box{0, 0, 1000, 48}, Box{0, 0, 200, 48}, true, 100, 32, 6);
    in.feeds[0] = Box{200, 0, 400, 48};
    in.feedsCount = 1;
    Expect("feeds RTL", in, 406, 8, 32, 0, true);
    in.feeds[1] = Box{200, 0, 300, 48};
    in.feedsCount = 2;
    Expect("feeds RTL inboard", in, 306, 8, 32, 0, true);
    in.feeds[0] = Box{960, 0, 1000, 48};
    in.feeds[1] = Box{};
    in.feedsCount = 1;
    Expect("feeds RTL fallback", in, 206, 8, 32, 0, true);
  }

  Expect("vertical preserved",
         Make(Box{0, 0, 48, 1000}, Box{0, 900, 48, 1000}, false, 40, 32, 6), 4, 966, 32, 0);
  Expect("vertical borderless preserved",
         Make(Box{0, 0, 48, 1000}, Box{0, 900, 48, 1000}, false, 40, 32, 6, true, 8), 4, 6, 992,
         480);

  Check(anchor::DragGap(40, 100, 90, 1.0f, false) == 50, "drag LTR moves gap left");
  Check(anchor::DragGap(40, 90, 100, 1.0f, true) == 50, "drag RTL moves gap right");
  Check(anchor::DragGap(0, 100, 110, 1.0f, false) == 0, "LTR outward drag clamps at zero");
  Check(anchor::DragGap(0, 100, 90, 1.0f, true) == 0, "RTL outward drag clamps at zero");
  Check(anchor::DragGap(0, 100, 100, 1.0f, false) == 0, "zero drag delta");
  Check(anchor::DragGap(4000, 100, 1000, 1.0f, true) == 4000, "drag clamps at 4000");
  Check(anchor::DragGap(0, 100, 89, 1.25f, false) == 9, "drag rounds at 125%");

  std::printf("anchor tests: %d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
