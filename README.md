# ZOYI ZT703S desktop app

A desktop front end for the ZOYI ZT703S scopemeter running
[OpenOSC](https://github.com/bve/OpenOSC) firmware. It talks to the meter over
USB using OpenOSC's SCPI commands and gives you a big, live oscilloscope view
on your computer, plus panels for the multimeter and the signal generator.

It runs on Windows, macOS and Linux.

## What it does

- Live waveforms from both channels, drawn with sin(x)/x interpolation or as
  plain lines, with optional persistence and an XY mode.
- An overview strip under the plot showing the whole capture buffer, with a box
  marking what is on screen. Drag or scroll to move around in the record.
- Timebase, V/div, offset and trigger controls that change the meter directly,
  plus Run/Stop, Single, Force and Auto.
- Roll mode at slow timebases.
- A measurement bar (frequency, period, Vpp, RMS and so on) and time/voltage
  cursors.
- Multimeter and signal generator pages that switch in when the meter is in
  DMM or GEN mode.
- Screenshots of the meter's own display (Ctrl+P), saved and copied to the
  clipboard.
- CSV export of the frame on screen (Ctrl+S) or of the whole record.
- A "clean view" (F11) that shows just the plot, handy for screen recording.

Keyboard: Space starts or stops, A arms a single capture, C centres the
trigger point.

## Requirements

- A ZOYI ZT703S with OpenOSC firmware. The fast transfer commands it uses
  (`WAV:FRAM?`, `WAV:ENV?`, `WAV:MEAS?`) are in OpenOSC 0.21.9 and later.
- USB data needs the meter's high-speed clock profile. If the app connects but
  sees no data, check the clock setting on the meter.
- The ZT706S runs the same firmware and may work as well, but only the
  ZT703S has been tested.

## Download

Ready-made builds for Windows and macOS are on the
[Releases](../../releases) page.

- **Windows:** unzip and run `zt703s.exe`. It is a single static executable,
  nothing to install.
- **macOS:** unzip and move `ZT703S.app` to Applications. The app is not
  signed with a paid Apple developer ID, so the first time you have to
  right-click it, choose Open, and confirm.

## Using it

Plug the meter in over USB, start the app and pick the meter's serial port
from the toolbar (it shows up as a COM port on Windows, `/dev/cu.usbmodem...`
on macOS and `/dev/ttyACM...` on Linux). With Auto-reconnect on, the app finds
the meter again after a mode change or an unplug.

You can also pass the port on the command line to connect straight away:

    zt703s COM5
    zt703s -v /dev/ttyACM0      (-v also prints the SCPI traffic)

## Building

The app is plain C++17 and Qt 6 (Widgets and SerialPort), built with make.

**Windows** (MSYS2, static build): install [MSYS2](https://www.msys2.org/),
run `setup_msys2.sh` once from a UCRT64 shell, then `make`. The result is
`build/win/zt703s.exe`. `build.sh` does the same when called from PowerShell.

**macOS** (Homebrew):

    brew install qt
    make -f Makefile.mac zip

**Linux** (Debian/Ubuntu package names):

    sudo apt install qt6-base-dev libqt6serialport6-dev
    make -f Makefile.linux

## Credits

Made by a few friends who wanted a bigger screen for their scopemeter.
OpenOSC, the firmware that makes all of this possible, is by
[bve](https://github.com/bve/OpenOSC).

## License

MIT, see [LICENSE](LICENSE).
