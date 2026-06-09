# CAB-TR v1.4

<br/><br/>

CAB-TR is a 3-slot impulse response loader and convolver built for cabinet simulation, parallel IR blending, and M/S routing.
It combines zero-latency partitioned convolution with per-loader processing chains, flexible routing topologies, and a minimal CRT-inspired interface.

## Concept

CAB-TR treats impulse responses as composable building blocks rather than static snapshots. Three independent loader slots can be routed in series, parallel, or hybrid topologies - each with its own filter chain, Fredman off-axis simulation, size resampling, dynamic expander, realtime Variation drift, and M/S bus assignment.

The Sum Bus system lets each loader contribute to the Stereo, Mid, or Side bus independently at the summing stage, enabling true M/S separation without external routing tools. Combined with six routing modes, this produces configurations that are impossible in conventional dual-loader IR plugins.

## Interface

CAB-TR uses a text-based UI with horizontal bar sliders. Loader tabs switch between visible loader slots at compact widths, and the bottom `GLOBAL` tab switches between loader controls and the global control page.

- **Bar sliders**: Click and drag horizontally. Right-click for numeric entry.
- **Toggle buttons**: The collapsed loader row uses `INV`, `NRM`, `RVS`, and `EXP`. The expanded I/O section adds `CHSF` and `CHSD`.
- **VAR**: Per-loader realtime variation in the collapsed control set. Adds subtle cab/mic drift without reloading the IR.
- **Empty loaders**: A loader is only operative when it is enabled and has an IR loaded. Empty enabled loaders stay transparent, and their DSP controls remain inactive until a file is loaded.
- **Combo boxes**: MODE IN, MODE OUT, FILTER/TILT position, and SUM BUS per loader. Click to cycle options.
- **Loader tabs**: Use the side tabs to switch visible loaders when the window is compact. Wider sizes reveal more loaders at once.
- **Collapsible I/O section**: Click the toggle bar (triangle) between the file display and the sliders to show or hide per-loader controls (IN, OUT, TILT, FILTER, PAN, MIX, MODE IN/OUT, F/T, SUM BUS, `CHSF`, `CHSD`). State persists across sessions.
- **Bottom global section**: Click the `GLOBAL` tab to switch between loader controls and the global control page.
- **Browse button**: Opens a built-in file explorer with drive selector, folder navigation, and scrollable file list. Supports WAV, AIFF, FLAC, MP3, OGG.
- **Filter bar**: Click to open the HP/LP filter configuration prompt with frequency, slope, and enable/disable controls.
- **Gear icon** (top-right): Opens the info popup with version, credits, and a link to Graphics settings.
- **Export icon** (top-right): Opens the export dialog to render the combined static IR chain to disk.
- **Graphics popup**: Toggle CRT post-processing effect and switch between default/custom colour palettes.
- **Resize**: Drag the bottom-right corner. New instances open at one-loader minimum size (`360 x 752`); width persists as 1/2/3-loader views and height stays fixed.

## Parameters

### Global

#### INPUT (-INF to +24 dB)

Pre-processing gain. Applied before the IR loaders.
The fader floor is -144 dB, displayed as -INF; 0 dB is centered on the control.

#### OUTPUT (-INF to +24 dB)

Post-processing gain. Applied after routing and summing.

#### MIX (0-100%)

Dry/wet blend. 0% = fully dry (bypass), 100% = fully processed.

#### ROUTE

Routing topology for the three loaders:
- **A->B->C**: Full series chain. Signal passes through A, then B, then C.
- **A|B|C** (default): All three in parallel. Each processes the input independently; results are summed directly.
- **A->B|C**: A feeds into B (series), C runs in parallel. Two-path sum at the output.
- **A|B->C**: A runs in parallel, B feeds into C (series). Two-path sum at the output.
- **(A|B)->C**: A and B run in parallel first; their combined result then feeds C.
- **A->(B|C)**: A runs first as a shared pre-stage; its output then splits to B and C in parallel.

