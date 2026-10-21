#!/bin/sh
# install.sh - installs um1x-midi-driver for the current user and starts it
# at login (a LaunchAgent). Safe to run again to update.
#
#   scripts/install.sh [path/to/um1x-midi-driver]
#
# Puts in place:
#   ~/.local/libexec/um1x-midi-driver/um1x-midi-driver   the driver binary
#   ~/.local/libexec/um1x-midi-driver/uninstall.sh       to remove it again
#   ~/.local/bin/um1x-midi-driver                        launcher script (also the CLI)
#   ~/Library/LaunchAgents/io.github.mihasm.um1x-midi-driver.plist
#   ~/Library/Logs/um1x-midi-driver.log                  log (rotated at 1 MB)
set -eu

LABEL=io.github.mihasm.um1x-midi-driver
REPO=https://github.com/mihasm/um1x-macos
HERE=$(cd "$(dirname "$0")" && pwd)
LIBEXEC="$HOME/.local/libexec/um1x-midi-driver"
LAUNCHER="$HOME/.local/bin/um1x-midi-driver"
PLIST="$HOME/Library/LaunchAgents/$LABEL.plist"
LOG="$HOME/Library/Logs/um1x-midi-driver.log"
DOMAIN="gui/$(id -u)"

SRC=${1:-}
if [ -z "$SRC" ]; then
    if [ -f "$HERE/um1x-midi-driver" ]; then SRC="$HERE/um1x-midi-driver"     # release archive
    else SRC="$HERE/../build/um1x-midi-driver"; fi                             # source checkout
fi
[ -f "$SRC" ] || { echo "install: no driver binary at $SRC (run make first)" >&2; exit 1; }
SRC=$(cd "$(dirname "$SRC")" && pwd)/$(basename "$SRC")
UNINSTALL_SRC="$HERE/uninstall.sh"
[ -f "$UNINSTALL_SRC" ] || UNINSTALL_SRC="$HERE/../scripts/uninstall.sh"

# A file downloaded with a browser carries the quarantine flag, and Gatekeeper
# would refuse to start an unsigned binary from launchd.
if xattr -p com.apple.quarantine "$SRC" >/dev/null 2>&1; then
    xattr -d com.apple.quarantine "$SRC"
fi
VERSION=$("$SRC" --version | sed 's/^um1x-midi-driver //')
echo "installing um1x-midi-driver $VERSION"

# Stop a running copy first (no error if none is loaded).
if launchctl print "$DOMAIN/$LABEL" >/dev/null 2>&1; then
    launchctl bootout "$DOMAIN/$LABEL"
    sleep 1
fi

mkdir -p "$LIBEXEC" "$HOME/.local/bin" "$HOME/Library/LaunchAgents" "$HOME/Library/Logs"
install -m 755 "$SRC" "$LIBEXEC/um1x-midi-driver"
install -m 755 "$UNINSTALL_SRC" "$LIBEXEC/uninstall.sh"

cat > "$LAUNCHER" <<EOF
#!/bin/sh
# um1x-midi-driver — driver for the Roland/Edirol UM-1X USB MIDI interface.
#
# macOS has no working driver for the UM-1X. This runs a small user-space
# driver that talks to it over USB (libusb) and shows it to Logic and every
# other MIDI app as the CoreMIDI port "UM-1X" (in and out).
# Run with no arguments it is the driver itself; "um1x-midi-driver help"
# lists the diagnostic commands (list, monitor, send, test-out).
#
# Source: $REPO (version $VERSION)
# Installed $(date +%Y-%m-%d) by scripts/install.sh from $SRC
# Started at login by launchd: ~/Library/LaunchAgents/$LABEL.plist
# Log: ~/Library/Logs/um1x-midi-driver.log
# Remove: ~/.local/libexec/um1x-midi-driver/uninstall.sh (or make uninstall)

exec "\$HOME/.local/libexec/um1x-midi-driver/um1x-midi-driver" "\$@"
EOF
chmod 755 "$LAUNCHER"

cat > "$PLIST" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>Label</key>
	<string>$LABEL</string>
	<key>ProgramArguments</key>
	<array>
		<string>$LAUNCHER</string>
		<string>run</string>
		<string>--log</string>
		<string>$LOG</string>
	</array>
	<key>RunAtLoad</key>
	<true/>
	<key>KeepAlive</key>
	<true/>
	<key>ThrottleInterval</key>
	<integer>5</integer>
	<key>ProcessType</key>
	<string>Interactive</string>
	<key>StandardOutPath</key>
	<string>$LOG</string>
	<key>StandardErrorPath</key>
	<string>$LOG</string>
</dict>
</plist>
EOF
plutil -lint "$PLIST" >/dev/null

launchctl bootstrap "$DOMAIN" "$PLIST"
launchctl enable "$DOMAIN/$LABEL"

# Wait for it to come up.
i=0
while [ $i -lt 10 ]; do
    if launchctl print "$DOMAIN/$LABEL" 2>/dev/null | grep -q "state = running"; then break; fi
    sleep 0.5; i=$((i + 1))
done
if launchctl print "$DOMAIN/$LABEL" 2>/dev/null | grep -q "state = running"; then
    echo "running: launchd job $LABEL"
else
    echo "warning: the driver is not running; see $LOG" >&2
fi
sleep 1
tail -n 5 "$LOG"
case ":$PATH:" in
    *":$HOME/.local/bin:"*) ;;
    *) echo "note: ~/.local/bin is not in your PATH; the CLI is $LAUNCHER" ;;
esac
