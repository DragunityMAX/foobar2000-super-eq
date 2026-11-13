# `foo_super_eq` — Left/Right Separated Parametric & Linear-Phase EQ for foobar2000

A high-precision foobar2000 v2.0+ component (`x64` & `x86`) built for detailed **Left / Right / Mid / Side** room, speaker, and headphone equalization.

## Features

- **Up to 64 Parametric Bands per Preset** with per-band channel routing:
  - `Stereo (L+R)`, `Left (L)`, `Right (R)`, `Mid (M)`, and `Side (S)`
  - `Link L+R` mode moves paired Left/Right bands synchronously
  - `L→R`, `R→L`, and `L↔R` channel bank utilities
- **9 Pro Filter Types & 6 Slopes**:
  - `Bell`, `Low Shelf`, `High Shelf`, `Low Pass`, `High Pass`, `Notch`, `Band Pass`, `All-Pass`, `Flat Tilt`
  - Slopes: `6, 12, 18, 24, 36, 48 dB/oct`
  - Per-band **Solo** (isolated bandpass audition) and **Bypass**
- **Dual DSP Processing Engine**:
  - **Minimum-Phase 64-bit IIR Biquad Cascade**: zero latency, instant glitch-free parameter updates
  - **Linear-Phase FIR Convolver**: overlap-add FFT convolution with Blackman-Harris windowed symmetric kernels (exact zero phase shift) and automatic latency reporting (`get_latency()`)
- **Channel Calibration & Stream Handling**:
  - Per-channel Preamp (`±30 dB`), Delay (`0–100 ms` with fractional sample interpolation), Polarity Invert (`Ø`), and Mute
  - Automatic Mono (`1ch → 2ch`) upmixing when Left and Right EQ or calibration differ
  - Multichannel/Surround (`5.1`, `7.1`) support: processes `FL`/`FR` and delays remaining surround channels by exact FIR latency in Linear-Phase mode
  - Auto-Gain compensation and transparent soft-knee Clip Limiter
- **Direct2D 60fps Interactive UI (Dark Mode Ready)**:
  - Synchronized across **DSP Manager Popup**, **Default UI Dockable Panel**, and **View → Super EQ** floating window
  - Switchable **Overlay (L+R+M+S)** vs **Split (Top Left / Bottom Right)** graph modes
  - Real-time **Pre-EQ & Post-EQ L/R FFT Spectrum Analyzer**, optional **Phase** curves, and vertical **L/R Peak/RMS Meters + Clip Latch**
  - Floating **Node HUD**, `Shift` fine-drag, mouse-wheel `Q` adjustment, double-click add/bypass, right-click band menu, and collapsible **Band Table**
- **Preset & Import/Export Suite**:
  - **Equalizer APO / REW (`.txt`)** multi-channel import (`Channel: all / L / R`, `BW Oct` & `Q`) + force-into-Left / force-into-Right append workflow + full APO export
  - **foobar2000 (`.feq`)** 18-band import
  - Native **JSON** preset save/load and **A/B** instant comparison slots

## Building & Testing

### Linux / Portable Core Tests & CLI
```bash
make test
make cli
```

### Windows foobar2000 Component (`foo_super_eq.dll`)
Requires Visual Studio 2022, CMake, [foobar2000 SDK v2.x](https://www.foobar2000.org/SDK), and [WTL 10](https://sourceforge.net/projects/wtl/). See [`.github/workflows/build-fb2k-component.yml`](.github/workflows/build-fb2k-component.yml) for the automated `x64` + `Win32` build and `.fb2k-component` packaging pipeline.
