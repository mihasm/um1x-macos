# um1x-macos

A small user-space driver that makes the **Roland / Edirol UM-1X** USB MIDI
interface work on current macOS, Apple silicon included. It shows up in
Logic Pro, GarageBand, Ableton Live, MainStage, Audio MIDI Setup and every
other CoreMIDI app as a MIDI port called **UM-1X**, in both directions.

Roland's own UM-1 driver dates from the Mac OS X era (PowerPC/Intel, kernel
extension) and does not load on modern macOS. The UM-1X is not class
compliant, so macOS loads nothing for it and it stays invisible to MIDI apps.
This driver talks to it directly over USB with libusb and publishes virtual
CoreMIDI ports. No kernel extension, no system extension, no admin rights.

- One self-contained universal binary (arm64 + x86_64), macOS 11 or later.
  libusb is linked in statically; no Homebrew needed.
- MIDI in and out: notes, controllers, pedals, running status, real-time
  messages (clock, active sensing) and SysEx of any length, both ways.
- Hot-plug: start it with or without the interface plugged in; it reconnects
  after unplugging, after sleep and when a USB hub is replugged. The CoreMIDI
  ports stay in place while the interface is away (marked offline), so Logic
  keeps its routing.
- Low latency: interrupt-endpoint input with asynchronous transfers on a
  high-priority thread; scheduled output is sent on time by the driver.
  Near zero CPU when idle.
- Starts at login as a LaunchAgent; logs to `~/Library/Logs`.
- Diagnostic commands: `list`, `monitor`, `send`, `test-out`.

## Supported devices

| Device | USB ID | Status |
|---|---|---|
| Edirol UM-1X (same USB ID as the UM-1) | 0582:0009 | **tested** |

Several other old Roland/Edirol/BOSS devices use exactly the same vendor-mode
protocol (Linux drives them all with one "fixed endpoint" quirk), so they are
in the device table too. They are **untested** — they should work, but nobody
has tried them with this driver yet. If you have one, please open an issue
either way.

UM-4, SC-8850, U-8, UM-2, SC-8820, PC-300, SK-500, XV-5050, UM-880, MMP-2,
V-Synth, UM-550, SD-20, SD-80, XV-2020, VariOS, PCR, Roland Digital Piano
(0582:0037), GI-20, RS-70, UR-80, PCR-A, PCR-1, Fantom-X, BOSS DR-880, G-70,
PC-50. `um1x-midi-driver devices` prints the table with USB IDs. Devices with
several MIDI ports get one CoreMIDI port per cable ("UM-4 1" … "UM-4 4").

Roland audio interfaces are deliberately left out, as is anything class
compliant (UM-ONE, UM-ONE mk2 in class-compliant mode, …): those either have
a current Roland driver or need none.

## Install

### From the release

```sh
curl -fsSLO https://github.com/mihasm/um1x-macos/releases/latest/download/um1x-midi-driver-macos-universal.tar.gz
tar xzf um1x-midi-driver-macos-universal.tar.gz
cd um1x-midi-driver-*/
./install.sh
```

The binary is ad-hoc signed, not notarised. Downloading with `curl` as above
avoids Gatekeeper's quarantine; `install.sh` also clears the quarantine flag
if you downloaded the archive with a browser. Each release lists SHA-256
checksums of the archive and of the binary.

### From source

Needs the Xcode command line tools (`xcode-select --install`).

```sh
git clone https://github.com/mihasm/um1x-macos
cd um1x-macos
make            # downloads and builds libusb once (checksum verified)
make test       # unit tests of the packet conversion
make install
```

### What gets installed

| Path | What |
|---|---|
| `~/.local/libexec/um1x-midi-driver/um1x-midi-driver` | the driver binary |
| `~/.local/bin/um1x-midi-driver` | launcher script; also the CLI (`um1x-midi-driver help`) |
| `~/Library/LaunchAgents/io.github.mihasm.um1x-midi-driver.plist` | starts it at login, restarts it if it ever exits |
| `~/Library/Logs/um1x-midi-driver.log` | log, rotated at 1 MB |

In System Settings → General → Login Items & Extensions it appears as
**um1x-midi-driver**.

### Uninstall

```sh
make uninstall                                  # from the source folder, or:
~/.local/libexec/um1x-midi-driver/uninstall.sh  # add --purge to delete the log too
```

## Using it

Plug the UM-1X in. Its **OUT** plug goes to the instrument's MIDI **IN**, its
**IN** plug to the instrument's MIDI **OUT**. In Logic Pro the port appears as
**UM-1X** under Settings → MIDI → Inputs, and as an output for external MIDI
tracks. Nothing else to set up.

```text
$ um1x-midi-driver list           # is the interface there, and which ports exist?
USB devices:
  0582:0009 at USB 0-1.4    UM-1X

CoreMIDI sources (MIDI in to apps):
  UM-1X                        Roland     UM-1X          id 1403774927  online
...

$ um1x-midi-driver monitor        # what arrives from the instrument (Ctrl-C to stop)
monitoring "UM-1X"
    2.315  UM-1X        Note On        ch 1  C4 (60) vel 71       90 3C 47
    2.512  UM-1X        Control Change ch 1  cc 64 (Sustain) value 127   B0 40 7F
    2.890  UM-1X        Note Off       ch 1  C4 (60) vel 64       80 3C 40

$ um1x-midi-driver test-out       # plays an arpeggio and a sustained chord
$ um1x-midi-driver send F0 7E 7F 06 01 F7    # SysEx identity request
$ um1x-midi-driver send -f patch.syx         # send a SysEx file
```

`monitor` hides MIDI clock and active sensing unless you pass `-a`; `-r`
also prints the raw UMP words.

