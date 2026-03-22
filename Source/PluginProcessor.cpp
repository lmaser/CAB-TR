#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "DspDebugLog.h"

#if JUCE_DSP_USE_SHARED_FFTW
  #define CABTR_FFTW_STATUS "shared"
#elif JUCE_DSP_USE_STATIC_FFTW
  #define CABTR_FFTW_STATUS "static"
#else
  #define CABTR_FFTW_STATUS "off(fallback)"
#endif

// ---------------------------------------------------------------------------
// FFTW DLL loading helpers (Windows only, shared FFTW mode)
// Searches multiple candidate directories for libfftw3f.dll and pre-loads
// it so that JUCE's DynamicLibrary::open finds it at runtime.
// ---------------------------------------------------------------------------
#if JUCE_WINDOWS && JUCE_DSP_USE_SHARED_FFTW
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #include <windows.h>

 namespace {
   static juce::String g_fftwDiag;

   static bool tryLoadFrom (const wchar_t* dir, const wchar_t* label)
   {
       wchar_t full[MAX_PATH];
       wcscpy_s (full, MAX_PATH, dir);
       wcscat_s (full, MAX_PATH, L"libfftw3f.dll");

       if (GetFileAttributesW (full) == INVALID_FILE_ATTRIBUTES)
       {
           g_fftwDiag += juce::String (label) + ": not found\n";
           return false;
       }

       HMODULE h = LoadLibraryW (full);
       if (h != nullptr)
       {
           g_fftwDiag += juce::String (label) + ": LOADED from " + juce::String (full) + "\n";
           // Also add directory to search path so JUCE's LoadLibrary("libfftw3f.dll") works
           SetDllDirectoryW (dir);
           return true;
       }
       g_fftwDiag += juce::String (label) + ": file exists but LoadLibrary FAILED(err="
                     + juce::String ((int) GetLastError()) + ") " + juce::String (full) + "\n";
       return false;
   }

   static void ensureFFTWLoaded()
   {
       // Already loaded?
       if (GetModuleHandleW (L"libfftw3f.dll") != nullptr)
       {
           g_fftwDiag = "FFTW: already loaded in process";
           return;
       }

       g_fftwDiag = "";

       // 1) Our own module directory (works for both VST3 and Standalone)
       {
           static int anchor = 0;
           HMODULE hm = nullptr;
           wchar_t modDir[MAX_PATH];
           if (GetModuleHandleExW (
                   GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                   reinterpret_cast<LPCWSTR> (&anchor), &hm) &&
               GetModuleFileNameW (hm, modDir, MAX_PATH))
           {
               wchar_t* sep = wcsrchr (modDir, L'\\');
               if (sep) *(sep + 1) = L'\0';
               if (tryLoadFrom (modDir, L"[module-dir]")) return;

               // 1b) Parent of module dir (for VST3 bundles: .../Contents/x86_64-win/../)
               wchar_t parent[MAX_PATH];
               wcscpy_s (parent, MAX_PATH, modDir);
               if (sep) *sep = L'\0'; // remove trailing backslash
               wchar_t* sep2 = wcsrchr (parent, L'\\');
               if (sep2) { *(sep2 + 1) = L'\0'; if (tryLoadFrom (parent, L"[module-parent]")) return; }
           }
       }

       // 2) ThirdParty/fftw3 relative to the source tree
       //    (useful during development — the DLL lives in the repo)
       {
           wchar_t tp[MAX_PATH] = L"E:\\Workspace\\Production\\JUCE_projects\\CAB-TR\\ThirdParty\\fftw3\\";
           if (tryLoadFrom (tp, L"[thirdparty]")) return;
       }

       // 3) The VST3 install directory under Program Files
       {
           wchar_t pf[MAX_PATH] = L"C:\\Program Files\\Common Files\\VST3\\NEMESTER PLUGINS\\TR SERIES\\1.3\\";
           if (tryLoadFrom (pf, L"[programfiles]")) return;
       }

       g_fftwDiag += "FFTW: NOT FOUND in any candidate directory";
   }
 }
#endif

//==============================================================================
CABTRAudioProcessor::CABTRAudioProcessor()
#ifndef JucePlugin_PreferredChannelConfigurations
     : AudioProcessor (BusesProperties()
                     #if ! JucePlugin_IsMidiEffect
                      #if ! JucePlugin_IsSynth
                       .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                      #endif
                       .withOutput ("Output", juce::AudioChannelSet::stereo(), true)
                     #endif
                       ),
#else
     :
#endif
      parameters (*this, nullptr, juce::Identifier ("CABTRState"), createParameterLayout())
{
	// Initialize convolution processors
	stateA.convolution.reset();
	stateB.convolution.reset();
	
	// Register audio formats once
	formatManager.registerBasicFormats();
	
	// Start periodic timer to check for parameter changes (1000ms = 1 time per second)
	// OPTIMIZED: Debounced to avoid excessive reloads when user drags sliders
	startTimer (200); // Check every 200ms for parameter changes (rate-limited to 300ms min interval)
}

CABTRAudioProcessor::~CABTRAudioProcessor()
{
}

//==============================================================================
const juce::String CABTRAudioProcessor::getName() const
{
    return JucePlugin_Name;
}

bool CABTRAudioProcessor::acceptsMidi() const
{
   #if JucePlugin_WantsMidiInput
    return true;
   #else
    return false;
   #endif
}

bool CABTRAudioProcessor::producesMidi() const
{
   #if JucePlugin_ProducesMidiOutput
    return true;
   #else
    return false;
   #endif
}

bool CABTRAudioProcessor::isMidiEffect() const
{
   #if JucePlugin_IsMidiEffect
    return true;
   #else
    return false;
   #endif
}

double CABTRAudioProcessor::getTailLengthSeconds() const
{
    return 0.0;
}

int CABTRAudioProcessor::getNumPrograms()
{
    return 1;   // NB: some hosts don't cope very well if you tell them there are 0 programs,
                // so this should be at least 1, even if you're not really implementing programs.
}

int CABTRAudioProcessor::getCurrentProgram()
{
    return 0;
}

void CABTRAudioProcessor::setCurrentProgram (int index)
{
	juce::ignoreUnused (index);
}

const juce::String CABTRAudioProcessor::getProgramName (int index)
{
    juce::ignoreUnused (index);
    return {};
}

void CABTRAudioProcessor::changeProgramName (int index, const juce::String& newName)
{
	juce::ignoreUnused (index, newName);
}

