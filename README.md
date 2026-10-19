# Plasma Notification Mirror

[![Build](https://github.com/MysteriousAeon/plasma-notification-mirror/actions/workflows/build.yml/badge.svg)](https://github.com/MysteriousAeon/plasma-notification-mirror/actions/workflows/build.yml)

A small Wayland utility for KDE Plasma that mirrors desktop notification popups to one or more monitors.

Plasma normally shows a notification popup on only one monitor. Plasma Notification Mirror monitors accepted Freedesktop notification requests on the session D-Bus and draws lightweight, non-focus-stealing copies using KDE's `layer-shell-qt`.

![Plasma Notification Mirror showing stacked mirrored notifications](docs/screenshot.png)

*Mirrored KDE Plasma notifications on a secondary monitor.*

## Features

- Zero-configuration default: mirrors to every non-primary monitor
- Optional multi-monitor modes
- Nine popup positions, including edge centers and the screen center
- Optional margin positioning mode preserving the original four-corner behavior
- Automatic horizontal centering and centered notification stacks
- Optional common margin with independent edge overrides
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
position=bottom-center
bottom_margin=88
width=400
lifetime_ms=8000
max_notifications=6
background_opacity=90
gap=18

[Screen:DP-2]
position=center-left
margin=24
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

### Positioning mode

`position_mode` can be set in `[General]` or overridden per screen:

- `grid` (default): nine positions with automatic centering on centered axes.
- `margin`: original four-corner placement, with both axes anchored to screen edges.
  Popup placement is controlled by the matching horizontal and vertical margins.
  No automatic centering is performed, even after resolution or popup width changes.

For the original manual bottom-left placement:

```ini
[Screen:HDMI-A-1]
position_mode=margin
position=bottom-left
left_margin=1118
bottom_margin=88
```

Margin mode accepts `top-left`, `top-right`, `bottom-left`, and `bottom-right`.
Set a corner explicitly when changing from a centered grid position. An unsupported
position is logged and falls back to the inherited corner, or `bottom-right` if the
inherited position is also incompatible. Margins, gap, lifetime, and other popup
settings retain the same per-screen inheritance rules. The `margin` shorthand and
individual edge overrides work in both modes.

Choose `position_mode=grid` on another screen to keep automatic centering there.
The global monitor-selection `mode` is independent of `position_mode`.

### Popup position

In `position_mode=grid`, choose one of nine positions in either `[General]` or a screen section:

| Left | Center | Right |
| --- | --- | --- |
| `top-left` | `top-center` | `top-right` |
| `center-left` | `center` | `center-right` |
| `bottom-left` | `bottom-center` | `bottom-right` (default) |

For example, bottom-center needs only a bottom margin:

```ini
[Screen:HDMI-A-1]
position=bottom-center
bottom_margin=88
```

Horizontal centering follows the output width automatically, including after changes
to resolution, scaling, or popup width. Do not calculate `left_margin` yourself.
Top positions stack downward; bottom positions stack upward. The middle row centers
the entire notification stack vertically and arranges its cards from top to bottom.
The group recenters when notifications are added, replaced, or dismissed. If a
centered stack exceeds the screen height, it starts at the top of the screen.

Short forms `top`, `bottom`, `left`, and `right` mean the corresponding edge center.
`left-center` / `middle-left` and `right-center` / `middle-right` are also accepted;
`middle` and `center-center` mean `center`.

### Optional margins

Every margin is optional. Without any margin keys, defaults remain 18 logical pixels
on the left, right, and top, and 58 on the bottom. These distances are measured from
the full screen edges, including the area occupied by panels.

Use `margin` to set a common distance and add an individual edge only when needed:

```ini
[General]
margin=24
bottom_margin=72

[Screen:DP-2]
position=top-right
margin=32
```

Within each section, `margin` sets all four edges first, then `left_margin`,
`right_margin`, `top_margin`, and `bottom_margin` override individual edges. A screen's
`margin` replaces all inherited edge margins; a screen's individual edge key replaces
only that edge. Omitted values inherit `[General]`.

Only edges used by the selected position affect placement:

| Position | Relevant margins |
| --- | --- |
| Four corners | The matching horizontal and vertical edge |
| `top-center` / `bottom-center` | `top_margin` / `bottom_margin` only |
| `center-left` / `center-right` | `left_margin` / `right_margin` only |
| `center` | None |

In grid mode, centered axes ignore their edge margins. For example, `center-left` needs no top or
bottom margins, and `center` needs no margins at all. Zero is valid; negative margins
are clamped to zero. Existing configurations using only individual edge keys remain
compatible, and large offsets remain supported.

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

margin=18
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
all nine positions, centered stack reflow, margin-mode compatibility and inheritance,
and common/edge margin precedence,
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
