# CAB-TR v1.4

<br/><br/>

CAB-TR is a 3-slot impulse response loader and convolver built for cabinet simulation, parallel IR blending, and M/S routing.
It combines zero-latency partitioned convolution with per-loader processing chains, flexible routing topologies, and a minimal CRT-inspired interface.

## Concept

CAB-TR treats impulse responses as composable building blocks rather than static snapshots. Three independent loader slots can be routed in series, parallel, or hybrid topologies - each with its own filter chain, Fredman off-axis simulation, size resampling, dynamic expander, and M/S bus assignment.

The Sum Bus system lets each loader contribute to the Stereo, Mid, or Side bus independently at the summing stage, enabling true M/S separation without external routing tools. Combined with four routing modes, this produces configurations that are impossible in conventional dual-loader IR plugins.

## Interface

CAB-TR uses a text-based UI with horizontal bar sliders. All controls are accessible without pages, tabs, or hidden menus.

- **Bar sliders**: Click and drag horizontally. Right-click for numeric entry.
- **Toggle buttons**: The collapsed loader row uses `INV`, `NRM`, `RVS`, and `EXP`. The expanded I/O section adds `CHSF` and `CHSD`.
- **Combo boxes**: MODE IN, MODE OUT, SUM BUS per loader. Click to cycle options.
- **Collapsible I/O section**: Click the toggle bar (triangle) between the file display and the sliders to show or hide per-loader controls (IN, OUT, TILT, FILTER, RESO, MIX, MODE IN/OUT, SUM BUS, `CHSF`, `CHSD`). State persists across sessions.
- **Browse button**: Opens a built-in file explorer with drive selector, folder navigation, and scrollable file list. Supports WAV, AIFF, FLAC, MP3, OGG.
- **Filter bar**: Click to open the HP/LP filter configuration prompt with frequency, slope, and enable/disable controls.
- **Gear icon** (top-right): Opens the info popup with version, credits, and a link to Graphics settings.
- **Export icon** (top-right): Opens the export dialog to render the combined static IR chain to disk.
- **Graphics popup**: Toggle CRT post-processing effect and switch between default/custom colour palettes.
- **Resize**: Drag the bottom-right corner. Size persists across sessions.

## Parameters

### Global

#### INPUT (-100 to 0 dB)

Pre-processing gain. Applied before the IR loaders.

#### OUTPUT (-100 to +24 dB)

Post-processing gain. Applied after routing and summing.

#### MIX (0-100%)

Dry/wet blend. 0% = fully dry (bypass), 100% = fully processed.

#### ROUTE

Routing topology for the three loaders:
- **A->B->C**: Full series chain. Signal passes through A, then B, then C.
- **A|B|C** (default): All three in parallel. Each processes the input independently; results are summed with `1/sqrt(N)` compensation.
- **A->B|C**: A feeds into B (series), C runs in parallel. Two-path sum at the output.
- **A|B->C**: A runs in parallel, B feeds into C (series). Two-path sum at the output.

#### ALIGN

Automatic phase and timing alignment for active loaders. CAB-TR cross-correlates the IR responses, then applies per-loader delay and polarity correction where needed to reduce comb-filtering when blending multiple IRs.

#### MATCH (Tilt EQ)

Adaptive spectral tilt equalization applied after the routing stage. It analyzes the loaded IR responses via FFT and applies a compensating first-order shelf to reshape the overall balance:
- **None**: No tilt correction.
- **White** (0 dB/oct): Target flat spectral density.
- **Pink** (-3 dB/oct): 1/f noise character - natural, balanced.
- **Brown** (-6 dB/oct): Warmer, darker tilt.
- **Bright** (+3 dB/oct): Presence emphasis.
- **Bright+** (+6 dB/oct): Aggressive brightness boost.

#### LIM THRESHOLD (-36 to 0 dB)

Peak limiter threshold. Sets the ceiling above which the limiter engages.
At `0 dB` (default) the limiter acts as a transparent safety net. Lower values compress the signal harder.

#### LIM MODE

Limiter insertion point:
- **NONE**: Limiter disabled.
- **WET**: Limiter applied to the wet signal only (after processing, before dry/wet mix).
- **GLOBAL**: Limiter applied to the final output (after output gain and dry/wet mix).

The limiter is a dual-stage transparent peak limiter:
- **Stage 1 (Leveler)**: 2 ms attack, 10 ms release - catches sustained overs.
- **Stage 2 (Brickwall)**: Instant attack, 100 ms release - catches transient peaks.

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

#### IN (-100 to 0 dB)

Per-loader input gain. Applied before convolution and the rest of the realtime loader chain. Useful for level-matching IRs with different native gains before they enter the IR stage.

#### OUT (-100 to +24 dB)

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

Per-loader spectral tilt. A first-order symmetric shelf filter pivoted at 1 kHz. Positive values boost highs and cut lows; negative values cut highs and boost lows. Independent of the global MATCH tilt.

#### RESO (0-200%)

Per-loader resonance intensity. Scales the IR's resonant character by blending between a smoothed (resonance-free) version and the original. `100%` = original IR, `0%` = fully smoothed, `200%` = exaggerated resonances.

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

This makes `EXP ORDER` in CAB-TR directly analogous to `SAT-TR`: it decides whether the dynamics stage conditions what feeds the core black box (the IR) or shapes the result after it.

#### MODE IN / MODE OUT

Stereo encoding mode applied before (IN) and after (OUT) convolution:
- **L+R**: Standard stereo (default).
- **MID**: Mono sum - `(L+R) / 2`.
- **SIDE**: Stereo difference - `(L-R) / 2`.

#### SUM BUS

Per-loader bus assignment for the parallel summing stage:
- **ST**: Stereo - contributes directly to L and R (default).
- **->M**: Routes to the Mid bus - `(L+R) * 0.5` added to both L and R equally.
- **->S**: Routes to the Side bus - `(L-R) * 0.5` added with opposite polarity to L and R.

Sum Bus is applied at the summing point in Routes `A|B|C`, `A->B|C`, and `A|B->C`. In the full-series route (`A->B->C`), the signal is never split, so Sum Bus has no effect.

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

The export path includes the IR chain, routing, filters, delay, angle/FRED, pan, gains, and other static processing. Dynamic or non-static stages are intentionally excluded:
- **CHAOS**
- **EXP**

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
- **Rate-limited reloads**: Minimum 300 ms between IR rebuilds to prevent CPU spikes during slider automation.
- **Filters**: Butterworth IIR (HP/LP), transposed Direct Form II. Coefficients updated every 32 samples for efficient automation.
- **Fredman comb**: 7-tap circular delay buffer with wet/dry blend.
- **Variable delay**: Per-loader delay stage uses smoothed fractional delay for fine alignment and manual offset control.
- **M/S summing**: Per-sample bus accumulation with fast path (no M/S overhead) when all loaders are set to ST.
- **Parallel compensation**: `1/sqrt(N)` gain correction per routing mode (`N` = number of parallel paths).

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
- Added dual-stage transparent peak limiter with LIM THRESHOLD (-36 to 0 dB) and LIM MODE (NONE/WET/GLOBAL). Stereo-linked gain reduction with 2 ms/10 ms leveler + instant/100 ms brickwall stages.
- Added per-loader `EXP` with `PRE/POST` order around the IR loader plus THRESH, RATIO, KNEE, ATK, and REL controls.
- Refined prompt UX and delay readouts for consistent numeric editing and 0.001 ms precision display.
- Export now excludes dynamic/non-static stages such as `CHAOS` and `EXP`.