//==============================================================================
juce::AudioProcessorValueTreeState::ParameterLayout CABTRAudioProcessor::createParameterLayout()
{
	juce::AudioProcessorValueTreeState::ParameterLayout layout;

	// ══════════════════════════════════════════════════════════════
	//  IR Loader A Parameters
	// ══════════════════════════════════════════════════════════════
	layout.add (std::make_unique<juce::AudioParameterBool> (
		kParamEnableA, "Enable A", false));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamHpFreqA, "HP Freq A", 
		juce::NormalisableRange<float> (kFilterFreqMin, kFilterFreqMax, 1.0f, 0.3f), 
		kFilterHpFreqDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamLpFreqA, "LP Freq A", 
		juce::NormalisableRange<float> (kFilterFreqMin, kFilterFreqMax, 1.0f, 0.3f), 
		kFilterLpFreqDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamOutA, "Out A", kOutMin, kOutMax, kOutDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamStartA, "Start A",
		juce::NormalisableRange<float> (kStartMin, kStartMax, 0.1f, 0.15f), // Skew 0.15 = deep log (high resolution at low values)
		kStartDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamEndA, "End A",
		juce::NormalisableRange<float> (kEndMin, kEndMax, 0.1f, 0.15f), // Skew 0.15 = deep log
		kEndDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamPitchA, "Pitch A", 
		juce::NormalisableRange<float> (kPitchMin, kPitchMax, 0.01f, 0.5f), 
		kPitchDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamDelayA, "Delay A",
		juce::NormalisableRange<float> (kDelayMin, kDelayMax, 0.001f, 0.3f), // Skew 0.3 = logarítmico
		kDelayDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamPanA, "Pan A", kPanMin, kPanMax, kPanDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamFredA, "Angle A", kFredMin, kFredMax, kFredDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamPosA, "Distance A", kPosMin, kPosMax, kPosDefault));
	layout.add (std::make_unique<juce::AudioParameterBool> (
		kParamInvA, "Invert A", false));
	layout.add (std::make_unique<juce::AudioParameterBool> (
		kParamNormA, "Normalize A", false));
	layout.add (std::make_unique<juce::AudioParameterBool> (
		kParamRvsA, "Reverse A", false));
	layout.add (std::make_unique<juce::AudioParameterBool> (
		kParamChaosA, "Chaos A", false));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamChaosAmtA, "Chaos Amount A",
		juce::NormalisableRange<float> (kChaosAmtMin, kChaosAmtMax, 0.1f), kChaosAmtDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamChaosSpdA, "Chaos Speed A",
		juce::NormalisableRange<float> (kChaosSpdMin, kChaosSpdMax, 0.01f, 0.3f), kChaosSpdDefault));

	// ══════════════════════════════════════════════════════════════
	//  IR Loader B Parameters
	// ══════════════════════════════════════════════════════════════
	layout.add (std::make_unique<juce::AudioParameterBool> (
		kParamEnableB, "Enable B", false));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamHpFreqB, "HP Freq B", 
		juce::NormalisableRange<float> (kFilterFreqMin, kFilterFreqMax, 1.0f, 0.3f), 
		kFilterHpFreqDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamLpFreqB, "LP Freq B", 
		juce::NormalisableRange<float> (kFilterFreqMin, kFilterFreqMax, 1.0f, 0.3f), 
		kFilterLpFreqDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamOutB, "Out B", kOutMin, kOutMax, kOutDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamStartB, "Start B",
		juce::NormalisableRange<float> (kStartMin, kStartMax, 0.1f, 0.15f), // Skew 0.15 = deep log (high resolution at low values)
		kStartDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamEndB, "End B",
		juce::NormalisableRange<float> (kEndMin, kEndMax, 0.1f, 0.15f), // Skew 0.15 = deep log
		kEndDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamPitchB, "Pitch B", 
		juce::NormalisableRange<float> (kPitchMin, kPitchMax, 0.01f, 0.5f), 
		kPitchDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamDelayB, "Delay B",
		juce::NormalisableRange<float> (kDelayMin, kDelayMax, 0.001f, 0.3f), // Skew 0.3 = logarítmico
		kDelayDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamPanB, "Pan B", kPanMin, kPanMax, kPanDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamFredB, "Angle B", kFredMin, kFredMax, kFredDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamPosB, "Distance B", kPosMin, kPosMax, kPosDefault));
	layout.add (std::make_unique<juce::AudioParameterBool> (
		kParamInvB, "Invert B", false));
	layout.add (std::make_unique<juce::AudioParameterBool> (
		kParamNormB, "Normalize B", false));
	layout.add (std::make_unique<juce::AudioParameterBool> (
		kParamRvsB, "Reverse B", false));
	layout.add (std::make_unique<juce::AudioParameterBool> (
		kParamChaosB, "Chaos B", false));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamChaosAmtB, "Chaos Amount B",
		juce::NormalisableRange<float> (kChaosAmtMin, kChaosAmtMax, 0.1f), kChaosAmtDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamChaosSpdB, "Chaos Speed B",
		juce::NormalisableRange<float> (kChaosSpdMin, kChaosSpdMax, 0.01f, 0.3f), kChaosSpdDefault));

	// ══════════════════════════════════════════════════════════════
	//  Global Parameters
	// ══════════════════════════════════════════════════════════════
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamInput, "Input", kInputMin, kInputMax, kInputDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamOutput, "Output", kOutputMin, kOutputMax, kOutputDefault));
	layout.add (std::make_unique<juce::AudioParameterChoice> (
		kParamModeIn, "Mode In", juce::StringArray { "L+R", "MID", "SIDE" }, kModeDefault));
	layout.add (std::make_unique<juce::AudioParameterChoice> (
		kParamMode, "Mode Out", juce::StringArray { "L+R", "MID", "SIDE" }, kModeDefault));
	layout.add (std::make_unique<juce::AudioParameterChoice> (
		kParamRoute, "Route", juce::StringArray { "A->B", "A|B" }, kRouteDefault));
	layout.add (std::make_unique<juce::AudioParameterBool> (
		kParamAlign, "Align", false));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamMix, "Mix", kGlobalMixMin, kGlobalMixMax, kGlobalMixDefault));

	// ══════════════════════════════════════════════════════════════
	//  UI State Parameters (hidden from automation)
	// ══════════════════════════════════════════════════════════════
	layout.add (std::make_unique<juce::AudioParameterInt> (
		juce::ParameterID { kParamUiWidth, 1 }, "UI Width", 400, 2000, 800));

	layout.add (std::make_unique<juce::AudioParameterInt> (
		juce::ParameterID { kParamUiHeight, 1 }, "UI Height", 300, 1500, 600));

	layout.add (std::make_unique<juce::AudioParameterInt> (
		juce::ParameterID { kParamUiPalette, 1 }, "UI Palette", 0, 1, 0));

	layout.add (std::make_unique<juce::AudioParameterBool> (
		juce::ParameterID { kParamUiFxTail, 1 }, "UI FX Tail", false));

	layout.add (std::make_unique<juce::AudioParameterInt> (
		juce::ParameterID { kParamUiColor0, 1 }, "UI Color 0", 0, 0xFFFFFF, 0x00FF00));

	layout.add (std::make_unique<juce::AudioParameterInt> (
		juce::ParameterID { kParamUiColor1, 1 }, "UI Color 1", 0, 0xFFFFFF, 0x000000));

	return layout;
}

//==============================================================================
void CABTRAudioProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
	currentSampleRate = sampleRate;
	currentBlockSize = samplesPerBlock;
	
	LOG_IR_EVENT ("prepareToPlay: sr=" + juce::String (sampleRate) + 
	              " blockSize=" + juce::String (samplesPerBlock) +
	              " fftw=" CABTR_FFTW_STATUS);

	// ── FFTW: ensure DLL is loaded BEFORE any FFT instances are created ──
#if JUCE_WINDOWS && JUCE_DSP_USE_SHARED_FFTW
	ensureFFTWLoaded();
	LOG_IR_EVENT (g_fftwDiag);

	// Verify JUCE can now find it by short name
	{
		HMODULE juceCheck = LoadLibraryW (L"libfftw3f.dll");
		if (juceCheck)
		{
			LOG_IR_EVENT ("JUCE LoadLibrary(\"libfftw3f.dll\"): OK");
			FreeLibrary (juceCheck);
		}
		else
			LOG_IR_EVENT ("JUCE LoadLibrary(\"libfftw3f.dll\"): FAIL(err=" + juce::String ((int) GetLastError()) + ")");
	}
