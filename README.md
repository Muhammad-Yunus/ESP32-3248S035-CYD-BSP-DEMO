# ESP32-3248S035-CYD-BSP-DEMO

A peripheral demo for the **Sunton ESP32-3248S035** — the 3.5" 480×320
"**CYD**" (Cheap Yellow Display) board with **capacitive GT911 touch** and a
microSD slot — written as a native **ESP-IDF v6.1** project.

The application is a small two‑tab instrument built on **LVGL 9**:

| Tab | What it shows |
| --- | --- |
| **Monitoring & Control** | Live LDR light‑level chart (ADC), RGB LED colour sliders, and a DAC sine‑tone generator (frequency / volume sliders + on/off switch). |
| **File Explorer** | Browses the microSD card, opens `.txt` / `.json` / `.cfg` files in a scrollable viewer, and plays `.mp3` files with a modal music player (marquee title, elapsed/total time, progress slider, Play / Pause / Stop, volume slider). |

Everything the board can do is exposed through one reusable board‑support
package, and the demo app is a thin consumer of it.

---

## Repository layout

```
ESP32-3248S035-CYD-BSP-DEMO/
├── .gitignore
├── README.md                       ← this file
└── firmware/                       ← the ESP-IDF project
    ├── CMakeLists.txt              ← IDF project entry point (project(firmware))
    ├── sdkconfig                   ← committed, board-tuned configuration
    ├── dependencies.lock           ← pinned managed-component versions
    ├── components/
    │   └── bsp_esp323248s035/      ← the board support package (reusable)
    │       ├── include/bsp_esp323248s035.h   ← public API + board pin map
    │       ├── idf_component.yml             ← depends on lvgl/lvgl ^9.1
    │       ├── Kconfig                       ← board options (see below)
    │       └── src/
    │           ├── bsp.c           ← global bsp_init() orchestration
    │           ├── bsp_tft.c       ← ST7796 over SPI (esp_lcd)
    │           ├── bsp_touch.c     ← GT911 over I2C (raw register protocol)
    │           ├── bsp_led.c       ← on-board RGB LED (active low)
    │           ├── bsp_audio.c     ← DAC continuous: sine tone + PCM streaming
    │           ├── bsp_buzzer.c    ← alternative LEDC square-wave driver
    │           ├── bsp_ldr.c       ← light sensor via adc_oneshot polling
    │           ├── bsp_sd.c        ← microSD (SPI / FATFS)
    │           └── bsp_lvgl.c      ← LVGL 9 port (display, indev, tick, task)
    └── main/                       ← the application
        ├── CMakeLists.txt
        ├── idf_component.yml       ← depends on chmorgan/esp-libhelix-mp3
        ├── firmware.c              ← tabview, LDR chart, RGB, tone controls
        ├── file_explorer.c/.h      ← SD browser + text viewer
        └── mp3_player.c/.h         ← Helix MP3 decode + player overlay
```

`build/` and `managed_components/` are generated and **not** committed; the
Component Manager restores them from `dependencies.lock` + the two
`idf_component.yml` manifests.

---

## Pin map

