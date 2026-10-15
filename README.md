# CrowPanel MP3 Player

**An embedded touchscreen music-player project for the 7-inch CrowPanel ESP32-S3 display.**

This repository collects hardware documentation, reference firmware, examples, and development resources for building an MP3 player interface on the CrowPanel platform. It provides a starting point for experimenting with embedded audio playback and graphical interfaces using the ESP32-S3.

> **Project status:** The repository currently includes CrowPanel board support materials and factory examples. The MP3 playback pipeline, storage interface, audio hardware, and player controls should be documented once their implementation is confirmed.

![7-inch CrowPanel ESP32 display](7.0_hmi_esp32_display.png)

## Hardware platform

| Component | Specification |
| --- | --- |
| Board | CrowPanel 7.0-inch ESP32 display |
| MCU | ESP32-S3-WROOM-1-N4R8 |
| Flash | 4 MB |
| PSRAM | 8 MB |
| Display | 7-inch TFT LCD, 800 × 480 |
| Touch | Capacitive touchscreen |
| Display drivers | EK9716BD3 / EK73002ACGB |
| Power | External 5 V / 2 A |
| Exposed interfaces | UART, GPIO, I²C, battery connector |
| Buttons | BOOT and RESET |

The source README lists **LVGL 8.3.3** as a graphics dependency. Display examples also use LovyanGFX-style RGB panel configuration and GT911 touch pins.

## Repository layout

```text
CrowPanel-MP3-Player/
├── 3D file/               # Mechanical/CAD resources
├── Datasheet/             # Component and board datasheets
├── Eagle_SCH&PCB/         # EAGLE schematic and PCB files
├── example/               # Reference projects and examples
├── factory_firmare/       # Factory firmware assets (repository spelling)
├── factory_sourecode/     # Factory source code (repository spelling)
├── 7.0_hmi_esp32_display.png
├── arduino1.jpg
├── arduino2.jpg
├── platformIO.jpg
├── thonny.jpg
├── thonny2.jpg
└── readme.md
```

> Folder names above preserve the spellings currently used in the repository, including `factory_firmare` and `factory_sourecode`.

## Getting started

### 1. Clone the repository

```bash
git clone https://github.com/johntsaiainow/CrowPanel-MP3-Player.git
cd CrowPanel-MP3-Player
```

### 2. Identify your board revision

The supplied board documentation describes three revisions:

| Revision | Notes |
| --- | --- |
| V1.0 | Earlier hardware/software revision |
| V2.0 | Adds automatic download support |
| V3.0 | Adds touch initialization timing requirements |

Use examples and configuration appropriate for your specific board. The V3.0 touch sequence requires additional timing and GPIO-expander setup; consult the included board examples before flashing custom firmware.

### 3. Choose a development environment

The repository includes examples and instructions for several toolchains. **Use the build system required by the specific example you choose**; they are alternatives, not interchangeable commands.

**PlatformIO:** Open an example containing `platformio.ini` in a PlatformIO-enabled editor, configure the ESP32-S3 board and serial port, then build and upload. From a compatible example directory:

```bash
pio run
pio run -t upload
pio device monitor
```

**ESP-IDF:** For an ESP-IDF example containing `CMakeLists.txt`:

```bash
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

Replace `/dev/ttyACM0` with the serial device actually detected on your Linux host (it may be `/dev/ttyUSB0` or another path).

**Arduino IDE:** Install the ESP32 board support package and libraries required by the selected Arduino example. Open the example sketch, select the matching ESP32-S3 board settings and port, then compile and upload.

**MicroPython:** Use a compatible MicroPython firmware and the provided MicroPython examples, if applicable. This workflow is separate from the C/C++ firmware build.

> **Important:** Not every folder is a complete MP3 player application. Inspect each example's dependencies and board settings before building or flashing.

## Display and touch configuration

The published board reference uses an **800 × 480 RGB display** and identifies the following GT911 touch connections:

| Signal | ESP32-S3 GPIO |
| --- | --- |
| GT911 SCL | GPIO 20 |
| GT911 SDA | GPIO 19 |
| RGB HSYNC | GPIO 39 |
| RGB VSYNC | GPIO 40 |
| RGB DE | GPIO 41 |
| RGB PCLK | GPIO 0 |

The remaining RGB data pins and timing values are documented in the original board examples. Avoid copying pin mappings from a different CrowPanel size or revision.

For V3.0 hardware, the original reference includes a PCA9557 GPIO-expander initialization sequence to manage touch startup timing. Preserve that behavior when porting the display driver.

## MP3 player implementation

A complete player requires the following subsystems. **These are implementation areas, not a claim that every feature is already available in this repository.**

| Subsystem | Responsibility | Status |
| --- | --- | --- |
| User interface | Touchscreen player controls and status | Verify in application source |
| Media source | Read MP3 files from the selected storage medium | To document |
| Audio decoding | Decode compressed MP3 frames | To document |
| Audio output | Deliver decoded audio to a supported DAC/codec/amplifier | To document |
| Playback state | Play, pause, skip, position, and error handling | To document |
| Metadata | Track title, artist, duration, and artwork, if supported | To document |

**Suggested architecture:**

```text
       CrowPanel 7-inch Touchscreen
                   |
              LVGL Interface
                   |
            Player Controller
              /          \
       Media Reader    Playback State
              |
          MP3 Decoder
              |
       Audio Output Driver
              |
       DAC / Codec / Amplifier
```

The audio interface, storage pins, codec selection, and decoder library must be validated against the actual hardware and firmware before documenting wiring or build flags.

## Development notes

- Keep display initialization and touchscreen configuration specific to the board revision.
- Account for ESP32-S3 RAM and PSRAM constraints when allocating framebuffers and audio buffers.
- Keep audio processing independent from UI refresh where possible to avoid playback interruptions.
- Validate power requirements before connecting external audio peripherals.
- Test flashing and serial monitoring on the target hardware before changing factory firmware.

## Troubleshooting

| Symptom | Checks |
| --- | --- |
| Display remains blank | Confirm power, board revision, RGB pin mapping, panel timing, and backlight setup |
| Touch is unresponsive | Confirm GT911 wiring, I²C initialization, and V3.0 touch timing sequence |
| Upload fails | Confirm USB cable, serial port, permissions, boot mode, and target configuration |
| Firmware resets | Inspect serial logs, power stability, stack usage, and memory allocation |
| No audio | Verify that a complete decoder, media reader, and physical audio-output path are implemented |

On Linux, identify serial devices with:

```bash
ls /dev/ttyACM* /dev/ttyUSB* 2>/dev/null
```

## Documentation and references

- [Repository examples](example/)
- [Hardware datasheets](Datasheet/)
- [EAGLE schematics and PCB files](Eagle_SCH&PCB/)
- [Factory source code](factory_sourecode/)
- [Factory firmware](factory_firmare/)
- [LVGL documentation](https://docs.lvgl.io/8.3/)
- [ESP-IDF documentation](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/)

## Contributing

Issues and pull requests are welcome, especially for reproducible build instructions, verified hardware pinouts, audio-driver integration, and screenshots of working player functionality.

## License

No project license is specified here. Consult the repository and individual third-party components for their applicable licensing terms before redistribution.