Notes:
- In **A|B->C**, only the **B** branch feeds **C**. `A` is summed later, after the `B->C` chain.
- In **(A|B)->C**, `A` and `B` are summed first, and that combined signal is what enters `C`.
- In **A->(B|C)**, `A` is a shared pre-stage. Both `B` and `C` receive the output of `A`.

#### ALIGN

Automatic phase and timing alignment for active loaders. CAB-TR cross-correlates the IR responses, then applies per-loader delay and polarity correction where needed to reduce comb-filtering when blending multiple IRs.

Right-click the `ALIGN` text to toggle `A+DI` mode. In `A+DI`, the same left-click action also delays the per-loader and global dry/DI paths so dry/wet blends line up with the dominant IR arrival. Switching back to `ALIGN` returns dry paths to their original zero-delay behaviour.

#### MATCH (Adaptive EQ)

Adaptive spectral equalization applied after the routing stage. It analyzes the loaded IR responses via FFT over the useful cab range (`100 Hz-8 kHz`) and applies a compensating tilt, limited broad low/high shelves, and one conservative automatic broad bell (`±6 dB`, `Q 0.25-1.0`) to reshape the overall balance without chasing narrow resonances:
- **None**: No tilt correction.
- **White** (0 dB/oct): Target flat spectral density.
- **Pink** (-3 dB/oct): 1/f noise character - natural, balanced.
- **Brown** (-6 dB/oct): Warmer, darker tilt.
- **Bright** (+3 dB/oct): Presence emphasis.
- **Bright+** (+6 dB/oct): Aggressive brightness boost.

#### LIM (-36 to 0 dB)

Peak limiter threshold. Sets the ceiling above which the limiter engages.
At `0 dB` (default) the limiter acts as a transparent safety net. Lower values compress the signal harder.

#### LIM MODE

Limiter insertion point:
- **NONE**: Limiter disabled.
- **WET**: Limiter applied to the wet signal only (after processing, before dry/wet mix).
- **GLOBAL**: Limiter applied to the final output (after output gain and dry/wet mix).

The limiter is a dual-stage transparent peak limiter:
- **Stage 1 (Leveler)**: catches sustained overs.
- **Stage 2 (Brickwall)**: catches transient peaks.

Stereo-linked gain reduction ensures consistent imaging.

### Per-Loader (A, B, C)

Each loader slot has a full independent realtime processing chain.

#### ENABLE

Enables or disables the loader. Disabled loaders are skipped entirely - zero CPU cost.

#### FILE

The loaded impulse response file. Displayed as the filename in the loader header. Click the browse button (`...`) to open the file explorer.

#### HP / LP FILTER

High-pass and low-pass filters applied to the loader output.

- **HP FREQ (20-20 000 Hz)**: High-pass cutoff frequency. Default: 80 Hz.
- **LP FREQ (20-20 000 Hz)**: Low-pass cutoff frequency. Default: 12 000 Hz.
- **HP SLOPE / LP SLOPE (6 dB / 12 dB / 24 dB)**: Filter slope per filter.
- **HP / LP toggles**: Enable or disable each filter independently.

Slope modes:
- **6 dB/oct**: Single-pole filter.
- **12 dB/oct**: Second-order Butterworth.
- **24 dB/oct**: Two cascaded second-order Butterworth stages.

#### IN (-INF to +24 dB)

Per-loader input gain. Applied before convolution and the rest of the realtime loader chain. Useful for level-matching IRs with different native gains before they enter the IR stage.
The fader floor is -144 dB, displayed as -INF; 0 dB is centered on the control.

#### OUT (-INF to +24 dB)

Per-loader output gain.

#### START / END (0-10 s)

Time-domain trimming of the IR. START sets where the IR begins; END sets where it is truncated. Applied before convolution - changes trigger an IR reload.

