# OBS2LED

A C++20 OBS Studio plugin that sends resized RGB frames to an independent
receiver over UDP or USB serial. Use **Tools > OBS2LED - Global Output** to follow
the OBS Preview across scene changes, or add its **video filter** to sources or scenes
under **Filters > Effect Filters > + > OBS2LED**. Audio is not processed, so the
filter does not appear in the **Audio/Video Filters** add menu or on audio-only
sources.

## Source filter

1. Choose **Socket (UDP)** and enter the receiver's numeric IPv4/IPv6 address and
   port, or choose **USB-C (USB serial)** and select a compatible connected device.
2. Output starts automatically when the settings are valid and OBS renders the
   source. Default output is **64x32, nearest neighbor, up to 30 FPS**. Port 9090
   is only a configurable default; no particular receiver implementation is required.
3. Change width, height, frame rate, or scaling as needed. Nearest neighbor,
   bilinear, bicubic, Lanczos, and area sampling are available. Scaling stretches
   the source to the requested dimensions; it does not crop or letterbox.
4. Use **Refresh devices / status** after connecting hardware or to see a transport
   error. Saved devices remain selectable while disconnected and are retried.

Output dimensions range from 1 to 512 per axis. Raw UDP additionally requires
`width * height * 3 <= 65507`. Both ends must agree on UDP dimensions. The IP field
starts empty, so adding a filter alone does not send traffic. Hostnames, URLs,
and scoped IPv6 addresses (`%interface`) are not accepted.

OBS keeps the source's original resolution and color space. The LED output is
8-bit sRGB, with HDR converted to SDR and transparency rendered against black.
Capture happens as OBS renders the source; hidden/inactive sources that are not
being rendered do not generate new frames. Disabling/removing the filter stops
its output. Filters nearer the source in the chain are included in the captured
image. Put OBS2LED last in the processing chain to include all desired effects.

## Global preview output

Open **Tools > OBS2LED - Global Output**, configure the destination, and check
**Enable global LED output**. This window has the same UDP/USB, device selection,
baud rate, resolution, frame rate, scaling, validation, and refresh controls as
the filter. Global output is off by default and does not need a filter on any scene.
Click **OK** to keep your settings and close the window; output continues running.
Uncheck **Enable global LED output** to stop it and release the USB port.

The global stream follows what the main Preview pane **would** show, even when
the pane is disabled or minimized. Normally this is the current scene, including
scene transitions. In Studio Mode it follows the **left Preview pane**, which can
differ from the live Program pane. It captures the full video canvas, including
scene filters and empty margins, without editor outlines, handles, or guides.

Destination settings and the Enable checkbox are saved **per scene collection**.
A new collection starts disabled. Returning to a saved, enabled collection resumes
its output; changing collections briefly suspends capture while scenes are loaded.
Existing source filters remain independent. Use one output at a time for a given
USB device; simultaneous streams to the same UDP receiver can interleave frames.

## USB-C and microcontrollers

USB-C describes the connector. This plugin communicates with **USB CDC serial
devices and supported USB-to-UART bridges**. Its device menu lists USB-backed
serial ports, rather than peripherals with unrelated interfaces such as disks,
keyboards, or cameras. A board must expose a serial interface and run compatible
receiver firmware. A USB cable alone does not make an arbitrary board a receiver.

| Platform | Discovery and transport |
| --- | --- |
| Windows | SetupAPI / Configuration Manager, USB COM ports, overlapped Win32 serial writes |
| macOS | IOKit USB serial discovery, `/dev/cu.*`, termios and `IOSSIOSPEED` |
| Linux | sysfs USB ancestry, stable `/dev/serial/by-id` paths when available, termios and poll |

Native USB CDC is recommended. Serial defaults to **2,000,000 baud, 8N1**, with
DTR asserted and hardware/software flow control disabled. Native CDC firmware
often treats baud as informational; an actual UART bridge must support the
selected rate. Opening some boards' serial ports resets them. Close serial
monitors before selecting a port. On Linux, grant your user access to the device
through your distribution's serial-device group or udev rules; the plugin does
not change permissions. Sandboxed OBS installations also need device access.

