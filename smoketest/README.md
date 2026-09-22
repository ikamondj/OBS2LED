# Matrix scrolling-text smoke test

Standalone firmware for the Seengreat RGB Matrix HUB75 S3
(ESP32-S3-WROOM-1-N16R8) and one standard **64x32 HUB75 matrix**. It scrolls
**LED CONTROL WORKING** from right to left in green, repeating continuously.
Text is vertically centered, brightness is 25%, and the display starts without
OBS, Wi-Fi, or an open serial monitor.

Flashing this replaces the OBS receiver program on the board. A full flash erase
is not needed. You can flash the receiver back afterward using its own folder.

## Connect and flash

1. Close OBS and serial monitors so they release the COM port.
2. Connect the matrix to the board's HUB75 output (use the panel's **IN** connector
   for a ribbon cable), and connect the matrix's 5 V power. The board's lower
   **Power** USB-C connector is the dedicated matrix power input; the matrix
   needs its power connection as well as the HUB75 signals.
3. Connect the PC with a data-capable cable to the board's upper **USB-C**
   data/programming connector.
4. Enter download mode using the buttons marked on your board:
   - Hold **BOOT** down.
   - While holding BOOT, press and release **RST**.
   - Keep BOOT held for about a second, then release it.
   - Wait for Windows to show the COM port. Check **Device Manager > Ports
     (COM & LPT)**; the download-mode port can differ from the normal runtime port.
5. Open PowerShell in this `smoketest` folder and run:

   ```powershell
   .\flash.ps1
   ```

   To choose the board explicitly, replace `COM12` with its current port:

   ```powershell
   .\flash.ps1 -Port COM12
   ```

   You can also double-click `flash.bat`, or run `flash.bat COM12`. The batch file
   runs PowerShell with a process-only execution-policy bypass if script execution
   is otherwise blocked. PlatformIO is located automatically, including installs
   under `AppData\Roaming\Python\Python311\Scripts`, and installed if missing.
   The first build may download the compiler and libraries.
6. Wait for **Flash succeeded**. With **BOOT released**, tap **RST** once to start
   the smoke test. The text should begin scrolling within a few seconds and
   repeat about every nine seconds.

**RST is the reset button** used in these instructions; some documentation labels
the same reset function **EN**. A normal reset is just a quick press of RST with
BOOT released. Resetting does not erase or restore firmware. BOOT held during
reset enters the downloader instead of starting the installed program.

To compile without uploading or touching the board:

```powershell
.\flash.ps1 -BuildOnly
```

## If it does not work

- **Upload stays at Connecting:** repeat the BOOT/RST sequence, check the current
  COM number, and rerun with `-Port`. Ensure OBS and serial monitors are closed.
- **Upload succeeds but nothing starts:** release BOOT, then tap RST. The screen
  stays blank while the board is in download mode.
- **Program runs but the panel stays black:** check the matrix's 5 V power and
  the HUB75 connection to the panel input. An optional serial monitor at 115200
  baud prints a startup message or repeats a matrix-initialization error.
- **Repeated, scrambled, or partial rows:** this project assumes a conventional
  64x32, 1/16-scan panel. Other panel sizes, scan patterns, and driver chips need
  matching settings; the successful upload alone cannot establish compatibility.

The constants near the top of `src/main.cpp` control panel dimensions, brightness,
text size, and scroll speed. Larger `kScrollDelayMs` values make scrolling slower.

## Restore the OBS receiver

Close any serial monitor, use the same BOOT/RST sequence, then run from this folder:

```powershell
..\obs2led-hub75-s3\flash.ps1 -Port COM12
```

Replace `COM12` with the current download-mode port. After upload, release BOOT
and tap RST. Reopen OBS and refresh its USB device list to select the runtime port.

Hardware references: [Seengreat board pinout and connections](https://seengreat.com/wiki/214/rgb-matrix-hub75-s3)
and [Espressif ESP32-S3 boot-mode instructions](https://docs.espressif.com/projects/esptool/en/latest/esp32s3/advanced-topics/boot-mode-selection.html).
