#pragma once

#include <algorithm>
#include <cmath>
#include <optional>

namespace anchor {

struct Box {
  int left = 0;
  int top = 0;
  int right = 0;
  int bottom = 0;
};

struct Input {
  Box bar{};
  Box tray{};
  bool hasTray = false;
  Box feeds[2]{};
  int feedsCount = 0;
  bool horizontal = true;
  int widgetW = 0;
  int contentH = 0;
  int insetPx = 0;
  int gapPx = 0;
  bool borderless = false;
};

struct Result {
  int x = 0;
  int y = 0;
  int h = 0;
  int offsetY = 0;
  bool widgetRightOfTray = false;
};

enum class Reason { None, NoAnchor };

struct Outcome {
  std::optional<Result> result;
  Reason reason = Reason::None;
};

inline Outcome Compute(const Input& in) {
  if (!in.hasTray) return {std::nullopt, Reason::NoAnchor};
  if (in.tray.right <= in.tray.left || in.tray.bottom <= in.tray.top)
    return {std::nullopt, Reason::NoAnchor};
  if (in.tray.left < in.bar.left || in.tray.right > in.bar.right ||
      in.tray.top < in.bar.top || in.tray.bottom > in.bar.bottom)
    return {std::nullopt, Reason::NoAnchor};

  const int barH = in.bar.bottom - in.bar.top;
  const int gapPx = std::max(0, in.gapPx);
  int h = in.contentH;
  if (in.borderless) {
    const int full = barH - in.insetPx;
    if (full > h) h = full;
  }
  const int offsetY = in.borderless ? (h - in.contentH) / 2 : 0;

  Result r;
  r.h = h;
  r.offsetY = offsetY;

  const int minX = in.bar.left + 4;
  const int maxX = in.bar.right - in.widgetW - 4;
  const int minY = in.bar.top + 2;
  const int maxY = in.bar.bottom - h - 2;

  if (in.horizontal) {
    // The tray hugs the nearer end of the bar. An exact tie uses the high-end
    // convention, so the result is deterministic without the RTL style bit.
    const long long barMid2 = (long long)in.bar.left + in.bar.right;
    const long long trayMid2 = (long long)in.tray.left + in.tray.right;
    r.widgetRightOfTray = trayMid2 < barMid2;
    int x = r.widgetRightOfTray ? in.tray.right + gapPx
                                : in.tray.left - gapPx - in.widgetW;

    const int count = std::min(2, std::max(0, in.feedsCount));
    int chosen = -1;
    if (!r.widgetRightOfTray) {
      for (int i = 0; i < count; ++i)
        if (in.feeds[i].left <= in.tray.left &&
            (chosen < 0 || in.feeds[i].left > in.feeds[chosen].left))
          chosen = i;
      if (chosen >= 0) {
        const int candidate = in.feeds[chosen].left - gapPx - in.widgetW;
        if (candidate >= minX && candidate <= maxX) x = candidate;
      }
    } else {
      for (int i = 0; i < count; ++i)
        if (in.feeds[i].right >= in.tray.right &&
            (chosen < 0 || in.feeds[i].right < in.feeds[chosen].right))
          chosen = i;
      if (chosen >= 0) {
        const int candidate = in.feeds[chosen].right + gapPx;
        if (candidate >= minX && candidate <= maxX) x = candidate;
      }
    }
    r.x = x;
    r.y = in.bar.top + (barH - h) / 2;
  } else {
    // Deliberately preserved from 1.1.4; vertical taskbars are a 1.2 feature.
    r.y = in.tray.bottom + gapPx;
    r.x = in.bar.left + (in.bar.right - in.bar.left - in.widgetW) / 2;
  }

  // Keep 1.1.4's clamp order: the max clamp wins when the widget is wider than the bar.
  if (r.x < minX) r.x = minX;
  if (r.x > maxX) r.x = maxX;
  if (r.y < minY) r.y = minY;
  if (r.y > maxY) r.y = maxY;
  return {r, Reason::None};
}

inline int DragGap(int startOffsetDip, int startCursorX, int cursorX, float scale,
                   bool widgetRightOfTray) {
  if (!(scale > 0.0f)) scale = 1.0f;
  const int delta = widgetRightOfTray ? cursorX - startCursorX : startCursorX - cursorX;
  const int offset = startOffsetDip + (int)std::lround(delta / scale);
  return std::clamp(offset, 0, 4000);
}

}  // namespace anchor