### Driver options

The LaunchAgent runs `um1x-midi-driver run --log ~/Library/Logs/um1x-midi-driver.log`.
Options can be added to the `ProgramArguments` in the plist:

| Option | Effect |
|---|---|
| `--filter-clock` | drop clock/start/stop/continue coming from the instrument (some keyboards send clock all the time) |
| `--filter-sensing` | drop active sensing from the instrument |
| `--bulk` | read input with bulk transfers (alt setting 0) instead of the interrupt endpoint |
| `-v` | log more detail |

`kill -HUP` makes the running driver close and reopen every device.

## How it works

The UM-1X has three vendor-specific USB interfaces; only interface 2 is used,
with a bulk OUT endpoint (0x01) and, in alternate setting 1, an interrupt IN
endpoint (0x81). Despite the vendor class, the data are ordinary USB-MIDI 1.0
event packets: 4 bytes each, cable number + Code Index Number in the first
byte, then up to three MIDI bytes. This is what the Linux kernel's
`snd-usb-audio` does for the device (quirk `QUIRK_MIDI_FIXED_ENDPOINT` in
`sound/usb/quirks-table.h`), including choosing alternate setting 1 with the
interrupt endpoint on Roland devices ("light load" mode in `midi.c`).

```text
instrument ─DIN─ UM-1X ─USB─ libusb ─ driver ─ CoreMIDI source "UM-1X"      ─ Logic
                                            └─ CoreMIDI destination "UM-1X" ─┘
```

- **Device → Mac:** four asynchronous IN transfers stay queued on the
  interrupt endpoint. Each completed transfer is split into packets (empty
  and padding packets skipped), the MIDI bytes go through a MIDI 1.0 stream
  parser (running status, real-time bytes inside SysEx, SysEx cut short by
  another status byte), and the messages are handed to CoreMIDI as UMP with
  the arrival time stamp, right in the completion callback.
- **Mac → device:** the destination receives UMP (MIDI 1.0 protocol). It asks
  CoreMIDI for scheduled events 2 ms early and keeps them in a time-ordered
  queue; a high-priority thread encodes them into USB-MIDI packets when they
  are due and writes them in 64-byte transfers. The interface NAKs while its
  31.25 kbit/s output buffer is full, which paces long SysEx dumps by itself.
- **Hot-plug and power:** libusb hot-plug events, a rescan now and then, and
  macOS sleep/wake notifications (the device is closed before sleep and
  reopened after wake). Port names and CoreMIDI unique IDs are fixed, so apps
  recognise the port after a reconnect or a driver restart.

Source layout: `src/usbmidi.c` (packet/byte/UMP conversion, unit-tested in
`tests/`), `src/driver.c` (USB, CoreMIDI, scheduling, hot-plug, power),
`src/devices.c` (device table), `src/tools.c` (diagnostic commands),
`scripts/` (install/uninstall).

## Troubleshooting

- **No "UM-1X" port.** Run `um1x-midi-driver list`. If the USB device is not
  listed, macOS does not see it: try another cable, port or hub. If it is
  listed but there are no ports, look at the log:
  `tail -f ~/Library/Logs/um1x-midi-driver.log`.
- **"cannot claim interface … another program or driver is using it".** Some
  other program holds the device (an old driver, another copy of this one
  started by hand). Only one program can own it.
- **Is the driver running?** `launchctl print gui/$(id -u)/io.github.mihasm.um1x-midi-driver | grep state`.
  Restart it with `launchctl kickstart -k gui/$(id -u)/io.github.mihasm.um1x-midi-driver`.
- **Nothing arrives when playing.** `um1x-midi-driver monitor -a`. If clock
  or active sensing shows up but no notes, the cables are fine and the
  instrument's MIDI settings (transmit channel, Local Control) are the place
  to look. If nothing at all shows up, swap the IN/OUT plugs.
- **The instrument does not sound.** `um1x-midi-driver test-out`. Check that
  the UM-1X's OUT plug is in the instrument's MIDI IN and that the instrument
  receives on channel 1.
- **Logic syncs to the keyboard's clock / shows MIDI activity all the time.**
  Some instruments send clock continuously. Add `--filter-clock` to the plist
  (see Driver options) and reload: `launchctl bootout gui/$(id -u)/io.github.mihasm.um1x-midi-driver; launchctl bootstrap gui/$(id -u) ~/Library/LaunchAgents/io.github.mihasm.um1x-midi-driver.plist`.
- **Run it by hand for debugging:** stop the agent (`launchctl bootout …`),
  then `um1x-midi-driver -v`.

## Credits

- The Linux kernel's `snd-usb-audio` (`sound/usb/quirks-table.h`, `midi.c`)
  for the device facts: USB IDs, interface numbers, cable layouts and the
  Roland alternate-setting behaviour. No kernel code is used.
- [giulioz/um-one-macos](https://github.com/giulioz/um-one-macos), a libusb +
  PortMidi driver for the Roland UM-ONE, showed that this approach works on
  macOS. This project is a separate implementation (CoreMIDI instead of
  PortMidi) and contains no code from it; um-one-macos is GPL-3.0.
- [libusb](https://libusb.info) (LGPL-2.1), linked statically.

## License

MIT, see [LICENSE](LICENSE). Release binaries contain libusb, which is
LGPL-2.1: its licence ships with the release
([THIRD-PARTY-NOTICES.md](packaging/THIRD-PARTY-NOTICES.md)), and since the full
source and build scripts of this project are here, you can rebuild the binary
against a modified libusb with `make LIBUSB_PREFIX=/path/to/your/libusb`.

Roland, Edirol and BOSS are trademarks of Roland Corporation. This project is
not affiliated with Roland.
