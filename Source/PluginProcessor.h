#pragma once

#include <JuceHeader.h>
#include "ThreadedConvolver.h"
#include <array>
#include <atomic>
#include <vector>

class CABTRAudioProcessor : public juce::AudioProcessor,
                             private juce::Timer
{
public:
	CABTRAudioProcessor();
	~CABTRAudioProcessor() override;

	// ══════════════════════════════════════════════════════════════
	//  Parameter IDs — IR Loader A
	// ══════════════════════════════════════════════════════════════
	static constexpr const char* kParamEnableA      = "enable_a";
	static constexpr const char* kParamFilePathA    = "file_path_a";
	static constexpr const char* kParamHpFreqA      = "hp_freq_a";
	static constexpr const char* kParamLpFreqA      = "lp_freq_a";
	static constexpr const char* kParamHpOnA        = "hp_on_a";
	static constexpr const char* kParamLpOnA        = "lp_on_a";
	static constexpr const char* kParamHpSlopeA     = "hp_slope_a";
	static constexpr const char* kParamLpSlopeA     = "lp_slope_a";
	static constexpr const char* kParamOutA         = "out_a";
	static constexpr const char* kParamStartA       = "start_a";
	static constexpr const char* kParamEndA         = "end_a";
	static constexpr const char* kParamPitchA       = "pitch_a";
	static constexpr const char* kParamDelayA       = "delay_a";
	static constexpr const char* kParamPanA         = "pan_a";
	static constexpr const char* kParamFredA        = "fred_a";
	static constexpr const char* kParamPosA         = "pos_a";
	static constexpr const char* kParamInvA         = "inv_a";
	static constexpr const char* kParamNormA        = "norm_a";
	static constexpr const char* kParamRvsA         = "rvs_a";
	static constexpr const char* kParamChaosA       = "chaos_a";
	static constexpr const char* kParamChaosAmtA    = "chaos_amt_a";
	static constexpr const char* kParamChaosSpdA    = "chaos_spd_a";
	static constexpr const char* kParamModeInA      = "mode_in_a";  // 0=L+R, 1=MID, 2=SIDE
	static constexpr const char* kParamModeOutA     = "mode_out_a"; // 0=L+R, 1=MID, 2=SIDE
	static constexpr const char* kParamSumBusA      = "sum_bus_a"; // 0=ST, 1=→M, 2=→S
	static constexpr const char* kParamMixA         = "mix_a";     // Per-loader dry/wet

	// ══════════════════════════════════════════════════════════════
	//  Parameter IDs — IR Loader B
	// ══════════════════════════════════════════════════════════════
	static constexpr const char* kParamEnableB      = "enable_b";
	static constexpr const char* kParamFilePathB    = "file_path_b";
	static constexpr const char* kParamHpFreqB      = "hp_freq_b";
	static constexpr const char* kParamLpFreqB      = "lp_freq_b";
	static constexpr const char* kParamHpOnB        = "hp_on_b";
	static constexpr const char* kParamLpOnB        = "lp_on_b";
	static constexpr const char* kParamHpSlopeB     = "hp_slope_b";
	static constexpr const char* kParamLpSlopeB     = "lp_slope_b";
	static constexpr const char* kParamOutB         = "out_b";
	static constexpr const char* kParamStartB       = "start_b";
	static constexpr const char* kParamEndB         = "end_b";
	static constexpr const char* kParamPitchB       = "pitch_b";
	static constexpr const char* kParamDelayB       = "delay_b";
	static constexpr const char* kParamPanB         = "pan_b";
	static constexpr const char* kParamFredB        = "fred_b";
	static constexpr const char* kParamPosB         = "pos_b";
	static constexpr const char* kParamInvB         = "inv_b";
	static constexpr const char* kParamNormB        = "norm_b";
	static constexpr const char* kParamRvsB         = "rvs_b";
	static constexpr const char* kParamChaosB       = "chaos_b";
	static constexpr const char* kParamChaosAmtB    = "chaos_amt_b";
	static constexpr const char* kParamChaosSpdB    = "chaos_spd_b";
	static constexpr const char* kParamModeInB      = "mode_in_b";  // 0=L+R, 1=MID, 2=SIDE
	static constexpr const char* kParamModeOutB     = "mode_out_b"; // 0=L+R, 1=MID, 2=SIDE
	static constexpr const char* kParamSumBusB      = "sum_bus_b"; // 0=ST, 1=→M, 2=→S
	static constexpr const char* kParamMixB         = "mix_b";     // Per-loader dry/wet

	// ══════════════════════════════════════════════════════════════
	//  Parameter IDs — IR Loader C
	// ══════════════════════════════════════════════════════════════
	static constexpr const char* kParamEnableC      = "enable_c";
	static constexpr const char* kParamFilePathC    = "file_path_c";
	static constexpr const char* kParamHpFreqC      = "hp_freq_c";
	static constexpr const char* kParamLpFreqC      = "lp_freq_c";
	static constexpr const char* kParamHpOnC        = "hp_on_c";
	static constexpr const char* kParamLpOnC        = "lp_on_c";
	static constexpr const char* kParamHpSlopeC     = "hp_slope_c";
	static constexpr const char* kParamLpSlopeC     = "lp_slope_c";
	static constexpr const char* kParamOutC         = "out_c";
	static constexpr const char* kParamStartC       = "start_c";
	static constexpr const char* kParamEndC         = "end_c";
	static constexpr const char* kParamPitchC       = "pitch_c";
	static constexpr const char* kParamDelayC       = "delay_c";
	static constexpr const char* kParamPanC         = "pan_c";
	static constexpr const char* kParamFredC        = "fred_c";
	static constexpr const char* kParamPosC         = "pos_c";
	static constexpr const char* kParamInvC         = "inv_c";
	static constexpr const char* kParamNormC        = "norm_c";
	static constexpr const char* kParamRvsC         = "rvs_c";
	static constexpr const char* kParamChaosC       = "chaos_c";
	static constexpr const char* kParamChaosAmtC    = "chaos_amt_c";
	static constexpr const char* kParamChaosSpdC    = "chaos_spd_c";
	static constexpr const char* kParamModeInC      = "mode_in_c";
	static constexpr const char* kParamModeOutC     = "mode_out_c";
	static constexpr const char* kParamSumBusC      = "sum_bus_c"; // 0=ST, 1=→M, 2=→S
	static constexpr const char* kParamMixC         = "mix_c";

	// ══════════════════════════════════════════════════════════════
	//  Global Parameters
	// ══════════════════════════════════════════════════════════════
	static constexpr const char* kParamInput        = "input";
	static constexpr const char* kParamOutput       = "output";
	static constexpr const char* kParamRoute        = "route";     // 0=A->B->C, 1=A|B|C, 2=A->B|C, 3=A|B->C
	static constexpr const char* kParamAlign        = "align";     // Auto phase alignment
	static constexpr const char* kParamMix          = "mix";       // Global dry/wet mix
	static constexpr const char* kParamMatch        = "match";
	static constexpr const char* kParamTrim         = "trim";     // Tilt EQ profile (0=None..5=Bright+)

	// ══════════════════════════════════════════════════════════════
	//  UI State Parameters (hidden from DAW automation)
	// ══════════════════════════════════════════════════════════════
	static constexpr const char* kParamUiWidth      = "ui_width";
	static constexpr const char* kParamUiHeight     = "ui_height";
	static constexpr const char* kParamUiPalette    = "ui_palette";
	static constexpr const char* kParamUiFxTail     = "ui_fx_tail";
	static constexpr const char* kParamUiColor0     = "ui_color0";
	static constexpr const char* kParamUiColor1     = "ui_color1";

	// ══════════════════════════════════════════════════════════════
	//  UI State Keys (non-automatable, stored in APVTS tree)
	// ══════════════════════════════════════════════════════════════
	struct UiStateKeys
	{
		static constexpr const char* ioExpanded = "uiIoExpanded";
	};

	void setUiIoExpanded (bool expanded);
	bool getUiIoExpanded() const noexcept;

	// ══════════════════════════════════════════════════════════════
	//  Parameter Ranges & Defaults — Filters
	// ══════════════════════════════════════════════════════════════
	static constexpr float kFilterFreqMin           = 20.0f;
	static constexpr float kFilterFreqMax           = 20000.0f;
	static constexpr float kFilterHpFreqDefault     = 80.0f;
	static constexpr float kFilterLpFreqDefault     = 12000.0f;
	static constexpr int   kFilterSlopeMin          = 0;       // 6 dB/oct
	static constexpr int   kFilterSlopeMax          = 2;       // 24 dB/oct
	static constexpr int   kFilterSlopeDefault      = 1;       // 12 dB/oct

	// Butterworth Q constants for 24 dB/oct (4th-order cascaded pair)
	static constexpr float kBW4_Q1 = 0.54119610f;   // 1/(2cos(3π/8))
	static constexpr float kBW4_Q2 = 1.30656296f;   // 1/(2cos(π/8))

	// ══════════════════════════════════════════════════════════════
	//  Parameter Ranges & Defaults — IR Controls
	// ══════════════════════════════════════════════════════════════
	static constexpr float kOutMin                  = -100.0f;
	static constexpr float kOutMax                  = 24.0f;
	static constexpr float kOutDefault              = 0.0f;

	static constexpr float kStartMin                = 0.0f;
	static constexpr float kStartMax                = 10000.0f; // 10 seconds max IR
	static constexpr float kStartDefault            = 0.0f;

	static constexpr float kEndMin                  = 0.0f;
	static constexpr float kEndMax                  = 10000.0f;
	static constexpr float kEndDefault              = 10000.0f;

	static constexpr float kPitchMin                = 0.25f;      // 25%
	static constexpr float kPitchMax                = 4.0f;       // 400%
	static constexpr float kPitchDefault            = 1.0f;       // 100%

	static constexpr float kDelayMin                = 0.0f;
	static constexpr float kDelayMax                = 1000.0f;    // ms
	static constexpr float kDelayDefault            = 0.0f;

	static constexpr float kPanMin                  = 0.0f;       // 0% = full left
	static constexpr float kPanMax                  = 1.0f;       // 100% = full right
	static constexpr float kPanDefault              = 0.5f;       // 50% = center

	static constexpr float kChaosAmtMin              = 0.0f;
	static constexpr float kChaosAmtMax              = 100.0f;
	static constexpr float kChaosAmtDefault          = 50.0f;
	static constexpr float kChaosSpdMin              = 0.01f;      // Hz
	static constexpr float kChaosSpdMax              = 100.0f;     // Hz
	static constexpr float kChaosSpdDefault          = 5.0f;       // Hz

	static constexpr float kFredMin                 = 0.0f;
	static constexpr float kFredMax                 = 1.0f;
	static constexpr float kFredDefault             = 0.0f;       // 0% = no Fredman effect

	static constexpr float kGlobalMixMin            = 0.0f;
	static constexpr float kGlobalMixMax            = 1.0f;
	static constexpr float kGlobalMixDefault        = 1.0f;       // 100% wet by default

	static constexpr float kPosMin                  = 0.0f;       // 0% = no effect
	static constexpr float kPosMax                  = 1.0f;       // 100% = full Friedman simulation
	static constexpr float kPosDefault              = 0.0f;

	// ══════════════════════════════════════════════════════════════
	//  Parameter Ranges & Defaults — Global
	// ══════════════════════════════════════════════════════════════
	static constexpr float kInputMin                = -100.0f;
	static constexpr float kInputMax                = 0.0f;
	static constexpr float kInputDefault            = 0.0f;

	static constexpr float kOutputMin               = -100.0f;
	static constexpr float kOutputMax               = 24.0f;
	static constexpr float kOutputDefault           = 0.0f;

	static constexpr int   kModeMin                 = 0;
	static constexpr int   kModeMax                 = 2;          // L+R, MID, SIDE
	static constexpr int   kModeDefault             = 0;          // L+R

	static constexpr int   kSumBusMax               = 2;          // 0=ST, 1=→M, 2=→S
	static constexpr int   kSumBusDefault           = 0;          // ST (unchanged)

	static constexpr int   kRouteMin                = 0;
	static constexpr int   kRouteMax                = 3;          // 0=A->B->C, 1=A|B|C, 2=A->B|C, 3=A|B->C
	static constexpr int   kRouteDefault            = 1;          // Parallel by default
	static constexpr int   kMatchDefault            = 0;
	static constexpr int   kTrimDefault             = 0;          // None (no tilt)

	// ══════════════════════════════════════════════════════════════
	//  AudioProcessor overrides
	// ══════════════════════════════════════════════════════════════
	void prepareToPlay (double sampleRate, int samplesPerBlock) override;
	void releaseResources() override;

#if ! JucePlugin_PreferredChannelConfigurations
	bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
#endif

	void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

	juce::AudioProcessorEditor* createEditor() override;
	bool hasEditor() const override;

	const juce::String getName() const override;

	bool acceptsMidi() const override;
	bool producesMidi() const override;
	bool isMidiEffect() const override;
	double getTailLengthSeconds() const override;

	int getNumPrograms() override;
	int getCurrentProgram() override;
	void setCurrentProgram (int index) override;
	const juce::String getProgramName (int index) override;
	void changeProgramName (int index, const juce::String& newName) override;

	void getStateInformation (juce::MemoryBlock& destData) override;
	void setStateInformation (const void* data, int sizeInBytes) override;

	// ══════════════════════════════════════════════════════════════
	//  Public API — Parameter access
	// ══════════════════════════════════════════════════════════════
	juce::AudioProcessorValueTreeState& getValueTreeState() { return parameters; }

	// ══════════════════════════════════════════════════════════════
	//  UI State Accessors
	// ══════════════════════════════════════════════════════════════
	bool getUiUseCustomPalette() const
	{
		if (auto* p = parameters.getRawParameterValue (kParamUiPalette))
			return static_cast<int> (*p) != 0;
		return false;
	}

	void setUiUseCustomPalette (bool useCustom)
	{
		if (auto* p = parameters.getParameter (kParamUiPalette))
			p->setValueNotifyingHost (useCustom ? 1.0f : 0.0f);
	}

	bool getUiFxTailEnabled() const
	{
		if (auto* p = parameters.getRawParameterValue (kParamUiFxTail))
			return static_cast<bool> (*p);
		return false;
	}

	void setUiFxTailEnabled (bool enabled)
	{
		if (auto* p = parameters.getParameter (kParamUiFxTail))
			p->setValueNotifyingHost (enabled ? 1.0f : 0.0f);
	}

	juce::Colour getUiCustomPaletteColour (int index) const
	{
		if (index < 0 || index > 1)
			return juce::Colours::green;

		const char* paramId = (index == 0) ? kParamUiColor0 : kParamUiColor1;
		if (auto* p = parameters.getRawParameterValue (paramId))
		{
			const auto rgb = static_cast<juce::uint32> (*p);
			return juce::Colour (0xFF000000 | rgb);
		}
		return juce::Colours::green;
	}

	void setUiCustomPaletteColour (int index, juce::Colour colour)
	{
		if (index < 0 || index > 1)
			return;

		const char* paramId = (index == 0) ? kParamUiColor0 : kParamUiColor1;
		if (auto* p = parameters.getParameter (paramId))
		{
			const auto rgb = colour.getARGB() & 0x00FFFFFF;
			const float normalized = static_cast<float> (rgb) / static_cast<float> (0xFFFFFF);
			p->setValueNotifyingHost (normalized);
		}
	}

	// ══════════════════════════════════════════════════════════════
	//  DSP State — IR Convolution (public for editor access)
	// ══════════════════════════════════════════════════════════════
	struct IRLoaderState
	{
		juce::AudioBuffer<float> impulseResponse;
		StereoThreadedConvolver convolution;
		std::atomic<bool> needsUpdate { false };
		juce::String currentFilePath;
		double irSampleRate = 44100.0;  // Sample rate of stored impulseResponse buffer
		
		// Filter states (6/12/24 dB/oct slope-selectable)
		juce::dsp::ProcessorDuplicator<juce::dsp::IIR::Filter<float>, 
		                                juce::dsp::IIR::Coefficients<float>> hpFilter;
		juce::dsp::ProcessorDuplicator<juce::dsp::IIR::Filter<float>, 
		                                juce::dsp::IIR::Coefficients<float>> hpFilter2;  // 2nd stage for 24dB/oct
		juce::dsp::ProcessorDuplicator<juce::dsp::IIR::Filter<float>, 
		                                juce::dsp::IIR::Coefficients<float>> lpFilter;
		juce::dsp::ProcessorDuplicator<juce::dsp::IIR::Filter<float>, 
		                                juce::dsp::IIR::Coefficients<float>> lpFilter2;  // 2nd stage for 24dB/oct
		juce::dsp::ProcessorDuplicator<juce::dsp::IIR::Filter<float>, 
		                                juce::dsp::IIR::Coefficients<float>> posFilter;
		
		// Delay line for phase alignment (max 1 second)
		juce::dsp::DelayLine<float, juce::dsp::DelayLineInterpolationTypes::Linear> delayLine { 192000 };
		juce::SmoothedValue<float> smoothedDelay { 0.0f }; // Suavizado para delay
		
		// Cache last parameter values to detect changes and reload IR
		std::atomic<float> lastPitch { 1.0f };
		std::atomic<bool> lastInv { false };
		std::atomic<bool> lastNorm { false };
		std::atomic<bool> lastRvs { false };
		std::atomic<float> lastStart { 0.0f };
		std::atomic<float> lastEnd { 10000.0f };
		
		// Rate-limiting: minimum interval between reloads to avoid overload during slider drag
		juce::int64 lastReloadTime { 0 };
		
		// EMA-smoothed filter frequencies (per-sample smoothing, coeff update every 32 samples)
		float smoothedHpFreq = 80.0f;
		float smoothedLpFreq = 12000.0f;
		float smoothedPosFreq = -1.0f;
		float lastHpFreq = -1.0f;
		float lastLpFreq = -1.0f;
		int lastHpSlope = -1;
		int lastLpSlope = -1;
		float lastPosFreq = -1.0f;
		int filterCoeffCountdown = 0;
		static constexpr int kFilterCoeffUpdateInterval = 32;
		float lastPan = -1.0f;
		float lastPanLeft = 1.0f;
		float lastPanRight = 1.0f;
		
		// FRED (Fredman miking) state: circular delay buffer for off-axis simulation
		// 7 samples = ~0.15ms @ 48kHz ≈ 5cm path difference (realistic Fredman setup)
		// First comb null at ~6.8kHz — musically useful tonal shaping
		static constexpr int kFredDelaySamples = 7;
		float fredDelayBuffer[2][kFredDelaySamples] = {};
		int fredDelayIndex = 0;

		// CHAOS (S&H micro-delay pitch modulation)
		// Micro-delay line: max ±5ms @ 192kHz = 960 samples, round to 1024
		static constexpr int kChaosDelayMaxSamples = 1024;
		float chaosDelayBuffer[2][kChaosDelayMaxSamples] = {};
		int chaosDelayWritePos = 0;
		float chaosCurrentTarget = 0.0f;    // S&H target (pitch): -1..+1
		float chaosSmoothedValue = 0.0f;    // EMA-smoothed pitch value
		float chaosPhaseSamples = 0.0f;     // phase accumulator for pitch S&H
		juce::Random chaosRng;

		// CHAOS gain S&H (independent from pitch S&H)
		float chaosGainTarget = 0.0f;       // S&H target (gain): -1..+1
		float chaosGainSmoothed = 0.0f;     // EMA-smoothed gain value
		float chaosGainPhase = 0.0f;        // phase accumulator for gain S&H
		juce::Random chaosGainRng;

		// Spectral slope of this IR (dB/octave), measured over 100Hz-10kHz
		std::atomic<float> irSlopeDbPerOct { 0.0f };
		
		// Delete copy operations (contains atomic)
		IRLoaderState() = default;
		IRLoaderState (const IRLoaderState&) = delete;
		IRLoaderState& operator= (const IRLoaderState&) = delete;
		IRLoaderState (IRLoaderState&&) = default;
		IRLoaderState& operator= (IRLoaderState&&) = default;
	};

	IRLoaderState stateA;
	IRLoaderState stateB;
	IRLoaderState stateC;

	// ══════════════════════════════════════════════════════════════
	//  Helper methods (public for editor to trigger IR loading)
	// ══════════════════════════════════════════════════════════════
	void loadImpulseResponse (IRLoaderState& state, const juce::String& filePath);
	bool exportCombinedIR (double targetSampleRate, int formatType,
	                       double maxLengthSec, bool trimSilence,
	                       bool normalizeOutput,
	                       const juce::File& outputFile);

private:
	// Timer callback to monitor parameter changes
	void timerCallback() override;
	// ══════════════════════════════════════════════════════════════
	//  Parameter State
	// ══════════════════════════════════════════════════════════════
	juce::AudioProcessorValueTreeState parameters;
	juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

	double currentSampleRate = 44100.0;
	int currentBlockSize = 512;

	// Cached raw parameter pointers (resolved once in prepareToPlay, avoids hash lookup per block)
	std::atomic<float>* pEnableA = nullptr;
	std::atomic<float>* pEnableB = nullptr;
	std::atomic<float>* pEnableC = nullptr;
	std::atomic<float>* pRoute = nullptr;
	std::atomic<float>* pMix = nullptr;
	std::atomic<float>* pInput = nullptr;
	std::atomic<float>* pOutput = nullptr;
	std::atomic<float>* pHpFreqA = nullptr;
	std::atomic<float>* pLpFreqA = nullptr;
	std::atomic<float>* pHpOnA = nullptr;
	std::atomic<float>* pLpOnA = nullptr;
	std::atomic<float>* pHpSlopeA = nullptr;
	std::atomic<float>* pLpSlopeA = nullptr;
	std::atomic<float>* pDelayA = nullptr;
	std::atomic<float>* pPanA = nullptr;
	std::atomic<float>* pFredA = nullptr;
	std::atomic<float>* pPosA = nullptr;
	std::atomic<float>* pOutA = nullptr;
	std::atomic<float>* pHpFreqB = nullptr;
	std::atomic<float>* pLpFreqB = nullptr;
	std::atomic<float>* pHpOnB = nullptr;
	std::atomic<float>* pLpOnB = nullptr;
	std::atomic<float>* pHpSlopeB = nullptr;
	std::atomic<float>* pLpSlopeB = nullptr;
	std::atomic<float>* pDelayB = nullptr;
	std::atomic<float>* pPanB = nullptr;
	std::atomic<float>* pFredB = nullptr;
	std::atomic<float>* pPosB = nullptr;
	std::atomic<float>* pOutB = nullptr;
	std::atomic<float>* pChaosA = nullptr;
	std::atomic<float>* pChaosAmtA = nullptr;
	std::atomic<float>* pChaosSpdA = nullptr;
	std::atomic<float>* pModeInA = nullptr;
	std::atomic<float>* pModeOutA = nullptr;
	std::atomic<float>* pSumBusA = nullptr;
	std::atomic<float>* pChaosB = nullptr;
	std::atomic<float>* pChaosAmtB = nullptr;
	std::atomic<float>* pChaosSpdB = nullptr;
	std::atomic<float>* pModeInB = nullptr;
	std::atomic<float>* pModeOutB = nullptr;
	std::atomic<float>* pSumBusB = nullptr;
	std::atomic<float>* pMixA = nullptr;
	std::atomic<float>* pMixB = nullptr;
	std::atomic<float>* pHpFreqC = nullptr;
	std::atomic<float>* pLpFreqC = nullptr;
	std::atomic<float>* pHpOnC = nullptr;
	std::atomic<float>* pLpOnC = nullptr;
	std::atomic<float>* pHpSlopeC = nullptr;
	std::atomic<float>* pLpSlopeC = nullptr;
	std::atomic<float>* pDelayC = nullptr;
	std::atomic<float>* pPanC = nullptr;
	std::atomic<float>* pFredC = nullptr;
	std::atomic<float>* pPosC = nullptr;
	std::atomic<float>* pOutC = nullptr;
	std::atomic<float>* pChaosC = nullptr;
	std::atomic<float>* pChaosAmtC = nullptr;
	std::atomic<float>* pChaosSpdC = nullptr;
	std::atomic<float>* pModeInC = nullptr;
	std::atomic<float>* pModeOutC = nullptr;
	std::atomic<float>* pSumBusC = nullptr;
	std::atomic<float>* pMixC = nullptr;
	std::atomic<float>* pMatch = nullptr;
	std::atomic<float>* pTrim  = nullptr;

	// Tilt EQ filter state (1st-order shelf, per-channel)
	float tiltState_[2] = { 0.0f, 0.0f };
	int   tiltLastProfile_ = -1;
	float tiltLastSlope_   = 0.0f;  // last applied compensating slope
	float tiltB0_ = 1.0f, tiltB1_ = 0.0f, tiltA1_ = 0.0f;        // current (smoothed)
	float tiltTargetB0_ = 1.0f, tiltTargetB1_ = 0.0f, tiltTargetA1_ = 0.0f; // target

	// Wet NORM AGC state (peak follower + gain smoothing)
	float normPeakFollower_  = 0.0f;
	float normSmoothedGain_  = 1.0f;

	// Reusable format manager (avoid re-creating on every IR load)
	juce::AudioFormatManager formatManager;

	// ══════════════════════════════════════════════════════════════
	//  DSP Helpers — Reusable buffers to avoid allocations
	// ══════════════════════════════════════════════════════════════
	juce::AudioBuffer<float> tempBufferA;
	juce::AudioBuffer<float> tempBufferB;
	juce::AudioBuffer<float> tempBufferC;
	juce::AudioBuffer<float> globalDryBuffer;  // For global dry/wet MIX
	juce::AudioBuffer<float> loaderDryBuffer;  // For per-loader dry/wet MIX
	
	// Debug logging
	std::atomic<int> reloadCountA { 0 };
	std::atomic<int> reloadCountB { 0 };
	std::atomic<int> reloadCountC { 0 };
	juce::int64 lastDebugTime = 0;
	juce::int64 lastAlignTime = 0;
	
	// CPU profiling: track worst-case processBlock time
	std::atomic<double> worstBlockTimeUs { 0.0 };
	std::atomic<double> worstConvTimeUs { 0.0 }; // convolution-only timing
	std::atomic<int> blockCount { 0 };

	// Peak level tracking (per-block max absolute value)
	std::atomic<float> peakInputLevel  { 0.0f };
	std::atomic<float> peakOutputLevel { 0.0f };
	std::atomic<int>   clipCount       { 0 };    // blocks where output > 1.0

	// Per-loader process time (worst in window)
	std::atomic<double> worstLoaderAUs { 0.0 };
	std::atomic<double> worstLoaderBUs { 0.0 };
	std::atomic<double> worstLoaderCUs { 0.0 };

	// DC blocking filter state (per-channel)
	float dcBlockX_[2] = { 0.0f, 0.0f };
	float dcBlockY_[2] = { 0.0f, 0.0f };

	// ══════════════════════════════════════════════════════════════
	//  Private Helper methods
	// ══════════════════════════════════════════════════════════════
	// loaderIndex: 0=A, 1=B, 2=C
	void processLoader (IRLoaderState& state, 
	                    juce::AudioBuffer<float>& buffer,
	                    int loaderIndex);
	void applyDelay (juce::AudioBuffer<float>& buffer, float delayMs, int loaderIndex);
	void calculateAutoAlignment();
	void offlineProcessLoaderEffects (juce::AudioBuffer<float>& buffer, int loaderIndex, double sampleRate,
	                                  int modeIn, int modeOut);

	JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CABTRAudioProcessor)
};
