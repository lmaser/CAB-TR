# CAB-TR v1.4

<br/><br/>

CAB-TR is a 3-slot impulse response loader and convolver built for cabinet simulation, parallel IR blending, and M/S routing.
It combines zero-latency partitioned convolution with per-loader processing chains, flexible routing topologies, and a minimal CRT-inspired interface.

## Concept

CAB-TR treats impulse responses as composable building blocks rather than static snapshots. Three independent loader slots can be routed in series, parallel, or hybrid topologies — each with its own filter chain, Fredman off-axis simulation, size resampling, and M/S bus assignment.

The Sum Bus system lets each loader contribute to the Stereo, Mid, or Side bus independently at the summing stage, enabling true M/S separation without external routing tools. Combined with four routing modes, this produces configurations that are impossible in conventional dual-loader IR plugins.

## Interface

CAB-TR uses a text-based UI with horizontal bar sliders. All controls are accessible without pages, tabs, or hidden menus.

- **Bar sliders**: Click and drag horizontally. Right-click for numeric entry.
- **Toggle buttons**: INV, NORM, RVS, CHAOS. Click to enable/disable.
- **Combo boxes**: MODE IN, MODE OUT, SUM BUS per loader. Click to cycle options.
- **Collapsible IO section**: Click the toggle bar (triangle) between the file display and the sliders to show or hide per-loader controls (IN, OUT, TILT, FILTER, RESO, MIX, MODE IN/OUT, SUM BUS, CHAOS). State persists across sessions.
- **Browse button**: Opens a built-in file explorer with drive selector, folder navigation, and scrollable file list. Supports WAV, AIFF, FLAC, MP3, OGG.
- **Filter bar**: Click to open the HP/LP filter configuration prompt with frequency, slope, and enable/disable controls.
- **Gear icon** (top-right): Opens the info popup with version, credits, and a link to Graphics settings.
- **Export icon** (top-right): Opens the export dialog to render the combined IR to disk.
- **Graphics popup**: Toggle CRT post-processing effect and switch between default/custom colour palettes.
- **Resize**: Drag the bottom-right corner. Size persists across sessions.

## Parameters

### Global

#### INPUT (−100 to 0 dB)

Pre-processing gain. Applied before the IR loaders.

#### OUTPUT (−100 to +24 dB)

Post-processing gain. Applied after routing and summing.

#### MIX (0–100%)

Dry/wet blend. 0% = fully dry (bypass), 100% = fully processed.

#### ROUTE

Routing topology for the three loaders:
- **A→B→C**: Full series chain. Signal passes through A, then B, then C.
- **A|B|C** (default): All three in parallel. Each processes the input independently; results are summed with 1/√N compensation.
- **A→B|C**: A feeds into B (series), C runs in parallel. Two-path sum at the output.
- **A|B→C**: A runs in parallel, B feeds into C (series). Two-path sum at the output.

#### ALIGN

Phase alignment. Auto-detects and corrects phase misalignment between loaders to prevent comb-filtering artifacts when blending multiple IRs.

#### MATCH (Tilt EQ)

Adaptive spectral tilt equalization applied after the routing stage. Analyzes each IR's spectral slope via FFT and applies a compensating first-order shelf to reshape the overall balance:
- **None**: No tilt correction.
- **White** (0 dB/oct): Target flat spectral density.
- **Pink** (−3 dB/oct): 1/f noise character — natural, balanced.
- **Brown** (−6 dB/oct): Warmer, darker tilt.
- **Bright** (+3 dB/oct): Presence emphasis.
- **Bright+** (+6 dB/oct): Aggressive brightness boost.

#### TRIM (Normalization)

Global peak-hold normalization. Scales the combined output to hit the selected ceiling:
- **Off**: No normalization.
- **0 dB / −3 dB / −6 dB / −12 dB / −18 dB**: Target peak level.

### Per-Loader (A, B, C)

Each loader slot has a full independent processing chain:

#### ENABLE

Enables or disables the loader. Disabled loaders are skipped entirely — zero CPU cost.

#### FILE

The loaded impulse response file. Displayed as the filename in the loader header. Click the browse button (...) to open the file explorer.

#### HP / LP FILTER

High-pass and low-pass filters applied to the IR signal.

- **HP FREQ (20–20 000 Hz)**: High-pass cutoff frequency. Default: 80 Hz.
- **LP FREQ (20–20 000 Hz)**: Low-pass cutoff frequency. Default: 12 000 Hz.
- **HP SLOPE / LP SLOPE (6 dB / 12 dB / 24 dB)**: Filter slope per filter.
- **HP / LP toggles**: Enable or disable each filter independently.

Slope modes:
- **6 dB/oct**: Single-pole filter.
- **12 dB/oct**: Second-order Butterworth.
- **24 dB/oct**: Two cascaded second-order Butterworth stages.

#### IN (−100 to 0 dB)

Per-loader input gain. Applied before convolution and all per-loader effects. Useful for level-matching IRs with different native gains before they enter the processing chain.

#### OUT (−100 to +24 dB)

Per-loader output gain.

#### START / END (0–10 s)

Time-domain trimming of the IR. START sets where the IR begins; END sets where it is truncated. Applied before convolution — changes trigger an IR reload.

#### SIZE (0.25x–4.0x)

IR resampling ratio. Values below 1.0 stretch the IR (larger cabinet size, longer tail); values above 1.0 compress it (smaller cabinet size, shorter tail). Applied via high-quality resampling before convolution.

#### DELAY (0–1000 ms)

Post-convolution delay. Adds a time offset to the loader's output. Useful for manual alignment, Haas widening, or creative timing effects.

#### PAN (0–100%)

Stereo pan. 0% = full left, 50% = center (default), 100% = full right.