#endif

	// Prepare convolution
	juce::dsp::ProcessSpec spec;
	spec.sampleRate = sampleRate;
	spec.maximumBlockSize = static_cast<juce::uint32> (samplesPerBlock);
	spec.numChannels = 2;

	stateA.convolution.prepare (spec);
	stateB.convolution.prepare (spec);

	// FFT runtime benchmark — verify FFTW engine is actually active
	{
		juce::dsp::FFT fftBench (8); // order=8 → 256-point
		std::vector<float> benchBuf (512, 0.0f);
		benchBuf[0] = 1.0f;
		const auto t0 = juce::Time::getHighResolutionTicks();
		for (int i = 0; i < 100; ++i)
			fftBench.performRealOnlyForwardTransform (benchBuf.data());
		const auto t1 = juce::Time::getHighResolutionTicks();
		const double usPerFFT = juce::Time::highResolutionTicksToSeconds (t1 - t0) * 1e6 / 100.0;
		LOG_IR_EVENT ("FFT benchmark: 256-point x100 => " + juce::String (usPerFFT, 1) + "us/fft (" +
		              (usPerFFT < 5.0 ? "FFTW active" : "SLOW — likely fallback") + ")");
	}
	stateA.hpFilter.prepare (spec);
	stateA.lpFilter.prepare (spec);
	stateB.hpFilter.prepare (spec);
	stateB.lpFilter.prepare (spec);
	stateA.posFilter.prepare (spec);
	stateB.posFilter.prepare (spec);
	
	// Prepare delay lines
	stateA.delayLine.prepare (spec);
	stateB.delayLine.prepare (spec);
	stateA.delayLine.reset();
	stateB.delayLine.reset();
	
	// Prepare delay smoothing (50ms ramp)
	stateA.smoothedDelay.reset (sampleRate, 0.05); // 50ms ramp
	stateB.smoothedDelay.reset (sampleRate, 0.05);
	stateA.smoothedDelay.setCurrentAndTargetValue (0.0f);
	stateB.smoothedDelay.setCurrentAndTargetValue (0.0f);
	
	// Prepare temp buffers (pre-allocated for audio thread)
	tempBufferA.setSize (2, samplesPerBlock);
	tempBufferB.setSize (2, samplesPerBlock);
	globalDryBuffer.setSize (2, samplesPerBlock);

	// Cache raw parameter pointers (avoids hash-table lookup every processBlock)
	pEnableA = parameters.getRawParameterValue (kParamEnableA);
	pEnableB = parameters.getRawParameterValue (kParamEnableB);
	pModeIn  = parameters.getRawParameterValue (kParamModeIn);
	pMode    = parameters.getRawParameterValue (kParamMode);
	pRoute   = parameters.getRawParameterValue (kParamRoute);
	pMix     = parameters.getRawParameterValue (kParamMix);
	pInput   = parameters.getRawParameterValue (kParamInput);
	pOutput  = parameters.getRawParameterValue (kParamOutput);
	pHpFreqA = parameters.getRawParameterValue (kParamHpFreqA);
	pLpFreqA = parameters.getRawParameterValue (kParamLpFreqA);
	pDelayA  = parameters.getRawParameterValue (kParamDelayA);
	pPanA    = parameters.getRawParameterValue (kParamPanA);
	pFredA   = parameters.getRawParameterValue (kParamFredA);
	pPosA    = parameters.getRawParameterValue (kParamPosA);
	pOutA    = parameters.getRawParameterValue (kParamOutA);
	pHpFreqB = parameters.getRawParameterValue (kParamHpFreqB);
	pLpFreqB = parameters.getRawParameterValue (kParamLpFreqB);
	pDelayB  = parameters.getRawParameterValue (kParamDelayB);
	pPanB    = parameters.getRawParameterValue (kParamPanB);
	pFredB   = parameters.getRawParameterValue (kParamFredB);
	pPosB    = parameters.getRawParameterValue (kParamPosB);
	pOutB    = parameters.getRawParameterValue (kParamOutB);
	pChaosA    = parameters.getRawParameterValue (kParamChaosA);
	pChaosAmtA = parameters.getRawParameterValue (kParamChaosAmtA);
	pChaosSpdA = parameters.getRawParameterValue (kParamChaosSpdA);
	pChaosB    = parameters.getRawParameterValue (kParamChaosB);
	pChaosAmtB = parameters.getRawParameterValue (kParamChaosAmtB);
	pChaosSpdB = parameters.getRawParameterValue (kParamChaosSpdB);
	
	// Reset FRED delay state
	std::memset (stateA.fredDelayBuffer, 0, sizeof (stateA.fredDelayBuffer));
	std::memset (stateB.fredDelayBuffer, 0, sizeof (stateB.fredDelayBuffer));
	stateA.fredDelayIndex = 0;
	stateB.fredDelayIndex = 0;

	// Reset CHAOS state
	std::memset (stateA.chaosDelayBuffer, 0, sizeof (stateA.chaosDelayBuffer));
	std::memset (stateB.chaosDelayBuffer, 0, sizeof (stateB.chaosDelayBuffer));
	stateA.chaosDelayWritePos = 0;
	stateB.chaosDelayWritePos = 0;
	stateA.chaosCurrentTarget = 0.0f;
	stateB.chaosCurrentTarget = 0.0f;
	stateA.chaosSmoothedValue = 0.0f;
	stateB.chaosSmoothedValue = 0.0f;
	stateA.chaosPhaseSamples = 0.0f;
	stateB.chaosPhaseSamples = 0.0f;
	stateA.chaosGainTarget = 0.0f;
	stateB.chaosGainTarget = 0.0f;
	stateA.chaosGainSmoothed = 0.0f;
	stateB.chaosGainSmoothed = 0.0f;
	stateA.chaosGainPhase = 0.0f;
	stateB.chaosGainPhase = 0.0f;

	// Initialize EMA-smoothed filter frequencies to current parameter values
	stateA.smoothedHpFreq = pHpFreqA->load();
	stateA.smoothedLpFreq = pLpFreqA->load();
	stateB.smoothedHpFreq = pHpFreqB->load();
	stateB.smoothedLpFreq = pLpFreqB->load();
	stateA.smoothedPosFreq = 12000.0f;
	stateB.smoothedPosFreq = 12000.0f;
	stateA.filterCoeffCountdown = 0;
	stateB.filterCoeffCountdown = 0;
}

void CABTRAudioProcessor::releaseResources()
{
	// Reset delay lines
	stateA.delayLine.reset();
	stateB.delayLine.reset();
	
	// Print debug stats
	DBG ("CAB-TR CPU Debug Stats:");
	DBG ("  Loader A reloads: " + juce::String (reloadCountA.load()));
	DBG ("  Loader B reloads: " + juce::String (reloadCountB.load()));
}

#ifndef JucePlugin_PreferredChannelConfigurations
bool CABTRAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
  #if JucePlugin_IsMidiEffect
    juce::ignoreUnused (layouts);
    return true;
  #else
    // This is the place where you check if the layout is supported.
    // In this template code we only support mono or stereo.
    // Some plugin hosts, such as certain GarageBand versions, will only
    // load plugins that support stereo bus layouts.
    if (layouts.getMainOutputChannelSet() != juce::AudioChannelSet::mono()
     && layouts.getMainOutputChannelSet() != juce::AudioChannelSet::stereo())
        return false;

    // This checks if the input layout matches the output layout
   #if ! JucePlugin_IsSynth
    if (layouts.getMainOutputChannelSet() != layouts.getMainInputChannelSet())
        return false;
   #endif

    return true;
  #endif
}
#endif

//==============================================================================
// DSP ALGORITHM DOCUMENTATION:
//
// 1. CONVOLUTION: Uses JUCE's FFT-based convolution (Overlap-Add method)
//    - Complexity: O(N log N) vs O(N²) for time-domain
//    - Automatically partitions long IRs for optimal latency/CPU balance
//    - Zero-latency mode for low-latency monitoring
//
// 2. PITCH SHIFTING: Lagrange interpolation resampling
//    - 4-point Lagrange interpolator for quality/performance balance
//    - Linear phase response (no smearing)
//    - Range: 25%-400% (down 2 octaves to up 2 octaves)
//
// 3. MODE PROCESSING: Mid/Side conversion
//    - MID = (L+R) / sqrt(2)  — preserves RMS energy
//    - SIDE = (L-R) / sqrt(2)
//    - L+R = standard stereo pass-through
//
// 4. ROUTING:
//    - PARALLEL (A|B): Independent processing, summed output
//    - SERIES (A→B): A output becomes B input (cascade)
//
// 5. SIMD OPTIMIZATION: FloatVectorOperations for buffer operations
//    - applyGain, multiply, add use SIMD when available
//    - Significant speedup on modern CPUs (4x+ on AVX)
//
// 6. IR LENGTH LIMIT: 10 seconds maximum (480,000 samples @ 48kHz)
//    - Prevents excessive memory usage
//    - Typical guitar cab IRs are 100-500ms
//==============================================================================

void CABTRAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages)
{
	juce::ScopedNoDenormals noDenormals;
	juce::ignoreUnused (midiMessages);
	
	const auto blockStartTime = juce::Time::getHighResolutionTicks();

	auto totalNumInputChannels  = getTotalNumInputChannels();
	auto totalNumOutputChannels = getTotalNumOutputChannels();

	// Clear unused output channels
	for (auto i = totalNumInputChannels; i < totalNumOutputChannels; ++i)
		buffer.clear (i, 0, buffer.getNumSamples());

	const int numSamples = buffer.getNumSamples();
	if (numSamples == 0)
		return;

	// Get global parameters (cached pointers — no hash lookup)
	const bool enableA = pEnableA->load() > 0.5f;
	const bool enableB = pEnableB->load() > 0.5f;
	const int modeIn = static_cast<int> (pModeIn->load());
	const int mode = static_cast<int> (pMode->load());
	const int route = static_cast<int> (pRoute->load());
	const float globalMix = pMix->load();

	// Apply input gain (SIMD optimized)
	const float inputGain = juce::Decibels::decibelsToGain (pInput->load());
	buffer.applyGain (inputGain);

	// MODE IN: Convert stereo input to Mid or Side before processing
	if ((modeIn == 1 || modeIn == 2) && buffer.getNumChannels() >= 2)
	{
		auto* L = buffer.getWritePointer (0);
		auto* R = buffer.getWritePointer (1);
		for (int i = 0; i < numSamples; ++i)
		{
			const float l = L[i];
			const float r = R[i];
			if (modeIn == 1) // MID = (L+R) / sqrt(2)
			{
				const float mid = (l + r) * 0.707106781f;
				L[i] = R[i] = mid;
			}
			else // SIDE = (L-R) / sqrt(2)
			{
				const float side = (l - r) * 0.707106781f;
				L[i] = R[i] = side;
			}
		}
	}

	// Capture dry signal AFTER input gain + mode in, but BEFORE convolution
	// Used for global MIX: dry is unaffected by convolution, filters, mode, etc.
	const bool needsDry = (globalMix < 0.999f);
	if (needsDry)
	{
		for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
			globalDryBuffer.copyFrom (ch, 0, buffer, ch, 0, numSamples);
	}

	// Route: PARALLEL (A|B) or SERIES (A→B)
	if (route == 1) // PARALLEL
	{
		// Process independently and sum
		if (enableA && enableB)
		{
			// Copy input to temp buffers (pre-allocated, no heap alloc)
			for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
			{
				tempBufferA.copyFrom (ch, 0, buffer, ch, 0, numSamples);
				tempBufferB.copyFrom (ch, 0, buffer, ch, 0, numSamples);
			}
			
			// Process each loader
			processLoader (stateA, tempBufferA, "A", true);
			processLoader (stateB, tempBufferB, "B", false);
			
			// Sum both outputs into main buffer (SIMD) with -3dB compensation
			for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
			{
				juce::FloatVectorOperations::add (buffer.getWritePointer (ch),
				                                   tempBufferA.getReadPointer (ch),
				                                   tempBufferB.getReadPointer (ch),
				                                   numSamples);
			}
			buffer.applyGain (0.707106781f); // -3dB parallel compensation
		}
		else if (enableA)
		{
			processLoader (stateA, buffer, "A", true);
		}
		else if (enableB)
		{
			processLoader (stateB, buffer, "B", false);
		}
	}
	else // SERIES (A→B)
	{
		if (enableA)
			processLoader (stateA, buffer, "A", true);
		
		if (enableB)
			processLoader (stateB, buffer, "B", false);
	}

	// MODE OUT: Convert to Mid/Side at output
	if (mode == 1 || mode == 2) // MID or SIDE
	{
		if (buffer.getNumChannels() >= 2)
		{
			auto* L = buffer.getWritePointer (0);
			auto* R = buffer.getWritePointer (1);
			
			for (int i = 0; i < numSamples; ++i)
			{
				const float l = L[i];
				const float r = R[i];
				
				if (mode == 1) // MID = (L+R) / sqrt(2)
				{
					const float mid = (l + r) * 0.707106781f;
					L[i] = R[i] = mid;
				}
				else // SIDE = (L-R) / sqrt(2)
				{
					const float side = (l - r) * 0.707106781f;
					L[i] = R[i] = side;
				}
			}
		}
	}

	// Global MIX: blend unprocessed dry with fully processed wet
	// dry = input after gain, wet = after convolution + mode
	if (needsDry)
	{
		const float wet = globalMix;
		const float dry = 1.0f - globalMix;
		for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
		{
			auto* wetData = buffer.getWritePointer (ch);
			const auto* dryData = globalDryBuffer.getReadPointer (ch);
			juce::FloatVectorOperations::multiply (wetData, wet, numSamples);
			juce::FloatVectorOperations::addWithMultiply (wetData, dryData, dry, numSamples);
		}
	}

	// Apply output gain (SIMD optimized)
	const float outputGain = juce::Decibels::decibelsToGain (pOutput->load());
	buffer.applyGain (outputGain);
	
	// CPU profiling: track worst-case block time
	const auto blockEndTime = juce::Time::getHighResolutionTicks();
	const double blockTimeUs = juce::Time::highResolutionTicksToSeconds (blockEndTime - blockStartTime) * 1e6;
	double currentWorst = worstBlockTimeUs.load();
	if (blockTimeUs > currentWorst)
		worstBlockTimeUs.store (blockTimeUs);
	blockCount++;
}

