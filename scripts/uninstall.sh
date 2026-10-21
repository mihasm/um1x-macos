#!/bin/sh
# uninstall.sh - stops um1x-midi-driver and removes everything install.sh put
# in place. The log is kept unless --purge is given.
set -eu

LABEL=io.github.mihasm.um1x-midi-driver
LIBEXEC="$HOME/.local/libexec/um1x-midi-driver"
LAUNCHER="$HOME/.local/bin/um1x-midi-driver"
PLIST="$HOME/Library/LaunchAgents/$LABEL.plist"
LOG="$HOME/Library/Logs/um1x-midi-driver.log"
DOMAIN="gui/$(id -u)"

if launchctl print "$DOMAIN/$LABEL" >/dev/null 2>&1; then
    launchctl bootout "$DOMAIN/$LABEL"
    echo "stopped $LABEL"
fi
rm -f "$PLIST" "$LAUNCHER"
rm -rf "$LIBEXEC"
if [ "${1:-}" = "--purge" ]; then
    rm -f "$LOG" "$LOG.1"
    echo "removed um1x-midi-driver and its log"
else
    echo "removed um1x-midi-driver (log kept: $LOG)"
fi