#### FRED / Angle (0–100%)

Fredman off-axis microphone simulation. Models a secondary mic angled away from the speaker cone using a 7-sample circular comb delay (~0.15 ms at 48 kHz, ~5 cm path difference). Creates a first null at ~6.8 kHz — a musically useful tonal shaping tool common in modern high-gain guitar tones.

#### TILT (−6 to +6 dB)

Per-loader spectral tilt. A first-order symmetric shelf filter pivoted at 1 kHz. Positive values boost highs and cut lows; negative values cut highs and boost lows. Independent of the global MATCH tilt.

#### RESO (0–200%)

Per-loader resonance intensity. Scales the IR's resonant character by blending between a smoothed (resonance-free) version and the original. 100% = original IR, 0% = fully smoothed (dry envelope only), 200% = exaggerated resonances.

#### POS / Distance (0–100%)

Position filter. Simulates the acoustic effect of moving the microphone further from the speaker. Higher values apply frequency-dependent filtering that models increased distance.

#### MIX (0–100%)

Per-loader dry/wet blend. Independent of the global MIX control.

#### INV (Invert)

Inverts the loader's output polarity (×−1). Useful for phase-cancellation tricks between loaders.

#### NORM (Normalize)

Per-loader peak normalization of the IR after all time-domain processing (trim, size, filters).

#### RVS (Reverse)

Reverses the IR in the time domain before convolution. Produces pre-echo and ambient wash effects.

#### MODE IN / MODE OUT

Stereo encoding mode applied before (IN) and after (OUT) convolution:
- **L+R**: Standard stereo (default).
- **MID**: Mono sum — (L+R)/2.
- **SIDE**: Stereo difference — (L−R)/2.

#### SUM BUS

Per-loader bus assignment for the parallel summing stage:
- **ST**: Stereo — contributes directly to L and R (default).
- **→M**: Routes to the Mid bus — `(L+R) × 0.5` added to both L and R equally.
- **→S**: Routes to the Side bus — `(L−R) × 0.5` added with opposite polarity to L and R.

Sum Bus is applied at the summing point in Routes 1 (A|B|C), 2 (A→B|C), and 3 (A|B→C). In the full-series route (A→B→C), the signal is never split, so Sum Bus has no effect.

The final output combines all three buses: `outL = stL + midBus + sideBus`, `outR = stR + midBus − sideBus`.

#### CHAOS

Micro-variation engine that adds organic randomness to the loader's output:
- **ENABLE**: Toggle chaos modulation on/off.
- **AMOUNT (0–100%)**: Modulation depth — controls the range of delay variation (0–1024 samples).
- **SPEED (0.01–100 Hz)**: Random target rate — how often a new random value is generated.

Creates subtle tape-like wobble or aggressive detuning effects depending on settings.

### Export

Renders the combined output of all active loaders (through the full routing and processing chain) to a single IR file on disk.

- **Sample Rates**: 44.1k, 48k, 88.2k, 96k, 176.4k, 192k Hz.
- **Formats**: WAV (16/24/32-bit), AIFF (24-bit), FLAC (24-bit).
- **Length**: 0.01–10 seconds (editable).

### LIM THRESHOLD (−36 to 0 dB)

Peak limiter threshold. Sets the ceiling above which the limiter engages.
At 0 dB (default) the limiter acts as a transparent safety net. Lower values compress the signal harder.

### LIM MODE

Limiter insertion point:
- **NONE**: Limiter disabled.
- **WET**: Limiter applied to the wet signal only (after processing, before dry/wet mix).
- **GLOBAL**: Limiter applied to the final output (after output gain and dry/wet mix).

The limiter is a dual-stage transparent peak limiter:
- **Stage 1 (Leveler)**: 2 ms attack, 10 ms release — catches sustained overs.
- **Stage 2 (Brickwall)**: Instant attack, 100 ms release — catches transient peaks.

Stereo-linked gain reduction ensures consistent imaging.

## Technical Details

### DSP Architecture
- **Convolver**: FFTConvolver (HiFi-LoFi, MIT) — two-stage partitioned convolution. Low-latency head block processed on the audio thread; long tail processed on a background thread.
- **Zero-latency**: No added latency from convolution — the head partition runs in real time.
- **IR crossfade**: 50 ms S-curve crossfade when swapping IRs, preventing clicks on parameter changes.
- **Rate-limited reloads**: Minimum 300 ms between IR rebuilds to prevent CPU spikes during slider automation.
- **Filters**: Butterworth IIR (HP/LP), transposed Direct Form II. Coefficients updated every 32 samples for efficient automation.
- **Fredman comb**: 7-tap circular delay buffer with wet/dry blend.
- **M/S summing**: Per-sample bus accumulation with fast path (no M/S overhead) when all loaders are set to ST.
- **Parallel compensation**: 1/√N gain correction per routing mode (N = number of parallel paths).

### FFT Backend
- **FFTW** (preferred): Auto-loaded from system DLL on Windows. Provides optimized FFT for convolution.
- **Fallback**: JUCE `juce::dsp::FFT` when FFTW is unavailable.

### State Persistence
- All parameters saved via JUCE AudioProcessorValueTreeState.
- IR file paths stored in the plugin state (irPathA, irPathB, irPathC) — reloaded on session restore.
- UI state (window size, palette, CRT toggle, custom colours) stored as non-automatable APVTS parameters — persists across sessions but hidden from the DAW automation list.
- IO section expanded/collapsed state persisted as a ValueTree property.
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
- Added dual-stage transparent peak limiter with LIM THRESHOLD (−36 to 0 dB) and LIM MODE (NONE/WET/GLOBAL). Stereo-linked gain reduction with 2 ms/10 ms leveler + instant/100 ms brickwall stages.