#### SIZE (0.25x-4.0x)

IR resampling ratio. Values below 1.0 stretch the IR (larger cabinet size, longer tail); values above 1.0 compress it (smaller cabinet size, shorter tail). Applied via high-quality resampling before convolution.

#### DELAY (0-1000 ms, 0.001 ms resolution)

Post-loader delay. Adds a time offset to the loader's output and is also used by `ALIGN` for fine phase/time compensation. Useful for manual alignment, Haas widening, or creative timing effects.

#### PAN (0-100%)

Stereo pan. 0% = full left, 50% = center (default), 100% = full right.

#### FRED / Angle (0-100%)

Fredman off-axis microphone simulation. Models a secondary mic angled away from the speaker cone using a 7-sample circular comb delay (~0.15 ms at 48 kHz, ~5 cm path difference). Creates a first null at ~6.8 kHz - a musically useful tonal shaping tool common in modern high-gain guitar tones.

#### TILT (-6 to +6 dB)

Per-loader spectral tilt. A first-order symmetric shelf filter pivoted at 1 kHz. Positive values boost highs and cut lows; negative values cut highs and boost lows. Independent of the global MATCH adaptive EQ.

#### RESO (0-200%)

Per-loader resonance intensity. Scales the IR decay over the audible tail so trailing silence does not dilute the effect. `100%` = original IR, lower values shorten resonance, higher values extend it.

#### VAR / Variation (0-100%)

Subtle realtime cab/mic drift for decorrelation and organic movement. At 100%, VAR applies small independent modulation to the loader's perceived SIZE (a realtime phase proxy that does not reload or rebuild the IR), ANGLE/FRED (up to +/-4%), and POS/Distance (up to +/-2%). The upper range adds an instance-persistent micro-wander layer for denser movement in deep decorrelation/null-test use, contributing up to roughly 40% of the final modulation weight. The instance seed is saved with the project state, so a reopened session keeps the same decorrelated drift.

#### POS / Distance (0-100%)

Position filter. Simulates the acoustic effect of moving the microphone further from the speaker. Higher values apply frequency-dependent filtering that models increased distance.

#### MIX (0-100%)

Per-loader dry/wet blend. Independent of the global MIX control.

#### INV (Invert)

Inverts the loader's output polarity (`x -1`). Useful for phase-cancellation tricks between loaders.

#### NRM (Normalize)

Per-loader peak normalization of the IR after time-domain preparation (trim, size, and related IR preprocessing).

#### RVS (Reverse)

Reverses the IR in the time domain before convolution. Produces pre-echo and ambient wash effects.

#### EXP (Expander)

Per-loader stereo-linked downward expander. Click `EXP` to enable it; right-click to open the prompt.

Prompt controls:
- **ORDER**: `PRE` places the expander before the IR loader. `POST` places it immediately after the IR loader.
- **THRESH (-60 to 0 dB)**: Expansion threshold.
- **RATIO (0.1 to 10.0)**: Ratio control centered at `1.0`.
  - **1.0**: Neutral.
  - **> 1.0**: Downward expansion below threshold.
  - **< 1.0**: Inverted action on material below threshold.
- **KNEE (0 to 12 dB)**: Soft-knee region around the threshold.
- **ATK (0.00 to 100 ms)**: Attack time.
- **REL (5 to 2000 ms)**: Release time.
- **SIDECHAIN GAIN (-INF to +24.0 dB)**: Internal detector trim; it changes only what the expander listens to, not the audio signal itself.
- **SIDECHAIN HP / LP (20 Hz to 20 kHz)**: Optional detector filters for frequency-selective expansion. Each band can be enabled independently and set to `6 dB`, `12 dB`, or `24 dB` per octave.

This makes `EXP ORDER` in CAB-TR directly analogous to `SAT-TR`: it decides whether the dynamics stage conditions what feeds the core black box (the IR) or shapes the result after it.

