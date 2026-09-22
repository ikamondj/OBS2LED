# OBS2LED firmware for Seengreat RGB Matrix HUB75 S3

This project receives the existing OBS2LED `O2LF` USB frame protocol over the
ESP32-S3's native USB CDC COM port and displays validated RGB888 frames on one
standard 64x32 HUB75 panel.

## Hardware assumptions

- Board: Seengreat RGB Matrix HUB75 S3, ESP32-S3-WROOM-1-N16R8.
- Matrix: one standard 64x32 HUB75 panel.
- PC USB cable goes to the upper USB-C port labeled `USB-C`.
- The lower USB-C port labeled `Power` is the dedicated matrix-power input.
- HUB75 pins used by this firmware:
  - R1=GPIO5, G1=GPIO4, B1=GPIO6
  - R2=GPIO15, G2=GPIO7, B2=GPIO17
  - A=GPIO8, B=GPIO18, C=GPIO10, D=GPIO9, E=GPIO16
  - LAT=GPIO11, OE=GPIO13, CLK=GPIO12

## Protocol

The receiver matches the packet format already produced by the supplied
desktop code:

- bytes 0..3: `O2LF`
- byte 4: version 1
- byte 5: flags 0
- bytes 6..7: 24-byte header size, little-endian
- bytes 8..9: width = 64
- bytes 10..11: height = 32
- bytes 12..15: sequence number
- bytes 16..19: payload size = 6144
- bytes 20..23: CRC32 of RGB payload
- bytes 24..: RGB888 pixels, row-major

Bad headers and bad CRCs are discarded. Rendering runs on the other ESP32-S3
core, and the one-element queue keeps the newest complete frame instead of
building latency if rendering briefly falls behind. HUB75 DMA double buffering
is enabled to avoid showing a half-drawn frame.

## Flash from Windows

1. Close OBS and any serial terminal.
2. Connect the PC to the board's upper data/programming USB-C port.
3. Open this folder.
4. Double-click `flash.bat`, or run:

   `.\flash.ps1`

PlatformIO is installed into your Windows user account automatically if needed.

If automatic COM detection fails, look in Device Manager and specify it:

`.\flash.ps1 -Port COM12`

or:

`flash.bat COM12`

For the first flash, if the uploader cannot connect:

1. Hold `BOOT`.
2. Press and release `RST`.
3. Release `BOOT`.
4. Run the flash command again.

After a successful first flash, pressing `RST` once may be required before the
runtime USB CDC COM port appears.

## Use with the OBS plugin

- Transport: USB
- Device: select the ESP32-S3 COM device after refreshing
- Width: 64
- Height: 32
- FPS: 30 initially; 60 can be tested later
- Baud: 2000000 is fine

The baud value is USB CDC line-coding metadata here, not a physical 2 Mbaud
UART bottleneck.

Do not keep a Serial Monitor open while OBS is using the same COM port.

## Panel variants

This is configured for a conventional 64x32 HUB75 panel. If the panel is an
unusual 1/4-scan, 1/8-scan, or uses a driver requiring special initialization,
the HUB75 library configuration may need a scan/driver change even though the
board wiring is correct.