At 64x32, RGB payload is 6,144 bytes/frame: **184,320 bytes/s at 30 FPS** or
368,640 bytes/s at 60 FPS. Including the USB header and 8N1 overhead, a UART needs
about **1.85 Mbaud at 30 FPS**. A 115200-baud UART is limited to roughly 1.9 FPS.
Slow devices drop intermediate frames rather than accumulating a video backlog.

The [wire protocol](docs/protocol.md) defines both transports. A portable,
allocation-free USB parser and Arduino-style integration sketch are in
[examples/usb-receiver](examples/usb-receiver). Connect the sketch's
`display_frame()` callback to the matrix driver for your board; HUB75 pins,
DMA, panel scanning, and wiring are hardware-specific. Increase the example's
receive buffer if you select a resolution larger than 64x32.

The implementation follows the native interfaces documented by
[Microsoft](https://learn.microsoft.com/en-us/windows-hardware/drivers/usbcon/usb-driver-installation-based-on-compatible-ids),
[Apple](https://developer.apple.com/documentation/iokit/communicating_with_a_modem_on_a_serial_port),
and the [Linux kernel](https://docs.kernel.org/usb/acm.html).

## Build

Requirements: CMake 3.28+, a C++20 compiler, and a configured and built OBS Studio
worktree. Keep this repository at **`obs-studio/plugins/OBS2LED`** and build OBS's
`libobs` and `obs-frontend-api` targets first, using the same architecture and
configuration you will use for OBS2LED.
The Tools menu uses the OBS frontend API and OBS's native properties window;
no separate Qt development SDK, GStreamer, libusb, or third-party serial library
is needed.
See [OBS build instructions](https://github.com/obsproject/obs-studio/wiki/Build-Instructions)
for preparing the worktree and its dependencies.

From this folder:

```powershell
cmake --preset default
cmake --build --preset default
ctest --test-dir build -C RelWithDebInfo --output-on-failure
cmake --install build --config RelWithDebInfo
```

The default preset uses your platform's CMake generator and writes to this
repository's ignored **`build/`** directory, with staging in **`build/stage/`**.
It looks for OBS's CMake exports in the surrounding worktree's platform build
directory (`build_x64`, `build_arm64`, `build_macos`, or `build_ubuntu`), then
`build`. It reuses that build's headers, libraries, dependency prefixes, and
CMake find modules; no separately installed OBS SDK is needed.

For an OBS build in another directory, select it explicitly:

```powershell
cmake --preset default -DOBS2LED_OBS_BUILD_DIR=C:/path/to/obs-studio/build
```

The selected OBS build directory is cached. Use a 64-bit toolchain; on Windows
add `-A x64` or `-A ARM64` on the first configure as appropriate. Build OBS's
libraries in `RelWithDebInfo` for the default build preset, or use
`cmake --build build --config Release` if OBS was built in `Release`.
Machine-specific settings can go in an untracked `CMakeUserPresets.json`.
Other plugin build directories also work with `cmake -S . -B <directory>`;
only building directly into the source root is rejected.

To build OBS2LED as part of OBS itself, add this to OBS's
`plugins/CMakeLists.txt`:

```cmake
add_subdirectory(OBS2LED)
```

Then reconfigure OBS normally and build its `obs2led` target, for example
`cmake --build ../../build --target obs2led --config RelWithDebInfo` from this
folder. This mode links directly to OBS's targets and uses OBS's plugin staging
and installation helpers, including locale data. OBS's parent build controls
`BUILD_TESTING`; OBS2LED does not enable tests globally when added as a subdirectory.

Standalone SDK builds remain supported when no worktree build is found. To
explicitly use an installed SDK instead:

```powershell
cmake --preset default -DOBS2LED_USE_OBS_WORKTREE=OFF "-DCMAKE_PREFIX_PATH=C:/SDK/obs;C:/SDK/obs-deps"
```

The SDK must provide `libobsConfig.cmake`, `obs-frontend-apiConfig.cmake`,
headers, libraries, and dependencies. The regular OBS installer does not supply it.

## Install

For the standalone plugin build on Windows, close OBS and copy the entire
`build/stage/obs2led` folder into
`C:\ProgramData\obs-studio\plugins\`. Restart OBS. The staged directory contains:

```text
obs2led/
  bin/64bit/obs2led.dll
  data/locale/en-US.ini
```

If you install directly into the OBS application directory instead, copy the DLL
to `obs-plugins/64bit/obs2led.dll` and the contents of this project's `data/`
directory to `data/obs-plugins/obs2led/`. Use one installation layout to avoid
duplicate plugin copies. Copying only the DLL leaves the interface labels missing.

If OBS2LED is loaded but you cannot find it, select a video source or scene and
use the **+** under **Effect Filters**. Older builds with missing locale data
appear as **Filter.Name**. Check **Help > Log Files > View Current Log** for
`Failed to load 'en-US' text for module: 'obs2led.dll'`; installing the data folder
in the matching location and restarting OBS restores the labels. The plugin now
keeps the **OBS2LED** menu name even when locale data is missing.

On macOS, staging produces `obs2led.plugin`; copy it into
`~/Library/Application Support/obs-studio/plugins/`. On Linux, installation uses
the platform's GNU library directory under `lib/obs-plugins` and the data path
`share/obs/obs-plugins/obs2led` under the selected prefix. Choose a prefix used by
your OBS installation. See [OBS's plugin guide](https://obsproject.com/kb/plugins-guide).

## Implementation and verification

- `src/video-filter.cpp`: properties, GPU resampling, double-buffered readback,
  HDR conversion, and shared capture/settings for filters and global output.
- `src/global-output.cpp`: Tools menu, Preview/scene tracking, scene-collection
  settings persistence, and shutdown cleanup.
- `src/output.*`: validation, RGB packing, USB framing, and one background sender
  per filter instance. Only the newest pending frame is retained. Settings changes
  discard queued frames from the previous destination. Failed endpoints retry
  once per second; serial writes support cancellation and a two-second deadline.
- `src/udp.cpp`, `src/serial-*.cpp`, `src/devices-*.cpp`: native OS transports.
- `tests/output-tests.cpp`: validation, padded GPU rows, CRC/firmware decoder,
  UDP loopback, destination changes, invalid settings, and lifecycle checks.
  Linux additionally checks raw serial bytes/disconnects through a pseudo-terminal.

The transport tests can run without the OBS SDK:

```sh
cmake -S . -B build-transport-tests -DOBS2LED_BUILD_PLUGIN=OFF
cmake --build build-transport-tests --config RelWithDebInfo
ctest --test-dir build-transport-tests -C RelWithDebInfo --output-on-failure
```

The included GitHub Actions workflow runs these tests on Windows, macOS, and
Linux when pushed. The Windows GPU integration test is optional. Build OBS's
`libobs-d3d11` target in the same configuration first, and adjust the rundir path
below if your OBS build directory differs:

```powershell
cmake --preset default -DOBS2LED_BUILD_GPU_TEST=ON
cmake --build --preset default
$obsRunDir = (Resolve-Path ../../build/rundir/RelWithDebInfo).Path
$env:PATH = "$obsRunDir/bin/64bit;" + $env:PATH
./build/RelWithDebInfo/obs2led-filter-smoke.exe ./build/RelWithDebInfo/obs2led.dll ./data "$obsRunDir/data/libobs/" "$obsRunDir/bin/64bit/libobs-d3d11.dll"
```

It starts a separate libobs instance and verifies filter registration and its
menu name, real GPU pixel values through UDP, all five scaling modes, output
cadence, resizing, properties, and stopping or switching transports. The global
output checks use simulated frontend events with real GPU rendering and UDP:
scene switching, Studio Preview selection with no preview display, canvas margins,
saved collection settings, enable/disable, and cleanup. This tests the same
frontend controller used by the plugin without changing your OBS configuration;
it does not automate the actual OBS dialog.
Physical USB throughput and panel output still need verification with your board.
