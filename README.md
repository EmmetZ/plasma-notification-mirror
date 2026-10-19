# Plasma Notification Mirror

[![Build](https://github.com/MysteriousAeon/plasma-notification-mirror/actions/workflows/build.yml/badge.svg)](https://github.com/MysteriousAeon/plasma-notification-mirror/actions/workflows/build.yml)

A small Wayland utility for KDE Plasma that mirrors desktop notification popups to one or more monitors.

Plasma normally shows a notification popup on only one monitor. Plasma Notification Mirror monitors accepted Freedesktop notification requests on the session D-Bus and draws lightweight, non-focus-stealing copies using KDE's `layer-shell-qt`.

![Plasma Notification Mirror showing stacked mirrored notifications](docs/screenshot.png)

*Mirrored KDE Plasma notifications on a secondary monitor.*

## Features

- Zero-configuration default: mirrors to every non-primary monitor
- Optional multi-monitor modes
- Configurable popup corner:
  - top-left
  - top-right
  - bottom-left
  - bottom-right
- Independent margins for all four screen edges
- Unrestricted margin offsets for large and high-resolution displays
- Configurable popup background opacity
- Independent per-screen popup settings and enable switches
- Per-monitor notification stacking
- Dynamic popup height
- KDE-like compact layout and expiration bar
- Transparent rounded corners
- Countdown pauses while the pointer is over a popup
- Manual close button
- Existing notifications compact when one is dismissed
- Icons from raw `image-data` / `image_data` / `icon_data`, image paths, application icons, desktop entries, and icon themes
- Click a mirrored notification to invoke the application-provided default action through Plasma
- Automatic monitor reselection after hotplug and primary-monitor changes; waits when no target is available
- Typed D-Bus monitoring: notifications are mirrored only after the notification daemon accepts them and returns their notification ID
- Handles notification replacement and `NotificationClosed` events
- Configurable width, lifetime, stack gap, and maximum popup count
- User-level systemd service
- No root process required

## Requirements

- KDE Plasma on Wayland
- Qt 6 (Core, Gui, DBus; Qt SVG plugin for SVG icons)
- `layer-shell-qt`
- KDE Frameworks 6 `KService`
- CMake
- `libsystemd` / sd-bus
- `pkgconf`

### Arch Linux / CachyOS

```bash
sudo pacman -S --needed cmake qt6-base qt6-svg kservice layer-shell-qt systemd-libs pkgconf
```

## Build

```bash
git clone https://github.com/MysteriousAeon/plasma-notification-mirror.git
cd plasma-notification-mirror

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

## Install for the current user

Install the binary, example configuration, and systemd user service under `~/.local`:

```bash
cmake --install build --prefix "$HOME/.local"

systemctl --user daemon-reload
systemctl --user enable --now plasma-notification-mirror.service
```

No configuration file is required.

Every non-primary monitor is selected automatically. Notifications appear in the bottom-right corner using the built-in defaults; the primary monitor is always left to Plasma.

### Manual installation

If you prefer to install the files manually:

```bash
mkdir -p ~/.local/bin ~/.config/systemd/user

cp build/plasma-notification-mirror \
  ~/.local/bin/plasma-notification-mirror

cp systemd/plasma-notification-mirror.service \
  ~/.config/systemd/user/

systemctl --user daemon-reload
systemctl --user enable --now plasma-notification-mirror.service
```

## Configuration

The optional configuration file is:

```text
~/.config/plasma-notification-mirror/config.ini
```

To start from the included example:

```bash
mkdir -p ~/.config/plasma-notification-mirror

cp config/config.example.ini \
  ~/.config/plasma-notification-mirror/config.ini

systemctl --user restart plasma-notification-mirror.service
```

### Monitor modes

```ini
[General]
mode=all-secondary
```

Available values:

- `all-secondary` — every enabled non-primary monitor; default
- `secondary` — first enabled non-primary monitor
- `screens` — enabled non-primary monitors listed in `screens=`
- `all` — compatibility alias for `all-secondary`
- `primary` — deprecated; selects no mirror targets

The primary monitor is excluded in every mode, including when it is explicitly listed
in `screens=`. Existing configurations that omit `mode` now select all secondary
monitors; an explicit `mode=secondary` still selects only the first.

Example for selected monitors:

```ini
[General]
mode=screens
screens=DP-2,DP-3
```

Monitor selection is updated automatically when a display is connected or disconnected,
when the primary display changes, and when screen geometry or DPI changes. No service
restart is needed. In `secondary` / `all-secondary` mode, a single-monitor session has
no mirror target: the process stays running and resumes mirroring future notifications
when a secondary display becomes available. Plasma's original notifications continue
normally.

In `screens` mode, unavailable configured displays are skipped; there is no fallback
to unlisted displays. This replaces the previous first-secondary fallback. When a
configured display returns, it is selected again with its own settings. Popups belonging to removed targets are discarded; old notifications
are not replayed onto newly connected targets.

Monitor names can be viewed with:

```bash
kscreen-doctor -o
```

### Per-screen configuration

`[General]` supplies defaults. Add `[Screen:<name>]` sections to override popup
settings for individual monitors. Names match `kscreen-doctor -o` exactly. For example:

```ini
[General]
mode=all-secondary
position=bottom-right
width=332
lifetime_ms=5000

[Screen:HDMI-A-1]
position=bottom-left
left_margin=40
bottom_margin=88
width=400
lifetime_ms=8000
max_notifications=6
background_opacity=90
gap=18

[Screen:DP-2]
position=top-right
top_margin=32
right_margin=24
width=300
max_notifications=3

[Screen:DP-3]
enabled=false
```

Every popup option below, plus `enabled`, can be set per screen. Omitted keys inherit
`[General]`; screens without a section use those defaults. `mode` and `screens` are
only global selection options. A screen section customizes a selected screen; it does
not override `mode=screens` or `mode=secondary` selection. `enabled=false` excludes
a screen; `enabled=true` in a screen section can override a global `enabled=false`.

Each screen has its own stack, popup limit, countdowns, and local dismissals. Application
actions and daemon-side close events still apply to all copies of that notification.
Screen settings follow the connector name after reconnecting and apply only while the
screen is non-primary. Changing the primary screen removes its mirrored popups;
newly selected secondary screens show future notifications only.

Restart the service after editing the configuration file:

```bash
systemctl --user restart plasma-notification-mirror.service
```

### Popup position

```ini
[General]
position=bottom-right
```

Available values:

- `top-left`
- `top-right`
- `bottom-left`
- `bottom-right` — default

Notifications stack away from the selected corner. For example, notifications positioned at the top of the screen stack downward, while notifications positioned at the bottom stack upward.

### Margins

Each screen edge has its own margin:

```ini
[General]
left_margin=18
right_margin=18
top_margin=18
bottom_margin=58
```

Only the two margins relevant to the selected corner are used.

For example:

```ini
[General]
position=top-right
right_margin=24
top_margin=32
```

Margin values are not artificially capped, so large offsets can be used on high-resolution or unusual monitor layouts.

### Background opacity

```ini
[General]
background_opacity=100
```

`background_opacity` accepts values from `0` to `100`:

- `100` — fully opaque; default
- `0` — fully transparent

Values between them produce a translucent popup background. Rounded areas outside the notification card remain transparent.

### Other settings

```ini
[General]
lifetime_ms=5000
width=332
max_notifications=5
gap=12
```

Defaults:

- `lifetime_ms=5000` — popup lifetime in milliseconds
- `width=332` — popup width in pixels
- `max_notifications=5` — maximum simultaneous mirrored notifications per target monitor
- `gap=12` — space between stacked notifications in pixels

### Full configuration example

```ini
[General]

mode=all-secondary
screens=

position=bottom-right

lifetime_ms=5000
width=332
max_notifications=5
background_opacity=100

left_margin=18
right_margin=18
top_margin=18
bottom_margin=58

gap=12
```

## Testing

Run automated tests in an isolated D-Bus session (requires Qt Test and `dbus-run-session`):

```bash
ctest --test-dir build --output-on-failure
```

Tests cover RGB/RGBA image payloads, deprecated image hints, malformed image buffers,
per-screen inheritance and isolation, primary exclusion, disabled and unavailable screens,
file and desktop-entry icons, application/content icon separation and duplicate suppression,
icon lookup without a desktop platform plugin, default action invocation, duplicate clicks, resident
notifications, and action errors. The isolated session does not use your desktop's
notification service. Tests can be omitted at configure time with `-DBUILD_TESTING=OFF`.

Send a test notification:

```bash
notify-send -i dialog-information \
  "Mirror test" \
  "This should appear on the configured mirror monitor."
```

Send several notifications to test stacking:

```bash
notify-send -a "Mirror Test" -i dialog-information "Notification 1" "First item"
notify-send -a "Mirror Test" -i dialog-information "Notification 2" "Second item"
notify-send -a "Mirror Test" -i dialog-information "Notification 3" "Third item"
```

Test a clickable default action (click the mirrored card):

```bash
notify-send --wait --action=default=Open -i dialog-information \
  "Clickable mirror test" "Click this card to print default in the terminal."
```

`notify-send` prints the action key. Test opening a real window with an application
that already provides clickable notifications, such as a chat client. A plain
`notify-send` without `--action` has no default click behavior.

To check hotplug, start with one display in the default `all-secondary` mode, connect a
second display, and send a new notification. Unplug it while a popup is visible,
then reconnect it and send another notification. Also test switching the primary
display and the configured `screens` mode. The process should stay running throughout.
With multiple secondary displays, configure different widths, corners, lifetimes,
and popup limits, then verify that each uses its own settings. Disable one with
`enabled=false` and restart; it should receive no mirrored popups.

Check the service:

```bash
systemctl --user status plasma-notification-mirror.service
```

Recent logs can be viewed with:

```bash
journalctl --user \
  -u plasma-notification-mirror.service \
  --no-pager \
  -n 100
```

## Updating

Pull and rebuild:

```bash
git pull

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

If installed with `cmake --install`:

```bash
cmake --install build --prefix "$HOME/.local"
systemctl --user daemon-reload
systemctl --user restart plasma-notification-mirror.service
```

If you installed the binary manually, replacing a currently executing executable directly can produce `Text file busy`.

Use an atomic replacement instead:

```bash
cp build/plasma-notification-mirror \
  ~/.local/bin/plasma-notification-mirror.new

chmod +x ~/.local/bin/plasma-notification-mirror.new

mv -f \
  ~/.local/bin/plasma-notification-mirror.new \
  ~/.local/bin/plasma-notification-mirror

systemctl --user restart plasma-notification-mirror.service
```

## Uninstall

If installed under `~/.local` with CMake:

```bash
systemctl --user disable --now plasma-notification-mirror.service

rm -f ~/.local/bin/plasma-notification-mirror
rm -f ~/.local/share/systemd/user/plasma-notification-mirror.service
rm -rf ~/.local/share/plasma-notification-mirror

systemctl --user daemon-reload
```

For a manual installation:

```bash
systemctl --user disable --now plasma-notification-mirror.service

rm -f ~/.config/systemd/user/plasma-notification-mirror.service
rm -f ~/.local/bin/plasma-notification-mirror

systemctl --user daemon-reload
```

The optional user configuration can also be removed:

```bash
rm -rf ~/.config/plasma-notification-mirror
```

## Notes

Plasma Notification Mirror is intentionally a notification **mirror**, not a replacement notification daemon.

It does not attempt to duplicate:

- notification action buttons
- Plasma's notification history
- every internal Plasma notification UI behavior

It does track daemon-side notification replacement and close events so mirrored popups remain aligned with the notification service.

Application and notification icons follow Plasma's separate roles: a small application
icon appears beside the application name in the header, while an optional content icon
or image appears on the right of the notification body. Identical named icons are not
shown twice. A plain `notify-send -i dialog-information` therefore has a right-hand
information icon, while an application such as Spectacle has its application icon in
the header.

Application icons use KDE's KService desktop-file lookup and KDE notification configuration
(`x-kde-appname`). Missing Flatpak application identities are resolved from the sender's
`.flatpak-info`, with exact desktop name/display name/WM class matching as a fallback.
System and Flatpak icon export directories are included even when a systemd service
starts without a desktop platform theme plugin. The selected KDE icon theme is read
from KDE settings, with Breeze as a fallback; install `qt6-svg` for SVG-based themes.

The reference implementation is Plasma 6.8.0's
[NotificationHeader.qml](https://github.com/KDE/plasma-workspace/blob/v6.8.0/applets/notifications/components/NotificationHeader.qml),
[DelegatePopup.qml](https://github.com/KDE/plasma-workspace/blob/v6.8.0/applets/notifications/delegates/DelegatePopup.qml),
and [notification.cpp / server_p.cpp](https://github.com/KDE/plasma-workspace/tree/v6.8.0/libnotificationmanager).

Clicking the card invokes only the `default` action advertised by the application,
using Plasma's `org.kde.NotificationManager.InvokeAction` interface on a separate
D-Bus connection. Notifications without that action do not open an application.
A successful action closes non-resident notifications and their mirrored copies;
resident notifications remain. The × button continues to dismiss only the local
mirror. An action failure is logged and leaves the mirror visible.

This Plasma interface does not accept a Wayland activation token. The action is
forwarded, but whether an existing window is raised/focused depends on the application
and compositor. This does not reproduce all of Plasma's native window-activation
behavior. Inline replies, custom action buttons, and clickable body links are not
implemented.

## License

MIT