//==============================================================================
void CABTRAudioProcessor::loadImpulseResponse (IRLoaderState& state, const juce::String& filePath)
{
	if (filePath.isEmpty())
		return;

	juce::File irFile (filePath);
	if (! irFile.existsAsFile())
		return;

	// Load audio file (reusable format manager — no re-registration per load)
	std::unique_ptr<juce::AudioFormatReader> reader (
		formatManager.createReaderFor (irFile));

	if (reader == nullptr)
		return;

	// Smart read: only read the samples we actually need (avoids loading 480K samples to discard 99%)
	// 1. Calculate trim boundaries first, then read only that range + margin for pitch
	const bool isA_load = (&state == &stateA);
	const float startMs_pre = parameters.getRawParameterValue (isA_load ? kParamStartA : kParamStartB)->load();
	const float endMs_pre = parameters.getRawParameterValue (isA_load ? kParamEndA : kParamEndB)->load();
	const float pitch_pre = parameters.getRawParameterValue (isA_load ? kParamPitchA : kParamPitchB)->load();

	const juce::int64 totalFileSamples = reader->lengthInSamples;
	const double sr = reader->sampleRate;

	// Compute the actual sample range we need from the file
	juce::int64 readStart = static_cast<juce::int64> (startMs_pre * 0.001 * sr);
	juce::int64 readEnd = static_cast<juce::int64> (endMs_pre * 0.001 * sr);
	readStart = juce::jlimit<juce::int64> (0, totalFileSamples, readStart);
	readEnd = juce::jlimit<juce::int64> (readStart + 64, totalFileSamples, readEnd);

	// Add margin for pitch < 1.0 (stretching needs more source samples)
	// pitch=0.25 means we need 4x more source samples than output
	const float safePitch = juce::jmax (0.25f, pitch_pre);
	const juce::int64 pitchMargin = (safePitch < 1.0f)
	    ? static_cast<juce::int64> ((readEnd - readStart) * (1.0f / safePitch - 1.0f)) + 64
	    : 0;
	readEnd = juce::jmin (readEnd + pitchMargin, totalFileSamples);

	const auto samplesToRead = readEnd - readStart;

	// Read only the needed range from file
	juce::AudioBuffer<float> tempIR;
	tempIR.setSize (static_cast<int> (reader->numChannels),
	                static_cast<int> (samplesToRead));
	reader->read (&tempIR, 0, static_cast<int> (samplesToRead), 
	              static_cast<int> (readStart), true, true);

	// Apply START/END trimming if needed
	// (Parameters are in milliseconds, convert to samples)
	const char* startParam = isA_load ? kParamStartA : kParamStartB;
	const char* endParam = isA_load ? kParamEndA : kParamEndB;
	const char* pitchParam = isA_load ? kParamPitchA : kParamPitchB;
	const char* invParam = isA_load ? kParamInvA : kParamInvB;
	const char* normParam = isA_load ? kParamNormA : kParamNormB;
	const char* rvsParam = isA_load ? kParamRvsA : kParamRvsB;
	
	const float startMs = parameters.getRawParameterValue (startParam)->load();
	const float endMs = parameters.getRawParameterValue (endParam)->load();
	const float pitch = parameters.getRawParameterValue (pitchParam)->load();
	const bool invert = parameters.getRawParameterValue (invParam)->load() > 0.5f;
	const bool normalize = parameters.getRawParameterValue (normParam)->load() > 0.5f;
	const bool reverse = parameters.getRawParameterValue (rvsParam)->load() > 0.5f;
	
	// Now trim within the already-cropped buffer (offsets are relative to readStart)
	const int totalSamples = static_cast<int> (samplesToRead);
	const int rawStart = static_cast<int> (startMs * 0.001 * reader->sampleRate) - static_cast<int> (readStart);
	const int rawEnd = static_cast<int> (endMs * 0.001 * reader->sampleRate) - static_cast<int> (readStart);
	
	// Ensure minimum 64-sample IR to prevent degenerate convolution
	constexpr int kMinIrSamples = 64;
	const int startSample = juce::jlimit (0, juce::jmax (0, totalSamples - kMinIrSamples), rawStart);
	const int endSample = juce::jlimit (startSample + kMinIrSamples, totalSamples, 
	                                     juce::jmax (rawEnd, startSample + kMinIrSamples));
	const int trimmedLength = endSample - startSample;
	
	// Log START/END trim for diagnostics
	if (startSample > 0 || endSample < totalSamples)
	{
		LOG_IR_EVENT ("START/END trim: " + juce::String (startSample) + " to " + 
		              juce::String (endSample) + " (" + juce::String (trimmedLength) + " samples, " + 
		              juce::String (trimmedLength / reader->sampleRate * 1000.0, 1) + "ms)" +
		              (rawStart != startSample ? " [START clamped from " + juce::String (rawStart) + "]" : "") +
		              (rawEnd != endSample ? " [END clamped from " + juce::String (rawEnd) + "]" : ""));
	}
	
	// Create final IR buffer with trimmed range
	state.impulseResponse.setSize (tempIR.getNumChannels(), trimmedLength);
	
	for (int ch = 0; ch < tempIR.getNumChannels(); ++ch)
	{
		state.impulseResponse.copyFrom (ch, 0, tempIR, ch, startSample, trimmedLength);
	}
	
	// Apply PITCH SHIFT usando Lagrange interpolation (alta calidad, sin aliasing)
	// pitch < 1.0 = slower playback (lower pitch, longer IR)
	// pitch > 1.0 = faster playback (higher pitch, shorter IR)
	if (std::abs (pitch - 1.0f) > 0.001f && pitch > 0.1f && pitch < 10.0f)
	{
		const int pitchedLength = static_cast<int> (trimmedLength / pitch);
		if (pitchedLength > 0 && pitchedLength < trimmedLength * 10)
		{
			juce::AudioBuffer<float> pitchedIR;
			pitchedIR.setSize (state.impulseResponse.getNumChannels(), pitchedLength);
			
			// Lagrange interpolator para alta calidad (4-point)
			for (int ch = 0; ch < state.impulseResponse.getNumChannels(); ++ch)
			{
				juce::LagrangeInterpolator interpolator;
				const float* input = state.impulseResponse.getReadPointer (ch);
				float* output = pitchedIR.getWritePointer (ch);
				
				// Resample con ratio inverso (pitch=2.0 -> ratio=0.5)
				const double resampleRatio = 1.0 / pitch;
				interpolator.process (resampleRatio, input, output, pitchedLength, trimmedLength, 0);
			}
			
			// Replace with pitched version
			state.impulseResponse = std::move (pitchedIR);
		}
	}
	
	// Enforce minimum length AFTER pitch shift (pitch>1 shrinks the IR)
	if (state.impulseResponse.getNumSamples() < kMinIrSamples)
	{
		const int currentLen = state.impulseResponse.getNumSamples();
		const int numCh = state.impulseResponse.getNumChannels();
		juce::AudioBuffer<float> padded (numCh, kMinIrSamples);
		padded.clear();
		for (int ch = 0; ch < numCh; ++ch)
			padded.copyFrom (ch, 0, state.impulseResponse, ch, 0, currentLen);
		state.impulseResponse = std::move (padded);
		LOG_IR_EVENT ("IR padded from " + juce::String (currentLen) + " to " + juce::String (kMinIrSamples) + " samples (post-pitch minimum)");
	}
	
	const int finalLength = state.impulseResponse.getNumSamples();
	
	// Apply INVERT if enabled (multiply all samples by -1)
	if (invert)
	{
		for (int ch = 0; ch < state.impulseResponse.getNumChannels(); ++ch)
			state.impulseResponse.applyGain (ch, 0, finalLength, -1.0f);
	}
	
	// Apply NORMALIZE if enabled (peak normalization to -18dBFS)
	// Industry standard for cab IR comparison: consistent perceived loudness
	// -18dBFS peak with max +24dB boost prevents ear-splitting gains on quiet tail regions
	if (normalize && finalLength > 10)
	{
		float maxLevel = 0.0f;
		for (int ch = 0; ch < state.impulseResponse.getNumChannels(); ++ch)
		{
			const float mag = state.impulseResponse.getMagnitude (ch, 0, finalLength);
			maxLevel = std::max (maxLevel, mag);
		}
		
		constexpr float kNormTarget = 0.1259f; // -18dBFS peak
		constexpr float kMaxBoost = 7.94f;     // +18dB max (industry standard)
		
		// Apply normalization if there's actual signal (> -60dB)
		if (maxLevel > 0.001f) // -60dB threshold (stricter than before)
		{
			const float normGain = kNormTarget / maxLevel;
			const float safeGain = juce::jlimit (0.01f, kMaxBoost, normGain);
			state.impulseResponse.applyGain (safeGain);
			
			LOG_IR_EVENT ("NORMALIZE applied: maxLevel=" + juce::String (maxLevel, 6) + 
			              " -> target -18dBFS, gain=" + juce::String (safeGain, 2) + "x (" + 
			              juce::String (juce::Decibels::gainToDecibels (safeGain), 1) + "dB)");
		}
		else
		{
			LOG_IR_EVENT ("NORMALIZE skipped: signal too quiet (maxLevel=" + juce::String (maxLevel, 6) + ", threshold=-60dB)");
		}
	}
	
	// Apply REVERSE if enabled (reverse sample order)
	if (reverse)
	{
		for (int ch = 0; ch < state.impulseResponse.getNumChannels(); ++ch)
		{
			auto* data = state.impulseResponse.getWritePointer (ch);
			std::reverse (data, data + finalLength);
		}
		LOG_IR_EVENT ("REVERSE applied to IR");
	}

	// Load into convolution engine
	// Cap IR length to prevent excessive CPU (convolution cost is proportional to IR length)
	constexpr int kMaxIrSamples = 4096; // ~85ms at 48kHz — generous for guitar cab responses
	if (state.impulseResponse.getNumSamples() > kMaxIrSamples)
	{
		const int oldLen = state.impulseResponse.getNumSamples();
		state.impulseResponse.setSize (state.impulseResponse.getNumChannels(), kMaxIrSamples, true);
		LOG_IR_EVENT ("IR truncated from " + juce::String (oldLen) + " to " + juce::String (kMaxIrSamples) + " samples (CPU cap)");
	}
	
	// Head size configured in Convolution constructor (NonUniform{256})
	// IMPORTANT: Copy the buffer — do NOT std::move it, we need state.impulseResponse
	// for cross-correlation alignment (calculateAutoAlignment)
	juce::AudioBuffer<float> convCopy (state.impulseResponse);
	state.convolution.loadImpulseResponse (
		std::move (convCopy),
		reader->sampleRate,
		juce::dsp::Convolution::Stereo::yes,
		juce::dsp::Convolution::Trim::no,
		juce::dsp::Convolution::Normalise::no);
	
	// Update cached parameter values AFTER successful load
	state.lastPitch.store (pitch);
	state.lastInv.store (invert);
	state.lastNorm.store (normalize);
	state.lastRvs.store (reverse);
	state.lastStart.store (startMs);
	state.lastEnd.store (endMs);
	
	// Increment reload counter
	if (isA_load)
		reloadCountA++;
	else
		reloadCountB++;

	state.currentFilePath = filePath;
	state.needsUpdate = false;
	
	const int cappedLength = juce::jmin (finalLength, 4096);
	
	LOG_IR_EVENT ("IR Reload " + juce::String (isA_load ? "A" : "B") + ": " + 
	     juce::String (cappedLength) + " samples" +
	     (finalLength > 4096 ? " (capped from " + juce::String (finalLength) + ")" : "") +
	     ", pitch=" + juce::String (pitch, 2) +
	     ", inv=" + juce::String (invert ? 1 : 0) + ", rvs=" + juce::String (reverse ? 1 : 0) +
	     ", norm=" + juce::String (normalize ? 1 : 0));
}

