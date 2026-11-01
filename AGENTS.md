# Repository Guidelines

## Project Structure & Module Organization

This is C firmware for the Waveshare ESP32-C6-Touch-LCD-1.47, built with PlatformIO and ESP-IDF. `src/tetris.c` contains the hardware-independent game rules; `src/main.c`, `input.c`, `render.c`, and `gfx.c` handle the game loop, touch, rendering, and display setup. Public headers and the board pin map live in `include/`. Vendored display and touch drivers are in `components/jd9853/` and `components/axs5106l/`. The embedded font is in `include/font5x7.h`; host tests are in `tools/host-test/`.

## Build, Test, and Development Commands

- `pio run` builds the firmware for the configured ESP32-C6 environment.
- `pio run -t upload -t monitor` flashes the connected board and opens the serial monitor; close the monitor before uploading again.
- `tools/host-test/run.sh` compiles the game rules with a host C compiler and runs the tests without hardware.

`platformio.ini` defaults to macOS `/dev/cu.usbmodem*` ports; adjust the upload and monitor ports for your system. The serial monitor runs at 115200 baud.

## Coding Style & Naming Conventions

Follow the surrounding C code: four-space indentation, braces on their own line for function definitions, and snake_case for functions and variables. Use uppercase for constants and macros, and `_t` for existing typedef patterns. Keep game logic in `tetris.c` independent of ESP-IDF and display code so it remains host-testable. No repository-wide formatter or linter is configured; the host test build enables compiler warnings.

## Testing Guidelines

Add rule regressions as descriptive `test_*` functions in `tools/host-test/test_tetris.c`, and register them in its test runner. Run the host script after changing game logic. For display, touch, timing, or board changes, also build the firmware and verify behavior on the target hardware. No coverage threshold is specified.

## Commit & Pull Request Guidelines

The available history contains only `init commit`, so it does not establish a detailed commit convention. Use short, descriptive commit subjects. Pull requests should describe the change and its motivation, link relevant issues, and report the host, build, and hardware checks performed. Include a photo or video for visible display or interaction changes when useful.

## Board Configuration

This project targets the touch board with a JD9853 panel, not the similarly named non-touch model. Preserve its pin map and shared display/TF SPI bus setup; consult the board notes in `README.md` before changing initialization or rendering timing.
