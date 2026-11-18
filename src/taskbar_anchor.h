#pragma once

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
  bool traySynthetic = false;  // temporary: skips validation for commit 1's fallback
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

  const int barH = in.bar.bottom - in.bar.top;
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
    r.x = in.tray.left - in.gapPx - in.widgetW;
    r.y = in.bar.top + (barH - h) / 2;
  } else {
    // Deliberately preserved from 1.1.4; vertical taskbars are a 1.2 feature.
    r.y = in.tray.bottom + in.gapPx;
    r.x = in.bar.left + (in.bar.right - in.bar.left - in.widgetW) / 2;
  }

  // Keep 1.1.4's clamp order: the max clamp wins when the widget is wider than the bar.
  if (r.x < minX) r.x = minX;
  if (r.x > maxX) r.x = maxX;
  if (r.y < minY) r.y = minY;
  if (r.y > maxY) r.y = maxY;
  return {r, Reason::None};
}

}  // namespace anchor