//==============================================================================
// PROCESSING CHAIN OPTIMIZADA: 
// convolution → INV → HP/LP → DIST → PAN → DELAY → ANGLE → OUT
//
// CPU OPTIMIZATIONS:
// - Zero allocations en audio thread
// - Cached filter coefficients (solo recalcula si cambia parameter)
// - ANGLE: minimal 7-sample delay (comb filter for off-axis simulation)
// - SIMD operations donde sea posible
// - Distance as exponential LPF + gain attenuation
// - Timer debounce: no reload during slider drag
//==============================================================================
void CABTRAudioProcessor::processLoader (IRLoaderState& state, 
                                          juce::AudioBuffer<float>& buffer,
                                          const char* paramPrefix,
                                          bool isA)
{
	const int numSamples = buffer.getNumSamples();
	const int numChannels = buffer.getNumChannels();
	
	const bool isLoaderA = (paramPrefix[0] == 'A' || paramPrefix[0] == 'a');
	
	// EARLY EXIT: No processing if no IR loaded
	if (state.currentFilePath.isEmpty())
	{
		buffer.clear(); // Clear buffer (no output)
		return;
	}
	
	// STEP 0: Check if IR parameters changed → set flag to reload on message thread
	// CRITICAL: Cannot call loadImpulseResponse here (file I/O illegal in audio thread!)
	// NOTE: IR reload is handled by timerCallback with debouncing (message thread).
	// No parameter change detection needed here on the audio thread.
	
	// Get runtime parameters (cached pointers — no hash lookup)
	const float hpFreq = (isLoaderA ? pHpFreqA : pHpFreqB)->load();
	const float lpFreq = (isLoaderA ? pLpFreqA : pLpFreqB)->load();
	const float delayMs = (isLoaderA ? pDelayA : pDelayB)->load();
	const float pan = (isLoaderA ? pPanA : pPanB)->load();
	const float fred = (isLoaderA ? pFredA : pFredB)->load();
	const float pos = (isLoaderA ? pPosA : pPosB)->load();
	const float outDb = (isLoaderA ? pOutA : pOutB)->load();
	const bool chaosEnabled = (isLoaderA ? pChaosA : pChaosB)->load() > 0.5f;
	const float chaosAmt = (isLoaderA ? pChaosAmtA : pChaosAmtB)->load();
	const float chaosSpd = (isLoaderA ? pChaosSpdA : pChaosSpdB)->load();
	
	// (FRED processing happens after convolution + filters)
	
	// 1. CONVOLUTION (NonUniform{256} — low-latency partitioned)
	{
		juce::dsp::AudioBlock<float> block (buffer);
		juce::dsp::ProcessContextReplacing<float> context (block);
		state.convolution.process (context);
	}
	
	// 2-3. HP + LP FILTERS with EMA smoothing (matching TR plugin pattern)
	// Per-sample EMA frequency smoothing + coefficient update every 32 samples
	{
		constexpr float kSmoothCoeff = 0.9955f; // ~5ms @ 44.1kHz
		const float oneMinusCoeff = 1.0f - kSmoothCoeff;

		// EMA smooth toward target frequencies
		state.smoothedHpFreq += (hpFreq - state.smoothedHpFreq) * oneMinusCoeff;
		state.smoothedLpFreq += (lpFreq - state.smoothedLpFreq) * oneMinusCoeff;

		// Recalculate coefficients every 32 samples (not every block)
		if (--state.filterCoeffCountdown <= 0)
		{
			state.filterCoeffCountdown = IRLoaderState::kFilterCoeffUpdateInterval;

			const float maxFreq = static_cast<float> (currentSampleRate) * 0.49f;

			// HP: recalc only if smoothed frequency changed
			const float clampedHp = juce::jlimit (20.0f, maxFreq, state.smoothedHpFreq);
			if (std::abs (clampedHp - state.lastHpFreq) > 0.01f)
			{
				*state.hpFilter.state = *juce::dsp::IIR::Coefficients<float>::makeHighPass (
					currentSampleRate, clampedHp, 0.707f);
				state.lastHpFreq = clampedHp;
			}

			// LP: recalc only if smoothed frequency changed
			const float clampedLp = juce::jlimit (20.0f, maxFreq, state.smoothedLpFreq);
			if (std::abs (clampedLp - state.lastLpFreq) > 0.01f)
			{
				*state.lpFilter.state = *juce::dsp::IIR::Coefficients<float>::makeLowPass (
					currentSampleRate, clampedLp, 0.707f);
				state.lastLpFreq = clampedLp;
			}
		}
	}

	// Apply HP filter
	if (state.smoothedHpFreq >= 21.0f)
	{
		juce::dsp::AudioBlock<float> block (buffer);
		juce::dsp::ProcessContextReplacing<float> context (block);
		state.hpFilter.process (context);
	}

	// Apply LP filter
	if (state.smoothedLpFreq <= 19900.0f)
	{
		juce::dsp::AudioBlock<float> block (buffer);
		juce::dsp::ProcessContextReplacing<float> context (block);
		state.lpFilter.process (context);
	}
	
	// 4. DISTANCE EFFECT (exponential LPF + gain attenuation)
	// 0% = close/bright (no change), 100% = far/dark (HF reduction + volume drop)
	if (pos > 0.01f)
	{
		// Exponential cutoff: 12kHz * exp(-pos * 2.08) → pos=0→12kHz, pos=1→1.5kHz
		const float cutoff = 12000.0f * std::exp (-pos * 2.0794f);
		constexpr float kPosSmooth = 0.9955f;
		state.smoothedPosFreq += (cutoff - state.smoothedPosFreq) * (1.0f - kPosSmooth);

		// Recalculate if frequency changed significantly
		const float maxFreq = static_cast<float> (currentSampleRate) * 0.49f;
		const float clampedPos = juce::jlimit (200.0f, maxFreq, state.smoothedPosFreq);
		if (std::abs (clampedPos - state.lastPosFreq) > 1.0f)
		{
			*state.posFilter.state = *juce::dsp::IIR::Coefficients<float>::makeLowPass (
				currentSampleRate, clampedPos, 0.707f);
			state.lastPosFreq = clampedPos;
		}
		
		juce::dsp::AudioBlock<float> block (buffer);
		juce::dsp::ProcessContextReplacing<float> context (block);
		state.posFilter.process (context);

		// Distance gain attenuation: 0dB at pos=0, -6dB at pos=1
		const float distGain = 1.0f - pos * 0.5f;
		buffer.applyGain (distGain);
	}
	else if (state.lastPosFreq > 0.0f)
	{
		state.lastPosFreq = -1.0f; // Mark as inactive
	}
	
	// 5. PAN (cached gains)
	if (numChannels >= 2 && std::abs (pan - 0.5f) > 0.001f)
	{
		if (std::abs (pan - state.lastPan) > 0.001f)
		{
			const float panAngle = pan * 1.5707963f;
			state.lastPanLeft = std::cos (panAngle);
			state.lastPanRight = std::sin (panAngle);
			state.lastPan = pan;
		}
		
		buffer.applyGain (0, 0, numSamples, state.lastPanLeft);
		buffer.applyGain (1, 0, numSamples, state.lastPanRight);
	}
	
	// 6. DELAY: Simple latency using juce::dsp::DelayLine
	// Properly handles delays longer than buffer size
	// Used for phase alignment between loaders A and B
	if (delayMs > 0.1f) // Threshold 0.1ms (suficiente para detectar cambios)
	{
		applyDelay (buffer, delayMs, isA);
	}
	
	// 7. ANGLE (off-axis mic simulation)
	// Simulates a second mic at an angle on a guitar cab.
	// 7-sample circular delay (~0.15ms at 48kHz ≈ 5cm path difference).
	// First comb null at ~6.8kHz — creates musically useful tonal shaping.
	// angle=0: pure on-axis (no effect), angle=1: full off-axis blend
	if (fred > 0.001f)
	{
		float* channelData[2] = { nullptr, nullptr };
		const int chCount = juce::jmin (numChannels, 2);
		for (int ch = 0; ch < chCount; ++ch)
			channelData[ch] = buffer.getWritePointer (ch);
		
		const int delayLen = IRLoaderState::kFredDelaySamples;
		const float compensate = 1.0f / (1.0f + fred); // Gain compensation for additive sum
		for (int i = 0; i < numSamples; ++i)
		{
			const int idx = state.fredDelayIndex;
			for (int ch = 0; ch < chCount; ++ch)
			{
				const float direct = channelData[ch][i];
				const float offAxis = state.fredDelayBuffer[ch][idx];
				state.fredDelayBuffer[ch][idx] = direct;
				// Additive comb: sum direct + delayed copy → creates frequency cancellations
				channelData[ch][i] = (direct + fred * offAxis) * compensate;
			}
			state.fredDelayIndex = (state.fredDelayIndex + 1) % delayLen;
		}
	}
	
	// 8. CHAOS (S&H micro-delay pitch modulation + independent gain modulation)
	// Pitch S&H: random targets at 'speed' Hz → EMA smoothing → variable delay line.
	// Gain S&H: independent random targets at same rate → EMA smoothing → ±1dB gain.
	// Amount controls depth for both. Each loader has fully independent generators.
	if (chaosEnabled && chaosAmt > 0.01f)
	{
		const float maxDelaySec = 0.005f; // ±5ms max
		const float amountNorm = chaosAmt * 0.01f; // 0..1
		const float maxDelaySamples = amountNorm * maxDelaySec * (float) currentSampleRate;
		
		// Pitch EMA τ scales with speed: 15ms @ 0.01Hz → 120ms @ 100Hz (log mapping)
		const float spdNorm = std::log (chaosSpd / 0.01f) / std::log (100.0f / 0.01f); // 0..1
		const float pitchTau = 0.015f + spdNorm * 0.105f; // 15ms → 120ms
		const float pitchSmoothCoeff = std::exp (-1.0f / ((float) currentSampleRate * pitchTau));
		const float gainSmoothCoeff  = std::exp (-1.0f / ((float) currentSampleRate * 0.015f));
		
		// S&H period in samples
		const float shPeriodSamples = (float) currentSampleRate / chaosSpd;
		
		// Gain modulation depth: ±1dB at amount=100% (±0.12 linear)
		const float gainModDepth = amountNorm * 0.12f;
		
		const int chCount = juce::jmin (numChannels, 2);
		const int delayBufLen = IRLoaderState::kChaosDelayMaxSamples;
		
		float* channelData[2] = { nullptr, nullptr };
		for (int ch = 0; ch < chCount; ++ch)
			channelData[ch] = buffer.getWritePointer (ch);
		
		for (int i = 0; i < numSamples; ++i)
		{
			// Pitch S&H: generate new random target when phase wraps
			state.chaosPhaseSamples += 1.0f;
			if (state.chaosPhaseSamples >= shPeriodSamples)
			{
				state.chaosPhaseSamples -= shPeriodSamples;
				state.chaosCurrentTarget = state.chaosRng.nextFloat() * 2.0f - 1.0f;
			}
			
			// Gain S&H: independent phase + random (decorrelated from pitch)
			state.chaosGainPhase += 1.0f;
			if (state.chaosGainPhase >= shPeriodSamples)
			{
				state.chaosGainPhase -= shPeriodSamples;
				state.chaosGainTarget = state.chaosGainRng.nextFloat() * 2.0f - 1.0f;
			}
			
			// EMA smoothing for both (pitch=30ms, gain=15ms)
			state.chaosSmoothedValue = pitchSmoothCoeff * state.chaosSmoothedValue
			                         + (1.0f - pitchSmoothCoeff) * state.chaosCurrentTarget;
			state.chaosGainSmoothed = gainSmoothCoeff * state.chaosGainSmoothed
			                        + (1.0f - gainSmoothCoeff) * state.chaosGainTarget;
			
			// Convert to delay in samples (centered around midpoint so average is 0 pitch change)
			const float centerDelay = maxDelaySamples;
			const float delaySamples = centerDelay + state.chaosSmoothedValue * maxDelaySamples;
			const float clampedDelay = juce::jlimit (0.0f, (float) (delayBufLen - 2), delaySamples);
			
			// Gain modulation: ±1dB max
			const float gainMod = 1.0f + state.chaosGainSmoothed * gainModDepth;
			
			// Write current sample into delay buffer
			const int wp = state.chaosDelayWritePos;
			for (int ch = 0; ch < chCount; ++ch)
				state.chaosDelayBuffer[ch][wp] = channelData[ch][i];
			
			// Read with 4-point Lagrange interpolation for high quality
			const float readPos = (float) wp - clampedDelay;
			const int iPos = (int) std::floor (readPos);
			const float frac = readPos - (float) iPos;
			
			const int mask = delayBufLen - 1; // 1024 is power-of-2
			for (int ch = 0; ch < chCount; ++ch)
			{
				const float p0 = state.chaosDelayBuffer[ch][(iPos - 1) & mask];
				const float p1 = state.chaosDelayBuffer[ch][(iPos    ) & mask];
				const float p2 = state.chaosDelayBuffer[ch][(iPos + 1) & mask];
				const float p3 = state.chaosDelayBuffer[ch][(iPos + 2) & mask];
				
				const float c0 = p1;
				const float c1 = p2 - (1.0f / 3.0f) * p0 - 0.5f * p1 - (1.0f / 6.0f) * p3;
				const float c2 = 0.5f * (p0 + p2) - p1;
				const float c3 = (1.0f / 6.0f) * (p3 - p0) + 0.5f * (p1 - p2);
				channelData[ch][i] = ((c3 * frac + c2) * frac + c1) * frac + c0;
				
				// Apply gain modulation
				channelData[ch][i] *= gainMod;
			}
			
			state.chaosDelayWritePos = (wp + 1) % delayBufLen;
		}
	}
	
	// 9. OUTPUT GAIN
	if (std::abs (outDb) > 0.01f)
	{
		const float outGain = juce::Decibels::decibelsToGain (outDb);
		buffer.applyGain (outGain);
	}
}