All assignments are identical to the reference Arduino BSP
[`ardnew/ESP32-3248S035`](https://github.com/ardnew/ESP32-3248S035), which this
component is a direct C port of.

### Display — ST7796, SPI2 (HSPI)

| Signal | GPIO | Notes |
| --- | --- | --- |
| MISO | 12 | |
| MOSI | 13 | |
| SCLK | 14 | |
| CS | 15 | |
| DC | 2 | |
| BL | 27 | Backlight, LEDC PWM (0–100 %) |
| RST | — | No dedicated reset pin on this board; reset is done in software |

### Touch — GT911, I2C0

| Signal | GPIO | Notes |
| --- | --- | --- |
| SCL | 32 | |
| SDA | 33 | |
| INT | 21 | |
| RST | 25 | |
| Address | — | `0x5D` |

### microSD — SPI3 (VSPI)

| Signal | GPIO |
| --- | --- |
| MISO | 19 |
| MOSI | 23 |
| SCLK | 18 |
| CS | 5 |

### Other peripherals

| Peripheral | GPIO / ADC | Notes |
| --- | --- | --- |
| RGB LED — R | 4 | Active **low** |
| RGB LED — G | 16 | Active low |
| RGB LED — B | 17 | Active low |
| Amplifier / buzzer | 26 | DAC channel 1 → on-board FM8002A class-AB amp |
| LDR | ADC1 channel 6 (GPIO34) | Light sensor |

> GPIO34 is input-only and has no internal pull-up; it is used purely as the
> LDR ADC input.

---

## Software architecture

The project is split into **one reusable BSP component** and **one application**:

```
        main (application)                     bsp_esp323248s035 (component)
 ┌───────────────────────────────┐        ┌───────────────────────────────────┐
 │ firmware.c                    │        │ bsp.c  → bsp_init()               │
 │   • tabview (2 tabs)          │        │  ├─ bsp_tft_init()   (ST7796/SPI2)│
 │   • LDR chart + RGB + tone    │        │  ├─ bsp_touch_init() (GT911/I2C0) │
 │ file_explorer.c               │        │  ├─ bsp_led_init()   (RGB)        │
 │   • SD browser + text viewer  │        │  ├─ bsp_audio_init() (DAC/I2S0)   │
 │ mp3_player.c                  │        │  ├─ bsp_ldr_init()   (ADC one-shot)│
 │   • Helix decode + player UI  │        │  └─ bsp_lvgl_init()  (LVGL 9 port) │
 └──────────────┬────────────────┘        └─────────────────┬─────────────────┘
                │   public C API (bsp_esp323248s035.h)      │
                └───────────────────────────────────────────┘
```

### Concurrent tasks and core affinity

The classic ESP32 has two cores; heavy work is deliberately spread across them.

| Task | Core | Priority | Role |
| --- | --- | --- | --- |
| LVGL handler (`bsp_lvgl.c`) | 0 | 4 | Runs `lv_timer_handler()` every 20 ms |
| MP3 player (`mp3_player.c`) | 0 | 5 | Decodes MP3 and feeds the DAC FIFO |
| DAC audio (`bsp_audio.c`) | 1 | 6 | Generates tone / resamples PCM into the DAC DMA |
| LDR polling (`bsp_ldr.c`) | 1 | 3 | Averages ADC1 ch6 at 1 kHz |

LVGL is **not** thread-safe, so any task that wants to touch a widget (e.g. the
LDR task refreshing the chart) must take the LVGL mutex first via
`bsp_lvgl_lock()` / `bsp_lvgl_unlock()`.

---

## Design decisions & rationale

### 1. A proper ESP-IDF component, not application code

The board glue lives in `components/bsp_esp323248s035` with a single public
header and a `Kconfig`. This keeps `main/` focused on the demo, makes the BSP
reusable in other projects, and lets board options (rotation, amplifier driver,
LVGL buffer size…) be changed in `menuconfig` instead of by editing code. The
pin map and the ST7796 init sequence are a **direct port of the proven
`ardnew/ESP32-3248S035` Arduino BSP**, so the panel and touch controller behave
exactly as they do in the reference (they are notoriously finicky).

### 2. Display: ST7796 over SPI at 80 MHz with a BGR fix

The panel is driven through Espressif's `esp_lcd` SPI panel driver at 80 MHz
(the clock the reference BSP uses). During bring-up the colours came out wrong
(red/blue swapped) because the ST7796 on this board latches RGB565 with the BGR
bit set; the fix is `MADCTL` bit `0x08` **plus** a software byte-swap of the
RGB565 pixels in the flush path. Rotation defaults to **270°**, which matches
the “wide” landscape orientation of the reference BSP.

### 3. Touch: GT911 driven directly, no extra component

The GT911 is read with its raw I²C register protocol (status register, 5-byte
coordinate records) rather than pulling in an external driver. The coordinate
mapping accounts for the current rotation, so `bsp_touch_read()` already returns
display coordinates — the application never has to think about it.

### 4. The I²S0 contention — and why the LDR polls instead of using DMA

On the **classic ESP32**, both the `adc_continuous` (DMA) driver and the
`dac_continuous` (DMA) driver hard-wire **I²S0** for their DMA engine. With the
DAC audio engine running, the ADC DMA driver aborted at `adc_share_hw_ctrl:
adc_apb_periph_free … s_adc_digi_ctrlr_cnt == 0`.

A decision had to be made about which peripheral “owns” I²S0. **Real analogue
audio output was judged more valuable than DMA digitised light readings**, so:

* the **DAC continuous (DMA) sine/audio engine keeps I²S0**, and
* the **LDR was moved to `adc_oneshot` polling** (`bsp_ldr.c`), averaging ADC1
  channel 6 at 1 kHz and publishing every 5 ms.

This loses nothing for a slow signal like a light sensor, and it removes the
resource conflict entirely. The LDR task is pinned to the core that does **not**
run the DAC task.

### 5. DAC tone generation: LUT + phase accumulator

`bsp_audio.c` streams a phase-continuous sine built from a 1024-entry
interpolated lookup table using a 32-bit phase accumulator. A dedicated task
computes only a few tens of milliseconds of samples ahead of the DMA
descriptors and then blocks, so CPU usage is negligible. The frequency can be
changed from any task with `bsp_audio_set_frequency()` (clamped to
`BSP_AUDIO_FREQ_MIN..MAX`) without touching the DMA configuration.

### 6. MP3 playback: fixed DAC rate + lock-free streaming FIFO

The DAC is kept at a **fixed 200 kHz** sample rate (configured once in
`bsp_audio_init()`). Rather than tearing down and re-creating the DAC for every
track (which would also disturb the tone generator), decoded audio is pushed
through a **lock-free single-producer/single-consumer FIFO** and **resampled to
200 kHz with linear interpolation** inside the existing audio task:

* `bsp_audio_pcm_start(rate)` switches the audio task from “tone” to “stream”.
* `bsp_audio_pcm_write()` blocks until frames are queued, so the decoder paces
  itself naturally against the real-time output.
* `bsp_audio_pcm_stop()` switches back to the tone.

The FIFO uses plain monotonic head/tail indices (no mutex) because there is
exactly one producer and one consumer, so the real-time consumer never blocks
on a lock.

### 7. MP3 decoding: Helix fixed-point (`chmorgan/esp-libhelix-mp3`)

The board has **no PSRAM**, so the decoder must fit in internal RAM and must not
need a floating-point DSP. The Helix **fixed-point** MPEG‑1/2/2.5 Layer III
decoder is small, well-proven, CPU-cheap on an Xtensa core, and available as an
ESP-IDF component. The decoded stereo PCM is downmixed to mono (single DAC
channel) and scaled to the 8‑bit DAC with a volume factor.

The player tracks progress from the decoded frame count and estimates total
duration from the file size and the frame header's bitrate/sample rate — enough
for an accurate progress bar and time display without a full VBR scan.

### 8. microSD on its own SPI host

The card uses **SPI3 (VSPI)** while the display uses **SPI2 (HSPI)**. Keeping
them on separate bus controllers means SD I/O never stalls display DMA, and the
SD card is mounted lazily from `app_main` (`bsp_sd_mount("/sdcard")`) rather
than from `bsp_init()`, so a missing card only degrades the file explorer
instead of failing boot.

### 9. UI framework choices (LVGL 9)

* A **tabview** cleanly separates “controls” from “files”; the active tab gets a
  thick accent underline so the tab bar reads as tabs even on a dark theme.
* `lv_list` is **deprecated in LVGL 9**, so the file list is built manually from
  buttons inside a flex-column container.
* The player title uses `LV_LABEL_LONG_SCROLL_CIRCULAR` for the marquee.
* Custom sliders on the dark theme required setting `bg_opa` explicitly: without
  it the *unfilled* track was transparent and invisible.

### 10. Text viewer heuristics

The viewer reads at most 8 KB, replaces non-printable bytes and embedded NULs
with `.`, and reports a blank file as `empty`. Genuinely binary `.conf`
files are intentionally **not** rendered — there is no meaningful text view for
them, so they fall through to the “unavailable to view” popup.

---

## Building and flashing

Prerequisites: **ESP-IDF v6.1**, a Python toolchain, and a USB‑serial
connection to the board.

```bash
# from the ESP-IDF environment
cd firmware
idf.py set-target esp32        # only the first time
idf.py build
idf.py -p COM3 flash           # replace COM3 with your port
idf.py -p COM3 monitor
```

On Windows PowerShell the usual export step applies:

```powershell
& "C:\esp\v6.1\esp-idf\export.ps1"
cd firmware
idf.py build
idf.py -p COM3 flash monitor
```

If you change `Kconfig`, run `idf.py reconfigure` before building.

---

## Configuration (`BSP ESP32-3248S035` menu)

| Option | Default | Purpose |
| --- | --- | --- |
| `BSP_TFT_ROTATION_*` | 270 | Default display rotation |
| `BSP_TFT_SPI_CLK_HZ` | 80 MHz | Display SPI clock |
| `BSP_TFT_BACKLIGHT_DEFAULT` | 100 | Backlight brightness (%) |
| `BSP_LVGL_BUFFER_LINES` | 10 | Draw-buffer height (×2 buffers) |
| `BSP_LVGL_TASK_CORE` | 0 | Core for the LVGL handler task |
| `BSP_LDR_POLL_HZ` | 1000 | LDR one-shot polling rate |
| `BSP_LDR_TASK_CORE` | 1 | Core for the LDR task |
| `BSP_AMP_DRIVER_DAC` / `_BUZZER` | DAC | GPIO26 driver: real audio vs. square-wave buzzer |
| `BSP_AUDIO_SAMPLE_RATE_HZ` | 200 kHz | DAC continuous sample rate |
| `BSP_AUDIO_TASK_CORE` | 1 | Core for the DAC task |
| `BSP_AUDIO_AMPLITUDE` | 100 | Sine amplitude (0–127) |
| `BSP_AUDIO_DEFAULT_FREQ_HZ` | 320 | Default tone frequency |

---

## Features at a glance

* 480×320 landscape LVGL UI with two tabs.
* Live LDR light-level line chart (FIFO, auto-scaled).
* RGB LED control (three sliders).
* DAC sine tone with frequency, volume and enable controls.
* microSD browser: folder navigation, text viewer (`.txt`, `.json`, `.cfg`).
* MP3 player: Helix decode → DAC, marquee title, progress, time, volume,
  Play / Pause / Stop.

---

## Notes & limitations

* The ESP32 is a dual-core 240 MHz chip **without PSRAM**; decode buffers and
  the UI share internal RAM, so extremely large files/artwork are out of scope.
* MP3 audio is downmixed to mono and output through the single 8-bit DAC; the
  gain is a fixed-point scaling constant (`STREAM_GAIN_DIV` in `bsp_audio.c`) and
  is intentionally easy to tune.
* The LDR uses polling (see §4); the ADC DMA path is intentionally not used.

---

## Credits

* Board pin map and ST7796/GT911 behaviour ported from
  [`ardnew/ESP32-3248S035`](https://github.com/ardnew/ESP32-3248S035).
* LVGL 9 (`lvgl/lvgl`) and the Helix MP3 decoder
  (`chmorgan/esp-libhelix-mp3`) are pulled in through the ESP-IDF Component
  Manager.
