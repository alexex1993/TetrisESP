# Tetris — Waveshare ESP32-C6-Touch-LCD-1.47

Guideline Tetris on the 1.47" 172×320 JD9853 panel, played entirely on the
AXS5106L touch screen, at a measured **60 fps**.

```sh
pio run -t upload -t monitor      # close the monitor before re-uploading
tools/host-test/run.sh            # the rules, tested on the host
```

## Controls

The panel is the only input on this board, so the scheme is the one mobile
Tetris players already know.

| Gesture | Action |
|---|---|
| drag left / right | move one column per 12 px of travel |
| tap the left half | rotate counter-clockwise |
| tap the right half | rotate clockwise |
| drag down | soft drop, one row per 12 px |
| flick down | hard drop — the piece streaks down over ~4 frames |
| flick up | hold |
| tap the status strip | pause |

A flick is told from a drag by **speed**, not distance, so a hard drop fires on
the frame the finger moves fast rather than after a threshold a slow soft drop
would also cross on its way down.

## Rules

Implemented from the Tetris Guideline as documented on tetris.wiki:

- 10×20 matrix with a four-row buffer above it,
- **Super Rotation System** with the standard wall-kick tables — one set for
  J/L/S/T/Z, its own set for I, no kicks for O,
- **7-bag Random Generator**, one-piece hold, ghost piece,
- **0.5 s lock delay** with move reset, capped at 15 resets (extended
  placement lock down),
- the **Tetris Worlds speed curve**, `Time = (0.8 − (L−1)·0.007)^(L−1)` seconds
  per row, tabulated in microseconds so no float enters the frame loop; level
  20 is 36.6 G and the table is clamped there,
- fixed goal: ten lines per level,
- top-out on block out or lock out; the high score lives in NVS.

### Scoring is the original table, not the guideline's

Points come **only from clearing lines**, and a tetris is worth thirty singles:

| Lines | Points |
|---|---|
| single | 40 × level |
| double | 100 × level |
| triple | 300 × level |
| **tetris** | **1200 × level** |

Nothing else pays: no points per dropped row, no combo, no back-to-back, no
perfect-clear bonus. The guideline's system charges 2 points for every
hard-dropped row, which lets a player who just slams pieces down run the score
up without clearing anything — 20 pieces to the floor is 800 points for an
empty board. Here that is worth zero, and `tools/host-test/run.sh` asserts it.

T-spins and perfect clears are still detected, and the banner over the matrix
names them, but they are recognition only and score as ordinary line clears.

## Animation

Two things are animated rather than instant, and both are driven from the game
state so the renderer's diff picks them up for free:

- **Hard drop** puts the piece into `TS_DROPPING` and walks it down one row
  every 4.5 ms — about 4 frames for the height of the matrix — leaving a
  one-frame streak of dark cells in the rows it just crossed. The piece is
  committed while it travels: move, rotate and soft drop all refuse.
- **A cleared row** flashes white for 60 ms and then burns away from the middle
  outwards, one cell each side per step, five steps over 180 ms, before the
  stack collapses onto it.

## How 60 fps is possible here

The board puts SCLK on GPIO1, which has no FSPI IO-MUX function, so the pixel
clock is stuck at 40 MHz through the GPIO matrix. A full-screen flush is
110,080 bytes — **22 ms on the wire**, which caps full redraws at about 45 fps
no matter how cheap the drawing is.

So nothing redraws the whole screen during play. `render.c` keeps a 10×20
shadow of what the panel is currently showing and pushes only the cells that
differ, merged into one horizontal span per row. Measured on the board:

| | |
|---|---|
| one 14×14 cell | 392 B → **78 µs** of wire time |
| one `esp_lcd_panel_draw_bitmap()` | about **450 µs** of address commands and DMA round trip |
| a piece moving one row | 8–16 cells over 4–6 rows → **3–5 ms** |
| the worst gameplay frame measured | **15 ms** of a 16.7 ms budget |
| share of frames under 12 ms | **97.5 %** |

Two consequences fall out of those numbers, and both are handled explicitly:

- **A whole-matrix repaint cannot fit in a frame.** 200 cells is 15.7 ms of
  pure wire time. A line-clear collapse and a hard drop's streak both ask for
  more than that, so the renderer spends a fixed budget per frame
  (`REDRAW_BUDGET`, in cell-equivalents, charging every `draw_bitmap` about six
  cells for its address commands) and leaves the rest dirty for the next frame,
  resuming where it stopped so no row can starve. The budget was picked by
  measurement: at 130 the tail reached 19 ms and the loop lost a frame here and
  there, at 105 the worst frame is 15 ms and the rate never leaves 60.
- **A full-screen repaint still costs 22 ms**, so the title screen and the
  first frame of a game each take one long frame (31 ms and 32 ms measured).
  Those are screen changes, not gameplay.

The frame rate is on screen, top right, and on the console once a second:

```
I (17098) tetris: 60 fps, frame work avg 5419 us, max 13004 us
```

Those figures come from an autoplay build that hard-drops a piece roughly three
times a second and forces a tetris every two seconds — considerably harder on
the renderer than a person is.

## Layout

```
 0                                        171
 0  ┌─────────────────────────────────────┐
    │ SCORE          HOLD      NEXT       │  37 px status strip
    │ 12480          ┌──┐      ┌──┐       │
    │ LV03 LN026     └──┘      └──┘   60  │
 38 ├───────────────┬─────────┬───────────┤
    │               │ 10 × 20 │           │  14 px cells,
    │               │  matrix │           │  140 × 280
319 └───────────────┴─────────┴───────────┘
```

## Files

| File | What is in it |
|---|---|
| `src/tetris.c` | the rules. Plain C, no ESP-IDF, no display — this is what the host test exercises |
| `src/render.c` | the shadow-buffer diff, the matrix, the HUD, the overlays |
| `src/input.c` | touch samples → game actions |
| `src/gfx.c` | SPI/JD9853 bring-up, backlight, the banded tile renderer |
| `src/main.c` | the 60 Hz loop, the screen state machine, NVS |
| `include/board_pins.h` | the board's pin map |
| `components/jd9853/`, `components/axs5106l/` | vendored panel and touch drivers |
| `tools/host-test/` | `run.sh` builds `src/tetris.c` for the host and runs the rule tests |

`components/`, `include/board_pins.h`, `include/font5x7.h` and the panel
bring-up in `src/gfx.c` come from the `esp32c6-touch-lcd147` skill's template.
`include/font5x7.h` is the Adafruit GFX 5×7 font, BSD licensed.

## Board notes worth not relearning

- The panel is **JD9853**, not ST7789, and the pin map shares nothing with the
  non-touch ESP32-C6-LCD-1.47 of almost the same name.
- `esp_lcd_panel_set_gap(panel, 34, 0)` — the visible 172 columns sit at column
  34 of the controller's 240-column RAM.
- `esp_lcd_panel_draw_bitmap()` is asynchronous; `gfx.c` takes a semaphore given
  from `on_color_trans_done` after every push, or bands overwrite each other.
- The backlight pin has a reset pull-up strong enough to switch the transistor
  on, so `gfx_init()` claims it at 0 % duty and `main` raises it to 90 % only
  after the first full flush.
- The TF card's CS is parked high before the SPI bus opens — it shares SCLK and
  MOSI with the panel.
- `CONFIG_FREERTOS_HZ=1000`: at the default 100 Hz tick the shortest sleep is
  10 ms, which cannot pace a 16.7 ms frame.
