# Third-party notices

## libusb

The `um1x-midi-driver` binary contains libusb 1.0.30, linked statically.

- Project: https://libusb.info — source: https://github.com/libusb/libusb
- Licence: GNU Lesser General Public License, version 2.1 or later. The full
  text is in `LICENSE.libusb` in the release archive (libusb's `COPYING`).
- The exact libusb source used is the release tarball
  `libusb-1.0.30.tar.bz2` (SHA-256
  `fea36f34f9156400209595e300840767ab1a385ede1dc7ee893015aea9c6dbaf`),
  unmodified.
- To relink with a modified libusb: get this project's source
  (https://github.com/mihasm/um1x-macos), build your libusb as a static
  library and run `make LIBUSB_PREFIX=/path/to/its/prefix`.
