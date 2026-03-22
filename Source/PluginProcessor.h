#pragma once

#include <JuceHeader.h>
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

	// ══════════════════════════════════════════════════════════════
	//  Parameter IDs — IR Loader B
	// ══════════════════════════════════════════════════════════════
	static constexpr const char* kParamEnableB      = "enable_b";
	static constexpr const char* kParamFilePathB    = "file_path_b";
	static constexpr const char* kParamHpFreqB      = "hp_freq_b";
	static constexpr const char* kParamLpFreqB      = "lp_freq_b";
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

	// ══════════════════════════════════════════════════════════════
	//  Global Parameters
	// ══════════════════════════════════════════════════════════════
	static constexpr const char* kParamInput        = "input";
	static constexpr const char* kParamOutput       = "output";
	static constexpr const char* kParamModeIn       = "mode_in";   // 0=L+R, 1=MID, 2=SIDE (input)
	static constexpr const char* kParamMode         = "mode";      // 0=L+R, 1=MID, 2=SIDE (output)
	static constexpr const char* kParamRoute        = "route";     // 0=A->B (series), 1=A|B (parallel)
	static constexpr const char* kParamAlign        = "align";     // Auto phase alignment
	static constexpr const char* kParamMix          = "mix";       // Global dry/wet mix

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
	//  Parameter Ranges & Defaults — Filters (12dB/oct)
	// ══════════════════════════════════════════════════════════════
	static constexpr float kFilterFreqMin           = 20.0f;
	static constexpr float kFilterFreqMax           = 20000.0f;
	static constexpr float kFilterHpFreqDefault     = 80.0f;
	static constexpr float kFilterLpFreqDefault     = 12000.0f;

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

	static constexpr int   kRouteMin                = 0;
	static constexpr int   kRouteMax                = 1;          // A->B or A|B
	static constexpr int   kRouteDefault            = 1;          // Parallel by default

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
		juce::dsp::Convolution convolution { juce::dsp::Convolution::NonUniform { 256 } };
		std::atomic<bool> needsUpdate { false };
		juce::String currentFilePath;
		
		// Filter states (12dB/oct = 2-pole Butterworth)
		juce::dsp::ProcessorDuplicator<juce::dsp::IIR::Filter<float>, 
		                                juce::dsp::IIR::Coefficients<float>> hpFilter;
		juce::dsp::ProcessorDuplicator<juce::dsp::IIR::Filter<float>, 
		                                juce::dsp::IIR::Coefficients<float>> lpFilter;
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
		
		// Delete copy operations (contains atomic)
		IRLoaderState() = default;
		IRLoaderState (const IRLoaderState&) = delete;
		IRLoaderState& operator= (const IRLoaderState&) = delete;
		IRLoaderState (IRLoaderState&&) = default;
		IRLoaderState& operator= (IRLoaderState&&) = default;
	};

	IRLoaderState stateA;
	IRLoaderState stateB;

	// ══════════════════════════════════════════════════════════════
	//  Helper methods (public for editor to trigger IR loading)
	// ══════════════════════════════════════════════════════════════
	void loadImpulseResponse (IRLoaderState& state, const juce::String& filePath);

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
	std::atomic<float>* pModeIn = nullptr;
	std::atomic<float>* pMode = nullptr;
	std::atomic<float>* pRoute = nullptr;
	std::atomic<float>* pMix = nullptr;
	std::atomic<float>* pInput = nullptr;
	std::atomic<float>* pOutput = nullptr;
	std::atomic<float>* pHpFreqA = nullptr;
	std::atomic<float>* pLpFreqA = nullptr;
	std::atomic<float>* pDelayA = nullptr;
	std::atomic<float>* pPanA = nullptr;
	std::atomic<float>* pFredA = nullptr;
	std::atomic<float>* pPosA = nullptr;
	std::atomic<float>* pOutA = nullptr;
	std::atomic<float>* pHpFreqB = nullptr;
	std::atomic<float>* pLpFreqB = nullptr;
	std::atomic<float>* pDelayB = nullptr;
	std::atomic<float>* pPanB = nullptr;
	std::atomic<float>* pFredB = nullptr;
	std::atomic<float>* pPosB = nullptr;
	std::atomic<float>* pOutB = nullptr;

	// Reusable format manager (avoid re-creating on every IR load)
	juce::AudioFormatManager formatManager;

	// ══════════════════════════════════════════════════════════════
	//  DSP Helpers — Reusable buffers to avoid allocations
	// ══════════════════════════════════════════════════════════════
	juce::AudioBuffer<float> tempBufferA;
	juce::AudioBuffer<float> tempBufferB;
	juce::AudioBuffer<float> globalDryBuffer;  // For global dry/wet MIX
	
	// Debug logging
	std::atomic<int> reloadCountA { 0 };
	std::atomic<int> reloadCountB { 0 };
	juce::int64 lastDebugTime = 0;
	
	// CPU profiling: track worst-case processBlock time
	std::atomic<double> worstBlockTimeUs { 0.0 };
	std::atomic<double> worstConvTimeUs { 0.0 }; // convolution-only timing
	std::atomic<int> blockCount { 0 };

	// ══════════════════════════════════════════════════════════════
	//  Private Helper methods
	// ══════════════════════════════════════════════════════════════
	void processLoader (IRLoaderState& state, 
	                    juce::AudioBuffer<float>& buffer,
	                    const char* paramPrefix,
	                    bool isA);
	void applyPitchShift (juce::AudioBuffer<float>& buffer, float pitchRatio);
	void applyPositionEffect (juce::AudioBuffer<float>& buffer, float position);
	void applyDelay (juce::AudioBuffer<float>& buffer, float delayMs, bool isA);
	void calculateAutoAlignment();

	JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CABTRAudioProcessor)
};
