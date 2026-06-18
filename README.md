# 岩心光谱智能解译系统

[![Windows Build](https://github.com/YaoHongYi-72/CoreScan-Pro-sub/actions/workflows/windows-build.yml/badge.svg)](https://github.com/YaoHongYi-72/CoreScan-Pro-sub/actions/workflows/windows-build.yml)

面向岩心高光谱数据的智能解译与矿物光谱匹配工具，
支持高光谱图像浏览、点位光谱分析、文本光谱导入和内置光谱库比对。

---

## Download (Windows)

Go to the [Releases](https://github.com/YaoHongYi-72/CoreScan-Pro-sub/releases) page and download the latest `岩心光谱智能解译系统-安装包-vX.Y.Z.exe` installer.
Run the installer and launch `岩心光谱智能解译系统` from the desktop shortcut after setup completes.

---

## Features

| Feature | Description |
|---|---|
| ENVI image input | Open `.hdr` / binary file pairs; click any pixel to analyze |
| Spectrum file input | Import two-column `txt` or `csv` files (wavelength, reflectance) |
| Spectral library | Load any number of USGS-format `.txt` or multi-column `.csv` reference libraries |
| SAM matching | Spectral Angle Mapper — rotation-invariant similarity |
| FCLS unmixing | Fully-Constrained Least Squares abundance estimation (OSQP solver) |
| Continuum Removal | Upper-convex-hull CR preprocessing for feature enhancement |
| Dual-gate confidence | Combined score: 60 % SAM + 40 % NRMSE, 0–100 scale |
| Chart overlay | Interactive Qt Charts view with reference spectrum overlay |
| Pixel inspection UX | Crosshair cursor, live hover coordinates, and persistent point markers |

---

## Usage

### Option A — ENVI Hyperspectral Image

1. **File → Open ENVI File (.hdr)...** — select the `.hdr` file (the binary `.img`/`.raw` must be in the same directory).
2. Choose a display band from the dropdown.
3. Move the cursor over the image to inspect live pixel coordinates.
4. Left-click any pixel to analyze it — matches appear in the **Match Results** panel on the right and the point stays marked on the image.
5. Use the right mouse button or middle mouse button to drag and pan large images.

### Option B — Spectrum File (txt / csv)

1. **File → Import Spectrum File (txt/csv)...** — select a two-column file:
   ```
   # wavelength(nm)  reflectance
   400.0   0.0512
   405.0   0.0534
   ...
   ```
   Comma-separated `.csv` is also supported.
2. Click **Analyze** — results appear in the Match Results panel and a detailed chart dialog opens.

### Loading a Reference Library

- **File → Load Spectral Library...** (or click **Load Library...** in the right panel).
- Accepts USGS ASCII `.txt` format and multi-column `.csv`.
- Multiple files can be loaded simultaneously; they are merged into one library.

---

## Building from Source

### Prerequisites

| Dependency | Version | Install |
|---|---|---|
| Qt6 | ≥ 6.4 | [Qt online installer](https://www.qt.io/download) or `aqtinstall` |
| Eigen3 | ≥ 3.4 | `vcpkg install eigen3` |
| OSQP | ≥ 0.6 | `vcpkg install osqp` |
| CMake | ≥ 3.21 | cmake.org |

### Build

```bash
# With vcpkg toolchain
cmake -B build -S . \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE=/path/to/vcpkg/scripts/buildsystems/vcpkg.cmake

cmake --build build --config Release
```

On Windows the build step automatically runs `windeployqt` to copy Qt DLLs next to the executable.

---

## Algorithm Details

### Spectral Angle Mapper (SAM)

$$\theta = \arccos\left(\frac{\mathbf{x} \cdot \mathbf{r}}{\|\mathbf{x}\|\,\|\mathbf{r}\|}\right)$$

Threshold: **0.15 rad** (configurable in `SpectralAnalyzer`).

### FCLS Unmixing

Solves the QP problem:

$$\min_{\mathbf{a}} \frac{1}{2}\mathbf{a}^T P \mathbf{a} + \mathbf{q}^T \mathbf{a}$$

subject to $\mathbf{a} \geq 0$ (ANC) and $\sum a_i = 1$ (ASC), using the [OSQP](https://osqp.org) solver.

### Dual-Gate Confidence Score

$$\text{Score} = 100 \times \exp\!\left(-\left(0.6\,\frac{\theta}{0.15} + 0.4\,\frac{\text{NRMSE}}{0.08}\right)\right)$$

---

## License

MIT — see [LICENSE](LICENSE).
