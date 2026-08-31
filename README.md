# HelioSim

**An interactive N-body gravity simulator that runs entirely in the browser.**

HelioSim combines a C++ physics engine compiled to WebAssembly with OpenGL ES 3 rendering. It includes a Barnes-Hut force solver, predictive orbit trails, collision merging, time controls, and interactive body spawning.

**[Live demo](https://koprolin.com/heliosim/)**

![HelioSim demo](docs/demo.gif)

## Highlights

- Barnes-Hut octree gravity with configurable accuracy
- Velocity Verlet integration with three physics substeps per frame
- Predicted and historical orbit trails
- Collision detection and momentum-preserving body merging
- Planet, star, and black-hole rendering
- Mouse and touch camera controls

## Performance

> **A single Barnes-Hut force pass sustains 60 Hz at 6,000 bodies in Chromium/WebAssembly. At 10,000 bodies it runs at about 32 solves/s, while 100,000 bodies solve in about 0.56 seconds.**

| Bodies | Native C++ | Chromium / WebAssembly |
| ------: | ---------: | ---------------------: |
| 1,000 | 1.32–1.68 ms | 1.30–1.50 ms |
| 6,000 | — | 16.40 ms |
| 10,000 | 26.51–27.12 ms | 30.80–31.30 ms |
| 50,000 | 212.77–215.64 ms | 236.70–246.90 ms |
| 100,000 | 494.44–521.93 ms | 544.10–579.40 ms |

At 10,000 bodies, Barnes-Hut is approximately **11× faster** than the original native direct solver: about 27 ms instead of 296 ms.

### What these numbers mean

The benchmark measures one complete force pass: rebuilding the tree and calculating acceleration for every body. It uses a deterministic uniform 3D distribution, `theta = 0.5`, leaf capacity 8, and a single-threaded `-O3` build on an Apple M3 Max.

These are solver timings, not rendered application FPS. HelioSim currently performs three Verlet substeps—six force passes—per frame. Its browser force-only 60 FPS ceiling is therefore approximately 1,750 bodies. Collision detection, future-trail prediction, and rendering reduce full-application throughput further.

## Controls

| Input | Action |
| :---- | :----- |
| Left drag | Orbit the camera |
| Scroll or pinch | Zoom in or out |
| Shift + drag or two-finger drag | Pan the camera target |
| Resize the window | Resize the viewport |

## Build locally

### Requirements

- [Emscripten](https://emscripten.org/docs/getting_started/downloads.html)
- `make`
- Python 3, used by the local development server

GLM is included in `external/glm`.

Build the WebAssembly application and open it in your browser:

```bash
make
```

Build without starting the local server:

```bash
make build/heliosim.js
```

## Test and benchmark

```bash
# Correctness, accuracy, and scaling tests
make test

# Native optimized benchmark
make benchmark

# Build the browser benchmark
make benchmark-wasm
```

To run the browser benchmark, serve the build directory and open `browser_benchmark.html`:

```bash
python3 -m http.server 8000 --directory build
```

Then visit [http://localhost:8000/browser_benchmark.html](http://localhost:8000/browser_benchmark.html).

## Other commands

```bash
# Enable desktop touch emulation
make TOUCH_EMULATOR=1

# Remove generated files
make clean
```

## Third-party libraries

| Library | License | Used for |
| :------ | :------ | :------- |
| [GLM](https://github.com/g-truc/glm) | MIT | Vector and matrix mathematics |
| [TouchEmulator](https://hammerjs.github.io/touch-emulator/) | MIT | Desktop touch-input emulation |
