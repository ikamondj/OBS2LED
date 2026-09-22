# RGB receiver protocol

This protocol is independent of any existing receiver project. No codec or
GStreamer pipeline is required. Receivers own the mapping from logical RGB
pixels to panel wiring, serpentine order, color-channel permutations, and scanout.

## Pixels

- Row-major, top to bottom, left to right; no row padding.
- Three bytes per pixel, in **R, G, B** order, each 0..255 (sRGB).
- Pixel `(x, y)` starts at byte `3 * (y * width + x)`.
- Alpha is composited against black. HDR footage is converted to SDR for output.
- Resolution is the filter's selected width/height, independent of OBS's canvas.

## UDP

Each UDP datagram is exactly one `width * height * 3` byte RGB frame, with **no
application header**. At the default 64x32 size this is **6,144 bytes**. The
receiver must know the dimensions in advance and reject unexpected packet sizes.
Receive into a large enough buffer; a short buffer can truncate a datagram.
Port 9090 is a UI default only and may be changed to any receiver port.

There are no acknowledgments, sequence numbers, retransmissions, timestamps, or
frame reassembly at the application layer. A successful send does not establish
that the receiver is running. Display complete received frames and retain the
last good one (or implement a receiver-side blackout timeout).

Frames are limited to 65,507 bytes, the maximum ordinary IPv4 UDP payload. A
6,144-byte frame already exceeds a typical Ethernet MTU, so the network uses IP
fragmentation/reassembly. The sender permits OS fragmentation, including
explicitly configuring Linux's socket MTU behavior. The network and
microcontroller's IP stack must support it; loss of any fragment loses that
frame. This format suits a controlled LAN with a capable receiver. If a board
cannot reassemble these datagrams, lower the resolution or introduce an agreed
application-level chunk format on both ends in a later version. See
[Linux UDP behavior](https://man7.org/linux/man-pages/man7/udp.7.html).

## USB serial version 1

Serial delivers a byte stream, so USB frames add a **24-byte header**, followed
by unchanged RGB bytes. USB packet boundaries and individual reads/writes have
no relationship to frame boundaries. A receiver must handle partial reads and
multiple frames arriving together.

All integer fields are unsigned and **little-endian**:

| Offset | Bytes | Field |
| --- | --- | --- |
| 0 | 4 | ASCII magic `O2LF` (`4f 32 4c 46`) |
| 4 | 1 | Version: `1` |
| 5 | 1 | Flags: `0` |
| 6 | 2 | Header size: `24` |
| 8 | 2 | Width in pixels |
| 10 | 2 | Height in pixels |
| 12 | 4 | Frame sequence, wraps at 2^32 |
| 16 | 4 | RGB payload byte count: `width * height * 3` |
| 20 | 4 | CRC-32/ISO-HDLC of RGB payload only |
| 24 | payload size | Raw RGB pixels |

CRC uses reflected polynomial `0xEDB88320`, initial register `0xFFFFFFFF`, and
final XOR `0xFFFFFFFF`. The ASCII test vector `123456789` has CRC `0xCBF43926`.
Sequence counts attempted USB frames and can skip or restart after reconnection
or filter recreation. It is informational, not an acknowledgment protocol.

Scan for magic, validate the header and dimensions against your available memory,
collect the exact payload, verify CRC, and only then present the image. Discard
malformed/corrupt candidates and resume scanning. Reset a partial frame after
a suitable inter-byte timeout or USB disconnect. Never assume the stream starts
at a frame boundary: hosts and devices can reconnect or reset at any time.

The [portable decoder](../examples/usb-receiver/rgb-receiver.hpp) and
[Arduino-style example](../examples/usb-receiver/usb-receiver.ino) implement this
contract without dynamic allocation. The example has a 6,144-byte capacity and
a 2.5-second inter-byte timeout. It leaves panel-driver integration to the board
application. It does not require a particular MCU vendor or matrix driver.

Serial is 8N1, no flow control, DTR asserted. The default line rate is 2,000,000
baud, configurable for UART bridges. Native CDC devices may ignore line rate.
Do not mix debug logs, a REPL, or other protocols into the receiver's input
stream. Ensure other host applications have closed the selected serial port.

## Bandwidth and latency

| Default 64x32 output | 30 FPS | 60 FPS |
| --- | --- | --- |
| RGB payload | 184,320 B/s | 368,640 B/s |
| USB framed byte stream | 185,040 B/s | 370,080 B/s |
| UART 8N1 minimum line rate | 1,850,400 bit/s | 3,700,800 bit/s |

The two GPU staging surfaces add roughly one output-frame interval of latency.
The sender retains one pending frame plus the frame currently being sent. If
the link is slower than the selected FPS, intermediate frames are replaced by
newer ones. There is no unbounded queue, compression, retry of individual frames,
or intentional burst to catch up after a pause. USB failures close and reopen
the configured device after a one-second retry interval.
