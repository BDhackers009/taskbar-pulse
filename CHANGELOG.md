# Changelog

## 1.1.0

- The whole top band of Settings is now a drag handle. Before, only the strip below the shadow moved the window, and the margin passed clicks through to the window underneath.
- Fixed the widget sometimes staying hidden after launching an app from Start or Search. Hidden and stale shell windows are no longer mistaken for a full-screen app, and the taskbar must actually be visible.
- Removed a cross-process hit test from the UI thread. `WindowFromPoint` sends a message to whatever window is under a point and could freeze the widget while another app was busy launching.
- Settings footer, tray tooltip and file metadata report 1.1.0.

## 1.0.0

- First release: CPU, GPU, RAM, network, CPU temperature, per-metric alerts, GPU pinning, borderless mode, keyboard settings.
