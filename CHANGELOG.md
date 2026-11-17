# Changelog

## 1.1.4

- Fixed a render-surface failure path that could leave a null Direct2D target and crash the next paint.
- Throttled the full-window taskbar coverage scan to once per second and foreground changes. DWM is now asked only about windows that actually cover the monitor.
- GPU readout now uses a hardware-only LUID set. WARP / Basic Render Driver is never shown as GPU load, a WARP-only VM shows a dash, and hot-plug or duplicate hardware LUIDs are included.
- GPU alerts use the stabilized display value, so the number and colour agree, and a brief counter miss no longer clears the alert early.
- A failed GPU counter collect retries on the next sample and reopens the query after three misses; the first collect after reopening the query is discarded to avoid a fake 0 or 100% spike.
- USB, dock and eGPU arrivals trigger an adapter rescan, coalesced so a burst of device events does not enumerate DXGI repeatedly.
- Shutdown clears the sampler target before windows are destroyed and, on a clean stop, releases the screen DC and event handles.
- `WM_SETTINGCHANGE` no longer dereferences `lParam`, which can point into another process.
- Settings opens without `AttachThreadInput`, so a slow foreground app cannot block it.
- Dependent DLLs load from System32, and the autostart path is no longer capped at `MAX_PATH`.
- Debug logging lists ACPI thermal zones even when temperatures are off.
- Settings footer and tray tooltip report 1.1.4; the exe metadata is 1.1.4.0.

## 1.1.3

- Settings now labels the temperature as the thermal zone. The README explains that it is the warmest ACPI zone, not a core sensor, and records the verified laptop (Acer Predator PHN16-72, one zone `\_tz.tz00`); an offset against HWiNFO or Core Temp was not measured.
- The GPU readout ignores WARP/Basic Render Driver instances, holds the last good value through brief counter misses, and requires a one-point change to persist for three samples before updating. This removes the 0/1% flicker reported on a 120 Hz setup.
- A failed GPU counter collect now retries after 1 s instead of 30 s.
- Debug logs list the ACPI thermal zones so OEM reports are easier.
- Settings footer and tray tooltip report 1.1.3; the exe metadata is 1.1.3.0.

## 1.1.2

- Accent-colored text and check marks now meet 4.5:1 against the menu, including the hovered row. The accent fills used by switches, load bars and the keyboard focus ring meet 3:1. Before, the fixed tint on dark failed for black and near-black accents, and the raw accent on light failed for bright ones.
- Check marks are drawn as a 1.7 px stroke instead of a 12.5 px glyph, so they stay thick at every DPI and no longer depend on the check glyph existing in the fallback font.
- Switch knobs get a hairline edge, so they stay visible on a light accent or a light-theme off track. Light-theme group labels were raised from 4.48:1 to 4.8:1.
- Black, white and gray accents now use the normal text color for text and checks instead of a washed-out tint.
- Added a contrast regression test that runs as part of build.ps1.
- Settings footer and tray tooltip report 1.1.2; the exe metadata is 1.1.2.0.

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