//==============================================================================
// DELAY: Proper delay line with smoothing to prevent clicks
// Handles any delay length correctly (even > buffer size)
// Used for phase alignment between loaders A and B
//==============================================================================
void CABTRAudioProcessor::applyDelay (juce::AudioBuffer<float>& buffer, float delayMs, bool isA)
{
	if (delayMs < 0.1f)
		return;
	
	const int numSamples = buffer.getNumSamples();
	const int numChannels = buffer.getNumChannels();
	
	// Convert ms to samples
	const float targetDelaySamples = delayMs * 0.001f * static_cast<float> (currentSampleRate);
	
	if (targetDelaySamples <= 0.0f)
		return;
	
	// Select delay line and smoother for this loader
	auto& delayLine = isA ? stateA.delayLine : stateB.delayLine;
	auto& smoother = isA ? stateA.smoothedDelay : stateB.smoothedDelay;
	
	// Set target delay with smoothing (prevents clicks when changing delay)
	smoother.setTargetValue (targetDelaySamples);
	
	// Apply smoothed delay sample-by-sample
	for (int i = 0; i < numSamples; ++i)
	{
		const float currentDelay = smoother.getNextValue();
		delayLine.setDelay (currentDelay);
		
		// Process each channel
		for (int ch = 0; ch < numChannels; ++ch)
		{
			auto* channelData = buffer.getWritePointer (ch);
			const float input = channelData[i];
			
			// Push input and pop delayed output
			delayLine.pushSample (ch, input);
			channelData[i] = delayLine.popSample (ch);
		}
	}
}

void CABTRAudioProcessor::calculateAutoAlignment()
{
	// Both IRs must be loaded
	if (stateA.currentFilePath.isEmpty() || stateB.currentFilePath.isEmpty())
	{
		LOG_IR_EVENT ("ALIGN: skipped — both IRs must be loaded");
		return;
	}

	const auto& irA = stateA.impulseResponse;
	const auto& irB = stateB.impulseResponse;
	const int lenA = irA.getNumSamples();
	const int lenB = irB.getNumSamples();

	if (lenA == 0 || lenB == 0)
	{
		LOG_IR_EVENT ("ALIGN: skipped — IR buffer empty (A=" + juce::String (lenA) + ", B=" + juce::String (lenB) + ")");
		return;
	}

	const float* dataA = irA.getReadPointer (0);
	const float* dataB = irB.getReadPointer (0);

	// Max lag for guitar cabs: 5ms (240 samples @ 48kHz) is plenty
	// Beyond that, xcorr hits resonance artifacts and false positives
	const int maxLagSamples = static_cast<int> (currentSampleRate * 0.005); // 5ms
	const int maxLag = juce::jmin (maxLagSamples, juce::jmin (lenA, lenB) - 1);

	// Cross-correlation: find lag + polarity combination with best phase coherence
	// We track the raw (signed) correlation at each lag.
	// Positive peak = in-phase alignment, Negative peak = inverted alignment
	float bestCorr = 0.0f;
	int bestLag = 0;

	for (int lag = -maxLag; lag <= maxLag; ++lag)
	{
		float sum = 0.0f;
		const int start = juce::jmax (0, -lag);
		const int end = juce::jmin (lenA, lenB - lag);

		for (int n = start; n < end; ++n)
			sum += dataA[n] * dataB[n + lag];

		if (std::abs (sum) > std::abs (bestCorr))
		{
			bestCorr = sum;
			bestLag = lag;
		}
	}

	// Determine polarity: the stored impulseResponse already has INV applied,
	// so we must compensate to find the "natural" correlation between raw IRs.
	// Each active INV flips the sign of the stored buffer → flips xcorr sign.
	const bool currentInvA = parameters.getRawParameterValue (kParamInvA)->load() > 0.5f;
	const bool currentInvB = parameters.getRawParameterValue (kParamInvB)->load() > 0.5f;
	float rawCorr = bestCorr;
	if (currentInvA) rawCorr = -rawCorr;  // undo A's inversion effect on xcorr
	if (currentInvB) rawCorr = -rawCorr;  // undo B's inversion effect on xcorr
	// rawCorr now reflects correlation as if both INV were OFF
	const bool needsInvert = (rawCorr < 0.0f);

	// Convert lag from samples to ms
	const float lagMs = static_cast<float> (std::abs (bestLag)) / static_cast<float> (currentSampleRate) * 1000.0f;
	const float clampedMs = juce::jmin (lagMs, kDelayMax);

	// Reset both delays
	if (auto* p = parameters.getParameter (kParamDelayA))
		p->setValueNotifyingHost (p->convertTo0to1 (0.0f));
	if (auto* p = parameters.getParameter (kParamDelayB))
		p->setValueNotifyingHost (p->convertTo0to1 (0.0f));

	// Apply delay to the IR that needs to be shifted
	if (bestLag > 0)
	{
		if (auto* p = parameters.getParameter (kParamDelayB))
			p->setValueNotifyingHost (p->convertTo0to1 (clampedMs));
	}
	else if (bestLag < 0)
	{
		if (auto* p = parameters.getParameter (kParamDelayA))
			p->setValueNotifyingHost (p->convertTo0to1 (clampedMs));
	}

	// Set INV: apply only on B, reset A to OFF
	if (auto* p = parameters.getParameter (kParamInvA))
		p->setValueNotifyingHost (0.0f);
	if (auto* p = parameters.getParameter (kParamInvB))
		p->setValueNotifyingHost (needsInvert ? 1.0f : 0.0f);

	LOG_IR_EVENT ("ALIGN: bestLag=" + juce::String (bestLag) + " samples (" +
	              juce::String (lagMs, 3) + "ms), rawCorr=" + juce::String (rawCorr, 4) +
	              " (measured=" + juce::String (bestCorr, 4) + 
	              " invA_was=" + juce::String (currentInvA ? "ON" : "OFF") +
	              " invB_was=" + juce::String (currentInvB ? "ON" : "OFF") +
	              "), invB=" + juce::String (needsInvert ? "ON" : "OFF") +
	              " | maxLag=" + juce::String (maxLag) + " (" + 
	              juce::String (maxLag * 1000.0f / currentSampleRate, 1) + "ms)");
}