#### MODE IN / MODE OUT

Per-loader Mid/Side routing:
- **MODE IN**
  - `L+R`: standard stereo input
  - `MID`: extracts the Mid component for processing
  - `SIDE`: extracts the Side component for processing
- **MODE OUT**
  - `L+R`: standard stereo output
  - `MID`: outputs a mono Mid signal on both channels
  - `SIDE`: outputs a stereo Side signal as `+S / -S`

#### SUM BUS

Per-loader bus assignment for the parallel summing stage:
- **ST**: Stereo - contributes directly to L and R (default).
- **->M**: Routes to the Mid bus - `(L+R) * 0.5` added to both L and R equally.
- **->S**: Routes to the Side bus - `(L-R) * 0.5` added with opposite polarity to L and R.

Practical note:
- `MODE IN` decides what component a loader processes.
- `MODE OUT` decides how that processed result is represented when it leaves the loader.
- `SUM BUS` decides how that loader output is injected at a parallel summing point.
- `SUM BUS` matters in routes with an actual split/summing stage: `A|B|C`, `A->B|C`, `A|B->C`, `(A|B)->C`, and `A->(B|C)`.
- In full series `A->B->C`, there is no parallel sum stage, so `SUM BUS` has no practical effect.

The final output combines all three buses:
- `outL = stL + midBus + sideBus`
- `outR = stR + midBus - sideBus`

#### CHAOS

CAB-TR has two loader-local chaos stages in the expanded I/O section:

- **CHSD**: Delay/gain chaos. Adds Hermite-interpolated micro-delay and small gain drift after the loader chain for subtle wobble or more obvious movement.
- **CHSF**: Filter chaos. Modulates the HP/LP targets over time for animated cabinet tone shifts.

Both chaos prompts expose:
- **AMOUNT (0-100%)**: Modulation depth.
- **SPEED (0.01-100 Hz)**: Random target rate.

### Export

Renders the combined static output of all active loaders to a single IR file on disk.

The export path mirrors the static routing and tone-shaping chain as closely as a stereo IR format allows. This includes:
- loader routing, `MODE IN`, `MODE OUT`, and `SUM BUS`
- per-loader static processing: gains, tilt, HP/LP, Distance (`DIST`), PAN, DELAY, ANGLE/FRED, and per-loader `MIX`
- global static processing: input gain, MATCH, NORM, INSERT/SEND mix blend, DC block, output gain, and `INV POL` / `INV STR`

Dynamic or non-static stages are intentionally excluded:
- **CHAOS D**
- **CHAOS F**
- **VAR**
- **EXP**
- **LIMITER**

Practical note:
- chained/internal `MODE IN MID/SIDE` stages are rendered from the real upstream buffer, so routed exports follow the realtime topology closely
- a **first** loader using `MODE IN MID/SIDE` is still constrained by the stereo IR format itself: the export starts from a correlated stereo unit impulse, so pre-convolution M/S matrixing at the very first stage is represented as the closest practical stereo approximation rather than a full 2x2 channel matrix

Export options:
- **Sample Rates**: 44.1k, 48k, 88.2k, 96k, 176.4k, 192k Hz.
- **Formats**: WAV (16/24/32-bit), AIFF (24-bit), FLAC (24-bit).
- **Length**: 0.01-10 seconds (editable).
- **Trim Silence**
- **Normalize 0 dB**
- **Minimum Phase Transform**

## Technical Details

