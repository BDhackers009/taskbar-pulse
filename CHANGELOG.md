# Changelog

## 1.1.1

- Menu checkmarks are now a light tint of the system accent on the dark menu instead of the raw accent. Contrast goes from about 3.7:1 to 7.5:1, so the checks are easy to see.
- Settings footer and tray tooltip report 1.1.1; the exe metadata is 1.1.1.0.

## 1.1.0

- The whole top band of Settings is now a drag handle. Before, only the strip below the shadow moved the window, and the margin passed clicks through to the window underneath.
- Fixed the widget sometimes staying hidden after launching an app from Start or Search. Hidden and stale shell windows are no longer mistaken for a full-screen app, and the taskbar must actually be visible.
- Removed a cross-process hit test from the UI thread. `WindowFromPoint` sends a message to whatever window is under a point and could freeze the widget while another app was busy launching.
- Settings footer and tray tooltip report 1.1; the exe metadata is 1.1.0.0.

## 1.0.0

- First release: CPU, GPU, RAM, network, CPU temperature, per-metric alerts, GPU pinning, borderless mode, keyboard settings.