//==============================================================================
bool CABTRAudioProcessor::hasEditor() const
{
    return true; // (change this to false if you choose to not supply an editor)
}

juce::AudioProcessorEditor* CABTRAudioProcessor::createEditor()
{
    return new CABTRAudioProcessorEditor (*this);
}

//==============================================================================
// TIMER CALLBACK: Check for parameter changes and trigger IR reload on message thread
// This ensures file I/O never happens in audio thread
// Rate-limited: max 1 reload per 300ms to prevent excessive reloads when dragging sliders
//==============================================================================
void CABTRAudioProcessor::timerCallback()
{
	bool didReload = false;
	const juce::int64 now = juce::Time::currentTimeMillis();
	constexpr juce::int64 minReloadIntervalMs = 300; // Max 1 reload per 300ms (rate-limiting)
	
	// Helper lambda: check if IR-affecting params changed for a loader
	auto checkLoader = [&] (IRLoaderState& state, const char* pitchId, const char* invId,
	                         const char* normId, const char* rvsId, const char* startId,
	                         const char* endId, const char* label)
	{
		if (state.currentFilePath.isEmpty())
			return;
		
		const float pitch = parameters.getRawParameterValue (pitchId)->load();
		const bool inv = parameters.getRawParameterValue (invId)->load() > 0.5f;
		const bool norm = parameters.getRawParameterValue (normId)->load() > 0.5f;
		const bool rvs = parameters.getRawParameterValue (rvsId)->load() > 0.5f;
		const float start = parameters.getRawParameterValue (startId)->load();
		const float end = parameters.getRawParameterValue (endId)->load();
		
		const float epsilon = 0.0001f;
		const bool changed = std::abs (pitch - state.lastPitch.load()) > epsilon ||
		                     inv != state.lastInv.load() ||
		                     norm != state.lastNorm.load() ||
		                     rvs != state.lastRvs.load() ||
		                     std::abs (start - state.lastStart.load()) > epsilon ||
		                     std::abs (end - state.lastEnd.load()) > epsilon;
		
		if (changed && (now - state.lastReloadTime >= minReloadIntervalMs))
		{
			LOG_IR_EVENT ("Loader " + juce::String (label) + " param change - reloading (pitch=" + 
			              juce::String (pitch, 3) + ", start=" + juce::String (start, 1) + 
			              "ms, end=" + juce::String (end, 1) + "ms, norm=" + 
			              juce::String (norm ? "ON" : "OFF") + ", inv=" +
			              juce::String (inv ? "ON" : "OFF") + ", rvs=" +
			              juce::String (rvs ? "ON" : "OFF") + ")");
			
			loadImpulseResponse (state, state.currentFilePath);
			state.lastReloadTime = now;
			didReload = true;
		}
	};
	
	checkLoader (stateA, kParamPitchA, kParamInvA, kParamNormA, kParamRvsA, kParamStartA, kParamEndA, "A");
	checkLoader (stateB, kParamPitchB, kParamInvB, kParamNormB, kParamRvsB, kParamStartB, kParamEndB, "B");
	
	// ALIGN: momentary action — calculate cross-correlation + set delay/inv, then turn off
	{
		const float alignVal = parameters.getRawParameterValue (kParamAlign)->load();
		if (alignVal > 0.5f)
		{
			const bool enabledA = parameters.getRawParameterValue (kParamEnableA)->load() > 0.5f;
			const bool enabledB = parameters.getRawParameterValue (kParamEnableB)->load() > 0.5f;
			const bool loadedA = stateA.currentFilePath.isNotEmpty();
			const bool loadedB = stateB.currentFilePath.isNotEmpty();

			if (enabledA && enabledB && loadedA && loadedB)
			{
				LOG_IR_EVENT ("ALIGN triggered! param=" + juce::String (alignVal, 3) +
				              " A_loaded=yes B_loaded=yes");
				calculateAutoAlignment();
			}
			else
			{
				LOG_IR_EVENT ("ALIGN skipped: enableA=" + juce::String (enabledA ? "ON" : "OFF") +
				              " enableB=" + juce::String (enabledB ? "ON" : "OFF") +
				              " loadedA=" + juce::String (loadedA ? "yes" : "no") +
				              " loadedB=" + juce::String (loadedB ? "yes" : "no"));
			}

			if (auto* p = parameters.getParameter (kParamAlign))
				p->setValueNotifyingHost (0.0f);
		}
	}
	
	// Debug logging every 10 seconds
	if (didReload || (now - lastDebugTime > 10000))
	{
		if (now - lastDebugTime > 10000)
		{
			const double worstUs = worstBlockTimeUs.exchange (0.0);
			const double budgetUs = (currentBlockSize / currentSampleRate) * 1e6;
			const double cpuPct = (worstUs / budgetUs) * 100.0;
			
			LOG_IR_EVENT ("CAB-TR Stats: A_reloads=" + juce::String (reloadCountA.load()) + 
			              " B_reloads=" + juce::String (reloadCountB.load()) +
			              " | CPU worst=" + juce::String (worstUs, 0) + "us / " +
			              juce::String (budgetUs, 0) + "us budget (" + 
			              juce::String (cpuPct, 1) + "%) blocks=" + 
			              juce::String (blockCount.load()) +
			              " | fredA=" + juce::String (parameters.getRawParameterValue (kParamFredA)->load(), 2) +
			              " fredB=" + juce::String (parameters.getRawParameterValue (kParamFredB)->load(), 2) +
			              " mix=" + juce::String (parameters.getRawParameterValue (kParamMix)->load(), 2) +
			              " route=" + juce::String (static_cast<int> (parameters.getRawParameterValue (kParamRoute)->load())) +
			              " modeIn=" + juce::String (static_cast<int> (parameters.getRawParameterValue (kParamModeIn)->load())) +
			              " modeOut=" + juce::String (static_cast<int> (parameters.getRawParameterValue (kParamMode)->load())) +
			              " | A_loaded=" + juce::String (stateA.currentFilePath.isNotEmpty() ? "yes" : "no") +
			              " B_loaded=" + juce::String (stateB.currentFilePath.isNotEmpty() ? "yes" : "no") +
			              " align=" + juce::String (parameters.getRawParameterValue (kParamAlign)->load(), 1) +
			              " blockSize=" + juce::String (currentBlockSize) +
			              " sr=" + juce::String (static_cast<int> (currentSampleRate)));
			lastDebugTime = now;
		}
	}
}

//==============================================================================
void CABTRAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
	// Save parameters (automatic via APVTS)
	auto state = parameters.copyState();
	
	// Add IR file paths to state
	state.setProperty ("irPathA", stateA.currentFilePath, nullptr);
	state.setProperty ("irPathB", stateB.currentFilePath, nullptr);
	
	// Convert to XML and save
	auto xml = state.createXml();
	if (xml != nullptr)
		copyXmlToBinary (*xml, destData);
}

void CABTRAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
	// Load parameters
	auto xml = getXmlFromBinary (data, sizeInBytes);
	if (xml != nullptr)
	{
		auto state = juce::ValueTree::fromXml (*xml);
		if (state.isValid())
		{
			// Restore APVTS parameters
			parameters.replaceState (state);
			
			// Restore IR file paths
			const juce::String pathA = state.getProperty ("irPathA", "");
			const juce::String pathB = state.getProperty ("irPathB", "");
			
			LOG_IR_EVENT ("State loaded: IR_A=" + pathA + ", IR_B=" + pathB);
			
			// Reload IRs if paths exist
			if (pathA.isNotEmpty())
				loadImpulseResponse (stateA, pathA);
			if (pathB.isNotEmpty())
				loadImpulseResponse (stateB, pathB);
			
			// Update UI file display labels
			if (auto* editor = dynamic_cast<CABTRAudioProcessorEditor*> (getActiveEditor()))
				editor->updateFileDisplayLabels (pathA, pathB);
		}
	}
}

//==============================================================================
// This creates new instances of the plugin..
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new CABTRAudioProcessor();
}
