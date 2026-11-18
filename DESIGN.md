# Taskbar Pulse design direction

This is a Windows taskbar instrument, not a dashboard. It should read as a piece of the operating system that Microsoft could have shipped, only denser and calmer.

## Identity

The taskbar is the surface. The widget borrows its colour, its typography scale and its restraint. No branding, no mascot, no ornament. The only signature is the compression: a handful of live numbers in the space of four tray icons.

## Palette

Neutral taskbar surface, system text colour, and the Windows accent for data. One accent only. Amber means warning and red means critical, and they appear only when a threshold is crossed. No gradients, no glow, no glass.

## Type

Segoe UI Variable. Labels at 9 px semibold, values at 12.5 px semibold, menu and settings rows at 12.5 px regular, section headers at 9 px semibold uppercase. Fixed-width columns and tabular figures so the numbers never dance.

## Surfaces

Widget: one translucent pill, radius 8, one hairline border, or no pill at all in borderless mode. Menu and settings: one solid panel, radius 10, one hairline border, one soft shadow to lift it off the taskbar. No second elevation level.

## Motion

None. The data changes once per second and the widget repaints only when the rounded values change. A menu or panel appears in one frame.

## Dials

ENERGY 2, RHYTHM 2, MOTION 1. Quiet by default, sharp when a threshold fires.

## Constraints

- Never wider than the free space beside the notification area unless the user drags it there.
- Never cover taskbar buttons by design. The overlay is a compromise Windows 11 forces, and the README says so.
- One accent moment per surface. In Settings the accent lives on the switches, the reset link and the keyboard focus ring, nowhere else.
- Accent-derived text and check marks stay at 4.5:1 against their surface; accent fills and focus rings at 3:1. Tint only as far as the ratio requires, and use the neutral text color when the accent has no hue.
- Every control does something. No decorative rows, no placeholder features.
- Network is not a percentage and gets no bar. It keeps the grid: upload on the label line, download on the value line, both tabular, so the column reads as the same instrument rather than a second widget.