### DSP Architecture
- **Convolver**: FFTConvolver (HiFi-LoFi, MIT) - two-stage partitioned convolution. Low-latency head block processed on the audio thread; long tail processed on a background thread.
- **Zero-latency**: No added latency from convolution - the head partition runs in real time.
- **IR crossfade**: 50 ms S-curve crossfade when swapping IRs, preventing clicks on parameter changes.
- **DC protection**: wet-only first-order DC blocking after the cabinet chain and polarity stages, matching the static export path.
- **Debounced IR reloads**: IR-affecting loader changes rebuild only after the controls have stayed stable for about `300 ms`, avoiding repeated reloads while dragging.
- **Filters**: Butterworth IIR (HP/LP), transposed Direct Form II. Coefficients updated every 32 samples for efficient automation.
- **Fredman comb**: 7-tap circular delay buffer with wet/dry blend.
- **Variation**: Deterministic per-loader smooth modulation for subtle cab/mic drift. SIZE variation is a realtime all-pass proxy and never triggers an IR reload; high VAR values add a faster instance-seeded layer without increasing the maximum modulation range. The instance seed is persisted in plugin state.
- **Variable delay**: Per-loader delay stage uses smoothed fractional delay for fine alignment and manual offset control.
- **M/S summing**: Per-sample bus accumulation with fast path (no M/S overhead) when all loaders are set to ST.
- **Parallel summing**: parallel branches are summed directly; use per-loader `OUT`/`MIX`, global `OUTPUT`, and the limiter for level management.

### FFT Backend
- **FFTW** (preferred): Auto-loaded from system DLL on Windows. Provides optimized FFT for convolution.
- **Fallback**: JUCE `juce::dsp::FFT` when FFTW is unavailable.

### State Persistence
- All parameters saved via JUCE AudioProcessorValueTreeState.
- IR file paths stored in the plugin state (`irPathA`, `irPathB`, `irPathC`) and reloaded on session restore.
- UI state (window size, palette, CRT toggle, custom colours) stored as non-automatable APVTS parameters - persists across sessions but hidden from the DAW automation list.
- I/O section expanded/collapsed state persisted as a ValueTree property.
- Parameter IDs are stable across versions for preset compatibility.

### Performance
- Zero-allocation audio thread. All buffers pre-allocated in `prepareToPlay`.
- Lock-free atomic parameter reads.
- Background-threaded convolution tail.
- Disabled loaders cost zero CPU.
- Global and per-loader dry buffers are skipped when the corresponding mix path is fully wet and stable, while preserving dry fade-out ramps during transitions.
- Stable per-loader delay blocks avoid redundant per-sample delay setup and keep delay lines continuously fed for click-free re-entry.
- CRT effect uses cached rendering with timer-driven animation.

### Build
- JUCE Framework, C++17, VST3 format.
- Visual Studio 2022 (MSBuild, x64).
- Dependencies: JUCE modules, FFTConvolver (MIT), FFTW (optional).

## Changelog

### v1.4
- Fixed temp buffer allocation causing distortion in hosts with small block sizes (e.g. FL Studio at 96 samples).
- Cached per-block `std::exp` coefficients in `prepareToPlay` (tilt smoothing, DC blocker, NORM AGC).
- Aligned tooltip rendering with shared TR-series style (centred `drawFittedText`).
- Added dual-stage transparent peak limiter with LIM (-36 to 0 dB) and LIM MODE (NONE/WET/GLOBAL). Stereo-linked gain reduction with leveler + brickwall stages.
- Added per-loader `EXP` with `PRE/POST` order around the IR loader plus THRESH, RATIO, KNEE, ATK, and REL controls.
- Refined prompt UX and delay editing for consistent numeric entry, while keeping compact time readouts in the main UI.
- Export now follows the full static routing/tone chain more closely, while still excluding dynamic/non-static stages such as `CHAOS`, `VAR`, `EXP`, and `LIMITER`.
- Added per-loader `VAR` for subtle realtime cab/mic drift without triggering IR rebuilds.
- Updated global and per-loader gain faders to a consistent -INF to +24 dB range with 0 dB centered.
- Optimized global/per-loader dry-wet mix paths and stable delay processing without changing the intended audio behavior.
- Refined per-loader FILTER and EXP sidechain HP/LP prompts for consistent Hz/kHz entry.
