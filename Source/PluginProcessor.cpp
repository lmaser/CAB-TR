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
       // Already loadedx
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
       //    (useful during development - the DLL lives in the repo)
       {
           wchar_t tp[MAX_PATH] = L"E:\\Workspace\\Production\\JUCE_projects\\CAB-TR\\ThirdParty\\fftw3\\";
           if (tryLoadFrom (tp, L"[thirdparty]")) return;
       }

       // 3) The VST3 install directory under Program Files
       {
           wchar_t pf[MAX_PATH] = L"C:\\Program Files\\Common Files\\VST3\\NEMESTER PLUGINS\\TR SERIES\\1.4\\";
           if (tryLoadFrom (pf, L"[programfiles]")) return;
       }

       g_fftwDiag += "FFTW: NOT FOUND in any candidate directory";
   }
 }
#endif

// ---------------------------------------------------------------------------
// DSP utility functions (consistent with ECHO-TR)
// ---------------------------------------------------------------------------
namespace
{
	constexpr const char* kVariationInstanceSeedProperty = "variationInstanceSeed";

	inline juce::int64 createVariationInstanceSeed() noexcept
	{
		auto& rng = juce::Random::getSystemRandom();
		const auto hi = static_cast<juce::int64> (static_cast<juce::uint32> (rng.nextInt()));
		const auto lo = static_cast<juce::int64> (static_cast<juce::uint32> (rng.nextInt()));
		const auto time = static_cast<juce::int64> (juce::Time::getMillisecondCounterHiRes() * 1000.0);
		const auto seed = (hi << 32) ^ lo ^ time ^ 0x5A7CABD15EEDLL;
		return seed != 0 ? seed : 0x5A7CABD15EEDLL;
	}

	// Fast dB->linear conversion using exp2 instead of pow(10, dB/20).
	// Mathematically equivalent: 10^(dB/20) = 2^(dB * log2(10)/20) = 2^(dB * 0.16609640474)
	inline float fastDecibelsToGain (float dB) noexcept
	{
		return (dB <= -100.0f) ? 0.0f : std::exp2 (dB * 0.16609640474f);
	}

	inline float gainFaderDecibelsToGain (float dB) noexcept
	{
		return (dB <= CABTRAudioProcessor::kGainFloorDb) ? 0.0f : std::exp2 (dB * 0.16609640474f);
	}

	inline juce::NormalisableRange<float> makeGainFaderRange() noexcept
	{
		return juce::NormalisableRange<float> (CABTRAudioProcessor::kGainFloorDb,
		                                       CABTRAudioProcessor::kGainMaxDb,
		                                       0.0f,
		                                       CABTRAudioProcessor::kGainSkew);
	}

	// Relaxed atomic load helpers - safe for audio thread (single-writer GUI, single-reader audio).
	// Avoids unnecessary memory fences from default seq_cst ordering.
	inline float loadRelaxed (std::atomic<float>* p, float def = 0.0f) noexcept
	{
		return p != nullptr ? p->load (std::memory_order_relaxed) : def;
	}
	inline bool loadRelaxedBool (std::atomic<float>* p, bool def = false) noexcept
	{
		return loadRelaxed (p, def ? 1.0f : 0.0f) > 0.5f;
	}
	inline int loadRelaxedInt (std::atomic<float>* p, int def = 0) noexcept
	{
		return static_cast<int> (std::lround (loadRelaxed (p, static_cast<float> (def))));
	}

	struct BiquadCoefficients
	{
		float b0 = 1.0f;
		float b1 = 0.0f;
		float b2 = 0.0f;
		float a1 = 0.0f;
		float a2 = 0.0f;
	};

	inline BiquadCoefficients makeDetectorFirstOrderHighPass (float frequency, float sampleRate) noexcept
	{
		const float freq = juce::jlimit (20.0f, sampleRate * 0.45f, frequency);
		const float k = std::tan (juce::MathConstants<float>::pi * freq / sampleRate);
		const float norm = 1.0f / (1.0f + k);

		BiquadCoefficients c;
		c.b0 = norm;
		c.b1 = -norm;
		c.a1 = (k - 1.0f) * norm;
		return c;
	}

	inline BiquadCoefficients makeDetectorFirstOrderLowPass (float frequency, float sampleRate) noexcept
	{
		const float freq = juce::jlimit (20.0f, sampleRate * 0.45f, frequency);
		const float k = std::tan (juce::MathConstants<float>::pi * freq / sampleRate);
		const float norm = 1.0f / (1.0f + k);

		BiquadCoefficients c;
		c.b0 = k * norm;
		c.b1 = c.b0;
		c.a1 = (k - 1.0f) * norm;
		return c;
	}

	inline BiquadCoefficients makeDetectorHighPass (float frequency, float sampleRate, float q) noexcept
	{
		const float freq = juce::jlimit (20.0f, sampleRate * 0.45f, frequency);
		const float omega = juce::MathConstants<float>::twoPi * freq / sampleRate;
		const float sinOmega = std::sin (omega);
		const float cosOmega = std::cos (omega);
		const float alpha = sinOmega / (2.0f * q);
		const float invA0 = 1.0f / (1.0f + alpha);

		BiquadCoefficients c;
		c.b0 = ((1.0f + cosOmega) * 0.5f) * invA0;
		c.b1 = (-(1.0f + cosOmega)) * invA0;
		c.b2 = c.b0;
		c.a1 = (-2.0f * cosOmega) * invA0;
		c.a2 = (1.0f - alpha) * invA0;
		return c;
	}

	inline BiquadCoefficients makeDetectorLowPass (float frequency, float sampleRate, float q) noexcept
	{
		const float freq = juce::jlimit (20.0f, sampleRate * 0.45f, frequency);
		const float omega = juce::MathConstants<float>::twoPi * freq / sampleRate;
		const float sinOmega = std::sin (omega);
		const float cosOmega = std::cos (omega);
		const float alpha = sinOmega / (2.0f * q);
		const float invA0 = 1.0f / (1.0f + alpha);

		BiquadCoefficients c;
		c.b0 = ((1.0f - cosOmega) * 0.5f) * invA0;
		c.b1 = (1.0f - cosOmega) * invA0;
		c.b2 = c.b0;
		c.a1 = (-2.0f * cosOmega) * invA0;
		c.a2 = (1.0f - alpha) * invA0;
		return c;
	}

	inline BiquadCoefficients makeDetectorHighPassForSlope (float frequency, float sampleRate, int slope, bool secondStage) noexcept
	{
		if (slope <= 0)
			return secondStage ? BiquadCoefficients {} : makeDetectorFirstOrderHighPass (frequency, sampleRate);

		const float q = (slope >= 2)
		              ? (secondStage ? CABTRAudioProcessor::kBW4_Q2 : CABTRAudioProcessor::kBW4_Q1)
		              : CABTRAudioProcessor::kSqrt2Over2;
		return makeDetectorHighPass (frequency, sampleRate, q);
	}

	inline BiquadCoefficients makeDetectorLowPassForSlope (float frequency, float sampleRate, int slope, bool secondStage) noexcept
	{
		if (slope <= 0)
			return secondStage ? BiquadCoefficients {} : makeDetectorFirstOrderLowPass (frequency, sampleRate);

		const float q = (slope >= 2)
		              ? (secondStage ? CABTRAudioProcessor::kBW4_Q2 : CABTRAudioProcessor::kBW4_Q1)
		              : CABTRAudioProcessor::kSqrt2Over2;
		return makeDetectorLowPass (frequency, sampleRate, q);
	}

	inline float processDetectorBiquad (float x,
	                                    CABTRAudioProcessor::IRLoaderState::ExpSidechainBiquadState& state,
	                                    const BiquadCoefficients& coeffs,
	                                    int channel) noexcept
	{
		auto& z1 = state.z1[channel];
		auto& z2 = state.z2[channel];
		const float y = coeffs.b0 * x + z1;
		z1 = coeffs.b1 * x - coeffs.a1 * y + z2;
		z2 = coeffs.b2 * x - coeffs.a2 * y;
		return y;
	}

	// Compute 1st-order symmetric tilt shelf coefficients (bilinear, pivot 1kHz).
	// Shared by per-loader tilt EQ and global MATCH tilt EQ.
	inline void computeTiltShelfCoeffs (double sampleRate, float slopeDb,
	                                    float& outB0, float& outB1, float& outA1) noexcept
	{
		if (std::abs (slopeDb) < 0.1f)
		{
			outB0 = 1.0f; outB1 = 0.0f; outA1 = 0.0f;
			return;
		}
		const double pivot = 1000.0;
		const double octavesToNyquist = std::log2 ((sampleRate * 0.5) / pivot);
		const double gainAtNyquistDb  = static_cast<double> (slopeDb) * octavesToNyquist;
		const double gNy = std::pow (10.0, gainAtNyquistDb / 20.0);
		const double wc = 2.0 * sampleRate * std::tan (juce::MathConstants<double>::pi * pivot / sampleRate);
		const double K  = wc / (2.0 * sampleRate);
		const double g  = std::sqrt (gNy);
		const double norm = 1.0 / (1.0 + K * g);
		outB0 = static_cast<float> ((g + K) * norm);
		outB1 = static_cast<float> ((K - g) * norm);
		outA1 = static_cast<float> ((K * g - 1.0) * norm);
	}
}

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
	// Convolution engines (StereoThreadedConvolver) - prepared in prepareToPlay()
	variationInstanceSeed_ = createVariationInstanceSeed();
	
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

	// ============================================================================
	//  IR Loader A Parameters
	// ============================================================================
	layout.add (std::make_unique<juce::AudioParameterBool> (
		kParamEnableA, "Enable A", false));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamHpFreqA, "HP Freq A", 
		juce::NormalisableRange<float> (kFilterFreqMin, kFilterFreqMax, 0.01f, 0.35f),
		kFilterHpFreqDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamLpFreqA, "LP Freq A", 
		juce::NormalisableRange<float> (kFilterFreqMin, kFilterFreqMax, 0.01f, 0.35f),
		kFilterLpFreqDefault));
	layout.add (std::make_unique<juce::AudioParameterBool> (
		kParamHpOnA, "HP On A", false));
	layout.add (std::make_unique<juce::AudioParameterBool> (
		kParamLpOnA, "LP On A", false));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamHpSlopeA, "HP Slope A",
		juce::NormalisableRange<float> ((float) kFilterSlopeMin, (float) kFilterSlopeMax, 1.0f),
		(float) kFilterSlopeDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamLpSlopeA, "LP Slope A",
		juce::NormalisableRange<float> ((float) kFilterSlopeMin, (float) kFilterSlopeMax, 1.0f),
		(float) kFilterSlopeDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamInA, "In A",
		makeGainFaderRange(),
		kInDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamOutA, "Out A", makeGainFaderRange(), kOutDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamTiltA, "Tilt A",
		juce::NormalisableRange<float> (kTiltMin, kTiltMax, 0.01f),
		kTiltDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamStartA, "Start A",
		juce::NormalisableRange<float> (kStartMin, kStartMax, 0.1f, 0.15f), // Skew 0.15 = deep log (high resolution at low values)
		kStartDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamEndA, "End A",
		juce::NormalisableRange<float> (kEndMin, kEndMax, 0.1f, 0.15f), // Skew 0.15 = deep log
		kEndDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamSizeA, "Size A", 
		juce::NormalisableRange<float> (kSizeMin, kSizeMax, 0.01f, 0.5f), 
		kSizeDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamDelayA, "Delay A",
		juce::NormalisableRange<float> (kDelayMin, kDelayMax, 0.001f, 0.25f),
		kDelayDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamPanA, "Pan A", kPanMin, kPanMax, kPanDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamFredA, "Angle A", kFredMin, kFredMax, kFredDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamPosA, "Distance A", kPosMin, kPosMax, kPosDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamVariationA, "Variation A",
		juce::NormalisableRange<float> (kVariationMin, kVariationMax, 0.001f),
		kVariationDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamResoA, "Reso A",
		juce::NormalisableRange<float> (kResoMin, kResoMax, 0.01f), kResoDefault));
	layout.add (std::make_unique<juce::AudioParameterBool> (
		kParamInvA, "Invert A", false));
	layout.add (std::make_unique<juce::AudioParameterBool> (
		kParamNormA, "Normalize A", false));
	layout.add (std::make_unique<juce::AudioParameterBool> (
		kParamRvsA, "Reverse A", false));
	layout.add (std::make_unique<juce::AudioParameterBool> (
		kParamExpA, "Exp A", false));
	layout.add (std::make_unique<juce::AudioParameterBool> (
		kParamExpOrderA, "Exp Order A", false)); // false=PRE, true=POST
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamExpRatioA, "Exp Ratio A",
		juce::NormalisableRange<float> (kExpRatioMin, kExpRatioMax, 0.1f), kExpRatioDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamExpThreshA, "Exp Thresh A",
		juce::NormalisableRange<float> (kExpThreshMin, kExpThreshMax, 0.1f), kExpThreshDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamExpKneeA, "Exp Knee A",
		juce::NormalisableRange<float> (kExpKneeMin, kExpKneeMax, 0.1f), kExpKneeDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamExpAtkA, "Exp Atk A",
		juce::NormalisableRange<float> (kExpAtkMin, kExpAtkMax, 0.01f, 0.3f), kExpAtkDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamExpRelA, "Exp Rel A",
		juce::NormalisableRange<float> (kExpRelMin, kExpRelMax, 0.01f, 0.3f), kExpRelDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamExpScHpA, "Exp SC HP A",
		juce::NormalisableRange<float> (kExpScFreqMin, kExpScFreqMax, 0.01f, 0.35f), kExpScHpDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamExpScLpA, "Exp SC LP A",
		juce::NormalisableRange<float> (kExpScFreqMin, kExpScFreqMax, 0.01f, 0.35f), kExpScLpDefault));
	layout.add (std::make_unique<juce::AudioParameterBool> (
		kParamExpScHpOnA, "Exp SC HP On A", kExpScHpOnDefault));
	layout.add (std::make_unique<juce::AudioParameterBool> (
		kParamExpScLpOnA, "Exp SC LP On A", kExpScLpOnDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamExpScHpSlopeA, "Exp SC HP Slope A",
		juce::NormalisableRange<float> ((float) kFilterSlopeMin, (float) kFilterSlopeMax, 1.0f),
		(float) kExpScHpSlopeDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamExpScLpSlopeA, "Exp SC LP Slope A",
		juce::NormalisableRange<float> ((float) kFilterSlopeMin, (float) kFilterSlopeMax, 1.0f),
		(float) kExpScLpSlopeDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamExpScGainA, "Exp SC Gain A", makeGainFaderRange(), kExpScGainDefault));
	layout.add (std::make_unique<juce::AudioParameterBool> (
		kParamChaosA, "Chaos D A", false));
	layout.add (std::make_unique<juce::AudioParameterBool> (
		kParamChaosFilterA, "Chaos F A", false));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamChaosAmtA, "Chaos Amount A",
		juce::NormalisableRange<float> (kChaosAmtMin, kChaosAmtMax, 0.1f), kChaosAmtDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamChaosSpdA, "Chaos Speed A",
		juce::NormalisableRange<float> (kChaosSpdMin, kChaosSpdMax, 0.01f, 0.3f), kChaosSpdDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamChaosAmtFilterA, "Chaos Filter Amount A",
		juce::NormalisableRange<float> (kChaosAmtMin, kChaosAmtMax, 0.1f), kChaosAmtDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamChaosSpdFilterA, "Chaos Filter Speed A",
		juce::NormalisableRange<float> (kChaosSpdMin, kChaosSpdMax, 0.01f, 0.3f), kChaosSpdDefault));
	layout.add (std::make_unique<juce::AudioParameterChoice> (
		kParamModeInA, "Mode In A", juce::StringArray { "L+R", "MID", "SIDE" }, kModeDefault));
	layout.add (std::make_unique<juce::AudioParameterChoice> (
		kParamModeOutA, "Mode Out A", juce::StringArray { "L+R", "MID", "SIDE" }, kModeDefault));
	layout.add (std::make_unique<juce::AudioParameterChoice> (
		kParamSumBusA, "Sum Bus A", juce::StringArray { "ST", "->M", "->S" }, kSumBusDefault));
	layout.add (std::make_unique<juce::AudioParameterChoice> (
		kParamFilterPosA, "Filter Pos A",
		juce::StringArray { "FvTv", "F^T^", "F^Tv", "FvT^" },
		kFilterPosDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamMixA, "Mix A", kGlobalMixMin, kGlobalMixMax, kGlobalMixDefault));

	// ============================================================================
	//  IR Loader B Parameters
	// ============================================================================
	layout.add (std::make_unique<juce::AudioParameterBool> (
		kParamEnableB, "Enable B", false));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamHpFreqB, "HP Freq B", 
		juce::NormalisableRange<float> (kFilterFreqMin, kFilterFreqMax, 0.01f, 0.35f),
		kFilterHpFreqDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamLpFreqB, "LP Freq B", 
		juce::NormalisableRange<float> (kFilterFreqMin, kFilterFreqMax, 0.01f, 0.35f),
		kFilterLpFreqDefault));
	layout.add (std::make_unique<juce::AudioParameterBool> (
		kParamHpOnB, "HP On B", false));
	layout.add (std::make_unique<juce::AudioParameterBool> (
		kParamLpOnB, "LP On B", false));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamHpSlopeB, "HP Slope B",
		juce::NormalisableRange<float> ((float) kFilterSlopeMin, (float) kFilterSlopeMax, 1.0f),
		(float) kFilterSlopeDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamLpSlopeB, "LP Slope B",
		juce::NormalisableRange<float> ((float) kFilterSlopeMin, (float) kFilterSlopeMax, 1.0f),
		(float) kFilterSlopeDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamInB, "In B",
		makeGainFaderRange(),
		kInDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamOutB, "Out B", makeGainFaderRange(), kOutDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamTiltB, "Tilt B",
		juce::NormalisableRange<float> (kTiltMin, kTiltMax, 0.01f),
		kTiltDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamStartB, "Start B",
		juce::NormalisableRange<float> (kStartMin, kStartMax, 0.1f, 0.15f), // Skew 0.15 = deep log (high resolution at low values)
		kStartDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamEndB, "End B",
		juce::NormalisableRange<float> (kEndMin, kEndMax, 0.1f, 0.15f), // Skew 0.15 = deep log
		kEndDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamSizeB, "Size B", 
		juce::NormalisableRange<float> (kSizeMin, kSizeMax, 0.01f, 0.5f), 
		kSizeDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamDelayB, "Delay B",
		juce::NormalisableRange<float> (kDelayMin, kDelayMax, 0.001f, 0.25f),
		kDelayDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamPanB, "Pan B", kPanMin, kPanMax, kPanDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamFredB, "Angle B", kFredMin, kFredMax, kFredDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamPosB, "Distance B", kPosMin, kPosMax, kPosDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamVariationB, "Variation B",
		juce::NormalisableRange<float> (kVariationMin, kVariationMax, 0.001f),
		kVariationDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamResoB, "Reso B",
		juce::NormalisableRange<float> (kResoMin, kResoMax, 0.01f), kResoDefault));
	layout.add (std::make_unique<juce::AudioParameterBool> (
		kParamInvB, "Invert B", false));
	layout.add (std::make_unique<juce::AudioParameterBool> (
		kParamNormB, "Normalize B", false));
	layout.add (std::make_unique<juce::AudioParameterBool> (
		kParamRvsB, "Reverse B", false));
	layout.add (std::make_unique<juce::AudioParameterBool> (
		kParamExpB, "Exp B", false));
	layout.add (std::make_unique<juce::AudioParameterBool> (
		kParamExpOrderB, "Exp Order B", false));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamExpRatioB, "Exp Ratio B",
		juce::NormalisableRange<float> (kExpRatioMin, kExpRatioMax, 0.1f), kExpRatioDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamExpThreshB, "Exp Thresh B",
		juce::NormalisableRange<float> (kExpThreshMin, kExpThreshMax, 0.1f), kExpThreshDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamExpKneeB, "Exp Knee B",
		juce::NormalisableRange<float> (kExpKneeMin, kExpKneeMax, 0.1f), kExpKneeDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamExpAtkB, "Exp Atk B",
		juce::NormalisableRange<float> (kExpAtkMin, kExpAtkMax, 0.01f, 0.3f), kExpAtkDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamExpRelB, "Exp Rel B",
		juce::NormalisableRange<float> (kExpRelMin, kExpRelMax, 0.01f, 0.3f), kExpRelDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamExpScHpB, "Exp SC HP B",
		juce::NormalisableRange<float> (kExpScFreqMin, kExpScFreqMax, 0.01f, 0.35f), kExpScHpDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamExpScLpB, "Exp SC LP B",
		juce::NormalisableRange<float> (kExpScFreqMin, kExpScFreqMax, 0.01f, 0.35f), kExpScLpDefault));
	layout.add (std::make_unique<juce::AudioParameterBool> (
		kParamExpScHpOnB, "Exp SC HP On B", kExpScHpOnDefault));
	layout.add (std::make_unique<juce::AudioParameterBool> (
		kParamExpScLpOnB, "Exp SC LP On B", kExpScLpOnDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamExpScHpSlopeB, "Exp SC HP Slope B",
		juce::NormalisableRange<float> ((float) kFilterSlopeMin, (float) kFilterSlopeMax, 1.0f),
		(float) kExpScHpSlopeDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamExpScLpSlopeB, "Exp SC LP Slope B",
		juce::NormalisableRange<float> ((float) kFilterSlopeMin, (float) kFilterSlopeMax, 1.0f),
		(float) kExpScLpSlopeDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamExpScGainB, "Exp SC Gain B", makeGainFaderRange(), kExpScGainDefault));
	layout.add (std::make_unique<juce::AudioParameterBool> (
		kParamChaosB, "Chaos D B", false));
	layout.add (std::make_unique<juce::AudioParameterBool> (
		kParamChaosFilterB, "Chaos F B", false));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamChaosAmtB, "Chaos Amount B",
		juce::NormalisableRange<float> (kChaosAmtMin, kChaosAmtMax, 0.1f), kChaosAmtDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamChaosSpdB, "Chaos Speed B",
		juce::NormalisableRange<float> (kChaosSpdMin, kChaosSpdMax, 0.01f, 0.3f), kChaosSpdDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamChaosAmtFilterB, "Chaos Filter Amount B",
		juce::NormalisableRange<float> (kChaosAmtMin, kChaosAmtMax, 0.1f), kChaosAmtDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamChaosSpdFilterB, "Chaos Filter Speed B",
		juce::NormalisableRange<float> (kChaosSpdMin, kChaosSpdMax, 0.01f, 0.3f), kChaosSpdDefault));
	layout.add (std::make_unique<juce::AudioParameterChoice> (
		kParamModeInB, "Mode In B", juce::StringArray { "L+R", "MID", "SIDE" }, kModeDefault));
	layout.add (std::make_unique<juce::AudioParameterChoice> (
		kParamModeOutB, "Mode Out B", juce::StringArray { "L+R", "MID", "SIDE" }, kModeDefault));
	layout.add (std::make_unique<juce::AudioParameterChoice> (
		kParamSumBusB, "Sum Bus B", juce::StringArray { "ST", "->M", "->S" }, kSumBusDefault));
	layout.add (std::make_unique<juce::AudioParameterChoice> (
		kParamFilterPosB, "Filter Pos B",
		juce::StringArray { "FvTv", "F^T^", "F^Tv", "FvT^" },
		kFilterPosDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamMixB, "Mix B", kGlobalMixMin, kGlobalMixMax, kGlobalMixDefault));

	// ============================================================================
	//  IR Loader C Parameters
	// ============================================================================
	layout.add (std::make_unique<juce::AudioParameterBool> (
		kParamEnableC, "Enable C", false));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamHpFreqC, "HP Freq C", 
		juce::NormalisableRange<float> (kFilterFreqMin, kFilterFreqMax, 0.01f, 0.35f),
		kFilterHpFreqDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamLpFreqC, "LP Freq C", 
		juce::NormalisableRange<float> (kFilterFreqMin, kFilterFreqMax, 0.01f, 0.35f),
		kFilterLpFreqDefault));
	layout.add (std::make_unique<juce::AudioParameterBool> (
		kParamHpOnC, "HP On C", false));
	layout.add (std::make_unique<juce::AudioParameterBool> (
		kParamLpOnC, "LP On C", false));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamHpSlopeC, "HP Slope C",
		juce::NormalisableRange<float> ((float) kFilterSlopeMin, (float) kFilterSlopeMax, 1.0f),
		(float) kFilterSlopeDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamLpSlopeC, "LP Slope C",
		juce::NormalisableRange<float> ((float) kFilterSlopeMin, (float) kFilterSlopeMax, 1.0f),
		(float) kFilterSlopeDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamInC, "In C",
		makeGainFaderRange(),
		kInDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamOutC, "Out C", makeGainFaderRange(), kOutDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamTiltC, "Tilt C",
		juce::NormalisableRange<float> (kTiltMin, kTiltMax, 0.01f),
		kTiltDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamStartC, "Start C",
		juce::NormalisableRange<float> (kStartMin, kStartMax, 0.1f, 0.15f),
		kStartDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamEndC, "End C",
		juce::NormalisableRange<float> (kEndMin, kEndMax, 0.1f, 0.15f),
		kEndDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamSizeC, "Size C", 
		juce::NormalisableRange<float> (kSizeMin, kSizeMax, 0.01f, 0.5f), 
		kSizeDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamDelayC, "Delay C",
		juce::NormalisableRange<float> (kDelayMin, kDelayMax, 0.001f, 0.25f),
		kDelayDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamPanC, "Pan C", kPanMin, kPanMax, kPanDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamFredC, "Angle C", kFredMin, kFredMax, kFredDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamPosC, "Distance C", kPosMin, kPosMax, kPosDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamVariationC, "Variation C",
		juce::NormalisableRange<float> (kVariationMin, kVariationMax, 0.001f),
		kVariationDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamResoC, "Reso C",
		juce::NormalisableRange<float> (kResoMin, kResoMax, 0.01f), kResoDefault));
	layout.add (std::make_unique<juce::AudioParameterBool> (
		kParamInvC, "Invert C", false));
	layout.add (std::make_unique<juce::AudioParameterBool> (
		kParamNormC, "Normalize C", false));
	layout.add (std::make_unique<juce::AudioParameterBool> (
		kParamRvsC, "Reverse C", false));
	layout.add (std::make_unique<juce::AudioParameterBool> (
		kParamExpC, "Exp C", false));
	layout.add (std::make_unique<juce::AudioParameterBool> (
		kParamExpOrderC, "Exp Order C", false));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamExpRatioC, "Exp Ratio C",
		juce::NormalisableRange<float> (kExpRatioMin, kExpRatioMax, 0.1f), kExpRatioDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamExpThreshC, "Exp Thresh C",
		juce::NormalisableRange<float> (kExpThreshMin, kExpThreshMax, 0.1f), kExpThreshDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamExpKneeC, "Exp Knee C",
		juce::NormalisableRange<float> (kExpKneeMin, kExpKneeMax, 0.1f), kExpKneeDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamExpAtkC, "Exp Atk C",
		juce::NormalisableRange<float> (kExpAtkMin, kExpAtkMax, 0.01f, 0.3f), kExpAtkDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamExpRelC, "Exp Rel C",
		juce::NormalisableRange<float> (kExpRelMin, kExpRelMax, 0.01f, 0.3f), kExpRelDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamExpScHpC, "Exp SC HP C",
		juce::NormalisableRange<float> (kExpScFreqMin, kExpScFreqMax, 0.01f, 0.35f), kExpScHpDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamExpScLpC, "Exp SC LP C",
		juce::NormalisableRange<float> (kExpScFreqMin, kExpScFreqMax, 0.01f, 0.35f), kExpScLpDefault));
	layout.add (std::make_unique<juce::AudioParameterBool> (
		kParamExpScHpOnC, "Exp SC HP On C", kExpScHpOnDefault));
	layout.add (std::make_unique<juce::AudioParameterBool> (
		kParamExpScLpOnC, "Exp SC LP On C", kExpScLpOnDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamExpScHpSlopeC, "Exp SC HP Slope C",
		juce::NormalisableRange<float> ((float) kFilterSlopeMin, (float) kFilterSlopeMax, 1.0f),
		(float) kExpScHpSlopeDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamExpScLpSlopeC, "Exp SC LP Slope C",
		juce::NormalisableRange<float> ((float) kFilterSlopeMin, (float) kFilterSlopeMax, 1.0f),
		(float) kExpScLpSlopeDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamExpScGainC, "Exp SC Gain C", makeGainFaderRange(), kExpScGainDefault));
	layout.add (std::make_unique<juce::AudioParameterBool> (
		kParamChaosC, "Chaos D C", false));
	layout.add (std::make_unique<juce::AudioParameterBool> (
		kParamChaosFilterC, "Chaos F C", false));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamChaosAmtC, "Chaos Amount C",
		juce::NormalisableRange<float> (kChaosAmtMin, kChaosAmtMax, 0.1f), kChaosAmtDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamChaosSpdC, "Chaos Speed C",
		juce::NormalisableRange<float> (kChaosSpdMin, kChaosSpdMax, 0.01f, 0.3f), kChaosSpdDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamChaosAmtFilterC, "Chaos Filter Amount C",
		juce::NormalisableRange<float> (kChaosAmtMin, kChaosAmtMax, 0.1f), kChaosAmtDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamChaosSpdFilterC, "Chaos Filter Speed C",
		juce::NormalisableRange<float> (kChaosSpdMin, kChaosSpdMax, 0.01f, 0.3f), kChaosSpdDefault));
	layout.add (std::make_unique<juce::AudioParameterChoice> (
		kParamModeInC, "Mode In C", juce::StringArray { "L+R", "MID", "SIDE" }, kModeDefault));
	layout.add (std::make_unique<juce::AudioParameterChoice> (
		kParamModeOutC, "Mode Out C", juce::StringArray { "L+R", "MID", "SIDE" }, kModeDefault));
	layout.add (std::make_unique<juce::AudioParameterChoice> (
		kParamSumBusC, "Sum Bus C", juce::StringArray { "ST", "->M", "->S" }, kSumBusDefault));
	layout.add (std::make_unique<juce::AudioParameterChoice> (
		kParamFilterPosC, "Filter Pos C",
		juce::StringArray { "FvTv", "F^T^", "F^Tv", "FvT^" },
		kFilterPosDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamMixC, "Mix C", kGlobalMixMin, kGlobalMixMax, kGlobalMixDefault));

	// ============================================================================
	//  Global Parameters
	// ============================================================================
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamInput, "Input", makeGainFaderRange(), kInputDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamOutput, "Output", makeGainFaderRange(), kOutputDefault));
	layout.add (std::make_unique<juce::AudioParameterChoice> (
		kParamRoute, "Route", juce::StringArray { "A>B>C", "A|B|C", "A>B|C", "A|B>C", "(A|B)>C", "A>(B|C)" }, kRouteDefault));
	layout.add (std::make_unique<juce::AudioParameterBool> (
		kParamAlign, "Align", false));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamMix, "Mix", kGlobalMixMin, kGlobalMixMax, kGlobalMixDefault));

	// Mix Mode (INSERT / SEND) + Dry/Wet levels for SEND mode
	layout.add (std::make_unique<juce::AudioParameterChoice> (
		kParamMixMode, "Mix Mode",
		juce::StringArray { "INSERT", "SEND" }, kMixModeDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamDryLevel, "Dry Level",
		juce::NormalisableRange<float> (0.0f, 1.0f, 0.0f, 1.0f), kDryLevelDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamWetLevel, "Wet Level",
		juce::NormalisableRange<float> (0.0f, 1.0f, 0.0f, 1.0f), kWetLevelDefault));

	layout.add (std::make_unique<juce::AudioParameterChoice> (
		kParamMatch, "Match", juce::StringArray { "None", "White", "Pink (-3dB)", "Brown (-6dB)", "Bright (+3dB)", "Bright+ (+6dB)" }, kMatchDefault));
	layout.add (std::make_unique<juce::AudioParameterChoice> (
		kParamTrim, "Norm", juce::StringArray { "Off", "0 dB", "-3 dB", "-6 dB", "-12 dB", "-18 dB" }, kTrimDefault));

	// Limiter
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamLimThreshold, "Lim Threshold",
		juce::NormalisableRange<float> (kLimThresholdMin, kLimThresholdMax, 0.1f), kLimThresholdDefault));
	layout.add (std::make_unique<juce::AudioParameterChoice> (
		kParamLimMode, "Lim Mode", juce::StringArray { "NONE", "WET", "GLOBAL" }, kLimModeDefault));

	// Invert Polarity / Invert Stereo
	layout.add (std::make_unique<juce::AudioParameterChoice> (
		kParamInvPol, "Invert Polarity",
		juce::StringArray { "NONE", "WET", "GLOBAL" }, kInvPolDefault));
	layout.add (std::make_unique<juce::AudioParameterChoice> (
		kParamInvStr, "Invert Stereo",
		juce::StringArray { "NONE", "WET", "GLOBAL" }, kInvStrDefault));

	// ============================================================================
	//  UI State Parameters (hidden from automation)
	// ============================================================================
	layout.add (std::make_unique<juce::AudioParameterInt> (
		juce::ParameterID { kParamUiWidth, 1 }, "UI Width", 360, 1080, 360,
		juce::AudioParameterIntAttributes().withAutomatable (false)));

	layout.add (std::make_unique<juce::AudioParameterInt> (
		juce::ParameterID { kParamUiHeight, 1 }, "UI Height", 300, 1500, 752,
		juce::AudioParameterIntAttributes().withAutomatable (false)));

	layout.add (std::make_unique<juce::AudioParameterInt> (
		juce::ParameterID { kParamUiPalette, 1 }, "UI Palette", 0, 1, 0,
		juce::AudioParameterIntAttributes().withAutomatable (false)));

	layout.add (std::make_unique<juce::AudioParameterBool> (
		juce::ParameterID { kParamUiFxTail, 1 }, "UI FX Tail", false,
		juce::AudioParameterBoolAttributes().withAutomatable (false)));

	layout.add (std::make_unique<juce::AudioParameterInt> (
		juce::ParameterID { kParamUiColor0, 1 }, "UI Color 0", 0, 0xFFFFFF, 0x00FF00,
		juce::AudioParameterIntAttributes().withAutomatable (false)));

	layout.add (std::make_unique<juce::AudioParameterInt> (
		juce::ParameterID { kParamUiColor1, 1 }, "UI Color 1", 0, 0xFFFFFF, 0x000000,
		juce::AudioParameterIntAttributes().withAutomatable (false)));

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

	// FFTW: ensure DLL is loaded BEFORE any FFT instances are created
#if JUCE_WINDOWS && JUCE_DSP_USE_SHARED_FFTW
	ensureFFTWLoaded();
	LOG_IR_EVENT (g_fftwDiag);
#endif

	// Prepare convolution
	juce::dsp::ProcessSpec spec;
	spec.sampleRate = sampleRate;
	spec.maximumBlockSize = static_cast<juce::uint32> (samplesPerBlock);
	spec.numChannels = 2;

	stateA.convolution.prepare (sampleRate, samplesPerBlock);
	stateB.convolution.prepare (sampleRate, samplesPerBlock);
	stateC.convolution.prepare (sampleRate, samplesPerBlock);
	stateA.hpFilter.prepare (spec);
	stateA.hpFilter2.prepare (spec);
	stateA.lpFilter.prepare (spec);
	stateA.lpFilter2.prepare (spec);
	stateB.hpFilter.prepare (spec);
	stateB.hpFilter2.prepare (spec);
	stateB.lpFilter.prepare (spec);
	stateB.lpFilter2.prepare (spec);
	stateC.hpFilter.prepare (spec);
	stateC.hpFilter2.prepare (spec);
	stateC.lpFilter.prepare (spec);
	stateC.lpFilter2.prepare (spec);
	stateA.posFilter.prepare (spec);
	stateB.posFilter.prepare (spec);
	stateC.posFilter.prepare (spec);
	
	// Prepare delay lines
	stateA.delayLine.prepare (spec);
	stateB.delayLine.prepare (spec);
	stateC.delayLine.prepare (spec);
	stateA.delayLine.reset();
	stateB.delayLine.reset();
	stateC.delayLine.reset();
	
	// Prepare delay smoothing (50ms ramp)
	stateA.smoothedDelay.reset (sampleRate, 0.05); // 50ms ramp
	stateB.smoothedDelay.reset (sampleRate, 0.05);
	stateC.smoothedDelay.reset (sampleRate, 0.05);
	stateA.smoothedDelay.setCurrentAndTargetValue (0.0f);
	stateB.smoothedDelay.setCurrentAndTargetValue (0.0f);
	stateC.smoothedDelay.setCurrentAndTargetValue (0.0f);
	
	// Prepare temp buffers (pre-allocated for audio thread)
	// Use generous size to handle hosts that send variable/larger blocks
	// (e.g. FL Studio). Runtime guard in processBlock handles anything larger.
	const int bufAlloc = juce::jmax (samplesPerBlock, 32768);
	tempBufferA.setSize (2, bufAlloc);
	tempBufferB.setSize (2, bufAlloc);
	tempBufferC.setSize (2, bufAlloc);
	globalDryBuffer.setSize (2, bufAlloc);
	loaderDryBuffer.setSize (2, bufAlloc);
	stateA.distanceDryBuffer.setSize (2, bufAlloc);
	stateB.distanceDryBuffer.setSize (2, bufAlloc);
	stateC.distanceDryBuffer.setSize (2, bufAlloc);

	// Cache raw parameter pointers (avoids hash-table lookup every processBlock)
	pEnableA = parameters.getRawParameterValue (kParamEnableA);
	pEnableB = parameters.getRawParameterValue (kParamEnableB);
	pRoute   = parameters.getRawParameterValue (kParamRoute);
	pMix     = parameters.getRawParameterValue (kParamMix);
	pInput   = parameters.getRawParameterValue (kParamInput);
	pOutput  = parameters.getRawParameterValue (kParamOutput);
	pHpFreqA = parameters.getRawParameterValue (kParamHpFreqA);
	pLpFreqA = parameters.getRawParameterValue (kParamLpFreqA);
	pHpOnA   = parameters.getRawParameterValue (kParamHpOnA);
	pLpOnA   = parameters.getRawParameterValue (kParamLpOnA);
	pHpSlopeA = parameters.getRawParameterValue (kParamHpSlopeA);
	pLpSlopeA = parameters.getRawParameterValue (kParamLpSlopeA);
	pExpA    = parameters.getRawParameterValue (kParamExpA);
	pExpOrderA = parameters.getRawParameterValue (kParamExpOrderA);
	pExpRatioA = parameters.getRawParameterValue (kParamExpRatioA);
	pExpThreshA = parameters.getRawParameterValue (kParamExpThreshA);
	pExpKneeA = parameters.getRawParameterValue (kParamExpKneeA);
	pExpAtkA = parameters.getRawParameterValue (kParamExpAtkA);
	pExpRelA = parameters.getRawParameterValue (kParamExpRelA);
	pExpScHpA = parameters.getRawParameterValue (kParamExpScHpA);
	pExpScLpA = parameters.getRawParameterValue (kParamExpScLpA);
	pExpScHpOnA = parameters.getRawParameterValue (kParamExpScHpOnA);
	pExpScLpOnA = parameters.getRawParameterValue (kParamExpScLpOnA);
	pExpScHpSlopeA = parameters.getRawParameterValue (kParamExpScHpSlopeA);
	pExpScLpSlopeA = parameters.getRawParameterValue (kParamExpScLpSlopeA);
	pExpScGainA = parameters.getRawParameterValue (kParamExpScGainA);
	pDelayA  = parameters.getRawParameterValue (kParamDelayA);
	pPanA    = parameters.getRawParameterValue (kParamPanA);
	pFredA   = parameters.getRawParameterValue (kParamFredA);
	pPosA    = parameters.getRawParameterValue (kParamPosA);
	pVariationA = parameters.getRawParameterValue (kParamVariationA);
	pOutA    = parameters.getRawParameterValue (kParamOutA);
	pInA     = parameters.getRawParameterValue (kParamInA);
	pTiltA   = parameters.getRawParameterValue (kParamTiltA);
	pHpFreqB = parameters.getRawParameterValue (kParamHpFreqB);
	pLpFreqB = parameters.getRawParameterValue (kParamLpFreqB);
	pHpOnB   = parameters.getRawParameterValue (kParamHpOnB);
	pLpOnB   = parameters.getRawParameterValue (kParamLpOnB);
	pHpSlopeB = parameters.getRawParameterValue (kParamHpSlopeB);
	pLpSlopeB = parameters.getRawParameterValue (kParamLpSlopeB);
	pExpB    = parameters.getRawParameterValue (kParamExpB);
	pExpOrderB = parameters.getRawParameterValue (kParamExpOrderB);
	pExpRatioB = parameters.getRawParameterValue (kParamExpRatioB);
	pExpThreshB = parameters.getRawParameterValue (kParamExpThreshB);
	pExpKneeB = parameters.getRawParameterValue (kParamExpKneeB);
	pExpAtkB = parameters.getRawParameterValue (kParamExpAtkB);
	pExpRelB = parameters.getRawParameterValue (kParamExpRelB);
	pExpScHpB = parameters.getRawParameterValue (kParamExpScHpB);
	pExpScLpB = parameters.getRawParameterValue (kParamExpScLpB);
	pExpScHpOnB = parameters.getRawParameterValue (kParamExpScHpOnB);
	pExpScLpOnB = parameters.getRawParameterValue (kParamExpScLpOnB);
	pExpScHpSlopeB = parameters.getRawParameterValue (kParamExpScHpSlopeB);
	pExpScLpSlopeB = parameters.getRawParameterValue (kParamExpScLpSlopeB);
	pExpScGainB = parameters.getRawParameterValue (kParamExpScGainB);
	pDelayB  = parameters.getRawParameterValue (kParamDelayB);
	pPanB    = parameters.getRawParameterValue (kParamPanB);
	pFredB   = parameters.getRawParameterValue (kParamFredB);
	pPosB    = parameters.getRawParameterValue (kParamPosB);
	pVariationB = parameters.getRawParameterValue (kParamVariationB);
	pOutB    = parameters.getRawParameterValue (kParamOutB);
	pInB     = parameters.getRawParameterValue (kParamInB);
	pTiltB   = parameters.getRawParameterValue (kParamTiltB);
	pChaosA    = parameters.getRawParameterValue (kParamChaosA);
	pChaosFilterA = parameters.getRawParameterValue (kParamChaosFilterA);
	pChaosAmtA = parameters.getRawParameterValue (kParamChaosAmtA);
	pChaosSpdA = parameters.getRawParameterValue (kParamChaosSpdA);
	pChaosAmtFilterA = parameters.getRawParameterValue (kParamChaosAmtFilterA);
	pChaosSpdFilterA = parameters.getRawParameterValue (kParamChaosSpdFilterA);
	pModeInA   = parameters.getRawParameterValue (kParamModeInA);
	pModeOutA  = parameters.getRawParameterValue (kParamModeOutA);
	pSumBusA   = parameters.getRawParameterValue (kParamSumBusA);
	pFilterPosA = parameters.getRawParameterValue (kParamFilterPosA);
	pChaosB    = parameters.getRawParameterValue (kParamChaosB);
	pChaosFilterB = parameters.getRawParameterValue (kParamChaosFilterB);
	pChaosAmtB = parameters.getRawParameterValue (kParamChaosAmtB);
	pChaosSpdB = parameters.getRawParameterValue (kParamChaosSpdB);
	pChaosAmtFilterB = parameters.getRawParameterValue (kParamChaosAmtFilterB);
	pChaosSpdFilterB = parameters.getRawParameterValue (kParamChaosSpdFilterB);
	pModeInB   = parameters.getRawParameterValue (kParamModeInB);
	pModeOutB  = parameters.getRawParameterValue (kParamModeOutB);
	pSumBusB   = parameters.getRawParameterValue (kParamSumBusB);
	pFilterPosB = parameters.getRawParameterValue (kParamFilterPosB);
	pMixA      = parameters.getRawParameterValue (kParamMixA);
	pMixB      = parameters.getRawParameterValue (kParamMixB);
	pEnableC   = parameters.getRawParameterValue (kParamEnableC);
	pHpFreqC   = parameters.getRawParameterValue (kParamHpFreqC);
	pLpFreqC   = parameters.getRawParameterValue (kParamLpFreqC);
	pHpOnC     = parameters.getRawParameterValue (kParamHpOnC);
	pLpOnC     = parameters.getRawParameterValue (kParamLpOnC);
	pHpSlopeC  = parameters.getRawParameterValue (kParamHpSlopeC);
	pLpSlopeC  = parameters.getRawParameterValue (kParamLpSlopeC);
	pExpC      = parameters.getRawParameterValue (kParamExpC);
	pExpOrderC = parameters.getRawParameterValue (kParamExpOrderC);
	pExpRatioC = parameters.getRawParameterValue (kParamExpRatioC);
	pExpThreshC = parameters.getRawParameterValue (kParamExpThreshC);
	pExpKneeC  = parameters.getRawParameterValue (kParamExpKneeC);
	pExpAtkC   = parameters.getRawParameterValue (kParamExpAtkC);
	pExpRelC   = parameters.getRawParameterValue (kParamExpRelC);
	pExpScHpC = parameters.getRawParameterValue (kParamExpScHpC);
	pExpScLpC = parameters.getRawParameterValue (kParamExpScLpC);
	pExpScHpOnC = parameters.getRawParameterValue (kParamExpScHpOnC);
	pExpScLpOnC = parameters.getRawParameterValue (kParamExpScLpOnC);
	pExpScHpSlopeC = parameters.getRawParameterValue (kParamExpScHpSlopeC);
	pExpScLpSlopeC = parameters.getRawParameterValue (kParamExpScLpSlopeC);
	pExpScGainC = parameters.getRawParameterValue (kParamExpScGainC);
	pDelayC    = parameters.getRawParameterValue (kParamDelayC);
	pPanC      = parameters.getRawParameterValue (kParamPanC);
	pFredC     = parameters.getRawParameterValue (kParamFredC);
	pPosC      = parameters.getRawParameterValue (kParamPosC);
	pVariationC = parameters.getRawParameterValue (kParamVariationC);
	pOutC      = parameters.getRawParameterValue (kParamOutC);
	pInC       = parameters.getRawParameterValue (kParamInC);
	pTiltC     = parameters.getRawParameterValue (kParamTiltC);
	pChaosC    = parameters.getRawParameterValue (kParamChaosC);
	pChaosFilterC = parameters.getRawParameterValue (kParamChaosFilterC);
	pChaosAmtC = parameters.getRawParameterValue (kParamChaosAmtC);
	pChaosSpdC = parameters.getRawParameterValue (kParamChaosSpdC);
	pChaosAmtFilterC = parameters.getRawParameterValue (kParamChaosAmtFilterC);
	pChaosSpdFilterC = parameters.getRawParameterValue (kParamChaosSpdFilterC);
	pModeInC   = parameters.getRawParameterValue (kParamModeInC);
	pModeOutC  = parameters.getRawParameterValue (kParamModeOutC);
	pSumBusC   = parameters.getRawParameterValue (kParamSumBusC);
	pFilterPosC = parameters.getRawParameterValue (kParamFilterPosC);
	pMixC      = parameters.getRawParameterValue (kParamMixC);
	pMatch     = parameters.getRawParameterValue (kParamMatch);
	pTrim      = parameters.getRawParameterValue (kParamTrim);
	pLimThreshold = parameters.getRawParameterValue (kParamLimThreshold);
	pLimMode     = parameters.getRawParameterValue (kParamLimMode);
	pInvPol      = parameters.getRawParameterValue (kParamInvPol);
	pInvStr      = parameters.getRawParameterValue (kParamInvStr);
	pMixMode     = parameters.getRawParameterValue (kParamMixMode);
	pDryLevel    = parameters.getRawParameterValue (kParamDryLevel);
	pWetLevel    = parameters.getRawParameterValue (kParamWetLevel);

	// Reset tilt EQ state
	tiltState_[0] = tiltState_[1] = 0.0f;
	tiltLastProfile_ = -1;

	// Reset wet NORM AGC state
	normPeakFollower_  = 0.0f;
	normSmoothedGain_  = 1.0f;
	normWarmupSamples_ = 0;

	// Limiter state reset
	limEnv1_[0] = limEnv1_[1] = kLimFloor;
	limEnv2_[0] = limEnv2_[1] = kLimFloor;
	limAtt1_ = std::exp (-1.0f / ((float) sampleRate * 0.002f));
	limRel1_ = std::exp (-1.0f / ((float) sampleRate * 0.010f));
	limRel2_ = std::exp (-1.0f / ((float) sampleRate * 0.100f));

	// Pre-compute coefficients that depend only on sample rate (avoids per-block std::exp)
	{
		const float sr = static_cast<float> (sampleRate);
		cachedTiltSmoothCoeff_ = 1.0f - std::exp (-1.0f / (sr * 0.03f));
		cachedDcBlockR_        = 1.0f - (juce::MathConstants<float>::twoPi * 5.0f / sr);
		// NORM AGC: per-block coefficients depend on numSamples, but the base tau is fixed.
		// We store the per-sample versions; processBlock scales by numSamples.
		cachedNormFastCoeff_   = 1.0f - std::exp (-1.0f / (sr * 0.01f)); // 10ms ramp-down
		cachedNormSlowCoeff_   = 1.0f - std::exp (-1.0f / (sr * 0.02f)); // 20ms ramp-up
	}

	lastInputGain_ = gainFaderDecibelsToGain (loadRelaxed (pInput, 0.0f));
	lastOutputGain_ = gainFaderDecibelsToGain (loadRelaxed (pOutput, 0.0f));
	if (loadRelaxedInt (pMixMode, 0) == 1)
	{
		lastGlobalDryMix_ = loadRelaxed (pDryLevel, 0.0f);
		lastGlobalWetMix_ = loadRelaxed (pWetLevel, 1.0f);
	}
	else
	{
		lastGlobalWetMix_ = loadRelaxed (pMix, 1.0f);
		lastGlobalDryMix_ = 1.0f - lastGlobalWetMix_;
	}
	lastLimiterThresholdLin_ = fastDecibelsToGain (loadRelaxed (pLimThreshold, kLimThresholdDefault));

	auto initLoaderSmoothing = [] (IRLoaderState& state, float inDb, float outDb,
	                               float mixVal, float posVal) noexcept
	{
		state.lastInGain = gainFaderDecibelsToGain (inDb);
		state.lastOutGain = gainFaderDecibelsToGain (outDb);
		state.lastMix = mixVal;
		state.lastPosGain = 1.0f - juce::jlimit (0.0f, 1.0f, posVal) * 0.5f;
		state.distanceWet = posVal > 0.01f ? 1.0f : 0.0f;
	};

	initLoaderSmoothing (stateA, loadRelaxed (pInA, 0.0f), loadRelaxed (pOutA, 0.0f),
	                     loadRelaxed (pMixA, 1.0f), loadRelaxed (pPosA, 0.0f));
	initLoaderSmoothing (stateB, loadRelaxed (pInB, 0.0f), loadRelaxed (pOutB, 0.0f),
	                     loadRelaxed (pMixB, 1.0f), loadRelaxed (pPosB, 0.0f));
	initLoaderSmoothing (stateC, loadRelaxed (pInC, 0.0f), loadRelaxed (pOutC, 0.0f),
	                     loadRelaxed (pMixC, 1.0f), loadRelaxed (pPosC, 0.0f));

	// Fade-in to suppress convolver/filter warmup transients (~5ms)
	fadeInTotalSamples_     = juce::jmax (64, (int) (sampleRate * 0.005));
	fadeInSamplesRemaining_ = fadeInTotalSamples_;

	// Reset FRED, CHAOS and VAR state for all loaders
	int loaderSeedIndex = 0;
	for (auto* state : { &stateA, &stateB, &stateC })
	{
		std::memset (state->fredDelayBuffer, 0, sizeof (state->fredDelayBuffer));
		state->fredDelayIndex = 0;
		state->lastFred = 0.0f;
		std::memset (state->chaosDelayBuffer, 0, sizeof (state->chaosDelayBuffer));
		state->chaosDelayWritePos = 0;
		state->chaosLoaderParamSmoothReady = false;
		state->chaosFilterParamSmoothReady = false;
		for (int c = 0; c < 2; ++c)
		{
			state->chaosDelaySmoothedSamples[c] = 0.0f;
			state->chaosDelaySmoothReady[c] = false;
			state->chaosDPrev[c] = state->chaosDCurr[c] = state->chaosDNext[c] = 0.0f;
			state->chaosDPhase[c] = state->chaosDDriftPhase[c] = state->chaosDDriftFreqHz[c] = 0.0f;
			state->chaosDOut[c] = 0.0f;
			state->chaosGPrev[c] = state->chaosGCurr[c] = state->chaosGNext[c] = 0.0f;
			state->chaosGPhase[c] = state->chaosGDriftPhase[c] = state->chaosGDriftFreqHz[c] = 0.0f;
			state->chaosGOut[c] = 0.0f;
		}
		state->chaosFPrev = state->chaosFCurr = state->chaosFNext = 0.0f;
		state->chaosFPhase = state->chaosFDriftPhase = state->chaosFDriftFreqHz = 0.0f;
		state->chaosFOut[0] = state->chaosFOut[1] = 0.0f;
		resetVariationStateForLoader (*state, loaderSeedIndex);
		state->smoothedPosFreq = 12000.0f;
		state->filterCoeffCountdown = 0;
		state->expLinkedEnv = 0.0f;
		++loaderSeedIndex;
	}

	// Initialize EMA-smoothed filter frequencies to current parameter values
	std::atomic<float>* hpPtrs[] = { pHpFreqA, pHpFreqB, pHpFreqC };
	std::atomic<float>* lpPtrs[] = { pLpFreqA, pLpFreqB, pLpFreqC };
	std::atomic<float>* chaosLoaderAmtPtrs[] = { pChaosAmtA, pChaosAmtB, pChaosAmtC };
	std::atomic<float>* chaosLoaderSpdPtrs[] = { pChaosSpdA, pChaosSpdB, pChaosSpdC };
	std::atomic<float>* chaosFilterAmtPtrs[] = { pChaosAmtFilterA, pChaosAmtFilterB, pChaosAmtFilterC };
	std::atomic<float>* chaosFilterSpdPtrs[] = { pChaosSpdFilterA, pChaosSpdFilterB, pChaosSpdFilterC };
	std::atomic<float>* expScGainPtrs[] = { pExpScGainA, pExpScGainB, pExpScGainC };
	IRLoaderState* states[] = { &stateA, &stateB, &stateC };
	for (int i = 0; i < 3; ++i)
	{
		states[i]->smoothedHpFreq = hpPtrs[i]->load();
		states[i]->smoothedLpFreq = lpPtrs[i]->load();
		states[i]->chaosLoaderAmtSmoothed = chaosLoaderAmtPtrs[i]->load();
		states[i]->chaosLoaderSpdSmoothed = juce::jlimit (kChaosSpdMin, kChaosSpdMax, chaosLoaderSpdPtrs[i]->load());
		states[i]->chaosFilterAmtSmoothed = chaosFilterAmtPtrs[i]->load();
		states[i]->chaosFilterSpdSmoothed = juce::jlimit (kChaosSpdMin, kChaosSpdMax, chaosFilterSpdPtrs[i]->load());
		states[i]->expScHpState.reset();
		states[i]->expScHpState2.reset();
		states[i]->expScLpState.reset();
		states[i]->expScLpState2.reset();
		states[i]->expScLastGain = gainFaderDecibelsToGain (loadRelaxed (expScGainPtrs[i], kExpScGainDefault));
	}
}

void CABTRAudioProcessor::releaseResources()
{
	// Reset delay lines
	stateA.delayLine.reset();
	stateB.delayLine.reset();
	stateC.delayLine.reset();
	

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
//    - Complexity: O(N log N) vs O(N^2) for time-domain
//    - Automatically partitions long IRs for optimal latency/CPU balance
//    - Zero-latency mode for low-latency monitoring
//
// 2. SIZE RESAMPLING: Lagrange interpolation
//    - 4-point Lagrange interpolator for quality/performance balance
//    - Linear phase response (no smearing)
//    - Range: 25%-400% (shrink to expand cab size)
//
// 3. MODE PROCESSING: Mid/Side conversion
//    - MID = (L+R) / sqrt(2)  - preserves RMS energy
//    - SIDE = (L-R) / sqrt(2)
//    - L+R = standard stereo pass-through
//
// 4. ROUTING:
//    - PARALLEL (A|B): Independent processing, summed output
//    - SERIES (A->B): A output becomes B input (cascade)
//
// 5. SIMD OPTIMIZATION: FloatVectorOperations for buffer operations
//    - applyGain, multiply, add use SIMD when available
//    - Significant speedup on modern CPUs (4x+ on AVX)
//
// 6. IR LENGTH LIMIT: 10 seconds maximum (480,000 samples @ 48kHz)
//    - Prevents excessive memory usage
//    - Typical guitar cab IRs are 100-500ms
//==============================================================================

static inline void injectMSBus (float l, float r, int bus,
                                float& stL, float& stR,
                                float& midBus, float& sideBus)
{
	if (bus == 0)      { stL += l; stR += r; }
	else if (bus == 1) { midBus += (l + r) * 0.5f; }
	else               { sideBus += (l - r) * 0.5f; }
}

void CABTRAudioProcessor::applyMidSideInputMode (juce::AudioBuffer<float>& buf, int modeVal, int nSamples)
{
	if ((modeVal == 1 || modeVal == 2) && buf.getNumChannels() >= 2)
	{
		auto* L = buf.getWritePointer (0);
		auto* R = buf.getWritePointer (1);
		for (int i = 0; i < nSamples; ++i)
		{
			const float l = L[i];
			const float r = R[i];
			if (modeVal == 1) // MID = (L+R) / sqrt(2)
			{
				const float mid = (l + r) * kSqrt2Over2;
				L[i] = R[i] = mid;
			}
			else // SIDE = (L-R) / sqrt(2)
			{
				const float side = (l - r) * kSqrt2Over2;
				L[i] = R[i] = side;
			}
		}
	}
}

void CABTRAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages)
{
	juce::ScopedNoDenormals noDenormals;
	juce::ignoreUnused (midiMessages);

	auto totalNumInputChannels  = getTotalNumInputChannels();
	auto totalNumOutputChannels = getTotalNumOutputChannels();

	// Clear unused output channels
	for (auto i = totalNumInputChannels; i < totalNumOutputChannels; ++i)
		buffer.clear (i, 0, buffer.getNumSamples());

	const int numSamples = buffer.getNumSamples();
	if (numSamples == 0)
		return;

	// Resize work buffers to match actual block size each call.
	// avoidReallocating=true keeps existing allocation if big enough (zero-alloc path).
	// This is CRITICAL: the temp buffers must report the correct numSamples so that
	// processLoader (which reads buffer.getNumSamples()) only processes valid data.
	// Without this, stale data beyond numSamples creates a feedback loop.
	{
		const int nc = buffer.getNumChannels();
		tempBufferA.setSize  (nc, numSamples, false, false, true);
		tempBufferB.setSize  (nc, numSamples, false, false, true);
		tempBufferC.setSize  (nc, numSamples, false, false, true);
		globalDryBuffer.setSize (nc, numSamples, false, false, true);
		loaderDryBuffer.setSize (nc, numSamples, false, false, true);
		stateA.distanceDryBuffer.setSize (nc, numSamples, false, false, true);
		stateB.distanceDryBuffer.setSize (nc, numSamples, false, false, true);
		stateC.distanceDryBuffer.setSize (nc, numSamples, false, false, true);
	}

	// Get global parameters (cached pointers - relaxed atomic, no hash lookup)
	const bool enableA = loadRelaxedBool (pEnableA);
	const bool enableB = loadRelaxedBool (pEnableB);
	const bool enableC = loadRelaxedBool (pEnableC);

	// A loader is "active" only if enabled AND has an IR loaded.
	// Enabled without IR = transparent pass-through (no silence).
	const bool activeA = enableA && stateA.impulseResponse.getNumSamples() > 0;
	const bool activeB = enableB && stateB.impulseResponse.getNumSamples() > 0;
	const bool activeC = enableC && stateC.impulseResponse.getNumSamples() > 0;

	const int route = loadRelaxedInt (pRoute);
	const float globalMix = loadRelaxed (pMix);
	const int   mixMode   = loadRelaxedInt (pMixMode);
	const float dryLevel  = (mixMode == 1) ? loadRelaxed (pDryLevel) : 0.0f;
	const float wetLevel  = (mixMode == 1) ? loadRelaxed (pWetLevel) : 0.0f;
	const float globalWetEnd = (mixMode == 0) ? globalMix : wetLevel;
	const float globalDryEnd = (mixMode == 0) ? (1.0f - globalMix) : dryLevel;
	constexpr float kMixBypassEps = 1.0e-4f;
	const bool needsGlobalDry = std::abs (globalDryEnd) > kMixBypassEps
	                         || std::abs (lastGlobalDryMix_) > kMixBypassEps;
	const bool needsGlobalWetGain = std::abs (globalWetEnd - 1.0f) > kMixBypassEps
	                             || std::abs (lastGlobalWetMix_ - globalWetEnd) > kMixBypassEps;

	// Limiter
	const int limMode = loadRelaxedInt (pLimMode);
	const float limThreshLinStart = lastLimiterThresholdLin_;
	const float limThreshLin = (limMode != 0) ? fastDecibelsToGain (loadRelaxed (pLimThreshold, kLimThresholdDefault)) : 1.0f;
	lastLimiterThresholdLin_ = limThreshLin;

	const int invPol = loadRelaxedInt (pInvPol);
	const int invStr = loadRelaxedInt (pInvStr);

	// Per-loader mode parameters
	const int modeInA  = loadRelaxedInt (pModeInA);
	const int modeOutA = loadRelaxedInt (pModeOutA);
	const int modeInB  = loadRelaxedInt (pModeInB);
	const int modeOutB = loadRelaxedInt (pModeOutB);
	const int modeInC  = loadRelaxedInt (pModeInC);
	const int modeOutC = loadRelaxedInt (pModeOutC);
	const int sumBusA  = loadRelaxedInt (pSumBusA);
	const int sumBusB  = loadRelaxedInt (pSumBusB);
	const int sumBusC  = loadRelaxedInt (pSumBusC);
	const float mixA = loadRelaxed (pMixA);
	const float mixB = loadRelaxed (pMixB);
	const float mixC = loadRelaxed (pMixC);
	const bool wetPathAudible = std::abs (globalWetEnd) > kMixBypassEps
	                         || std::abs (lastGlobalWetMix_) > kMixBypassEps;
	const bool wetHasActiveLoader =
		(activeA && (std::abs (mixA) > kMixBypassEps || std::abs (stateA.lastMix) > kMixBypassEps)) ||
		(activeB && (std::abs (mixB) > kMixBypassEps || std::abs (stateB.lastMix) > kMixBypassEps)) ||
		(activeC && (std::abs (mixC) > kMixBypassEps || std::abs (stateC.lastMix) > kMixBypassEps));

	// Apply input gain
	const float inputGain = gainFaderDecibelsToGain (loadRelaxed (pInput));
	for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
		buffer.applyGainRamp (ch, 0, numSamples, lastInputGain_, inputGain);
	lastInputGain_ = inputGain;

	// DIAGNOSTIC LOG (throttled ~1s)
#if CABTR_DSP_DEBUG_LOG
	{
		static int diagBlockCount = 0;
		++diagBlockCount;
		// Log every ~1 second (sampleRate / numSamples = blocks per second)
		const int blocksPerSecond = juce::jmax (1, (int)(currentSampleRate / juce::jmax (1, numSamples)));
		if (diagBlockCount >= blocksPerSecond)
		{
			diagBlockCount = 0;

			// Peak levels of input (post input gain)
			float peakL = 0.0f, peakR = 0.0f;
			if (buffer.getNumChannels() >= 1)
				peakL = buffer.getMagnitude (0, 0, numSamples);
			if (buffer.getNumChannels() >= 2)
				peakR = buffer.getMagnitude (1, 0, numSamples);

			const float inputDb = loadRelaxed (pInput);
			const float outputDb = loadRelaxed (pOutput);
			const float outA_dB = loadRelaxed (pOutA);
			const float outB_dB = loadRelaxed (pOutB);
			const float outC_dB = loadRelaxed (pOutC);
			const float inA_dB = loadRelaxed (pInA);
			const float inB_dB = loadRelaxed (pInB);
			const float inC_dB = loadRelaxed (pInC);

			juce::String diag;
			diag << "BLOCK numSamples=" << numSamples
			     << " sRate=" << (int) currentSampleRate
			     << " ch=" << buffer.getNumChannels()
			     << " | inPeak L=" << juce::String (peakL, 4)
			     << " R=" << juce::String (peakR, 4)
			     << " | route=" << route
			     << " globalMix=" << juce::String (globalMix, 3)
			     << " | Input=" << juce::String (inputDb, 2) << "dB"
			     << " Output=" << juce::String (outputDb, 2) << "dB"
			     << " | enableA=" << (int) enableA << " activeA=" << (int) activeA
			     << " enableB=" << (int) enableB << " activeB=" << (int) activeB
			     << " enableC=" << (int) enableC << " activeC=" << (int) activeC
			     << " | InA=" << juce::String (inA_dB, 2) << " OutA=" << juce::String (outA_dB, 2)
			     << " InB=" << juce::String (inB_dB, 2) << " OutB=" << juce::String (outB_dB, 2)
			     << " InC=" << juce::String (inC_dB, 2) << " OutC=" << juce::String (outC_dB, 2)
			     << " | mixA=" << juce::String (mixA, 3) << " mixB=" << juce::String (mixB, 3) << " mixC=" << juce::String (mixC, 3)
			     << " | tempBufA=" << tempBufferA.getNumSamples() << " tempBufB=" << tempBufferB.getNumSamples();
			LOG_IR_EVENT (diag);
		}
	}
#endif

	// Capture dry signal AFTER input gain, but BEFORE any loader processing
	// Used for global MIX: dry is unaffected by convolution, filters, mode, etc.
	if (needsGlobalDry)
	{
		for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
			globalDryBuffer.copyFrom (ch, 0, buffer, ch, 0, numSamples);
	}

	// Helper lambdas
	// Save dry copy before processing a loader only when per-loader mix needs it.
	auto saveDry = [&] (const juce::AudioBuffer<float>& src)
	{
		for (int ch = 0; ch < src.getNumChannels(); ++ch)
			loaderDryBuffer.copyFrom (ch, 0, src, ch, 0, numSamples);
	};

	// Process one loader with mode in/out and per-loader mix
	auto processOne = [&] (IRLoaderState& state, juce::AudioBuffer<float>& buf,
	                        int loaderIndex, int modeIn, int modeOut, float loaderMix)
	{
		const float wetStart = state.lastMix;
		const float wetEnd   = loaderMix;
		const float dryStart = 1.0f - wetStart;
		const float dryEnd   = 1.0f - wetEnd;
		const bool needsLoaderMix = std::abs (wetStart - wetEnd) > kMixBypassEps
		                         || std::abs (wetEnd - 1.0f) > kMixBypassEps;

		if (needsLoaderMix)
			saveDry (buf);

		applyMidSideInputMode (buf, modeIn, numSamples);
		processLoader (state, buf, loaderIndex);
		applyMidSideOutputMode (buf, modeOut, numSamples);

		if (needsLoaderMix)
		{
			for (int ch = 0; ch < buf.getNumChannels(); ++ch)
			{
				buf.applyGainRamp (ch, 0, numSamples, wetStart, wetEnd);
				buf.addFromWithRamp (ch, 0, loaderDryBuffer.getReadPointer (ch), numSamples,
				                     dryStart, dryEnd);
			}
		}
		state.lastMix = wetEnd;
	};

	// Count how many loaders are active for parallel routing decisions.
	auto countEnabled = [&] (bool a, bool b, bool c) -> int
	{
		return (a ? 1 : 0) + (b ? 1 : 0) + (c ? 1 : 0);
	};

	// ROUTING
	// Route 0: A->B->C   (full series)
	// Route 1: A|B|C     (all parallel)
	// Route 2: A->B|C    (series A->B, C parallel)
	// Route 3: A|B->C    (A parallel, series B->C)
	// Route 4: (A|B)->C  (A and B parallel, then C in series)
	// Route 5: A->(B|C)  (A in series, then B and C in parallel)

	if (route == 1) // A|B|C - all parallel
	{
		const int numActive = countEnabled (activeA, activeB, activeC);
		if (numActive >= 2)
		{
			// Copy input into each temp buffer for active loaders
			if (activeA)
				for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
					tempBufferA.copyFrom (ch, 0, buffer, ch, 0, numSamples);
			if (activeB)
				for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
					tempBufferB.copyFrom (ch, 0, buffer, ch, 0, numSamples);
			if (activeC)
				for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
					tempBufferC.copyFrom (ch, 0, buffer, ch, 0, numSamples);

			if (activeA) processOne (stateA, tempBufferA, 0, modeInA, modeOutA, mixA);
			if (activeB) processOne (stateB, tempBufferB, 1, modeInB, modeOutB, mixB);
			if (activeC) processOne (stateC, tempBufferC, 2, modeInC, modeOutC, mixC);

			// Sum active buffers - M/S bus-aware
			const bool anyMSBus = (activeA && sumBusA != 0)
			                   || (activeB && sumBusB != 0)
			                   || (activeC && sumBusC != 0);

			if (anyMSBus && buffer.getNumChannels() >= 2)
			{
				// M/S bus routing: each loader contributes to ST, ->M, or ->S bus
				auto* outL = buffer.getWritePointer (0);
				auto* outR = buffer.getWritePointer (1);

				const float* srcL[3] = { nullptr, nullptr, nullptr };
				const float* srcR[3] = { nullptr, nullptr, nullptr };
				int buses[3] = { sumBusA, sumBusB, sumBusC };
				bool active[3] = { activeA, activeB, activeC };

				if (activeA) { srcL[0] = tempBufferA.getReadPointer (0); srcR[0] = tempBufferA.getReadPointer (1); }
				if (activeB) { srcL[1] = tempBufferB.getReadPointer (0); srcR[1] = tempBufferB.getReadPointer (1); }
				if (activeC) { srcL[2] = tempBufferC.getReadPointer (0); srcR[2] = tempBufferC.getReadPointer (1); }

				for (int i = 0; i < numSamples; ++i)
				{
					float stL = 0.0f, stR = 0.0f;
					float midBus = 0.0f, sideBus = 0.0f;

					for (int k = 0; k < 3; ++k)
					{
						if (!active[k]) continue;
						injectMSBus (srcL[k][i], srcR[k][i], buses[k],
						             stL, stR, midBus, sideBus);
					}

					outL[i] = stL + midBus + sideBus;
					outR[i] = stR + midBus - sideBus;
				}
			}
			else
			{
				// Fast path: all ST - simple L+R addition (no M/S overhead)
				buffer.clear();
				if (activeA)
					for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
						juce::FloatVectorOperations::add (buffer.getWritePointer (ch), tempBufferA.getReadPointer (ch), numSamples);
				if (activeB)
					for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
						juce::FloatVectorOperations::add (buffer.getWritePointer (ch), tempBufferB.getReadPointer (ch), numSamples);
				if (activeC)
					for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
						juce::FloatVectorOperations::add (buffer.getWritePointer (ch), tempBufferC.getReadPointer (ch), numSamples);
			}

		}
		else if (activeA)
			processOne (stateA, buffer, 0, modeInA, modeOutA, mixA);
		else if (activeB)
			processOne (stateB, buffer, 1, modeInB, modeOutB, mixB);
		else if (activeC)
			processOne (stateC, buffer, 2, modeInC, modeOutC, mixC);
	}
	else if (route == 2) // A->B|C - series A->B, C parallel
	{
		const bool seriesActive = activeA || activeB;
		if (seriesActive && activeC)
		{
			// Copy input for parallel path C
			for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
				tempBufferC.copyFrom (ch, 0, buffer, ch, 0, numSamples);
			// Series path: A->B stays in buffer
			if (activeA) processOne (stateA, buffer, 0, modeInA, modeOutA, mixA);
			if (activeB) processOne (stateB, buffer, 1, modeInB, modeOutB, mixB);
			// Parallel path C
			processOne (stateC, tempBufferC, 2, modeInC, modeOutC, mixC);
			// Sum both paths - M/S bus-aware
			// Series path bus = last active stage in the A->B chain, parallel path bus = sumBusC
			const int seriesBus = activeB ? sumBusB : sumBusA;
			const int parallelBus = sumBusC;
			if ((seriesBus != 0 || parallelBus != 0) && buffer.getNumChannels() >= 2)
			{
				auto* bL = buffer.getWritePointer (0);
				auto* bR = buffer.getWritePointer (1);
				const auto* cL = tempBufferC.getReadPointer (0);
				const auto* cR = tempBufferC.getReadPointer (1);
				for (int i = 0; i < numSamples; ++i)
				{
					float stL = 0.0f, stR = 0.0f, midBus = 0.0f, sideBus = 0.0f;
					injectMSBus (bL[i], bR[i], seriesBus, stL, stR, midBus, sideBus);
					injectMSBus (cL[i], cR[i], parallelBus, stL, stR, midBus, sideBus);
					bL[i] = stL + midBus + sideBus;
					bR[i] = stR + midBus - sideBus;
				}
			}
			else
			{
				for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
					juce::FloatVectorOperations::add (buffer.getWritePointer (ch), tempBufferC.getReadPointer (ch), numSamples);
			}
		}
		else
		{
			// Only one path active - series or C alone
			if (activeA) processOne (stateA, buffer, 0, modeInA, modeOutA, mixA);
			if (activeB) processOne (stateB, buffer, 1, modeInB, modeOutB, mixB);
			if (activeC) processOne (stateC, buffer, 2, modeInC, modeOutC, mixC);
		}
	}
	else if (route == 3) // A|B->C - A parallel, series B->C
	{
		const bool seriesActive = activeB || activeC;
		if (activeA && seriesActive)
		{
			// Copy input for parallel path A
			for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
				tempBufferA.copyFrom (ch, 0, buffer, ch, 0, numSamples);
			// Parallel path A
			processOne (stateA, tempBufferA, 0, modeInA, modeOutA, mixA);
			// Series path: B->C stays in buffer
			if (activeB) processOne (stateB, buffer, 1, modeInB, modeOutB, mixB);
			if (activeC) processOne (stateC, buffer, 2, modeInC, modeOutC, mixC);
			// Sum both paths - M/S bus-aware
			// Parallel path bus = sumBusA, series path bus = last active stage in the B->C chain
			const int parallelBus = sumBusA;
			const int seriesBus = activeC ? sumBusC : sumBusB;
			if ((parallelBus != 0 || seriesBus != 0) && buffer.getNumChannels() >= 2)
			{
				auto* bL = buffer.getWritePointer (0);
				auto* bR = buffer.getWritePointer (1);
				const auto* aL = tempBufferA.getReadPointer (0);
				const auto* aR = tempBufferA.getReadPointer (1);
				for (int i = 0; i < numSamples; ++i)
				{
					float stL = 0.0f, stR = 0.0f, midBus = 0.0f, sideBus = 0.0f;
					injectMSBus (bL[i], bR[i], seriesBus, stL, stR, midBus, sideBus);
					injectMSBus (aL[i], aR[i], parallelBus, stL, stR, midBus, sideBus);
					bL[i] = stL + midBus + sideBus;
					bR[i] = stR + midBus - sideBus;
				}
			}
			else
			{
				for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
					juce::FloatVectorOperations::add (buffer.getWritePointer (ch), tempBufferA.getReadPointer (ch), numSamples);
			}
		}
		else
		{
			// Only one path active
			if (activeA) processOne (stateA, buffer, 0, modeInA, modeOutA, mixA);
			if (activeB) processOne (stateB, buffer, 1, modeInB, modeOutB, mixB);
			if (activeC) processOne (stateC, buffer, 2, modeInC, modeOutC, mixC);
		}
	}
	else if (route == 4) // (A|B)->C - A and B parallel, then C in series
	{
		const int numParallel = (activeA ? 1 : 0) + (activeB ? 1 : 0);

		if (numParallel >= 2)
		{
			for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
			{
				tempBufferA.copyFrom (ch, 0, buffer, ch, 0, numSamples);
				tempBufferB.copyFrom (ch, 0, buffer, ch, 0, numSamples);
			}

			processOne (stateA, tempBufferA, 0, modeInA, modeOutA, mixA);
			processOne (stateB, tempBufferB, 1, modeInB, modeOutB, mixB);

			const bool anyMSBus = (sumBusA != 0) || (sumBusB != 0);
			if (anyMSBus && buffer.getNumChannels() >= 2)
			{
				auto* outL = buffer.getWritePointer (0);
				auto* outR = buffer.getWritePointer (1);
				const auto* aL = tempBufferA.getReadPointer (0);
				const auto* aR = tempBufferA.getReadPointer (1);
				const auto* bL = tempBufferB.getReadPointer (0);
				const auto* bR = tempBufferB.getReadPointer (1);

				for (int i = 0; i < numSamples; ++i)
				{
					float stL = 0.0f, stR = 0.0f, midBus = 0.0f, sideBus = 0.0f;
					injectMSBus (aL[i], aR[i], sumBusA, stL, stR, midBus, sideBus);
					injectMSBus (bL[i], bR[i], sumBusB, stL, stR, midBus, sideBus);
					outL[i] = stL + midBus + sideBus;
					outR[i] = stR + midBus - sideBus;
				}
			}
			else
			{
				buffer.clear();
				for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
				{
					juce::FloatVectorOperations::add (buffer.getWritePointer (ch), tempBufferA.getReadPointer (ch), numSamples);
					juce::FloatVectorOperations::add (buffer.getWritePointer (ch), tempBufferB.getReadPointer (ch), numSamples);
				}
			}

		}
		else if (activeA)
		{
			processOne (stateA, buffer, 0, modeInA, modeOutA, mixA);
		}
		else if (activeB)
		{
			processOne (stateB, buffer, 1, modeInB, modeOutB, mixB);
		}

		if (activeC)
			processOne (stateC, buffer, 2, modeInC, modeOutC, mixC);
	}
	else if (route == 5) // A->(B|C) - A in series, then B and C in parallel
	{
		if (activeA)
			processOne (stateA, buffer, 0, modeInA, modeOutA, mixA);

		const int numParallel = (activeB ? 1 : 0) + (activeC ? 1 : 0);

		if (numParallel >= 2)
		{
			for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
			{
				tempBufferB.copyFrom (ch, 0, buffer, ch, 0, numSamples);
				tempBufferC.copyFrom (ch, 0, buffer, ch, 0, numSamples);
			}

			processOne (stateB, tempBufferB, 1, modeInB, modeOutB, mixB);
			processOne (stateC, tempBufferC, 2, modeInC, modeOutC, mixC);

			const bool anyMSBus = (sumBusB != 0) || (sumBusC != 0);
			if (anyMSBus && buffer.getNumChannels() >= 2)
			{
				auto* outL = buffer.getWritePointer (0);
				auto* outR = buffer.getWritePointer (1);
				const auto* bL = tempBufferB.getReadPointer (0);
				const auto* bR = tempBufferB.getReadPointer (1);
				const auto* cL = tempBufferC.getReadPointer (0);
				const auto* cR = tempBufferC.getReadPointer (1);

				for (int i = 0; i < numSamples; ++i)
				{
					float stL = 0.0f, stR = 0.0f, midBus = 0.0f, sideBus = 0.0f;
					injectMSBus (bL[i], bR[i], sumBusB, stL, stR, midBus, sideBus);
					injectMSBus (cL[i], cR[i], sumBusC, stL, stR, midBus, sideBus);
					outL[i] = stL + midBus + sideBus;
					outR[i] = stR + midBus - sideBus;
				}
			}
			else
			{
				buffer.clear();
				for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
				{
					juce::FloatVectorOperations::add (buffer.getWritePointer (ch), tempBufferB.getReadPointer (ch), numSamples);
					juce::FloatVectorOperations::add (buffer.getWritePointer (ch), tempBufferC.getReadPointer (ch), numSamples);
				}
			}

		}
		else if (activeB)
		{
			processOne (stateB, buffer, 1, modeInB, modeOutB, mixB);
		}
		else if (activeC)
		{
			processOne (stateC, buffer, 2, modeInC, modeOutC, mixC);
		}
	}
	else // route == 0: A->B->C (full series)
	{
		if (activeA) processOne (stateA, buffer, 0, modeInA, modeOutA, mixA);
		if (activeB) processOne (stateB, buffer, 1, modeInB, modeOutB, mixB);
		if (activeC) processOne (stateC, buffer, 2, modeInC, modeOutC, mixC);
	}

	// MATCH: Adaptive tilt EQ based on IR spectral analysis
	// Measures the spectral slope of the combined IR (computed at load time),
	// then applies compensating tilt to match the selected target profile.
	// Target slopes: White=0, Pink=-3, Brown=-6, Bright=+3, Bright+=+6 dB/oct
	{
		const int matchProfile = loadRelaxedInt (pMatch);

		if (matchProfile != 0) // 0 = None
		{
			const float targetSlope = kTargetSlopes[juce::jlimit (0, kNumTargetSlopes - 1, matchProfile)];

			// Compute combined IR slope based on routing mode
			float combinedSlope = 0.0f;
			if (route == 0) // A->B->C series: slopes add
			{
				if (activeA) combinedSlope += stateA.irSlopeDbPerOct.load();
				if (activeB) combinedSlope += stateB.irSlopeDbPerOct.load();
				if (activeC) combinedSlope += stateC.irSlopeDbPerOct.load();
			}
			else if (route == 1) // A|B|C parallel: weighted average
			{
				float totalWeight = 0.0f;
				if (activeA) { combinedSlope += stateA.irSlopeDbPerOct.load() * mixA; totalWeight += mixA; }
				if (activeB) { combinedSlope += stateB.irSlopeDbPerOct.load() * mixB; totalWeight += mixB; }
				if (activeC) { combinedSlope += stateC.irSlopeDbPerOct.load() * mixC; totalWeight += mixC; }
				if (totalWeight > 0.0f) combinedSlope /= totalWeight;
			}
			else if (route == 2) // A->B | C
			{
				float seriesSlope = 0.0f;
				if (activeA) seriesSlope += stateA.irSlopeDbPerOct.load();
				if (activeB) seriesSlope += stateB.irSlopeDbPerOct.load();
				if (activeC)
					combinedSlope = (seriesSlope + stateC.irSlopeDbPerOct.load()) * 0.5f;
				else
					combinedSlope = seriesSlope;
			}
			else if (route == 3) // A | B->C
			{
				float seriesSlope = 0.0f;
				if (activeB) seriesSlope += stateB.irSlopeDbPerOct.load();
				if (activeC) seriesSlope += stateC.irSlopeDbPerOct.load();
				if (activeA)
					combinedSlope = (stateA.irSlopeDbPerOct.load() + seriesSlope) * 0.5f;
				else
					combinedSlope = seriesSlope;
			}
			else if (route == 4) // (A | B)->C
			{
				float parallelSlope = 0.0f;
				int parallelCount = 0;
				if (activeA) { parallelSlope += stateA.irSlopeDbPerOct.load(); ++parallelCount; }
				if (activeB) { parallelSlope += stateB.irSlopeDbPerOct.load(); ++parallelCount; }
				if (parallelCount > 1)
					parallelSlope /= static_cast<float> (parallelCount);

				if (activeC)
					combinedSlope = parallelSlope + stateC.irSlopeDbPerOct.load();
				else
					combinedSlope = parallelSlope;
			}
			else if (route == 5) // A -> (B | C)
			{
				float parallelSlope = 0.0f;
				int parallelCount = 0;
				if (activeB) { parallelSlope += stateB.irSlopeDbPerOct.load(); ++parallelCount; }
				if (activeC) { parallelSlope += stateC.irSlopeDbPerOct.load(); ++parallelCount; }
				if (parallelCount > 1)
					parallelSlope /= static_cast<float> (parallelCount);

				if (activeA)
					combinedSlope = stateA.irSlopeDbPerOct.load() + parallelSlope;
				else
					combinedSlope = parallelSlope;
			}

			// Compensating slope: what tilt we need to get from measured -> target
			const float compensatingSlope = targetSlope - combinedSlope;

			// Update target coefficients when profile or slope changes
			if (matchProfile != tiltLastProfile_ || std::abs (compensatingSlope - tiltLastSlope_) > 0.05f)
			{
				tiltLastProfile_ = matchProfile;
				tiltLastSlope_ = compensatingSlope;
				computeTiltShelfCoeffs (getSampleRate(), compensatingSlope,
				                       tiltTargetB0_, tiltTargetB1_, tiltTargetA1_);
			}

			// Smooth coefficients towards target (~30ms ramp) to avoid zipper noise
			const float smoothCoeff = cachedTiltSmoothCoeff_;

			for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
			{
				auto* data = buffer.getWritePointer (ch);
				float s = tiltState_[ch];
				float b0 = tiltB0_, b1 = tiltB1_, a1 = tiltA1_;
				const float tb0 = tiltTargetB0_, tb1 = tiltTargetB1_, ta1 = tiltTargetA1_;

				for (int i = 0; i < numSamples; ++i)
				{
					b0 += (tb0 - b0) * smoothCoeff;
					b1 += (tb1 - b1) * smoothCoeff;
					a1 += (ta1 - a1) * smoothCoeff;

					const float x = data[i];
					const float y = b0 * x + s;
					s = b1 * x - a1 * y;
					data[i] = y;
				}
				tiltState_[ch] = s;
			}
			// Store smoothed coefficients for next block
			const float blendFactor = 1.0f - std::pow (1.0f - smoothCoeff, static_cast<float> (numSamples));
			tiltB0_ += (tiltTargetB0_ - tiltB0_) * blendFactor;
			tiltB1_ += (tiltTargetB1_ - tiltB1_) * blendFactor;
			tiltA1_ += (tiltTargetA1_ - tiltA1_) * blendFactor;
		}
		else
		{
			// Reset state when match is off
			if (tiltLastProfile_ != 0)
			{
				tiltState_[0] = tiltState_[1] = 0.0f;
				tiltLastProfile_ = 0;
				tiltLastSlope_ = 0.0f;
				tiltB0_ = tiltTargetB0_ = 1.0f;
				tiltB1_ = tiltTargetB1_ = 0.0f;
				tiltA1_ = tiltTargetA1_ = 0.0f;
			}
		}
	}

	// NORM: static peak-normalize wet signal to target level
	// Captures maximum peak (no release) and applies fixed gain.
	// Toggling NORM off/on resets the peak measurement.
	// A warmup period (~150 ms) prevents boost during convolver crossfade /
	// session restore, where early peaks are unreliable and could cause a spike.
	{
		const int normIdx = loadRelaxedInt (pTrim);
		if (normIdx > 0)
		{
			const float target = kNormTargets[juce::jlimit (0, kNumNormTargets - 1, normIdx)];

			// Measure block peak across all channels
			float blockPeak = 0.0f;
			for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
				blockPeak = std::max (blockPeak, buffer.getMagnitude (ch, 0, numSamples));

			// Peak-hold: only goes up, never down (static normalization)
			if (blockPeak > normPeakFollower_)
				normPeakFollower_ = blockPeak;

			// Calculate and apply gain once peak exceeds threshold (-60 dB)
			if (normPeakFollower_ > 0.001f)
			{
				// Accumulate warmup time (samples since first signal)
				normWarmupSamples_ += numSamples;
				const int warmupThreshold = static_cast<int> (getSampleRate() * 0.15f); // 150 ms
				const bool warmingUp = (normWarmupSamples_ < warmupThreshold);

				const float desiredGain = target / normPeakFollower_;
				const float clampedGain = juce::jlimit (0.01f, kMaxNormBoost, desiredGain);

				// During warmup: cap at unity (only allow cut, never boost).
				// After warmup: peak follower is reliable - allow full range.
				const float effectiveGain = warmingUp ? juce::jmin (1.0f, clampedGain)
				                                      : clampedGain;

				// Bi-directional smoothing: fast ramp-down (10 ms) when a louder
				// peak is captured, slower ramp-up (20 ms) for boost convergence.
				const float baseCoeff = (effectiveGain < normSmoothedGain_) ? cachedNormFastCoeff_ : cachedNormSlowCoeff_;
				const float coeff = 1.0f - std::pow (1.0f - baseCoeff, static_cast<float> (numSamples));
				normSmoothedGain_ += (effectiveGain - normSmoothedGain_) * coeff;

				for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
					juce::FloatVectorOperations::multiply (buffer.getWritePointer (ch), normSmoothedGain_, numSamples);
			}
		}
		else
		{
			// Reset state when off - next enable will re-measure
			normPeakFollower_  = 0.0f;
			normSmoothedGain_  = 1.0f;
			normWarmupSamples_ = 0;
		}
	}

	// Invert Polarity / Stereo (WET path: before wet DC/limiter and mix)
	{
		const int nc = buffer.getNumChannels();
		if (invPol == 1)
			for (int ch = 0; ch < nc; ++ch)
				juce::FloatVectorOperations::multiply (buffer.getWritePointer (ch), -1.0f, numSamples);
		if (invStr == 1 && nc >= 2)
		{
			float* sL = buffer.getWritePointer (0);
			float* sR = buffer.getWritePointer (1);
			for (int n = 0; n < numSamples; ++n)
				std::swap (sL[n], sR[n]);
		}
	}

	// Wet-only DC blocking filter (first-order high-pass ~5 Hz)
	// Removes DC offset introduced by convolution, resampled IRs, or filter artefacts
	// before WET limiting and before the global dry/wet mix.
	if (wetPathAudible && wetHasActiveLoader)
	{
		const float R = cachedDcBlockR_;
		for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
		{
			auto* data = buffer.getWritePointer (ch);
			float xPrev = dcBlockX_[ch];
			float yPrev = dcBlockY_[ch];
			for (int i = 0; i < numSamples; ++i)
			{
				const float x = data[i];
				const float y = x - xPrev + R * yPrev;
				xPrev = x;
				yPrev = y;
				data[i] = y;
			}
			dcBlockX_[ch] = xPrev;
			dcBlockY_[ch] = yPrev;
		}
	}
	else
	{
		dcBlockX_[0] = dcBlockX_[1] = 0.0f;
		dcBlockY_[0] = dcBlockY_[1] = 0.0f;
	}

	// User limiter (WET: after wet-only post processing, before global dry/wet mix)
	if (limMode == 1)
	{
		if (buffer.getNumChannels() >= 2)
			applyLimiter (buffer.getWritePointer (0), buffer.getWritePointer (1), numSamples,
			              limThreshLinStart, limThreshLin);
		else if (buffer.getNumChannels() == 1)
			applyLimiterMono (buffer.getWritePointer (0), numSamples,
			                  limThreshLinStart, limThreshLin);
	}

	// Global MIX: blend unprocessed dry with fully processed wet
	// dry = input after gain, wet = after all loader processing (mode + convolution + effects)
	if (needsGlobalDry || needsGlobalWetGain)
	{
		for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
		{
			if (needsGlobalWetGain)
				buffer.applyGainRamp (ch, 0, numSamples, lastGlobalWetMix_, globalWetEnd);
			if (needsGlobalDry)
				buffer.addFromWithRamp (ch, 0, globalDryBuffer.getReadPointer (ch), numSamples,
				                        lastGlobalDryMix_, globalDryEnd);
		}
	}
	lastGlobalWetMix_ = globalWetEnd;
	lastGlobalDryMix_ = globalDryEnd;

	// Flush denormals in global filter states (per-block, near-zero cost)
	{
		constexpr float kDnr = 1e-20f;
		if (std::abs (tiltState_[0]) < kDnr) tiltState_[0] = 0.0f;
		if (std::abs (tiltState_[1]) < kDnr) tiltState_[1] = 0.0f;
		if (std::abs (dcBlockX_[0])  < kDnr) dcBlockX_[0]  = 0.0f;
		if (std::abs (dcBlockX_[1])  < kDnr) dcBlockX_[1]  = 0.0f;
		if (std::abs (dcBlockY_[0])  < kDnr) dcBlockY_[0]  = 0.0f;
		if (std::abs (dcBlockY_[1])  < kDnr) dcBlockY_[1]  = 0.0f;
	}

	// Apply output gain
	const float outputGain = gainFaderDecibelsToGain (loadRelaxed (pOutput));
	for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
		buffer.applyGainRamp (ch, 0, numSamples, lastOutputGain_, outputGain);
	lastOutputGain_ = outputGain;

	// Post-prepare fade-in: suppress convolver/filter warmup transients
	if (fadeInSamplesRemaining_ > 0)
	{
		const int fadeThisBlock = juce::jmin (fadeInSamplesRemaining_, numSamples);
		const float invTotal = 1.0f / static_cast<float> (fadeInTotalSamples_);
		for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
		{
			auto* data = buffer.getWritePointer (ch);
			for (int i = 0; i < fadeThisBlock; ++i)
			{
				const int pos = fadeInTotalSamples_ - fadeInSamplesRemaining_ + i;
				data[i] *= static_cast<float> (pos) * invTotal;
			}
		}
		fadeInSamplesRemaining_ -= fadeThisBlock;
	}

	// User limiter (GLOBAL: after output gain, before safety clip)
	if (limMode == 2)
	{
		if (buffer.getNumChannels() >= 2)
			applyLimiter (buffer.getWritePointer (0), buffer.getWritePointer (1), numSamples,
			              limThreshLinStart, limThreshLin);
		else if (buffer.getNumChannels() == 1)
			applyLimiterMono (buffer.getWritePointer (0), numSamples,
			                  limThreshLinStart, limThreshLin);
	}

	// Invert Polarity / Stereo (GLOBAL mode: after Limiter GLOBAL, before safety clip)
	{
		const int nc = buffer.getNumChannels();
		if (invPol == 2)
			for (int ch = 0; ch < nc; ++ch)
				juce::FloatVectorOperations::multiply (buffer.getWritePointer (ch), -1.0f, numSamples);
		if (invStr == 2 && nc >= 2)
		{
			float* sL = buffer.getWritePointer (0);
			float* sR = buffer.getWritePointer (1);
			for (int n = 0; n < numSamples; ++n)
				std::swap (sL[n], sR[n]);
		}
	}

	// Safety hard-limiter: prevent catastrophic output only (NaN/Inf runaway).
	// Set very high (+48 dBFS) so it never engages during normal operation.
	{
#if CABTR_DSP_DEBUG_LOG
		// Log output levels before safety limiter (throttled - same ~1s cadence)
		{
			static int diagOutCount = 0;
			++diagOutCount;
			const int bps = juce::jmax (1, (int)(currentSampleRate / juce::jmax (1, numSamples)));
			if (diagOutCount >= bps)
			{
				diagOutCount = 0;
				float outPeakL = 0.0f, outPeakR = 0.0f;
				if (buffer.getNumChannels() >= 1) outPeakL = buffer.getMagnitude (0, 0, numSamples);
				if (buffer.getNumChannels() >= 2) outPeakR = buffer.getMagnitude (1, 0, numSamples);
				juce::String d;
				d << "OUTPUT peakL=" << juce::String (outPeakL, 4) << " peakR=" << juce::String (outPeakR, 4)
				  << " outputGain=" << juce::String (outputGain, 6)
				  << " outputDb=" << juce::String (loadRelaxed (pOutput), 2)
				  << " fadeRemaining=" << fadeInSamplesRemaining_;
				LOG_IR_EVENT (d);
			}
		}
#endif
		constexpr float kSafetyLimit = 251.19f; // +48 dBFS - only catches runaways
		for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
		{
			auto* data = buffer.getWritePointer (ch);
			juce::FloatVectorOperations::clip (data, data, -kSafetyLimit, kSafetyLimit, numSamples);
		}
	}

}

//==============================================================================
// Measure spectral slope of an IR in dB/octave via FFT + linear regression
// Analyses magnitude spectrum from 100 Hz to 10 kHz
static float computeIRSlopeDbPerOct (const juce::AudioBuffer<float>& ir, double sampleRate)
{
	const int numSamples = ir.getNumSamples();
	if (numSamples < 64 || sampleRate <= 0.0)
		return 0.0f;

	// FFT order: next power of 2 >= numSamples, capped at 8192 for efficiency
	const int fftOrder = juce::jmin (13, static_cast<int> (std::ceil (std::log2 (numSamples))));
	const int fftSize = 1 << fftOrder;
	juce::dsp::FFT fft (fftOrder);

	// Average magnitude across channels
	std::vector<float> magnitudeDb (fftSize / 2 + 1, 0.0f);

	const int numChannels = ir.getNumChannels();
	std::vector<float> fftData (fftSize * 2, 0.0f);

	for (int ch = 0; ch < numChannels; ++ch)
	{
		std::fill (fftData.begin(), fftData.end(), 0.0f);
		const float* src = ir.getReadPointer (ch);
		const int copyLen = juce::jmin (numSamples, fftSize);
		for (int i = 0; i < copyLen; ++i)
			fftData[i] = src[i];

		fft.performRealOnlyForwardTransform (fftData.data(), true);

		// Accumulate magnitude in dB
		for (int bin = 0; bin <= fftSize / 2; ++bin)
		{
			const float re = fftData[bin * 2];
			const float im = fftData[bin * 2 + 1];
			const float mag = std::sqrt (re * re + im * im);
			magnitudeDb[static_cast<size_t> (bin)] += (mag > 1e-10f) ? 20.0f * std ::log10 (mag)
				: -200.0f;
		}
	}

	// Average across channels
	const float chScale = 1.0f / static_cast<float> (numChannels);
	for (auto& v : magnitudeDb)
		v *= chScale;

	// Linear regression: dB vs log2(freq) over 100 Hz - 10 kHz
	const double freqPerBin = sampleRate / fftSize;
	const int binLow  = juce::jmax (1, static_cast<int> (std::ceil  (100.0  / freqPerBin)));
	const int binHigh = juce::jmin (fftSize / 2, static_cast<int> (std::floor (10000.0 / freqPerBin)));
	if (binHigh <= binLow + 2)
		return 0.0f;

	// Weighted linear regression (log2(freq) vs dB)
	double sumX = 0, sumY = 0, sumXX = 0, sumXY = 0;
	int n = 0;
	for (int bin = binLow; bin <= binHigh; ++bin)
	{
		const double freq = bin * freqPerBin;
		const double x = std::log2 (freq);  // octaves
		const double y = magnitudeDb[static_cast<size_t> (bin)];
		if (y < -100.0) continue; // skip bins with negligible energy
		sumX += x;
		sumY += y;
		sumXX += x * x;
		sumXY += x * y;
		++n;
	}

	if (n < 3)
		return 0.0f;

	// Slope = (n*sumXY - sumX*sumY) / (n*sumXX - sumX*sumX)
	const double denom = n * sumXX - sumX * sumX;
	if (std::abs (denom) < 1e-12)
		return 0.0f;

	return static_cast<float> ((n * sumXY - sumX * sumY) / denom);
}

//==============================================================================
void CABTRAudioProcessor::loadImpulseResponse (IRLoaderState& state, const juce::String& filePath)
{
	if (filePath.isEmpty())
		return;

	juce::File irFile (filePath);
	if (! irFile.existsAsFile())
		return;

	// Load audio file (reusable format manager - no re-registration per load)
	std::unique_ptr<juce::AudioFormatReader> reader (
		formatManager.createReaderFor (irFile));

	if (reader == nullptr)
		return;

	// Smart read: only read the samples we actually need (avoids loading 480K samples to discard 99%)
	// 1. Calculate trim boundaries first, then read only that range + margin for size
	const int loaderIdx = (&state == &stateA) ? 0 : ((&state == &stateB) ? 1 : 2);
	auto pickParam = [&] (const char* a, const char* b, const char* c) -> const char*
		{ return loaderIdx == 0 ? a : (loaderIdx == 1 ? b : c); };
	const float startMs_pre = parameters.getRawParameterValue (pickParam (kParamStartA, kParamStartB, kParamStartC))->load();
	const float endMs_pre = parameters.getRawParameterValue (pickParam (kParamEndA, kParamEndB, kParamEndC))->load();
	const float size_pre = parameters.getRawParameterValue (pickParam (kParamSizeA, kParamSizeB, kParamSizeC))->load();

	const juce::int64 totalFileSamples = reader->lengthInSamples;
	const double sr = reader->sampleRate;

	// Compute the actual sample range we need from the file
	juce::int64 readStart = static_cast<juce::int64> (startMs_pre * 0.001 * sr);
	juce::int64 readEnd = static_cast<juce::int64> (endMs_pre * 0.001 * sr);
	readStart = juce::jlimit<juce::int64> (0, totalFileSamples, readStart);
	readEnd = juce::jlimit<juce::int64> (readStart + 64, totalFileSamples, readEnd);

	// Add margin for size < 1.0 (stretching needs more source samples)
	// size=0.25 means we need 4x more source samples than output
	const float safeSize = juce::jmax (0.25f, size_pre);
	const juce::int64 sizeMargin = (safeSize < 1.0f) ? static_cast<juce ::int64> ((readEnd - readStart) * (1.0f / safeSize - 1.0f)) + 64
	    : 0;
	readEnd = juce::jmin (readEnd + sizeMargin, totalFileSamples);

	const auto samplesToRead = readEnd - readStart;

	// Read only the needed range from file
	juce::AudioBuffer<float> tempIR;
	tempIR.setSize (static_cast<int> (reader->numChannels),
	                static_cast<int> (samplesToRead));
	reader->read (&tempIR, 0, static_cast<int> (samplesToRead), 
	              static_cast<int> (readStart), true, true);

	// Apply START/END trimming if needed
	// (Parameters are in milliseconds, convert to samples)
	const char* startParam = pickParam (kParamStartA, kParamStartB, kParamStartC);
	const char* endParam = pickParam (kParamEndA, kParamEndB, kParamEndC);
	const char* sizeParam = pickParam (kParamSizeA, kParamSizeB, kParamSizeC);
	const char* invParam = pickParam (kParamInvA, kParamInvB, kParamInvC);
	const char* normParam = pickParam (kParamNormA, kParamNormB, kParamNormC);
	const char* rvsParam = pickParam (kParamRvsA, kParamRvsB, kParamRvsC);
	const char* resoParam = pickParam (kParamResoA, kParamResoB, kParamResoC);
	
	const float startMs = parameters.getRawParameterValue (startParam)->load();
	const float endMs = parameters.getRawParameterValue (endParam)->load();
	const float size = parameters.getRawParameterValue (sizeParam)->load();
	const bool invert = parameters.getRawParameterValue (invParam)->load() > 0.5f;
	const bool normalize = parameters.getRawParameterValue (normParam)->load() > 0.5f;
	const bool reverse = parameters.getRawParameterValue (rvsParam)->load() > 0.5f;
	const float reso = parameters.getRawParameterValue (resoParam)->load();
	
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
	
	// Apply SIZE RESAMPLING using Lagrange interpolation (high quality, no aliasing)
	// size < 1.0 = slower playback (larger cab, longer IR)
	// size > 1.0 = faster playback (smaller cab, shorter IR)
	if (std::abs (size - 1.0f) > 0.001f && size > 0.1f && size < 10.0f)
	{
		const int resampledLength = static_cast<int> (trimmedLength / size);
		if (resampledLength > 0 && resampledLength < trimmedLength * 10)
		{
			juce::AudioBuffer<float> resampledIR;
			resampledIR.setSize (state.impulseResponse.getNumChannels(), resampledLength);
			
			// Lagrange interpolator for high-quality resampling (4-point)
			for (int ch = 0; ch < state.impulseResponse.getNumChannels(); ++ch)
			{
				juce::LagrangeInterpolator interpolator;
				const float* input = state.impulseResponse.getReadPointer (ch);
				float* output = resampledIR.getWritePointer (ch);
				
				// Resample with inverse ratio (size=2.0 -> ratio=0.5)
				const double resampleRatio = 1.0 / size;
				interpolator.process (resampleRatio, input, output, resampledLength, trimmedLength, 0);
			}
			
			// Replace with resampled version
			state.impulseResponse = std::move (resampledIR);
		}
	}
	
	// Enforce minimum length AFTER size resampling (size>1 shrinks the IR)
	if (state.impulseResponse.getNumSamples() < kMinIrSamples)
	{
		const int currentLen = state.impulseResponse.getNumSamples();
		const int numCh = state.impulseResponse.getNumChannels();
		juce::AudioBuffer<float> padded (numCh, kMinIrSamples);
		padded.clear();
		for (int ch = 0; ch < numCh; ++ch)
			padded.copyFrom (ch, 0, state.impulseResponse, ch, 0, currentLen);
		state.impulseResponse = std::move (padded);
		LOG_IR_EVENT ("IR padded from " + juce::String (currentLen) + " to " + juce::String (kMinIrSamples) + " samples (post-size minimum)");
	}
	
	const int finalLength = state.impulseResponse.getNumSamples();
	
	// Apply RESO envelope (modifies IR decay rate)
	// reso < 1.0: faster decay (less resonance), reso = 1.0: unchanged, reso > 1.0: slower decay (more resonance)
	if (std::abs (reso - 1.0f) > 0.001f && finalLength > 1)
	{
		const int numCh = state.impulseResponse.getNumChannels();
		
		// Measure peak before envelope
		float peakBefore = 0.0f;
		for (int ch = 0; ch < numCh; ++ch)
			peakBefore = juce::jmax (peakBefore, state.impulseResponse.getMagnitude (ch, 0, finalLength));
		
		// k maps reso to exponential rate: 0->-4, 1->0, 2->+4
		// At 0%: tail attenuated ~35dB. At 200%: tail boosted ~35dB (peak-compensated)
		const float k = (reso - 1.0f) * 4.0f;
		const float invLen = 1.0f / juce::jmax (1.0f, static_cast<float> (finalLength - 1));
		
		for (int ch = 0; ch < numCh; ++ch)
		{
			float* data = state.impulseResponse.getWritePointer (ch);
			for (int i = 0; i < finalLength; ++i)
			{
				const float t = static_cast<float> (i) * invLen;
				data[i] *= std::exp (k * t);
			}
		}
		
		// Re-normalize to preserve original peak level (reso changes shape, not loudness)
		float peakAfter = 0.0f;
		for (int ch = 0; ch < numCh; ++ch)
			peakAfter = juce::jmax (peakAfter, state.impulseResponse.getMagnitude (ch, 0, finalLength));
		
		if (peakAfter > 0.0001f && peakBefore > 0.0001f)
		{
			const float compensationGain = peakBefore / peakAfter;
			state.impulseResponse.applyGain (compensationGain);
		}
		
		LOG_IR_EVENT ("RESO applied: " + juce::String (reso * 100.0f, 0) + "% (k=" + juce::String (k, 2) + ")");
	}
	
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
		constexpr float kMaxBoost = kMaxNormBoost; // +18dB max (industry standard)
		
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

	// Measure spectral slope for adaptive Match tilt EQ
	state.irSlopeDbPerOct.store (computeIRSlopeDbPerOct (state.impulseResponse, reader->sampleRate));
	LOG_IR_EVENT ("IR spectral slope: " + juce::String (state.irSlopeDbPerOct.load(), 2) + " dB/oct");

	// Load into convolution engine
	// NonUniform{256} uses partitioned FFT - handles long IRs efficiently
	LOG_IR_EVENT ("IR final length: " + juce::String (state.impulseResponse.getNumSamples()) +
	              " samples (" + juce::String (state.impulseResponse.getNumSamples() / reader->sampleRate * 1000.0, 1) + " ms)");

	// Load IR into two-stage threaded convolver (head on audio thread, tail on background)
	// We pass state.impulseResponse directly - loadIR copies internally
	state.convolution.loadIR (state.impulseResponse, reader->sampleRate);
	
	// Update cached parameter values AFTER successful load
	state.lastSize.store (size);
	state.lastInv.store (invert);
	state.lastNorm.store (normalize);
	state.lastRvs.store (reverse);
	state.lastStart.store (startMs);
	state.lastEnd.store (endMs);
	state.lastReso.store (reso);

	state.currentFilePath = filePath;
	state.irSampleRate = reader->sampleRate;
	state.needsUpdate = false;
	state.lastChangeTime = 0;

	LOG_IR_EVENT ("IR Reload " + juce::String ("ABC"[loaderIdx]) + ": " + 
	     juce::String (finalLength) + " samples (" +
	     juce::String (finalLength / reader->sampleRate * 1000.0, 1) + "ms)" +
	     ", size=" + juce::String (size, 2) +
	     ", inv=" + juce::String (invert ? 1 : 0) + ", rvs=" + juce::String (reverse ? 1 : 0) +
	     ", norm=" + juce::String (normalize ? 1 : 0));
}

//==============================================================================
// EXPORT COMBINED IR - Offline capture through the static loader chain
// Generates a unit impulse, processes it through both loaders (routing, filters,
// DIST, PAN, DELAY, ANGLE, mode in/out, mix, gains) except dynamic/non-static stages
// such as CHAOS and EXP.
// formatType: 0=WAV16, 1=WAV24, 2=WAV32f, 3=AIFF24, 4=FLAC24
//==============================================================================

// FFT-based linear convolution (for series routing: A output -> B convolution)
static juce::AudioBuffer<float> fftConvolve (const juce::AudioBuffer<float>& signal,
                                              const juce::AudioBuffer<float>& kernel)
{
	const int sigLen = signal.getNumSamples();
	const int kerLen = kernel.getNumSamples();
	const int outLen = sigLen + kerLen - 1;

	int fftOrder = 0;
	while ((1 << fftOrder) < outLen)
		++fftOrder;
	const int fftSize = 1 << fftOrder;

	const int numCh = juce::jmin (signal.getNumChannels(), kernel.getNumChannels());
	juce::AudioBuffer<float> result (numCh, outLen);
	result.clear();

	juce::dsp::FFT fft (fftOrder);

	std::vector<float> sigBuf (static_cast<size_t> (fftSize) * 2, 0.0f);
	std::vector<float> kerBuf (static_cast<size_t> (fftSize) * 2, 0.0f);
	std::vector<float> outBuf (static_cast<size_t> (fftSize) * 2, 0.0f);

	for (int ch = 0; ch < numCh; ++ch)
	{
		std::fill (sigBuf.begin(), sigBuf.end(), 0.0f);
		std::fill (kerBuf.begin(), kerBuf.end(), 0.0f);

		std::memcpy (sigBuf.data(), signal.getReadPointer (ch),
		             sizeof (float) * static_cast<size_t> (sigLen));
		const int kerCh = juce::jmin (ch, kernel.getNumChannels() - 1);
		std::memcpy (kerBuf.data(), kernel.getReadPointer (kerCh),
		             sizeof (float) * static_cast<size_t> (kerLen));

		fft.performRealOnlyForwardTransform (sigBuf.data());
		fft.performRealOnlyForwardTransform (kerBuf.data());

		// Complex multiply
		for (int i = 0; i < fftSize; ++i)
		{
			const float sr = sigBuf[static_cast<size_t> (i) * 2];
			const float si = sigBuf[static_cast<size_t> (i) * 2 + 1];
			const float kr = kerBuf[static_cast<size_t> (i) * 2];
			const float ki = kerBuf[static_cast<size_t> (i) * 2 + 1];
			outBuf[static_cast<size_t> (i) * 2]     = sr * kr - si * ki;
			outBuf[static_cast<size_t> (i) * 2 + 1] = sr * ki + si * kr;
		}

		fft.performRealOnlyInverseTransform (outBuf.data());

		std::memcpy (result.getWritePointer (ch), outBuf.data(),
		             sizeof (float) * static_cast<size_t> (outLen));
	}

	return result;
}

// Double-precision radix-2 Cooley-Tukey FFT (in-place)
// Used exclusively for MPT export where float32 precision is insufficient.
// N must be a power of 2.  When inverse=true, output is scaled by 1/N.
namespace
{
	void fftRadix2 (std::complex<double>* data, int N, bool inverse)
	{
		// Bit-reversal permutation
		for (int i = 1, j = 0; i < N; ++i)
		{
			int bit = N >> 1;
			for (; j & bit; bit >>= 1)
				j ^= bit;
			j ^= bit;
			if (i < j)
				std::swap (data[i], data[j]);
		}

		// Cooley-Tukey butterfly stages
		for (int len = 2; len <= N; len <<= 1)
		{
			const double ang = (inverse ? 1.0 : -1.0) * 2.0 * juce::MathConstants<double>::pi / len;
			const std::complex<double> wlen (std::cos (ang), std::sin (ang));

			for (int i = 0; i < N; i += len)
			{
				std::complex<double> w (1.0, 0.0);
				for (int j = 0; j < len / 2; ++j)
				{
					auto u = data[i + j];
					auto v = data[i + j + len / 2] * w;
					data[i + j]           = u + v;
					data[i + j + len / 2] = u - v;
					w *= wlen;
				}
			}
		}

		if (inverse)
		{
			const double invN = 1.0 / static_cast<double> (N);
			for (int i = 0; i < N; ++i)
				data[i] *= invN;
		}
	}
}

bool CABTRAudioProcessor::exportCombinedIR (double targetSampleRate,
                                             int formatType, double maxLengthSec,
                                             bool trimSilence,
                                             bool normalizeOutput,
                                             bool minimumPhase,
                                             const juce::File& outputFile)
{
	// Read current global parameters
	const bool enableA = pEnableA->load() > 0.5f;
	const bool enableB = pEnableB->load() > 0.5f;
	const bool enableC = pEnableC->load() > 0.5f;
	const int route = static_cast<int> (pRoute->load());
	const float inputGainDb = pInput->load();
	const float globalMix = pMix->load();
	const int mixMode = static_cast<int> (pMixMode->load());
	const float dryLevel = pDryLevel->load();
	const float wetLevel = pWetLevel->load();
	const float outputGainDb = pOutput->load();
	const int invPol = static_cast<int> (pInvPol->load());
	const int invStr = static_cast<int> (pInvStr->load());
	const int modeInA  = static_cast<int> (pModeInA->load());
	const int modeOutA = static_cast<int> (pModeOutA->load());
	const int modeInB  = static_cast<int> (pModeInB->load());
	const int modeOutB = static_cast<int> (pModeOutB->load());
	const int modeInC  = static_cast<int> (pModeInC->load());
	const int modeOutC = static_cast<int> (pModeOutC->load());
	const float mixA = pMixA->load();
	const float mixB = pMixB->load();
	const float mixC = pMixC->load();
	const int sumBusA = static_cast<int> (pSumBusA->load());
	const int sumBusB = static_cast<int> (pSumBusB->load());
	const int sumBusC = static_cast<int> (pSumBusC->load());
	const int filterPosA = static_cast<int> (pFilterPosA->load());
	const int filterPosB = static_cast<int> (pFilterPosB->load());
	const int filterPosC = static_cast<int> (pFilterPosC->load());

	// Need at least one enabled loader with an IR
	const bool hasA = enableA && stateA.impulseResponse.getNumSamples() > 0;
	const bool hasB = enableB && stateB.impulseResponse.getNumSamples() > 0;
	const bool hasC = enableC && stateC.impulseResponse.getNumSamples() > 0;
	if (! hasA && ! hasB && ! hasC)
		return false;

	CLEAR_IR_LOG();
	LOG_IR_EVENT ("====== EXPORT START ======");
	LOG_IR_EVENT ("route=" + juce::String (route)
	             + " enableA=" + juce::String ((int) enableA)
	             + " enableB=" + juce::String ((int) enableB)
	             + " enableC=" + juce::String ((int) enableC)
	             + " hasA=" + juce::String ((int) hasA)
	             + " hasB=" + juce::String ((int) hasB)
	             + " hasC=" + juce::String ((int) hasC));
	LOG_IR_EVENT ("modeInA=" + juce::String (modeInA) + " modeOutA=" + juce::String (modeOutA)
	             + " modeInB=" + juce::String (modeInB) + " modeOutB=" + juce::String (modeOutB)
	             + " modeInC=" + juce::String (modeInC) + " modeOutC=" + juce::String (modeOutC));
	LOG_IR_EVENT ("mixA=" + juce::String (mixA, 4) + " mixB=" + juce::String (mixB, 4) + " mixC=" + juce::String (mixC, 4));
	LOG_IR_EVENT ("sumBusA=" + juce::String (sumBusA) + " sumBusB=" + juce::String (sumBusB) + " sumBusC=" + juce::String (sumBusC));
	LOG_IR_EVENT ("inputGainDb=" + juce::String (inputGainDb, 3));

	const double workingSR = currentSampleRate;
	const int maxWorkingSamples = static_cast<int> (maxLengthSec * workingSR) + 1;

	const int irLenA = hasA ? stateA.impulseResponse.getNumSamples() : 0;
	const int irLenB = hasB ? stateB.impulseResponse.getNumSamples() : 0;
	const int irLenC = hasC ? stateC.impulseResponse.getNumSamples() : 0;

	// Determine working length based on routing
	int workingLen = 0;
	if (route == 0) // A->B->C series: convolution chain
	{
		int len = juce::jmax (irLenA, 1);
		if (hasA && hasB) len = irLenA + irLenB - 1;
		if ((hasA || hasB) && hasC) len = len + irLenC - 1;
		workingLen = juce::jmax (len, juce::jmax (irLenA, juce::jmax (irLenB, irLenC)));
	}
	else if (route == 1) // A|B|C parallel
		workingLen = juce::jmax (irLenA, juce::jmax (irLenB, irLenC));
	else if (route == 2) // A->B|C
	{
		int seriesLen = (hasA && hasB) ? (irLenA + irLenB - 1) : juce::jmax (irLenA, irLenB);
		workingLen = juce::jmax (seriesLen, irLenC);
	}
	else if (route == 3) // A|B->C
	{
		int seriesLen = (hasB && hasC) ? (irLenB + irLenC - 1) : juce::jmax (irLenB, irLenC);
		workingLen = juce::jmax (irLenA, seriesLen);
	}
	else if (route == 4) // (A|B)->C
	{
		const int parallelLen = juce::jmax (irLenA, irLenB);
		const bool hasParallelAB = hasA || hasB;
		workingLen = (hasParallelAB && hasC)
			? (parallelLen + irLenC - 1)
			: juce::jmax (parallelLen, irLenC);
	}
	else // route == 5: A->(B|C)
	{
		const int branchBLen = (hasA && hasB) ? (irLenA + irLenB - 1) : juce::jmax (irLenA, irLenB);
		const int branchCLen = (hasA && hasC) ? (irLenA + irLenC - 1) : juce::jmax (irLenA, irLenC);
		workingLen = juce::jmax (branchBLen, branchCLen);
	}

	// Add headroom for delay
	const float maxDelayMs = juce::jmax (
		hasA ? std::abs (pDelayA->load()) : 0.0f,
		juce::jmax (
			hasB ? std::abs (pDelayB->load()) : 0.0f,
			hasC ? std::abs (pDelayC->load()) : 0.0f));
	workingLen += static_cast<int> (maxDelayMs * 0.001 * workingSR) + 1;
	workingLen = juce::jmin (workingLen, maxWorkingSamples);

	auto convolveExistingPath = [&] (juce::AudioBuffer<float>& path, const juce::AudioBuffer<float>& ir) -> void
	{
		auto convolved = fftConvolve (path, ir);
		const int convLen = juce::jmin (convolved.getNumSamples(), workingLen);
		path.clear();
		for (int ch = 0; ch < juce::jmin (2, convolved.getNumChannels()); ++ch)
			path.copyFrom (ch, 0, convolved, ch, 0, convLen);
		if (convolved.getNumChannels() == 1)
			path.copyFrom (1, 0, path, 0, 0, workingLen);
	};

	auto makeRootInput = [&]() -> juce::AudioBuffer<float>
	{
		juce::AudioBuffer<float> buf (2, workingLen);
		buf.clear();
		if (workingLen > 0)
		{
			buf.setSample (0, 0, 1.0f);
			if (buf.getNumChannels() >= 2)
				buf.setSample (1, 0, 1.0f);
		}
		return buf;
	};

	auto applyOfflinePreTone = [&] (juce::AudioBuffer<float>& path, int loaderIndex, int filterPos) -> void
	{
		const bool filterPre = (filterPos == 1 || filterPos == 2);
		const bool tiltPre = (filterPos == 1 || filterPos == 3);
		if (! filterPre && ! tiltPre)
			return;

		auto pick = [&] (std::atomic<float>* a, std::atomic<float>* b, std::atomic<float>* c) -> std::atomic<float>*
		{
			return loaderIndex == 0 ? a : (loaderIndex == 1 ? b : c);
		};

		const int numSamples = path.getNumSamples();
		const int numChannels = path.getNumChannels();
		const float hpFreq = pick (pHpFreqA, pHpFreqB, pHpFreqC)->load();
		const float lpFreq = pick (pLpFreqA, pLpFreqB, pLpFreqC)->load();
		const bool hpOn = pick (pHpOnA, pHpOnB, pHpOnC)->load() > 0.5f;
		const bool lpOn = pick (pLpOnA, pLpOnB, pLpOnC)->load() > 0.5f;
		const int hpSlope = juce::jlimit (kFilterSlopeMin, kFilterSlopeMax,
		                                  static_cast<int> (pick (pHpSlopeA, pHpSlopeB, pHpSlopeC)->load()));
		const int lpSlope = juce::jlimit (kFilterSlopeMin, kFilterSlopeMax,
		                                  static_cast<int> (pick (pLpSlopeA, pLpSlopeB, pLpSlopeC)->load()));
		const float tiltDb = pick (pTiltA, pTiltB, pTiltC)->load();
		const float maxFreq = static_cast<float> (workingSR) * 0.49f;

		auto applyTilt = [&]()
		{
			if (std::abs (tiltDb) <= 0.05f)
				return;

			float b0, b1, a1;
			computeTiltShelfCoeffs (workingSR, tiltDb, b0, b1, a1);

			for (int ch = 0; ch < numChannels; ++ch)
			{
				auto* data = path.getWritePointer (ch);
				float s = 0.0f;
				for (int i = 0; i < numSamples; ++i)
				{
					const float x = data[i];
					const float y = b0 * x + s;
					s = b1 * x - a1 * y;
					data[i] = y;
				}
			}
		};

		auto applyFilters = [&]()
		{
			juce::dsp::ProcessSpec spec;
			spec.sampleRate = workingSR;
			spec.maximumBlockSize = static_cast<juce::uint32> (numSamples);
			spec.numChannels = static_cast<juce::uint32> (numChannels);
			juce::dsp::AudioBlock<float> block (path);
			juce::dsp::ProcessContextReplacing<float> ctx (block);

			if (hpOn && hpFreq >= 21.0f)
			{
				const float clampedHp = juce::jlimit (20.0f, maxFreq, hpFreq);
				juce::dsp::ProcessorDuplicator<juce::dsp::IIR::Filter<float>,
				                                juce::dsp::IIR::Coefficients<float>> hp;
				hp.prepare (spec);
				if (hpSlope == 0)
					*hp.state = *juce::dsp::IIR::Coefficients<float>::makeFirstOrderHighPass (workingSR, clampedHp);
				else if (hpSlope == 1)
					*hp.state = *juce::dsp::IIR::Coefficients<float>::makeHighPass (workingSR, clampedHp, kSqrt2Over2);
				else
					*hp.state = *juce::dsp::IIR::Coefficients<float>::makeHighPass (workingSR, clampedHp, kBW4_Q1);
				hp.process (ctx);

				if (hpSlope == 2)
				{
					juce::dsp::ProcessorDuplicator<juce::dsp::IIR::Filter<float>,
					                                juce::dsp::IIR::Coefficients<float>> hp2;
					hp2.prepare (spec);
					*hp2.state = *juce::dsp::IIR::Coefficients<float>::makeHighPass (workingSR, clampedHp, kBW4_Q2);
					hp2.process (ctx);
				}
			}

			if (lpOn && lpFreq <= 19900.0f)
			{
				const float clampedLp = juce::jlimit (20.0f, maxFreq, lpFreq);
				juce::dsp::ProcessorDuplicator<juce::dsp::IIR::Filter<float>,
				                                juce::dsp::IIR::Coefficients<float>> lp;
				lp.prepare (spec);
				if (lpSlope == 0)
					*lp.state = *juce::dsp::IIR::Coefficients<float>::makeFirstOrderLowPass (workingSR, clampedLp);
				else if (lpSlope == 1)
					*lp.state = *juce::dsp::IIR::Coefficients<float>::makeLowPass (workingSR, clampedLp, kSqrt2Over2);
				else
					*lp.state = *juce::dsp::IIR::Coefficients<float>::makeLowPass (workingSR, clampedLp, kBW4_Q1);
				lp.process (ctx);

				if (lpSlope == 2)
				{
					juce::dsp::ProcessorDuplicator<juce::dsp::IIR::Filter<float>,
					                                juce::dsp::IIR::Coefficients<float>> lp2;
					lp2.prepare (spec);
					*lp2.state = *juce::dsp::IIR::Coefficients<float>::makeLowPass (workingSR, clampedLp, kBW4_Q2);
					lp2.process (ctx);
				}
			}
		};

		if (filterPre) applyFilters();
		if (tiltPre) applyTilt();
	};

	auto getLoaderInputDb = [&] (int loaderIndex) -> float
	{
		switch (loaderIndex)
		{
			case 0:  return pInA->load();
			case 1:  return pInB->load();
			default: return pInC->load();
		}
	};

	auto processLoaderPath = [&] (const juce::AudioBuffer<float>* inputPath,
	                              IRLoaderState& state,
	                              int loaderIndex,
	                              int modeIn,
	                              int modeOut,
	                              float loaderMix) -> juce::AudioBuffer<float>
	{
		juce::AudioBuffer<float> dryPath;
		if (inputPath != nullptr && inputPath->getNumSamples() > 0)
		{
			dryPath.makeCopyOf (*inputPath, true);
		}
		else
		{
			dryPath = makeRootInput();
		}

		juce::AudioBuffer<float> wetPath;
		wetPath.makeCopyOf (dryPath, true);
		applyMidSideInputMode (wetPath, modeIn, wetPath.getNumSamples());

		const float inDb = getLoaderInputDb (loaderIndex);
		const float inGain = gainFaderDecibelsToGain (inDb);
		if (std::abs (inGain - 1.0f) > 1.0e-6f)
			wetPath.applyGain (inGain);

		const int filterPos = loaderIndex == 0 ? filterPosA : (loaderIndex == 1 ? filterPosB : filterPosC);
		applyOfflinePreTone (wetPath, loaderIndex, filterPos);
		convolveExistingPath (wetPath, state.impulseResponse);
		offlineProcessLoaderEffects (wetPath, dryPath, loaderIndex, workingSR, modeOut, filterPos, loaderMix);
		return wetPath;
	};

	// Build the combined IR offline
	juce::AudioBuffer<float> result;

	if (route == 1) // A|B|C parallel
	{
		int numActive = 0;
		result.setSize (2, workingLen);
		result.clear();

		juce::AudioBuffer<float> procA, procB, procC;
		if (hasA)
		{
			procA = processLoaderPath (nullptr, stateA, 0, modeInA, modeOutA, mixA);
			numActive++;
		}
		if (hasB)
		{
			procB = processLoaderPath (nullptr, stateB, 1, modeInB, modeOutB, mixB);
			numActive++;
		}
		if (hasC)
		{
			procC = processLoaderPath (nullptr, stateC, 2, modeInC, modeOutC, mixC);
			numActive++;
		}

		// Sum with SUM BUS M/S routing (matching realtime)
		const bool anyMSBus = (hasA && sumBusA != 0) || (hasB && sumBusB != 0) || (hasC && sumBusC != 0);
		if (anyMSBus)
		{
			auto* outL = result.getWritePointer (0);
			auto* outR = result.getWritePointer (1);
			const float* srcL[3] = { hasA ? procA.getReadPointer (0) : nullptr,
			                         hasB ? procB.getReadPointer (0) : nullptr,
			                         hasC ? procC.getReadPointer (0) : nullptr };
			const float* srcR[3] = { hasA ? procA.getReadPointer (1) : nullptr,
			                         hasB ? procB.getReadPointer (1) : nullptr,
			                         hasC ? procC.getReadPointer (1) : nullptr };
			const int buses[3] = { sumBusA, sumBusB, sumBusC };
			const bool active[3] = { hasA, hasB, hasC };
			for (int i = 0; i < workingLen; ++i)
			{
				float stL = 0.0f, stR = 0.0f, midBus = 0.0f, sideBus = 0.0f;
				for (int k = 0; k < 3; ++k)
				{
					if (!active[k]) continue;
					injectMSBus (srcL[k][i], srcR[k][i], buses[k], stL, stR, midBus, sideBus);
				}
				outL[i] = stL + midBus + sideBus;
				outR[i] = stR + midBus - sideBus;
			}
		}
		else
		{
			if (hasA)
				for (int ch = 0; ch < 2; ++ch)
					juce::FloatVectorOperations::add (result.getWritePointer (ch), procA.getReadPointer (ch), workingLen);
			if (hasB)
				for (int ch = 0; ch < 2; ++ch)
					juce::FloatVectorOperations::add (result.getWritePointer (ch), procB.getReadPointer (ch), workingLen);
			if (hasC)
				for (int ch = 0; ch < 2; ++ch)
					juce::FloatVectorOperations::add (result.getWritePointer (ch), procC.getReadPointer (ch), workingLen);
		}
	}
	else if (route == 2) // A->B|C
	{
		// Series path: A->B
		juce::AudioBuffer<float> seriesPath;
		bool seriesHasContent = false;

		if (hasA)
		{
			seriesPath = processLoaderPath (nullptr, stateA, 0, modeInA, modeOutA, mixA);
			seriesHasContent = true;
		}
		if (hasB)
		{
			seriesPath = processLoaderPath (seriesHasContent ? &seriesPath : nullptr,
			                                stateB, 1, modeInB, modeOutB, mixB);
			seriesHasContent = true;
		}

		// Parallel path: C - sum with SUM BUS routing
		if (hasC && seriesHasContent)
		{
			auto bufC = processLoaderPath (nullptr, stateC, 2, modeInC, modeOutC, mixC);
			result.setSize (2, workingLen);
			result.clear();
			const int sBus = hasB ? sumBusB : sumBusA;  // series path bus = last active loader in the A->B chain
			const int pBus = sumBusC;  // parallel path bus
			if (sBus != 0 || pBus != 0)
			{
				auto* outL = result.getWritePointer (0);
				auto* outR = result.getWritePointer (1);
				const auto* sL = seriesPath.getReadPointer (0);
				const auto* sR = seriesPath.getReadPointer (1);
				const auto* cL = bufC.getReadPointer (0);
				const auto* cR = bufC.getReadPointer (1);
				for (int i = 0; i < workingLen; ++i)
				{
					float stL = 0.0f, stR = 0.0f, midB = 0.0f, sideB = 0.0f;
					injectMSBus (sL[i], sR[i], sBus, stL, stR, midB, sideB);
					injectMSBus (cL[i], cR[i], pBus, stL, stR, midB, sideB);
					outL[i] = stL + midB + sideB;
					outR[i] = stR + midB - sideB;
				}
			}
			else
			{
				for (int ch = 0; ch < 2; ++ch)
				{
					juce::FloatVectorOperations::add (result.getWritePointer (ch), seriesPath.getReadPointer (ch), workingLen);
					juce::FloatVectorOperations::add (result.getWritePointer (ch), bufC.getReadPointer (ch), workingLen);
				}
			}
		}
		else if (hasC)
		{
			result = processLoaderPath (nullptr, stateC, 2, modeInC, modeOutC, mixC);
		}
		else
			result = std::move (seriesPath);
	}
	else if (route == 3) // A|B->C
	{
		// Series path: B->C
		juce::AudioBuffer<float> seriesPath;
		bool seriesHasContent = false;

		if (hasB)
		{
			seriesPath = processLoaderPath (nullptr, stateB, 1, modeInB, modeOutB, mixB);
			seriesHasContent = true;
		}
		if (hasC)
		{
			seriesPath = processLoaderPath (seriesHasContent ? &seriesPath : nullptr,
			                                stateC, 2, modeInC, modeOutC, mixC);
			seriesHasContent = true;
		}

		// Parallel path: A - sum with SUM BUS routing
		if (hasA && seriesHasContent)
		{
			auto bufA = processLoaderPath (nullptr, stateA, 0, modeInA, modeOutA, mixA);
			result.setSize (2, workingLen);
			result.clear();
			const int pBus = sumBusA;  // parallel path bus
			const int sBus = hasC ? sumBusC : sumBusB;  // series path bus = last active loader in the B->C chain
			if (pBus != 0 || sBus != 0)
			{
				auto* outL = result.getWritePointer (0);
				auto* outR = result.getWritePointer (1);
				const auto* sL = seriesPath.getReadPointer (0);
				const auto* sR = seriesPath.getReadPointer (1);
				const auto* aL = bufA.getReadPointer (0);
				const auto* aR = bufA.getReadPointer (1);
				for (int i = 0; i < workingLen; ++i)
				{
					float stL = 0.0f, stR = 0.0f, midB = 0.0f, sideB = 0.0f;
					injectMSBus (sL[i], sR[i], sBus, stL, stR, midB, sideB);
					injectMSBus (aL[i], aR[i], pBus, stL, stR, midB, sideB);
					outL[i] = stL + midB + sideB;
					outR[i] = stR + midB - sideB;
				}
			}
			else
			{
				for (int ch = 0; ch < 2; ++ch)
				{
					juce::FloatVectorOperations::add (result.getWritePointer (ch), bufA.getReadPointer (ch), workingLen);
					juce::FloatVectorOperations::add (result.getWritePointer (ch), seriesPath.getReadPointer (ch), workingLen);
				}
			}
		}
		else if (hasA)
		{
			result = processLoaderPath (nullptr, stateA, 0, modeInA, modeOutA, mixA);
		}
		else
			result = std::move (seriesPath);
	}
	else if (route == 4) // (A|B)->C
	{
		const int numParallel = (hasA ? 1 : 0) + (hasB ? 1 : 0);
		bool hasContent = false;

		if (numParallel >= 2)
		{
			auto bufA = processLoaderPath (nullptr, stateA, 0, modeInA, modeOutA, mixA);
			auto bufB = processLoaderPath (nullptr, stateB, 1, modeInB, modeOutB, mixB);

			result.setSize (2, workingLen);
			result.clear();

			if (sumBusA != 0 || sumBusB != 0)
			{
				auto* outL = result.getWritePointer (0);
				auto* outR = result.getWritePointer (1);
				const auto* aL = bufA.getReadPointer (0);
				const auto* aR = bufA.getReadPointer (1);
				const auto* bL = bufB.getReadPointer (0);
				const auto* bR = bufB.getReadPointer (1);
				for (int i = 0; i < workingLen; ++i)
				{
					float stL = 0.0f, stR = 0.0f, midBus = 0.0f, sideBus = 0.0f;
					injectMSBus (aL[i], aR[i], sumBusA, stL, stR, midBus, sideBus);
					injectMSBus (bL[i], bR[i], sumBusB, stL, stR, midBus, sideBus);
					outL[i] = stL + midBus + sideBus;
					outR[i] = stR + midBus - sideBus;
				}
			}
			else
			{
				for (int ch = 0; ch < 2; ++ch)
				{
					juce::FloatVectorOperations::add (result.getWritePointer (ch), bufA.getReadPointer (ch), workingLen);
					juce::FloatVectorOperations::add (result.getWritePointer (ch), bufB.getReadPointer (ch), workingLen);
				}
			}

			hasContent = true;
		}
		else if (hasA)
		{
			result = processLoaderPath (nullptr, stateA, 0, modeInA, modeOutA, mixA);
			hasContent = true;
		}
		else if (hasB)
		{
			result = processLoaderPath (nullptr, stateB, 1, modeInB, modeOutB, mixB);
			hasContent = true;
		}

		if (hasC)
		{
			result = processLoaderPath (hasContent ? &result : nullptr, stateC, 2, modeInC, modeOutC, mixC);
			hasContent = true;
		}
	}
	else if (route == 5) // A->(B|C)
	{
		juce::AudioBuffer<float> basePath;
		bool hasBasePath = false;

		if (hasA)
		{
			basePath = processLoaderPath (nullptr, stateA, 0, modeInA, modeOutA, mixA);
			hasBasePath = true;
		}

		const int numParallel = (hasB ? 1 : 0) + (hasC ? 1 : 0);
		if (numParallel >= 2)
		{
			auto bufB = processLoaderPath (hasBasePath ? &basePath : nullptr, stateB, 1, modeInB, modeOutB, mixB);
			auto bufC = processLoaderPath (hasBasePath ? &basePath : nullptr, stateC, 2, modeInC, modeOutC, mixC);

			result.setSize (2, workingLen);
			result.clear();

			if (sumBusB != 0 || sumBusC != 0)
			{
				auto* outL = result.getWritePointer (0);
				auto* outR = result.getWritePointer (1);
				const auto* bL = bufB.getReadPointer (0);
				const auto* bR = bufB.getReadPointer (1);
				const auto* cL = bufC.getReadPointer (0);
				const auto* cR = bufC.getReadPointer (1);
				for (int i = 0; i < workingLen; ++i)
				{
					float stL = 0.0f, stR = 0.0f, midBus = 0.0f, sideBus = 0.0f;
					injectMSBus (bL[i], bR[i], sumBusB, stL, stR, midBus, sideBus);
					injectMSBus (cL[i], cR[i], sumBusC, stL, stR, midBus, sideBus);
					outL[i] = stL + midBus + sideBus;
					outR[i] = stR + midBus - sideBus;
				}
			}
			else
			{
				for (int ch = 0; ch < 2; ++ch)
				{
					juce::FloatVectorOperations::add (result.getWritePointer (ch), bufB.getReadPointer (ch), workingLen);
					juce::FloatVectorOperations::add (result.getWritePointer (ch), bufC.getReadPointer (ch), workingLen);
				}
			}

		}
		else if (hasB)
		{
			result = processLoaderPath (hasBasePath ? &basePath : nullptr, stateB, 1, modeInB, modeOutB, mixB);
		}
		else if (hasC)
		{
			result = processLoaderPath (hasBasePath ? &basePath : nullptr, stateC, 2, modeInC, modeOutC, mixC);
		}
		else if (hasBasePath)
		{
			result = std::move (basePath);
		}
	}
	else // route == 0: A->B->C full series
	{
		bool hasContent = false;

		if (hasA)
		{
			result = processLoaderPath (nullptr, stateA, 0, modeInA, modeOutA, mixA);
			hasContent = true;
		}
		if (hasB)
		{
			result = processLoaderPath (hasContent ? &result : nullptr, stateB, 1, modeInB, modeOutB, mixB);
			hasContent = true;
		}
		if (hasC)
		{
			result = processLoaderPath (hasContent ? &result : nullptr, stateC, 2, modeInC, modeOutC, mixC);
		}
	}

	const int numSamples = result.getNumSamples();

	juce::AudioBuffer<float> globalDryIrBuffer (2, numSamples);
	globalDryIrBuffer.clear();
	if (numSamples > 0)
	{
		globalDryIrBuffer.setSample (0, 0, 1.0f);
		if (globalDryIrBuffer.getNumChannels() >= 2)
			globalDryIrBuffer.setSample (1, 0, 1.0f);
	}

	// Log peak levels per channel after combining
#if CABTR_DSP_DEBUG_LOG
	{
		float peakL = result.getMagnitude (0, 0, numSamples);
		float peakR = result.getNumChannels() >= 2 ? result.getMagnitude (1, 0, numSamples) : 0.0f;
		LOG_IR_EVENT ("POST-COMBINE: peakL=" + juce::String (peakL, 6) + " peakR=" + juce::String (peakR, 6)
		             + " numSamples=" + juce::String (numSamples));
		// Check side channel (L-R difference)
		if (result.getNumChannels() >= 2)
		{
			float maxSideDiff = 0.0f;
			const float* dL = result.getReadPointer (0);
			const float* dR = result.getReadPointer (1);
			for (int i = 0; i < numSamples; ++i)
				maxSideDiff = std::max (maxSideDiff, std::abs (dL[i] - dR[i]));
			LOG_IR_EVENT ("POST-COMBINE: maxSideDiff(L-R)=" + juce::String (maxSideDiff, 6));
		}
	}
#endif

	// Apply global processing

	// Input gain
	if (std::abs (inputGainDb) > 0.01f)
	{
		const float inputGain = gainFaderDecibelsToGain (inputGainDb);
		result.applyGain (inputGain);
		globalDryIrBuffer.applyGain (inputGain);
	}

	// MATCH tilt EQ (offline)
	{
		const int matchProfile = static_cast<int> (pMatch->load());
		LOG_IR_EVENT ("MATCH: matchProfile=" + juce::String (matchProfile));
		if (matchProfile != 0)
		{
			const float targetSlope = kTargetSlopes[juce::jlimit (0, kNumTargetSlopes - 1, matchProfile)];

			// Compute combined IR slope (same logic as processBlock)
			float combinedSlope = 0.0f;
			if (route == 0)
			{
				if (hasA) combinedSlope += stateA.irSlopeDbPerOct.load();
				if (hasB) combinedSlope += stateB.irSlopeDbPerOct.load();
				if (hasC) combinedSlope += stateC.irSlopeDbPerOct.load();
			}
			else if (route == 1)
			{
				float totalWeight = 0.0f;
				if (hasA) { combinedSlope += stateA.irSlopeDbPerOct.load() * mixA; totalWeight += mixA; }
				if (hasB) { combinedSlope += stateB.irSlopeDbPerOct.load() * mixB; totalWeight += mixB; }
				if (hasC) { combinedSlope += stateC.irSlopeDbPerOct.load() * mixC; totalWeight += mixC; }
				if (totalWeight > 0.0f) combinedSlope /= totalWeight;
			}
			else if (route == 2)
			{
				float seriesSlope = 0.0f;
				if (hasA) seriesSlope += stateA.irSlopeDbPerOct.load();
				if (hasB) seriesSlope += stateB.irSlopeDbPerOct.load();
				if (hasC)
					combinedSlope = (seriesSlope + stateC.irSlopeDbPerOct.load()) * 0.5f;
				else
					combinedSlope = seriesSlope;
			}
			else if (route == 3)
			{
				float seriesSlope = 0.0f;
				if (hasB) seriesSlope += stateB.irSlopeDbPerOct.load();
				if (hasC) seriesSlope += stateC.irSlopeDbPerOct.load();
				if (hasA)
					combinedSlope = (stateA.irSlopeDbPerOct.load() + seriesSlope) * 0.5f;
				else
					combinedSlope = seriesSlope;
			}
			else if (route == 4)
			{
				float parallelSlope = 0.0f;
				int parallelCount = 0;
				if (hasA) { parallelSlope += stateA.irSlopeDbPerOct.load(); ++parallelCount; }
				if (hasB) { parallelSlope += stateB.irSlopeDbPerOct.load(); ++parallelCount; }
				if (parallelCount > 1)
					parallelSlope /= static_cast<float> (parallelCount);

				if (hasC)
					combinedSlope = parallelSlope + stateC.irSlopeDbPerOct.load();
				else
					combinedSlope = parallelSlope;
			}
			else if (route == 5)
			{
				float parallelSlope = 0.0f;
				int parallelCount = 0;
				if (hasB) { parallelSlope += stateB.irSlopeDbPerOct.load(); ++parallelCount; }
				if (hasC) { parallelSlope += stateC.irSlopeDbPerOct.load(); ++parallelCount; }
				if (parallelCount > 1)
					parallelSlope /= static_cast<float> (parallelCount);

				if (hasA)
					combinedSlope = stateA.irSlopeDbPerOct.load() + parallelSlope;
				else
					combinedSlope = parallelSlope;
			}

			const float compensatingSlope = targetSlope - combinedSlope;

			LOG_IR_EVENT ("MATCH: target=" + juce::String (targetSlope, 3)
			             + " combined=" + juce::String (combinedSlope, 3)
			             + " compensating=" + juce::String (compensatingSlope, 3)
			             + " slopeA=" + juce::String (stateA.irSlopeDbPerOct.load(), 3)
			             + " slopeB=" + juce::String (stateB.irSlopeDbPerOct.load(), 3)
			             + " slopeC=" + juce::String (stateC.irSlopeDbPerOct.load(), 3)
			             + " workingSR=" + juce::String (workingSR)
			             + " numSamples=" + juce::String (numSamples));

			if (std::abs (compensatingSlope) >= 0.1f)
			{
				float b0, b1, a1;
				computeTiltShelfCoeffs (workingSR, compensatingSlope, b0, b1, a1);

				LOG_IR_EVENT ("MATCH: APPLYING tilt - b0=" + juce::String (b0, 6)
				             + " b1=" + juce::String (b1, 6)
				             + " a1=" + juce::String (a1, 6));

				for (int ch = 0; ch < result.getNumChannels(); ++ch)
				{
					auto* data = result.getWritePointer (ch);
					float s = 0.0f;
					for (int i = 0; i < numSamples; ++i)
					{
						const float x = data[i];
						const float y = b0 * x + s;
						s = b1 * x - a1 * y;
						data[i] = y;
					}
				}
			}
			else
			{
				LOG_IR_EVENT ("MATCH: SKIPPED - |compensatingSlope| < 0.1");
			}
		}
	}

	// NORM: apply peak normalization to wet signal
	{
		const int normIdx = static_cast<int> (pTrim->load());
		if (normIdx > 0)
		{
			const float target = kNormTargets[juce::jlimit (0, kNumNormTargets - 1, normIdx)];

			float peak = 0.0f;
			for (int ch = 0; ch < result.getNumChannels(); ++ch)
				peak = std::max (peak, result.getMagnitude (ch, 0, numSamples));

			if (peak > 0.001f)
			{
				const float gain = juce::jlimit (0.01f, kMaxNormBoost, target / peak);
				result.applyGain (gain);
			}
		}
	}

	// Invert Polarity / Stereo (WET mode: after wet-chain processing, before global mix)
	{
		const int nc = result.getNumChannels();
		if (invPol == 1)
			for (int ch = 0; ch < nc; ++ch)
				juce::FloatVectorOperations::multiply (result.getWritePointer (ch), -1.0f, numSamples);
		if (invStr == 1 && nc >= 2)
		{
			float* sL = result.getWritePointer (0);
			float* sR = result.getWritePointer (1);
			for (int n = 0; n < numSamples; ++n)
				std::swap (sL[n], sR[n]);
		}
	}

	// Wet-only DC blocking filter (same static stage as realtime, fresh offline state)
	{
		const float R = cachedDcBlockR_;
		for (int ch = 0; ch < result.getNumChannels(); ++ch)
		{
			auto* data = result.getWritePointer (ch);
			float xPrev = 0.0f;
			float yPrev = 0.0f;
			for (int i = 0; i < numSamples; ++i)
			{
				const float x = data[i];
				const float y = x - xPrev + R * yPrev;
				xPrev = x;
				yPrev = y;
				data[i] = y;
			}
		}
	}

	// Global MIX: mirror the static part of realtime INSERT / SEND behaviour.
	{
		float wetGain = globalMix;
		float dryGain = 1.0f - globalMix;
		if (mixMode == 1)
		{
			dryGain = dryLevel;
			wetGain = wetLevel;
		}

		result.applyGain (wetGain);
		if (std::abs (dryGain) > 1.0e-6f)
		{
			for (int ch = 0; ch < result.getNumChannels(); ++ch)
				result.addFrom (ch, 0, globalDryIrBuffer, ch, 0, numSamples, dryGain);
		}
	}

	// Global output gain
	if (std::abs (outputGainDb) > 0.01f)
		result.applyGain (gainFaderDecibelsToGain (outputGainDb));

	// Invert Polarity / Stereo (GLOBAL mode: after output gain)
	{
		const int nc = result.getNumChannels();
		if (invPol == 2)
			for (int ch = 0; ch < nc; ++ch)
				juce::FloatVectorOperations::multiply (result.getWritePointer (ch), -1.0f, numSamples);
		if (invStr == 2 && nc >= 2)
		{
			float* sL = result.getWritePointer (0);
			float* sR = result.getWritePointer (1);
			for (int n = 0; n < numSamples; ++n)
				std::swap (sL[n], sR[n]);
		}
	}

	// Final peaks after all global processing
#if CABTR_DSP_DEBUG_LOG
	{
		float peakL = result.getMagnitude (0, 0, numSamples);
		float peakR = result.getNumChannels() >= 2 ? result.getMagnitude (1, 0, numSamples) : 0.0f;
		LOG_IR_EVENT ("POST-GLOBAL: peakL=" + juce::String (peakL, 6) + " peakR=" + juce::String (peakR, 6));
		if (result.getNumChannels() >= 2)
		{
			float maxSideDiff = 0.0f;
			const float* dL = result.getReadPointer (0);
			const float* dR = result.getReadPointer (1);
			for (int i = 0; i < numSamples; ++i)
				maxSideDiff = std::max (maxSideDiff, std::abs (dL[i] - dR[i]));
			LOG_IR_EVENT ("POST-GLOBAL: maxSideDiff(L-R)=" + juce::String (maxSideDiff, 6));
		}
	}
#endif

	LOG_IR_EVENT ("====== EXPORT END ======");

	// Resample if target SR differs
	if (std::abs (targetSampleRate - workingSR) > 1.0)
	{
		const double ratio = workingSR / targetSampleRate;
		const int newLength = static_cast<int> (result.getNumSamples() / ratio);
		if (newLength > 0)
		{
			juce::AudioBuffer<float> resampled (result.getNumChannels(), newLength);
			for (int ch = 0; ch < result.getNumChannels(); ++ch)
			{
				juce::LagrangeInterpolator interpolator;
				interpolator.process (ratio,
				                      result.getReadPointer (ch),
				                      resampled.getWritePointer (ch),
				                      newLength,
				                      result.getNumSamples(), 0);
			}
			result = std::move (resampled);
		}
	}

	// Apply max length cap at target rate
	{
		const int maxSamples = static_cast<int> (maxLengthSec * targetSampleRate);
		if (result.getNumSamples() > maxSamples)
			result.setSize (result.getNumChannels(), maxSamples, true);
	}

	// Trim trailing silence (-80dBFS threshold)
	if (trimSilence)
	{
		constexpr float silenceThreshold = 0.0001f;
		int lastNonSilent = 0;

		for (int ch = 0; ch < result.getNumChannels(); ++ch)
		{
			const float* data = result.getReadPointer (ch);
			for (int i = result.getNumSamples() - 1; i >= 0; --i)
			{
				if (std::abs (data[i]) > silenceThreshold)
				{
					lastNonSilent = juce::jmax (lastNonSilent, i);
					break;
				}
			}
		}

		const int trimLength = juce::jmax (64, lastNonSilent + 1);
		if (trimLength < result.getNumSamples())
			result.setSize (result.getNumChannels(), trimLength, true);
	}

	// Minimum Phase Transform (cepstrum / homomorphic method)
	// Matches the SciPy scipy.signal.minimum_phase reference implementation
	// (Oppenheim+Schafer "Discrete-Time Signal Processing" 3rd ed, Sec.13.7).
	// Key improvements over previous version:
	//   - Double-precision FFT to avoid float32 error accumulation across 4 passes
	//   - FFT size follows SciPy formula: 2^ceil(log2(200*(N-1)))
	//     to minimise cepstral aliasing (epsilon < 0.01)
	//   - Relative magnitude floor matching SciPy: mag += 1e-7 * min(mag>0)
	if (minimumPhase)
	{
		const int irLen = result.getNumSamples();
		const int numCh = result.getNumChannels();

		// FFT size: SciPy-equivalent formula (Oppenheim+Schafer convention)
		// n_fft = 2^ceil(log2(200 * (irLen - 1)))   - ensures epsilon < 0.01
		int mptOrder = 1;
		{
			const double minSize = 200.0 * static_cast<double> (juce::jmax (1, irLen - 1));
			while ((1 << mptOrder) < minSize)
				++mptOrder;
			mptOrder = juce::jmin (mptOrder, 22);  // cap at 2^22 = 4M points
		}
		const int fftSize = 1 << mptOrder;
		const int halfSize = fftSize / 2;

		// Single double-precision complex buffer (in-place radix-2 FFT)
		using CpxD = std::complex<double>;
		std::vector<CpxD> buf (static_cast<size_t> (fftSize));

		for (int ch = 0; ch < numCh; ++ch)
		{
			const float* src = result.getReadPointer (ch);

			// 1) Copy IR into buffer, zero-pad
			for (int i = 0; i < irLen; ++i)
				buf[static_cast<size_t> (i)] = { static_cast<double> (src[i]), 0.0 };
			for (int i = irLen; i < fftSize; ++i)
				buf[static_cast<size_t> (i)] = { 0.0, 0.0 };

			// 2) Forward FFT
			fftRadix2 (buf.data(), fftSize, false);

			// 3) Log-magnitude with relative floor (matches SciPy)
			//    SciPy: h_temp += 1e-7 * h_temp[h_temp > 0].min()
			{
				double minPosMag = std::numeric_limits<double>::max();
				for (int k = 0; k < fftSize; ++k)
				{
					const double mag = std::abs (buf[static_cast<size_t> (k)]);
					if (mag > 0.0)
						minPosMag = std::min (minPosMag, mag);
				}
				const double floorAdd = 1e-7 * minPosMag;
				for (int k = 0; k < fftSize; ++k)
				{
					const double mag = std::abs (buf[static_cast<size_t> (k)]) + floorAdd;
					buf[static_cast<size_t> (k)] = { std::log (mag), 0.0 };
				}
			}

			// 4) IFFT -> real cepstrum
			fftRadix2 (buf.data(), fftSize, true);

			// 5) Causal window - Oppenheim+Schafer eq 13.42b
			//    c[0] x 1, c[1..N/2-1] x 2, c[N/2] x 1, c[N/2+1..N-1] = 0
			//    Discard tiny imaginary residue from floating-point IFFT
			buf[0] = { buf[0].real(), 0.0 };
			for (int i = 1; i < halfSize; ++i)
				buf[static_cast<size_t> (i)] = { 2.0 * buf[static_cast<size_t> (i)].real(), 0.0 };
			buf[static_cast<size_t> (halfSize)] = { buf[static_cast<size_t> (halfSize)].real(), 0.0 };
			for (int i = halfSize + 1; i < fftSize; ++i)
				buf[static_cast<size_t> (i)] = { 0.0, 0.0 };

			// 6) Forward FFT of windowed cepstrum -> complex log-spectrum
			fftRadix2 (buf.data(), fftSize, false);

			// 7) Exponentiate: exp(logMag + j*minPhase) -> complex spectrum
			for (int k = 0; k < fftSize; ++k)
				buf[static_cast<size_t> (k)] = std::exp (buf[static_cast<size_t> (k)]);

			// 8) IFFT -> minimum-phase IR
			fftRadix2 (buf.data(), fftSize, true);

			// 9) Copy real part back (only irLen samples)
			float* dst = result.getWritePointer (ch);
			for (int i = 0; i < irLen; ++i)
				dst[i] = static_cast<float> (buf[static_cast<size_t> (i)].real());
		}
	}

	// Normalize to 0dB peak if requested
	if (normalizeOutput)
	{
		float peak = 0.0f;
		for (int ch = 0; ch < result.getNumChannels(); ++ch)
			peak = juce::jmax (peak, result.getMagnitude (ch, 0, result.getNumSamples()));
		if (peak > 0.0001f)
			result.applyGain (1.0f / peak);
	}

	// Write to file
	std::unique_ptr<juce::AudioFormat> format;
	int bitsPerSample = 24;

	switch (formatType)
	{
		case 0: format = std::make_unique<juce::WavAudioFormat>();  bitsPerSample = 16; break;
		case 1: format = std::make_unique<juce::WavAudioFormat>();  bitsPerSample = 24; break;
		case 2: format = std::make_unique<juce::WavAudioFormat>();  bitsPerSample = 32; break;
		case 3: format = std::make_unique<juce::AiffAudioFormat>(); bitsPerSample = 24; break;
		case 4: format = std::make_unique<juce::FlacAudioFormat>(); bitsPerSample = 24; break;
		default: return false;
	}

	outputFile.deleteFile();
	auto outputStream = outputFile.createOutputStream();
	if (outputStream == nullptr)
		return false;

	auto* rawStream = outputStream.release();
	std::unique_ptr<juce::AudioFormatWriter> writer (
		format->createWriterFor (rawStream,
		                         targetSampleRate,
		                         static_cast<unsigned int> (result.getNumChannels()),
		                         bitsPerSample,
		                         {},
		                         0));

	if (writer == nullptr)
	{
		delete rawStream;
		return false;
	}

	writer->writeFromAudioSampleBuffer (result, 0, result.getNumSamples());
	return true;
}

//==============================================================================
// Offline loader finishing pass for export.
// The caller already handled:
// - root/input path creation
// - per-loader dry capture
// - MODE IN
// - loader input gain
// - convolution
//
// This function applies the static post-convolution loader stages:
// OUT -> TILT -> HP/LP -> DIST -> PAN -> DELAY -> ANGLE -> MODE OUT -> MIX
// CHAOS, VAR and EXP are intentionally skipped for export:
// - CHAOS/VAR are realtime time-varying modulation stages
// - EXP is signal-dynamic and cannot be represented faithfully in a static IR
//==============================================================================
void CABTRAudioProcessor::offlineProcessLoaderEffects (juce::AudioBuffer<float>& buffer,
                                                        const juce::AudioBuffer<float>& dryBuffer,
                                                        int loaderIndex, double sampleRate,
                                                        int modeOut, int filterPos, float loaderMix)
{
	const int numSamples = buffer.getNumSamples();
	const int numChannels = buffer.getNumChannels();
	const float maxFreq = static_cast<float> (sampleRate) * 0.49f;

	const bool needsMixBlend = loaderMix < 0.999f;

	static const char* loaderNames[] = { "A", "B", "C" };
#if CABTR_DSP_DEBUG_LOG
	const char* ln = loaderNames[juce::jlimit (0, 2, loaderIndex)];
	LOG_IR_EVENT (juce::String ("LOADER ") + ln + ": modeOut=" + juce::String (modeOut)
	             + " mix=" + juce::String (loaderMix, 4)
	             + " numSamples=" + juce::String (numSamples)
	             + " numCh=" + juce::String (numChannels));
#else
	juce::ignoreUnused (loaderNames);
#endif

	auto pick = [&] (std::atomic<float>* a, std::atomic<float>* b, std::atomic<float>* c) -> std::atomic<float>* { return loaderIndex == 0 ? a : (loaderIndex == 1 ? b : c); };

	const float hpFreq = pick (pHpFreqA, pHpFreqB, pHpFreqC)->load();
	const float lpFreq = pick (pLpFreqA, pLpFreqB, pLpFreqC)->load();
	const bool  hpOn   = pick (pHpOnA,   pHpOnB,   pHpOnC)->load() > 0.5f;
	const bool  lpOn   = pick (pLpOnA,   pLpOnB,   pLpOnC)->load() > 0.5f;
	const int   hpSlope = juce::jlimit (kFilterSlopeMin, kFilterSlopeMax,
	                                    (int) pick (pHpSlopeA, pHpSlopeB, pHpSlopeC)->load());
	const int   lpSlope = juce::jlimit (kFilterSlopeMin, kFilterSlopeMax,
	                                    (int) pick (pLpSlopeA, pLpSlopeB, pLpSlopeC)->load());
	const float delayMs = pick (pDelayA, pDelayB, pDelayC)->load();
	const float pan = pick (pPanA, pPanB, pPanC)->load();
	const float fred = pick (pFredA, pFredB, pFredC)->load();
	const float pos = pick (pPosA, pPosB, pPosC)->load();
	const float outDb = pick (pOutA, pOutB, pOutC)->load();
	const float tiltDb = pick (pTiltA, pTiltB, pTiltC)->load();
	const bool filterPre = (filterPos == 1 || filterPos == 2);
	const bool tiltPre = (filterPos == 1 || filterPos == 3);

#if CABTR_DSP_DEBUG_LOG
	LOG_IR_EVENT (juce::String ("LOADER ") + ln + ": hpOn=" + juce::String ((int) hpOn)
	             + " hpFreq=" + juce::String (hpFreq, 1) + " hpSlope=" + juce::String (hpSlope)
	             + " lpOn=" + juce::String ((int) lpOn)
	             + " lpFreq=" + juce::String (lpFreq, 1) + " lpSlope=" + juce::String (lpSlope)
	             + " delay=" + juce::String (delayMs, 2) + " pan=" + juce::String (pan, 3)
	             + " fred=" + juce::String (fred, 3) + " pos=" + juce::String (pos, 3)
	             + " outDb=" + juce::String (outDb, 2)
	             + " tiltDb=" + juce::String (tiltDb, 2)
	             + " filterPos=" + juce::String (filterPos));
#endif

	juce::dsp::ProcessSpec spec;
	spec.sampleRate = sampleRate;
	spec.maximumBlockSize = static_cast<juce::uint32> (numSamples);
	spec.numChannels = static_cast<juce::uint32> (numChannels);

	// OUT gain
	if (std::abs (outDb) > 0.01f)
		buffer.applyGain (gainFaderDecibelsToGain (outDb));

	// Per-loader TILT EQ (post path; pre path is applied before convolution)
	if (! tiltPre && std::abs (tiltDb) > 0.05f)
	{
		float b0, b1, a1;
		computeTiltShelfCoeffs (sampleRate, tiltDb, b0, b1, a1);

		for (int ch = 0; ch < numChannels; ++ch)
		{
			auto* data = buffer.getWritePointer (ch);
			float s = 0.0f;
			for (int i = 0; i < numSamples; ++i)
			{
				const float x = data[i];
				const float y = b0 * x + s;
				s = b1 * x - a1 * y;
				data[i] = y;
			}
		}
	}

	// HP filter (post path; pre path is applied before convolution)
	if (! filterPre && hpOn && hpFreq >= 21.0f)
	{
		const float clampedHp = juce::jlimit (20.0f, maxFreq, hpFreq);
		juce::dsp::ProcessorDuplicator<juce::dsp::IIR::Filter<float>,
		                                juce::dsp::IIR::Coefficients<float>> hp;
		hp.prepare (spec);

		if (hpSlope == 0) // 6 dB/oct - first-order
			*hp.state = *juce::dsp::IIR::Coefficients<float>::makeFirstOrderHighPass (sampleRate, clampedHp);
		else if (hpSlope == 1) // 12 dB/oct - Butterworth biquad
			*hp.state = *juce::dsp::IIR::Coefficients<float>::makeHighPass (sampleRate, clampedHp, kSqrt2Over2);
		else // 24 dB/oct - first stage
			*hp.state = *juce::dsp::IIR::Coefficients<float>::makeHighPass (sampleRate, clampedHp, kBW4_Q1);

		juce::dsp::AudioBlock<float> block (buffer);
		juce::dsp::ProcessContextReplacing<float> ctx (block);
		hp.process (ctx);

		if (hpSlope == 2) // 24 dB/oct - second stage
		{
			juce::dsp::ProcessorDuplicator<juce::dsp::IIR::Filter<float>,
			                                juce::dsp::IIR::Coefficients<float>> hp2;
			hp2.prepare (spec);
			*hp2.state = *juce::dsp::IIR::Coefficients<float>::makeHighPass (sampleRate, clampedHp, kBW4_Q2);
			hp2.process (ctx);
		}
	}

	// LP filter (post path; pre path is applied before convolution)
	if (! filterPre && lpOn && lpFreq <= 19900.0f)
	{
		const float clampedLp = juce::jlimit (20.0f, maxFreq, lpFreq);
		juce::dsp::ProcessorDuplicator<juce::dsp::IIR::Filter<float>,
		                                juce::dsp::IIR::Coefficients<float>> lp;
		lp.prepare (spec);

		if (lpSlope == 0) // 6 dB/oct - first-order
			*lp.state = *juce::dsp::IIR::Coefficients<float>::makeFirstOrderLowPass (sampleRate, clampedLp);
		else if (lpSlope == 1) // 12 dB/oct - Butterworth biquad
			*lp.state = *juce::dsp::IIR::Coefficients<float>::makeLowPass (sampleRate, clampedLp, kSqrt2Over2);
		else // 24 dB/oct - first stage
			*lp.state = *juce::dsp::IIR::Coefficients<float>::makeLowPass (sampleRate, clampedLp, kBW4_Q1);

		juce::dsp::AudioBlock<float> block (buffer);
		juce::dsp::ProcessContextReplacing<float> ctx (block);
		lp.process (ctx);

		if (lpSlope == 2) // 24 dB/oct - second stage
		{
			juce::dsp::ProcessorDuplicator<juce::dsp::IIR::Filter<float>,
			                                juce::dsp::IIR::Coefficients<float>> lp2;
			lp2.prepare (spec);
			*lp2.state = *juce::dsp::IIR::Coefficients<float>::makeLowPass (sampleRate, clampedLp, kBW4_Q2);
			lp2.process (ctx);
		}
	}

	// DIST (exponential LPF + gain attenuation)
	if (pos > 0.01f)
	{
		const float cutoff = 12000.0f * std::exp (-pos * kDistDecay);
		juce::dsp::ProcessorDuplicator<juce::dsp::IIR::Filter<float>,
		                                juce::dsp::IIR::Coefficients<float>> posF;
		posF.prepare (spec);
		*posF.state = *juce::dsp::IIR::Coefficients<float>::makeLowPass (
			sampleRate, juce::jlimit (200.0f, maxFreq, cutoff), kSqrt2Over2);
		juce::dsp::AudioBlock<float> block (buffer);
		juce::dsp::ProcessContextReplacing<float> ctx (block);
		posF.process (ctx);

		buffer.applyGain (1.0f - pos * 0.5f);
	}

	// PAN (constant-power)
	if (numChannels >= 2 && std::abs (pan - 0.5f) > 0.001f)
	{
		const float panAngle = pan * 1.5707963f;
		buffer.applyGain (0, 0, numSamples, std::cos (panAngle));
		buffer.applyGain (1, 0, numSamples, std::sin (panAngle));
	}

	// DELAY (offline: using DelayLine with linear interpolation to match realtime)
	if (delayMs > 0.1f)
	{
		const float delaySamples = delayMs * 0.001f * static_cast<float> (sampleRate);
		const int maxDelay = static_cast<int> (delaySamples) + 2;
		if (maxDelay > 0 && maxDelay < numSamples)
		{
			juce::dsp::DelayLine<float, juce::dsp::DelayLineInterpolationTypes::Linear> offlineDelay (maxDelay);
			offlineDelay.prepare (spec);
			offlineDelay.setDelay (delaySamples);

			juce::AudioBuffer<float> temp (numChannels, numSamples);
			temp.clear();

			for (int i = 0; i < numSamples; ++i)
				for (int ch = 0; ch < numChannels; ++ch)
				{
					offlineDelay.pushSample (ch, buffer.getSample (ch, i));
					temp.setSample (ch, i, offlineDelay.popSample (ch));
				}

			for (int ch = 0; ch < numChannels; ++ch)
				buffer.copyFrom (ch, 0, temp, ch, 0, numSamples);
		}
	}

	// ANGLE (7-sample comb filter)
	if (fred > 0.001f)
	{
		constexpr int kFredDelay = IRLoaderState::kFredDelaySamples;
		float fredBuf[2][kFredDelay] = {};
		int fredIdx = 0;
		const float compensate = 1.0f / (1.0f + fred);
		const int chCount = juce::jmin (numChannels, 2);

		for (int i = 0; i < numSamples; ++i)
		{
			for (int ch = 0; ch < chCount; ++ch)
			{
				auto* data = buffer.getWritePointer (ch);
				const float direct = data[i];
				const float offAxis = fredBuf[ch][fredIdx];
				fredBuf[ch][fredIdx] = direct;
				data[i] = (direct + fred * offAxis) * compensate;
			}
			fredIdx = (fredIdx + 1) % kFredDelay;
		}
	}

	// CHAOS intentionally skipped

	// (OUT gain and TILT already applied before filters - see above)

	// Apply MODE OUT after processing
	applyMidSideOutputMode (buffer, modeOut, numSamples);

	// Per-loader MIX: blend dry (pre-effects) with wet (post-effects)
	if (needsMixBlend)
	{
		const float wet = loaderMix;
		const float dry = 1.0f - loaderMix;
		for (int ch = 0; ch < numChannels; ++ch)
		{
			auto* wetData = buffer.getWritePointer (ch);
			const auto* dryData = dryBuffer.getReadPointer (ch);
			juce::FloatVectorOperations::multiply (wetData, wet, numSamples);
			juce::FloatVectorOperations::addWithMultiply (wetData, dryData, dry, numSamples);
		}
	}
}

//==============================================================================
// PROCESSING CHAIN OPTIMIZADA:
// IN -> EXP PRE -> CHAOS D -> CONVOLUTION -> EXP POST -> OUT -> TILT/HP/LP
// -> DIST -> PAN -> DELAY -> ANGLE
//
// CPU OPTIMIZATIONS:
// - Zero allocations en audio thread
// - Cached filter coefficients (solo recalcula si cambia parameter)
// - ANGLE: minimal 7-sample delay (comb filter for off-axis simulation)
// - SIMD operations donde sea posible
// - Distance as exponential LPF + gain attenuation
// - Timer debounce: no reload during slider drag
//==============================================================================
void CABTRAudioProcessor::applyLoaderTonePosition (IRLoaderState& state, juce::AudioBuffer<float>& buffer,
                                                   bool applyFiltersStage, bool applyTiltStage, bool filtersFirst,
                                                   float hpFreq, float lpFreq, bool hpOn, bool lpOn,
                                                   int hpSlope, int lpSlope, float tiltDb,
                                                   bool chaosFilterEnabled, float chaosAmtFilter, float chaosSpdFilter,
                                                   float chaosParamSmoothCoeff) noexcept
{
	const int numSamples = buffer.getNumSamples();
	const int numChannels = buffer.getNumChannels();

	auto applyTilt = [&]()
	{
	if (applyTiltStage && std::abs (tiltDb) > 0.05f)
	{
		// Recompute coefficients when tilt amount changes
		if (std::abs (tiltDb - state.lastTiltDb) > 0.02f)
		{
			state.lastTiltDb = tiltDb;
			computeTiltShelfCoeffs (currentSampleRate, tiltDb,
			                       state.tiltTargetB0, state.tiltTargetB1, state.tiltTargetA1);
		}

		const float smoothCoeff = cachedTiltSmoothCoeff_;
		float b0 = state.tiltB0, b1 = state.tiltB1, a1 = state.tiltA1;
		const float tb0 = state.tiltTargetB0, tb1 = state.tiltTargetB1, ta1 = state.tiltTargetA1;

		auto* leftData = buffer.getWritePointer (0);
		auto* rightData = numChannels > 1 ? buffer.getWritePointer (1) : nullptr;
		float leftState = state.tiltState[0];
		float rightState = state.tiltState[1];

		for (int i = 0; i < numSamples; ++i)
		{
			b0 += (tb0 - b0) * smoothCoeff;
			b1 += (tb1 - b1) * smoothCoeff;
			a1 += (ta1 - a1) * smoothCoeff;

			const float leftX = leftData[i];
			const float leftY = b0 * leftX + leftState;
			leftState = b1 * leftX - a1 * leftY;
			leftData[i] = leftY;

			if (rightData != nullptr)
			{
				const float rightX = rightData[i];
				const float rightY = b0 * rightX + rightState;
				rightState = b1 * rightX - a1 * rightY;
				rightData[i] = rightY;
			}
		}

		state.tiltState[0] = leftState;
		state.tiltState[1] = rightState;
		state.tiltB0 = b0; state.tiltB1 = b1; state.tiltA1 = a1;
	}
	else if (applyTiltStage && std::abs (state.lastTiltDb) > 0.05f)
	{
		// Reset tilt state when returning to 0
		state.lastTiltDb = 0.0f;
		state.tiltB0 = 1.0f; state.tiltB1 = 0.0f; state.tiltA1 = 0.0f;
		state.tiltTargetB0 = 1.0f; state.tiltTargetB1 = 0.0f; state.tiltTargetA1 = 0.0f;
		state.tiltState[0] = state.tiltState[1] = 0.0f;
	}
	};

	auto applyFilters = [&]()
	{
	if (! applyFiltersStage)
		return;

	// HP + LP FILTERS with slope selection (6/12/24 dB/oct)
	// Per-sample EMA frequency smoothing + coefficient update every 8 samples
	const float chaosFilterTargetAmt = chaosFilterEnabled
		? juce::jlimit (kChaosAmtMin, kChaosAmtMax, chaosAmtFilter)
		: 0.0f;
	const bool chaosFilterActive = chaosFilterTargetAmt > 0.01f
		|| (state.chaosFilterParamSmoothReady && state.chaosFilterAmtSmoothed > 0.01f);
	if (! chaosFilterActive)
	{
		state.chaosFilterAmtSmoothed = 0.0f;
		state.chaosFilterSpdSmoothed = juce::jlimit (kChaosSpdMin, kChaosSpdMax, chaosSpdFilter);
		state.chaosFilterParamSmoothReady = false;
	}
	{
		constexpr float kSmoothCoeff = 0.9955f; // ~5ms @ 44.1kHz
		const float oneMinusCoeff = 1.0f - kSmoothCoeff;
		const float maxFreq = static_cast<float> (currentSampleRate) * 0.49f;
		const bool processHp = hpOn || chaosFilterActive;
		const bool processLp = lpOn || chaosFilterActive;

		if (! processHp && ! processLp)
		{
			const float smoothPower = std::pow (kSmoothCoeff, (float) numSamples);
			state.smoothedHpFreq = hpFreq + (state.smoothedHpFreq - hpFreq) * smoothPower;
			state.smoothedLpFreq = lpFreq + (state.smoothedLpFreq - lpFreq) * smoothPower;
			state.filterCoeffCountdown = 0;
		}
		else
		{
			const float sr = (float) currentSampleRate;
			const float hpBase = hpOn ? hpFreq : kFilterFreqMin;
			const float lpBase = lpOn ? lpFreq : kFilterFreqMax;
			if (chaosFilterActive && ! state.chaosFilterParamSmoothReady)
			{
				if (! hpOn)
					state.smoothedHpFreq = kFilterFreqMin;
				if (! lpOn)
					state.smoothedLpFreq = kFilterFreqMax;
				state.filterCoeffCountdown = 0;
			}

			auto updateFilterCoefficients = [&]()
			{
				// HP: recalc if smoothed frequency or slope changed
				const float clampedHp = juce::jlimit (20.0f, maxFreq, state.smoothedHpFreq);
				if (std::abs (clampedHp - state.lastHpFreq) > 0.01f || hpSlope != state.lastHpSlope)
				{
					if (hpSlope == 0) // 6 dB/oct - first-order
					{
						*state.hpFilter.state = juce::dsp::IIR::ArrayCoefficients<float>::makeFirstOrderHighPass (
							currentSampleRate, clampedHp);
					}
					else if (hpSlope == 1) // 12 dB/oct - Butterworth biquad
					{
						*state.hpFilter.state = juce::dsp::IIR::ArrayCoefficients<float>::makeHighPass (
							currentSampleRate, clampedHp, kSqrt2Over2);
					}
					else // 24 dB/oct - cascaded biquad pair
					{
						*state.hpFilter.state = juce::dsp::IIR::ArrayCoefficients<float>::makeHighPass (
							currentSampleRate, clampedHp, kBW4_Q1);
						*state.hpFilter2.state = juce::dsp::IIR::ArrayCoefficients<float>::makeHighPass (
							currentSampleRate, clampedHp, kBW4_Q2);
					}
					state.lastHpFreq = clampedHp;
					state.lastHpSlope = hpSlope;
				}

				// LP: recalc if smoothed frequency or slope changed
				const float clampedLp = juce::jlimit (20.0f, maxFreq, state.smoothedLpFreq);
				if (std::abs (clampedLp - state.lastLpFreq) > 0.01f || lpSlope != state.lastLpSlope)
				{
					if (lpSlope == 0) // 6 dB/oct - first-order
					{
						*state.lpFilter.state = juce::dsp::IIR::ArrayCoefficients<float>::makeFirstOrderLowPass (
							currentSampleRate, clampedLp);
					}
					else if (lpSlope == 1) // 12 dB/oct - Butterworth biquad
					{
						*state.lpFilter.state = juce::dsp::IIR::ArrayCoefficients<float>::makeLowPass (
							currentSampleRate, clampedLp, kSqrt2Over2);
					}
					else // 24 dB/oct - cascaded biquad pair
					{
						*state.lpFilter.state = juce::dsp::IIR::ArrayCoefficients<float>::makeLowPass (
							currentSampleRate, clampedLp, kBW4_Q1);
						*state.lpFilter2.state = juce::dsp::IIR::ArrayCoefficients<float>::makeLowPass (
							currentSampleRate, clampedLp, kBW4_Q2);
					}
					state.lastLpFreq = clampedLp;
					state.lastLpSlope = lpSlope;
				}
			};

			juce::dsp::AudioBlock<float> block (buffer);
			int segmentStart = 0;

			auto processSegment = [&] (int segmentEnd)
			{
				const int segmentLength = segmentEnd - segmentStart;
				if (segmentLength <= 0)
					return;

				auto subBlock = block.getSubBlock ((size_t) segmentStart, (size_t) segmentLength);
				juce::dsp::ProcessContextReplacing<float> context (subBlock);

				// Apply HP filter (1 or 2 stages depending on slope).
				// Also apply when chaos filter is active, even if HP knob is off (full-range sweep).
				if (processHp && (chaosFilterActive || state.smoothedHpFreq >= 21.0f))
				{
					state.hpFilter.process (context);
					if (hpSlope == 2) // 24 dB/oct: second stage
						state.hpFilter2.process (context);
				}

				// Apply LP filter (1 or 2 stages depending on slope).
				// Also apply when chaos filter is active, even if LP knob is off (full-range sweep).
				if (processLp && (chaosFilterActive || state.smoothedLpFreq <= 19900.0f))
				{
					state.lpFilter.process (context);
					if (lpSlope == 2) // 24 dB/oct: second stage
						state.lpFilter2.process (context);
				}

				segmentStart = segmentEnd;
			};

			auto advanceFilterTargets = [&]()
			{
				float hpTarget = hpFreq;
				float lpTarget = lpFreq;

				if (chaosFilterActive)
				{
					const float targetAmt = chaosFilterTargetAmt;
					const float targetSpd = juce::jlimit (kChaosSpdMin, kChaosSpdMax, chaosSpdFilter);
					if (! state.chaosFilterParamSmoothReady)
					{
						state.chaosFilterParamSmoothReady = true;
						if (state.chaosFilterSpdSmoothed <= 0.0f)
							state.chaosFilterSpdSmoothed = targetSpd;
					}

					state.chaosFilterAmtSmoothed += (targetAmt - state.chaosFilterAmtSmoothed) * chaosParamSmoothCoeff;
					const float filterSpdLog = std::log (juce::jmax (kChaosSpdMin, state.chaosFilterSpdSmoothed));
					const float filterTargetSpdLog = std::log (targetSpd);
					state.chaosFilterSpdSmoothed = std::exp (filterSpdLog + (filterTargetSpdLog - filterSpdLog) * chaosParamSmoothCoeff);

					const float amountNorm = state.chaosFilterAmtSmoothed * 0.01f;
					const float chaosFilterMaxOct = amountNorm * 2.0f;  // +/-2 octaves at 100%
					const float shPeriodSamples = sr / juce::jmax (kChaosSpdMin, state.chaosFilterSpdSmoothed);

					advanceChaosEngine (state.chaosFPrev, state.chaosFCurr, state.chaosFNext,
					                    state.chaosFPhase, state.chaosFDriftPhase, state.chaosFDriftFreqHz,
					                    state.chaosFOut[0], state.chaosFRng, shPeriodSamples, amountNorm, sr);

					const float octaveShift = state.chaosFOut[0] * chaosFilterMaxOct;
					const float freqMult = std::exp2 (octaveShift);
					hpTarget = juce::jlimit (kFilterFreqMin, kFilterFreqMax, hpBase * freqMult);
					lpTarget = juce::jlimit (kFilterFreqMin, kFilterFreqMax, lpBase * freqMult);
				}

				state.smoothedHpFreq += (hpTarget - state.smoothedHpFreq) * oneMinusCoeff;
				state.smoothedLpFreq += (lpTarget - state.smoothedLpFreq) * oneMinusCoeff;
			};

			for (int i = 0; i < numSamples; ++i)
			{
				advanceFilterTargets();

				// Countdown is sample-based: samples before this boundary use the previous coefficients.
				if (--state.filterCoeffCountdown <= 0)
				{
					processSegment (i);
					state.filterCoeffCountdown = IRLoaderState::kFilterCoeffUpdateInterval;
					updateFilterCoefficients();
				}
			}

			processSegment (numSamples);
		}
	}
	};

	if (filtersFirst)
	{
		applyFilters();
		applyTilt();
	}
	else
	{
		applyTilt();
		applyFilters();
	}
}

void CABTRAudioProcessor::processLoader (IRLoaderState& state,
                                          juce::AudioBuffer<float>& buffer,
                                          int loaderIndex)
{
	const int numSamples = buffer.getNumSamples();
	const int numChannels = buffer.getNumChannels();

	// EARLY EXIT: No processing if no IR loaded - pass through unchanged
	if (state.currentFilePath.isEmpty())
	{
		return;
	}

	// STEP 0: Check if IR parameters changed -> set flag to reload on message thread
	// CRITICAL: Cannot call loadImpulseResponse here (file I/O illegal in audio thread!)
	// NOTE: IR reload is handled by timerCallback with debouncing (message thread).
	// No parameter change detection needed here on the audio thread.

	// Get runtime parameters (cached pointers - no hash lookup)
	// loaderIndex: 0=A, 1=B, 2=C
	auto pick = [&] (std::atomic<float>* a, std::atomic<float>* b, std::atomic<float>* c) -> std::atomic<float>* { return loaderIndex == 0 ? a : (loaderIndex == 1 ? b : c); };

	const float hpFreq = loadRelaxed (pick (pHpFreqA, pHpFreqB, pHpFreqC));
	const float lpFreq = loadRelaxed (pick (pLpFreqA, pLpFreqB, pLpFreqC));
	const bool  hpOn   = loadRelaxedBool (pick (pHpOnA,   pHpOnB,   pHpOnC));
	const bool  lpOn   = loadRelaxedBool (pick (pLpOnA,   pLpOnB,   pLpOnC));
	const int   hpSlope = juce::jlimit (kFilterSlopeMin, kFilterSlopeMax,
	                                    loadRelaxedInt (pick (pHpSlopeA, pHpSlopeB, pHpSlopeC)));
	const int   lpSlope = juce::jlimit (kFilterSlopeMin, kFilterSlopeMax,
	                                    loadRelaxedInt (pick (pLpSlopeA, pLpSlopeB, pLpSlopeC)));
	const bool  expanderEnabled = loadRelaxedBool (pick (pExpA, pExpB, pExpC));
	const bool  expPost = loadRelaxedBool (pick (pExpOrderA, pExpOrderB, pExpOrderC));
	const float expRatio = loadRelaxed (pick (pExpRatioA, pExpRatioB, pExpRatioC));
	const float expThreshDb = loadRelaxed (pick (pExpThreshA, pExpThreshB, pExpThreshC));
	const float expKneeDb = loadRelaxed (pick (pExpKneeA, pExpKneeB, pExpKneeC));
	const float expAtkMs = loadRelaxed (pick (pExpAtkA, pExpAtkB, pExpAtkC));
	const float expRelMs = loadRelaxed (pick (pExpRelA, pExpRelB, pExpRelC));
	const float expScHpHz = loadRelaxed (pick (pExpScHpA, pExpScHpB, pExpScHpC), kExpScHpDefault);
	const float expScLpHz = loadRelaxed (pick (pExpScLpA, pExpScLpB, pExpScLpC), kExpScLpDefault);
	const bool expScHpOn = loadRelaxedBool (pick (pExpScHpOnA, pExpScHpOnB, pExpScHpOnC), kExpScHpOnDefault);
	const bool expScLpOn = loadRelaxedBool (pick (pExpScLpOnA, pExpScLpOnB, pExpScLpOnC), kExpScLpOnDefault);
	const int expScHpSlope = juce::jlimit (kFilterSlopeMin, kFilterSlopeMax,
	                                       loadRelaxedInt (pick (pExpScHpSlopeA, pExpScHpSlopeB, pExpScHpSlopeC), kExpScHpSlopeDefault));
	const int expScLpSlope = juce::jlimit (kFilterSlopeMin, kFilterSlopeMax,
	                                       loadRelaxedInt (pick (pExpScLpSlopeA, pExpScLpSlopeB, pExpScLpSlopeC), kExpScLpSlopeDefault));
	const float expScGainDb = loadRelaxed (pick (pExpScGainA, pExpScGainB, pExpScGainC), kExpScGainDefault);
	const float delayMs = loadRelaxed (pick (pDelayA, pDelayB, pDelayC));
	const float pan = loadRelaxed (pick (pPanA, pPanB, pPanC));
	const float fred = loadRelaxed (pick (pFredA, pFredB, pFredC));
	const float pos = loadRelaxed (pick (pPosA, pPosB, pPosC));
	const float variation = loadRelaxed (pick (pVariationA, pVariationB, pVariationC));
	const float outDb = loadRelaxed (pick (pOutA, pOutB, pOutC));
	const float inDb  = loadRelaxed (pick (pInA,  pInB,  pInC));
	const float tiltDb = loadRelaxed (pick (pTiltA, pTiltB, pTiltC));
	const bool chaosEnabled = loadRelaxedBool (pick (pChaosA, pChaosB, pChaosC));
	const bool chaosFilterEnabled = loadRelaxedBool (pick (pChaosFilterA, pChaosFilterB, pChaosFilterC));
	const float chaosAmt = loadRelaxed (pick (pChaosAmtA, pChaosAmtB, pChaosAmtC));
	const float chaosSpd = loadRelaxed (pick (pChaosSpdA, pChaosSpdB, pChaosSpdC));
	const float chaosAmtFilter = loadRelaxed (pick (pChaosAmtFilterA, pChaosAmtFilterB, pChaosAmtFilterC));
	const float chaosSpdFilter = loadRelaxed (pick (pChaosSpdFilterA, pChaosSpdFilterB, pChaosSpdFilterC));
	const int filterPos = loadRelaxedInt (pick (pFilterPosA, pFilterPosB, pFilterPosC));
	const bool filterPre = (filterPos == 1 || filterPos == 2);
	const bool tiltPre = (filterPos == 1 || filterPos == 3);
	const float chaosParamSr = juce::jmax (1.0f, static_cast<float> (currentSampleRate));
	const float chaosParamSmoothCoeff = 1.0f - std::exp (-1.0f / (chaosParamSr * 0.010f));
	const float variationSmoothCoeff = 1.0f - std::exp (-1.0f / (chaosParamSr * 0.030f));
	float variationSizeOffset = 0.0f;
	float variationAngleOffset = 0.0f;
	float variationDistanceOffset = 0.0f;
	advanceVariationState (state, variation, variationSmoothCoeff, numSamples,
	                       variationSizeOffset, variationAngleOffset, variationDistanceOffset);

	// (FRED processing happens after convolution + filters)
	
	// 1. INPUT GAIN (IN)
	{
		const float inGain = gainFaderDecibelsToGain (inDb);
		for (int ch = 0; ch < numChannels; ++ch)
			buffer.applyGainRamp (ch, 0, numSamples, state.lastInGain, inGain);
		state.lastInGain = inGain;
	}

	// PRE-IR tone positioning. SAT-TR uses filter first when both are PRE.
	if (filterPre || tiltPre)
		applyLoaderTonePosition (state, buffer, filterPre, tiltPre, true,
		                         hpFreq, lpFreq, hpOn, lpOn, hpSlope, lpSlope, tiltDb,
		                         chaosFilterEnabled, chaosAmtFilter, chaosSpdFilter, chaosParamSmoothCoeff);

	// PRE-expander: after input conditioning, before the IR/convolution black box.
	if (expanderEnabled && ! expPost)
		applyExpanderBuffer (state, buffer, static_cast<float> (currentSampleRate),
		                     expanderEnabled, expRatio, expThreshDb,
		                     expKneeDb, expAtkMs, expRelMs,
		                     expScHpHz, expScLpHz, expScHpOn, expScLpOn, expScHpSlope, expScLpSlope,
		                     expScGainDb);

	// 1.5. CHAOS D: input decorrelation before the IR/convolution black box.
	applyChaosLoaderBuffer (state, buffer, chaosEnabled, chaosAmt, chaosSpd, chaosParamSmoothCoeff);

	// 2. CONVOLUTION (TwoStageFFTConvolver - head on audio thread, tail on background)
	{
		state.convolution.process (buffer);

		// Sanitise: kill NaN/Inf/denormals that can leak from the engine
		// during IR swaps (crossfade transients) or extreme input.
		for (int ch = 0; ch < numChannels; ++ch)
		{
			auto* d = buffer.getWritePointer (ch);
			for (int i = 0; i < numSamples; ++i)
				if (! std::isfinite (d[i])) d[i] = 0.0f;
		}
	}

	// POST-expander: immediately after the IR/convolution black box.
	if (expanderEnabled && expPost)
		applyExpanderBuffer (state, buffer, static_cast<float> (currentSampleRate),
		                     expanderEnabled, expRatio, expThreshDb,
		                     expKneeDb, expAtkMs, expRelMs,
		                     expScHpHz, expScLpHz, expScHpOn, expScLpOn, expScHpSlope, expScLpSlope,
		                     expScGainDb);

	// VAR size lane: tiny realtime cab-size proxy. It never touches the IR SIZE parameter,
	// so it cannot trigger an impulse reload or convolver swap.
	applyVariationSizeProxy (state, buffer, variationSizeOffset);

	// 3. OUTPUT GAIN (OUT) - current default keeps OUT before post tilt/filters.
	{
		const float outGain = gainFaderDecibelsToGain (outDb);
		for (int ch = 0; ch < numChannels; ++ch)
			buffer.applyGainRamp (ch, 0, numSamples, state.lastOutGain, outGain);
		state.lastOutGain = outGain;
	}

#if CABTR_DSP_DEBUG_LOG
	// Per-loader post-convolution + post-out diagnostic (throttled)
	{
		static int diagLoaderCount[3] = {0, 0, 0};
		++diagLoaderCount[loaderIndex];
		const int bps = juce::jmax (1, (int)(currentSampleRate / juce::jmax (1, numSamples)));
		if (diagLoaderCount[loaderIndex] >= bps)
		{
			diagLoaderCount[loaderIndex] = 0;
			float pk = 0.0f;
			for (int ch = 0; ch < numChannels; ++ch)
				pk = juce::jmax (pk, buffer.getMagnitude (ch, 0, numSamples));
			juce::String d;
			d << "LOADER[" << loaderIndex << "] postConv+outGain peak=" << juce::String (pk, 4)
			  << " inDb=" << juce::String (inDb, 2)
			  << " outDb=" << juce::String (outDb, 2)
			  << " tiltDb=" << juce::String (tiltDb, 2)
			  << " filterPos=" << filterPos
			  << " hpOn=" << (int) hpOn << " lpOn=" << (int) lpOn
			  << " hpFreq=" << juce::String (hpFreq, 1) << " lpFreq=" << juce::String (lpFreq, 1)
			  << " irLen=" << state.impulseResponse.getNumSamples()
			  << " hasIR=" << (int) (!state.currentFilePath.isEmpty());
			LOG_IR_EVENT (d);
		}
	}
#endif

	if (! tiltPre || ! filterPre)
		applyLoaderTonePosition (state, buffer, ! filterPre, ! tiltPre, false,
		                         hpFreq, lpFreq, hpOn, lpOn, hpSlope, lpSlope, tiltDb,
		                         chaosFilterEnabled, chaosAmtFilter, chaosSpdFilter, chaosParamSmoothCoeff);

	// 4. DISTANCE EFFECT (exponential LPF + gain attenuation)
	// 0% = close/bright (no change), 100% = far/dark (HF reduction + volume drop)
	const float posAmount = juce::jlimit (0.0f, 1.0f, pos + variationDistanceOffset);
	const float distanceWetStart = state.distanceWet;
	const float distanceWetEnd = posAmount > 0.01f ? 1.0f : 0.0f;
	const bool distanceActive = distanceWetEnd > 0.0f || distanceWetStart > 1.0e-4f;
	if (distanceActive)
	{
		const bool needsDistanceCrossfade = distanceWetStart < 0.999f || distanceWetEnd < 0.999f
		                                 || std::abs (distanceWetEnd - distanceWetStart) > 1.0e-4f;
		if (needsDistanceCrossfade)
		{
			for (int ch = 0; ch < numChannels; ++ch)
				state.distanceDryBuffer.copyFrom (ch, 0, buffer, ch, 0, numSamples);
		}

		// Exponential cutoff: 12kHz * exp(-pos * 2.08) -> pos=0->12kHz, pos=1->1.5kHz
		const float cutoff = 12000.0f * std::exp (-posAmount * kDistDecay);
		constexpr float kPosSmooth = 0.9955f;
		state.smoothedPosFreq += (cutoff - state.smoothedPosFreq) * (1.0f - kPosSmooth);

		// Recalculate if frequency changed significantly
		const float maxFreq = static_cast<float> (currentSampleRate) * 0.49f;
		const float clampedPos = juce::jlimit (200.0f, maxFreq, state.smoothedPosFreq);
		if (std::abs (clampedPos - state.lastPosFreq) > 1.0f)
		{
			*state.posFilter.state = *juce::dsp::IIR::Coefficients<float>::makeLowPass (
				currentSampleRate, clampedPos, kSqrt2Over2);
			state.lastPosFreq = clampedPos;
		}
		
		juce::dsp::AudioBlock<float> block (buffer);
		juce::dsp::ProcessContextReplacing<float> context (block);
		state.posFilter.process (context);

		// Distance gain attenuation: 0dB at pos=0, -6dB at pos=1
		const float distGain = 1.0f - posAmount * 0.5f;
		for (int ch = 0; ch < numChannels; ++ch)
			buffer.applyGainRamp (ch, 0, numSamples, state.lastPosGain, distGain);
		state.lastPosGain = distGain;

		if (needsDistanceCrossfade)
		{
			const int safeSamples = juce::jmax (1, numSamples);
			for (int ch = 0; ch < numChannels; ++ch)
			{
				auto* wet = buffer.getWritePointer (ch);
				const auto* dry = state.distanceDryBuffer.getReadPointer (ch);
				for (int i = 0; i < numSamples; ++i)
				{
					const float t = static_cast<float> (i + 1) / static_cast<float> (safeSamples);
					const float wetAmt = distanceWetStart + (distanceWetEnd - distanceWetStart) * t;
					wet[i] = dry[i] + (wet[i] - dry[i]) * wetAmt;
				}
			}
		}

		state.distanceWet = distanceWetEnd;
	}
	else
	{
		if (state.lastPosFreq > 0.0f)
			state.lastPosFreq = -1.0f; // Mark as inactive
		if (std::abs (state.lastPosGain - 1.0f) > 1.0e-4f)
			for (int ch = 0; ch < numChannels; ++ch)
				buffer.applyGainRamp (ch, 0, numSamples, state.lastPosGain, 1.0f);
		state.lastPosGain = 1.0f;
		state.distanceWet = 0.0f;
	}

	// 5. PAN
	if (numChannels >= 2)
	{
		float targetLeft = 1.0f;
		float targetRight = 1.0f;
		if (std::abs (pan - 0.5f) > 0.001f)
		{
			const float panAngle = pan * 1.5707963f;
			targetLeft = std::cos (panAngle);
			targetRight = std::sin (panAngle);
		}

		buffer.applyGainRamp (0, 0, numSamples, state.lastPanLeft, targetLeft);
		buffer.applyGainRamp (1, 0, numSamples, state.lastPanRight, targetRight);
		state.lastPanLeft = targetLeft;
		state.lastPanRight = targetRight;
		state.lastPan = pan;
	}
	
	// 6. DELAY: auto-align compensation
	// Always run the delay stage so the read head can glide cleanly to/from zero
	// without a hard bypass click or stale-buffer re-entry.
	applyDelay (buffer, delayMs, loaderIndex);
	
	// 7. ANGLE (off-axis mic simulation)
	// Simulates a second mic at an angle on a guitar cab.
	// 7-sample circular delay (~0.15ms at 48kHz ~= 5cm path difference).
	// First comb null at ~6.8kHz - creates musically useful tonal shaping.
	// angle=0: pure on-axis (no effect), angle=1: full off-axis blend
	const float fredStart = state.lastFred;
	const float fredEnd = juce::jlimit (0.0f, 1.0f, fred + variationAngleOffset);
	const bool fredAudible = fredStart > 1.0e-4f || fredEnd > 1.0e-4f;
	{
		float* channelData[2] = { nullptr, nullptr };
		const int chCount = juce::jmin (numChannels, 2);
		for (int ch = 0; ch < chCount; ++ch)
			channelData[ch] = buffer.getWritePointer (ch);
		
		const int delayLen = IRLoaderState::kFredDelaySamples;
		const int safeSamples = juce::jmax (1, numSamples);
		for (int i = 0; i < numSamples; ++i)
		{
			const int idx = state.fredDelayIndex;
			const float t = static_cast<float> (i + 1) / static_cast<float> (safeSamples);
			const float fredAmt = fredStart + (fredEnd - fredStart) * t;
			const float compensate = 1.0f / (1.0f + fredAmt); // Gain compensation for additive sum
			for (int ch = 0; ch < chCount; ++ch)
			{
				const float direct = channelData[ch][i];
				const float offAxis = state.fredDelayBuffer[ch][idx];
				state.fredDelayBuffer[ch][idx] = direct;
				// Additive comb: sum direct + delayed copy -> creates frequency cancellations
				if (fredAudible)
					channelData[ch][i] = (direct + fredAmt * offAxis) * compensate;
			}
			state.fredDelayIndex = (state.fredDelayIndex + 1) % delayLen;
		}
		state.lastFred = fredEnd;
	}
	
	// Flush denormals in filter/tilt states (per-block, near-zero cost)
	{
		constexpr float kDnr = 1e-20f;
		if (std::abs (state.tiltState[0])       < kDnr) state.tiltState[0]       = 0.0f;
		if (std::abs (state.tiltState[1])       < kDnr) state.tiltState[1]       = 0.0f;
	}
}

void CABTRAudioProcessor::applyChaosLoaderBuffer (IRLoaderState& state,
                                                  juce::AudioBuffer<float>& buffer,
                                                  bool chaosEnabled, float chaosAmt, float chaosSpd,
                                                  float chaosParamSmoothCoeff) noexcept
{
	const int numSamples = buffer.getNumSamples();
	const int numChannels = buffer.getNumChannels();
	const int chCount = juce::jmin (numChannels, 2);
	const bool chaosLoaderActive = chaosEnabled
		&& (chaosAmt > 0.01f || (state.chaosLoaderParamSmoothReady && state.chaosLoaderAmtSmoothed > 0.01f));

	if (! chaosLoaderActive || numSamples <= 0 || chCount <= 0)
	{
		state.chaosDelaySmoothedSamples[0] = state.chaosDelaySmoothedSamples[1] = 0.0f;
		state.chaosDelaySmoothReady[0] = state.chaosDelaySmoothReady[1] = false;
		state.chaosLoaderAmtSmoothed = juce::jlimit (kChaosAmtMin, kChaosAmtMax, chaosAmt);
		state.chaosLoaderSpdSmoothed = juce::jlimit (kChaosSpdMin, kChaosSpdMax, chaosSpd);
		state.chaosLoaderParamSmoothReady = false;
		return;
	}

	const float maxDelaySec = 0.005f; // +/-5ms max
	const float sr = static_cast<float> (currentSampleRate);
	const float chaosDelaySmoothCoeff = 1.0f - std::exp (-1.0f / (sr * 0.002f));
	const int delayBufLen = IRLoaderState::kChaosDelayMaxSamples;
	const int mask = delayBufLen - 1;

	float* channelData[2] = { nullptr, nullptr };
	for (int ch = 0; ch < chCount; ++ch)
		channelData[ch] = buffer.getWritePointer (ch);

	for (int i = 0; i < numSamples; ++i)
	{
		const float targetAmt = juce::jlimit (kChaosAmtMin, kChaosAmtMax, chaosAmt);
		const float targetSpd = juce::jlimit (kChaosSpdMin, kChaosSpdMax, chaosSpd);
		if (! state.chaosLoaderParamSmoothReady)
		{
			state.chaosLoaderParamSmoothReady = true;
			if (state.chaosLoaderSpdSmoothed <= 0.0f)
				state.chaosLoaderSpdSmoothed = targetSpd;
		}

		state.chaosLoaderAmtSmoothed += (targetAmt - state.chaosLoaderAmtSmoothed) * chaosParamSmoothCoeff;
		const float loaderSpdLog = std::log (juce::jmax (kChaosSpdMin, state.chaosLoaderSpdSmoothed));
		const float loaderTargetSpdLog = std::log (targetSpd);
		state.chaosLoaderSpdSmoothed = std::exp (loaderSpdLog + (loaderTargetSpdLog - loaderSpdLog) * chaosParamSmoothCoeff);

		const float amountNorm = state.chaosLoaderAmtSmoothed * 0.01f; // 0..1
		const float maxDelaySamples = amountNorm * maxDelaySec * sr;
		const float shPeriodSamples = sr / juce::jmax (kChaosSpdMin, state.chaosLoaderSpdSmoothed);
		const float chaosGainMaxDb = amountNorm * 1.0f; // +/-1dB at 100%

		// L/R-linked micro-delay and gain: decorrelate the loader, not the stereo image.
		advanceChaosEngine (state.chaosDPrev[0], state.chaosDCurr[0], state.chaosDNext[0],
		                    state.chaosDPhase[0], state.chaosDDriftPhase[0], state.chaosDDriftFreqHz[0],
		                    state.chaosDOut[0], state.chaosDRng[0], shPeriodSamples, amountNorm, sr);
		advanceChaosEngine (state.chaosGPrev[0], state.chaosGCurr[0], state.chaosGNext[0],
		                    state.chaosGPhase[0], state.chaosGDriftPhase[0], state.chaosGDriftFreqHz[0],
		                    state.chaosGOut[0], state.chaosGRng[0], shPeriodSamples, amountNorm, sr);
		state.chaosDOut[1] = state.chaosDOut[0];
		state.chaosGOut[1] = state.chaosGOut[0];

		const int wp = state.chaosDelayWritePos;
		for (int ch = 0; ch < chCount; ++ch)
			state.chaosDelayBuffer[ch][wp] = channelData[ch][i];

		const float targetDelaySamp = juce::jlimit (0.0f, static_cast<float> (delayBufLen - 2),
		                                            maxDelaySamples + state.chaosDOut[0] * maxDelaySamples);
		float& smoothedDelaySamp = state.chaosDelaySmoothedSamples[0];
		if (! state.chaosDelaySmoothReady[0])
		{
			smoothedDelaySamp = targetDelaySamp;
			state.chaosDelaySmoothReady[0] = true;
		}
		else
		{
			smoothedDelaySamp += (targetDelaySamp - smoothedDelaySamp) * chaosDelaySmoothCoeff;
		}
		state.chaosDelaySmoothedSamples[1] = smoothedDelaySamp;
		state.chaosDelaySmoothReady[1] = state.chaosDelaySmoothReady[0];

		const float readPos = static_cast<float> (wp) - smoothedDelaySamp;
		const int iPos = static_cast<int> (std::floor (readPos));
		const float frac = readPos - static_cast<float> (iPos);
		const float gainDb  = state.chaosGOut[0] * chaosGainMaxDb;
		const float ex = gainDb * 0.16609640474f;
		const float exln2 = ex * 0.6931472f;
		const float gainLin = 1.0f + exln2 * (1.0f + exln2 * 0.5f);

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
			channelData[ch][i] = (((c3 * frac + c2) * frac + c1) * frac + c0) * gainLin;
		}

		state.chaosDelayWritePos = (wp + 1) & mask;
	}
}

void CABTRAudioProcessor::resetVariationStateForLoader (IRLoaderState& state, int loaderSeedIndex) noexcept
{
	if (variationInstanceSeed_ == 0)
		variationInstanceSeed_ = createVariationInstanceSeed();

	state.variationAmtSmoothed = 0.0f;
	state.variationParamSmoothReady = false;
	state.variationSizeAllpassState[0] = state.variationSizeAllpassState[1] = 0.0f;

	auto resetVariationLane = [] (IRLoaderState::VariationLaneState& lane, juce::int64 seed) noexcept
	{
		lane.rng.setSeed (seed);
		lane.prev = lane.rng.nextFloat() * 2.0f - 1.0f;
		lane.curr = lane.prev;
		lane.next = lane.rng.nextFloat() * 2.0f - 1.0f;
		lane.phase = lane.rng.nextFloat();
		lane.driftPhase = lane.rng.nextFloat();
		lane.driftFreqHz = 0.0f;
		lane.out = 0.0f;
	};

	const juce::int64 seedBase = 0x5A7CAB00LL + static_cast<juce::int64> (loaderSeedIndex) * 0x101LL;
	const juce::int64 instanceSeedBase = variationInstanceSeed_
	                                    ^ (0x6C8E9CF570932BD5LL
	                                       + static_cast<juce::int64> (loaderSeedIndex) * 0x1F123BB5A6D1LL);
	resetVariationLane (state.variationAir, seedBase + 5);
	resetVariationLane (state.variationSize, seedBase + 11);
	resetVariationLane (state.variationAngle, seedBase + 23);
	resetVariationLane (state.variationDistance, seedBase + 37);
	resetVariationLane (state.variationAirFast, instanceSeedBase + 41);
	resetVariationLane (state.variationSizeFast, instanceSeedBase + 53);
	resetVariationLane (state.variationAngleFast, instanceSeedBase + 67);
	resetVariationLane (state.variationDistanceFast, instanceSeedBase + 79);
}

void CABTRAudioProcessor::resetAllVariationStates() noexcept
{
	resetVariationStateForLoader (stateA, 0);
	resetVariationStateForLoader (stateB, 1);
	resetVariationStateForLoader (stateC, 2);
}

void CABTRAudioProcessor::advanceVariationState (IRLoaderState& state, float variationTarget,
                                                  float variationSmoothCoeff, int numSamples,
                                                  float& sizeOffset, float& angleOffset,
                                                  float& distanceOffset) noexcept
{
	sizeOffset = 0.0f;
	angleOffset = 0.0f;
	distanceOffset = 0.0f;

	if (numSamples <= 0)
		return;

	const float target = juce::jlimit (kVariationMin, kVariationMax, variationTarget);
	const bool active = target > 0.0001f
	                 || (state.variationParamSmoothReady && state.variationAmtSmoothed > 0.0001f);
	if (! active)
	{
		state.variationAmtSmoothed = 0.0f;
		state.variationParamSmoothReady = false;
		state.variationSizeAllpassState[0] = state.variationSizeAllpassState[1] = 0.0f;
		return;
	}

	state.variationParamSmoothReady = true;
	const float sr = juce::jmax (1.0f, static_cast<float> (currentSampleRate));
	const float coeff = juce::jlimit (0.0f, 1.0f, variationSmoothCoeff);

	auto depthFromAmount = [] (float amount) noexcept
	{
		const float a = juce::jlimit (0.0f, 1.0f, amount);
		return a * std::sqrt (a); // close to pow(a, 1.5): subtle until the upper range.
	};
	auto smoothStep = [] (float x) noexcept
	{
		const float t = juce::jlimit (0.0f, 1.0f, x);
		return t * t * (3.0f - 2.0f * t);
	};

	for (int i = 0; i < numSamples; ++i)
	{
		state.variationAmtSmoothed += (target - state.variationAmtSmoothed) * coeff;
		const float depth = depthFromAmount (state.variationAmtSmoothed);
		const float highRangeRate = smoothStep ((state.variationAmtSmoothed - 0.55f) / 0.45f);
		const float rateScale = 0.70f + depth * 1.15f + highRangeRate * 4.0f;
		const float fastNorm = smoothStep ((state.variationAmtSmoothed - 0.65f) / 0.35f);

		const float sizePeriod = sr / (0.19f * rateScale);
		const float anglePeriod = sr / (0.37f * rateScale);
		const float distancePeriod = sr / (0.29f * rateScale);
		const float airPeriod = sr / (0.31f * rateScale);
		const float sizeFastPeriod = sr / (2.0f + fastNorm * 8.0f);      // 500 ms -> 100 ms
		const float angleFastPeriod = sr / (2.4f + fastNorm * 7.6f);     // ~417 ms -> 100 ms
		const float distanceFastPeriod = sr / (2.2f + fastNorm * 7.8f);  // ~455 ms -> 100 ms
		const float airFastPeriod = sr / (2.1f + fastNorm * 7.9f);       // ~476 ms -> 100 ms

		advanceChaosEngine (state.variationAir.prev, state.variationAir.curr, state.variationAir.next,
		                    state.variationAir.phase, state.variationAir.driftPhase, state.variationAir.driftFreqHz,
		                    state.variationAir.out, state.variationAir.rng, airPeriod, depth, sr);
		advanceChaosEngine (state.variationSize.prev, state.variationSize.curr, state.variationSize.next,
		                    state.variationSize.phase, state.variationSize.driftPhase, state.variationSize.driftFreqHz,
		                    state.variationSize.out, state.variationSize.rng, sizePeriod, depth, sr);
		advanceChaosEngine (state.variationAngle.prev, state.variationAngle.curr, state.variationAngle.next,
		                    state.variationAngle.phase, state.variationAngle.driftPhase, state.variationAngle.driftFreqHz,
		                    state.variationAngle.out, state.variationAngle.rng, anglePeriod, depth, sr);
		advanceChaosEngine (state.variationDistance.prev, state.variationDistance.curr, state.variationDistance.next,
		                    state.variationDistance.phase, state.variationDistance.driftPhase, state.variationDistance.driftFreqHz,
		                    state.variationDistance.out, state.variationDistance.rng, distancePeriod, depth, sr);

		if (fastNorm > 0.0f)
		{
			advanceChaosEngine (state.variationAirFast.prev, state.variationAirFast.curr, state.variationAirFast.next,
			                    state.variationAirFast.phase, state.variationAirFast.driftPhase, state.variationAirFast.driftFreqHz,
			                    state.variationAirFast.out, state.variationAirFast.rng, airFastPeriod, depth, sr);
			advanceChaosEngine (state.variationSizeFast.prev, state.variationSizeFast.curr, state.variationSizeFast.next,
			                    state.variationSizeFast.phase, state.variationSizeFast.driftPhase, state.variationSizeFast.driftFreqHz,
			                    state.variationSizeFast.out, state.variationSizeFast.rng, sizeFastPeriod, depth, sr);
			advanceChaosEngine (state.variationAngleFast.prev, state.variationAngleFast.curr, state.variationAngleFast.next,
			                    state.variationAngleFast.phase, state.variationAngleFast.driftPhase, state.variationAngleFast.driftFreqHz,
			                    state.variationAngleFast.out, state.variationAngleFast.rng, angleFastPeriod, depth, sr);
			advanceChaosEngine (state.variationDistanceFast.prev, state.variationDistanceFast.curr, state.variationDistanceFast.next,
			                    state.variationDistanceFast.phase, state.variationDistanceFast.driftPhase, state.variationDistanceFast.driftFreqHz,
			                    state.variationDistanceFast.out, state.variationDistanceFast.rng, distanceFastPeriod, depth, sr);
		}
	}

	if (target <= 0.0001f && state.variationAmtSmoothed <= 0.0001f)
	{
		state.variationAmtSmoothed = 0.0f;
		state.variationParamSmoothReady = false;
		state.variationSizeAllpassState[0] = state.variationSizeAllpassState[1] = 0.0f;
		return;
	}

	const float finalDepth = depthFromAmount (state.variationAmtSmoothed);
	const auto lane = [] (float v) noexcept { return juce::jlimit (-1.0f, 1.0f, v); };
	const float fastMix = 0.40f * smoothStep ((state.variationAmtSmoothed - 0.65f) / 0.35f);
	const auto blend = [lane, fastMix] (float slow, float fast) noexcept
	{
		return lane (lane (slow) * (1.0f - fastMix) + lane (fast) * fastMix);
	};
	const auto correlate = [lane] (float air, float local) noexcept
	{
		return lane (lane (air) * 0.70f + lane (local) * 0.30f);
	};
	constexpr float kVariationDepthScale = 4.0f;

	sizeOffset     = blend (correlate (state.variationAir.out, state.variationSize.out),
	                        correlate (state.variationAirFast.out, state.variationSizeFast.out)) * (0.020f * kVariationDepthScale) * finalDepth; // +/-8%
	angleOffset    = blend (correlate (state.variationAir.out, state.variationAngle.out),
	                        correlate (state.variationAirFast.out, state.variationAngleFast.out)) * (0.040f * kVariationDepthScale) * finalDepth; // +/-16%
	distanceOffset = blend (correlate (state.variationAir.out, state.variationDistance.out),
	                        correlate (state.variationAirFast.out, state.variationDistanceFast.out)) * (0.020f * kVariationDepthScale) * finalDepth; // +/-8%
}

void CABTRAudioProcessor::applyVariationSizeProxy (IRLoaderState& state,
                                                   juce::AudioBuffer<float>& buffer,
                                                   float sizeOffset) noexcept
{
	const int numSamples = buffer.getNumSamples();
	const int chCount = juce::jmin (buffer.getNumChannels(), 2);
	const float absOffset = std::abs (sizeOffset);

	if (numSamples <= 0 || chCount <= 0 || absOffset <= 1.0e-6f)
		return;

	const float allpassCoeff = juce::jlimit (-0.16f, 0.16f, sizeOffset * 8.0f);
	const float wet = juce::jlimit (0.0f, 0.16f, absOffset * 8.0f);

	for (int ch = 0; ch < chCount; ++ch)
	{
		auto* data = buffer.getWritePointer (ch);
		float z = state.variationSizeAllpassState[ch];

		for (int i = 0; i < numSamples; ++i)
		{
			const float x = data[i];
			const float y = -allpassCoeff * x + z;
			z = x + allpassCoeff * y;
			data[i] = x + (y - x) * wet;
		}

		state.variationSizeAllpassState[ch] = z;
	}
}

void CABTRAudioProcessor::applyExpanderBuffer (IRLoaderState& state, juce::AudioBuffer<float>& buffer, float sampleRate,
                                               bool expanderEnabled, float expRatio, float expThreshDb,
                                               float expKneeDb, float expAtkMs, float expRelMs,
                                               float expScHpHz, float expScLpHz,
                                               bool expScHpOn, bool expScLpOn,
                                               int expScHpSlope, int expScLpSlope,
                                               float expScGainDb) noexcept
{
	const int numSamples = buffer.getNumSamples();
	const int numChannels = buffer.getNumChannels();

	if (! expanderEnabled || numSamples <= 0 || numChannels <= 0)
		return;

	const float sr = (float) sampleRate;
	const float attCoeff = std::exp (-1.0f / (sr * juce::jmax (0.00001f, expAtkMs * 0.001f)));
	const float relCoeff = std::exp (-1.0f / (sr * juce::jmax (0.001f,   expRelMs * 0.001f)));
	const float ratio = juce::jlimit (kExpRatioMin, kExpRatioMax, expRatio);
	if (std::abs (ratio - 1.0f) <= 0.01f)
		return;
	const float kneeDb = juce::jlimit (kExpKneeMin, kExpKneeMax, expKneeDb);
	const float slope = ratio - 1.0f;  // >0 = downward expansion, <0 = upward compression below threshold
	const float maxDetectorFreq = juce::jmin (kExpScFreqMax, sr * 0.45f);
	float detectorHpHz = juce::jlimit (kExpScFreqMin, maxDetectorFreq, expScHpHz);
	const float detectorLpHz = juce::jlimit (kExpScFreqMin, maxDetectorFreq, expScLpHz);
	if (detectorHpHz >= detectorLpHz)
		detectorHpHz = juce::jmax (kExpScFreqMin, detectorLpHz * 0.95f);

	const bool useDetectorHp = expScHpOn;
	const bool useDetectorLp = expScLpOn;
	expScHpSlope = juce::jlimit (kFilterSlopeMin, kFilterSlopeMax, expScHpSlope);
	expScLpSlope = juce::jlimit (kFilterSlopeMin, kFilterSlopeMax, expScLpSlope);
	const BiquadCoefficients detectorHp = useDetectorHp ? makeDetectorHighPassForSlope (detectorHpHz, sr, expScHpSlope, false) : BiquadCoefficients {};
	const BiquadCoefficients detectorHp2 = (useDetectorHp && expScHpSlope >= 2) ? makeDetectorHighPassForSlope (detectorHpHz, sr, expScHpSlope, true) : BiquadCoefficients {};
	const BiquadCoefficients detectorLp = useDetectorLp ? makeDetectorLowPassForSlope (detectorLpHz, sr, expScLpSlope, false) : BiquadCoefficients {};
	const BiquadCoefficients detectorLp2 = (useDetectorLp && expScLpSlope >= 2) ? makeDetectorLowPassForSlope (detectorLpHz, sr, expScLpSlope, true) : BiquadCoefficients {};
	if (! useDetectorHp)
	{
		state.expScHpState.reset();
		state.expScHpState2.reset();
	}
	else if (expScHpSlope < 2)
	{
		state.expScHpState2.reset();
	}
	if (! useDetectorLp)
	{
		state.expScLpState.reset();
		state.expScLpState2.reset();
	}
	else if (expScLpSlope < 2)
	{
		state.expScLpState2.reset();
	}

	const float detectorGainTarget = gainFaderDecibelsToGain (juce::jlimit (kExpScGainMin, kExpScGainMax, expScGainDb));
	float detectorGain = state.expScLastGain;
	const float detectorGainStep = numSamples > 0 ? (detectorGainTarget - detectorGain) / static_cast<float> (numSamples) : 0.0f;

	const int chCount = juce::jmin (numChannels, 2);
	float* channelData[2] = { nullptr, nullptr };
	for (int ch = 0; ch < chCount; ++ch)
		channelData[ch] = buffer.getWritePointer (ch);

	for (int i = 0; i < numSamples; ++i)
	{
		float peak = 0.0f;
		for (int ch = 0; ch < chCount; ++ch)
		{
			float detectorSample = channelData[ch][i];
			detectorSample *= detectorGain;
			if (useDetectorHp)
			{
				detectorSample = processDetectorBiquad (detectorSample, state.expScHpState, detectorHp, ch);
				if (expScHpSlope >= 2)
					detectorSample = processDetectorBiquad (detectorSample, state.expScHpState2, detectorHp2, ch);
			}
			if (useDetectorLp)
			{
				detectorSample = processDetectorBiquad (detectorSample, state.expScLpState, detectorLp, ch);
				if (expScLpSlope >= 2)
					detectorSample = processDetectorBiquad (detectorSample, state.expScLpState2, detectorLp2, ch);
			}
			peak = juce::jmax (peak, std::abs (detectorSample));
		}
		detectorGain += detectorGainStep;

		if (peak > state.expLinkedEnv)
			state.expLinkedEnv = attCoeff * state.expLinkedEnv + (1.0f - attCoeff) * peak;
		else
			state.expLinkedEnv = relCoeff * state.expLinkedEnv + (1.0f - relCoeff) * peak;

		float gr = 1.0f;
		if (state.expLinkedEnv > 1.0e-12f)
		{
			const float envDb = 20.0f * std::log10 (state.expLinkedEnv);
			float gainDeltaDb = 0.0f;

			if (kneeDb <= 1.0e-6f)
			{
				if (envDb < expThreshDb)
					gainDeltaDb = slope * (expThreshDb - envDb);
			}
			else
			{
				const float deltaBelowThreshDb = expThreshDb - envDb;
				const float halfKneeDb = 0.5f * kneeDb;

				if (deltaBelowThreshDb >= halfKneeDb)
				{
					gainDeltaDb = slope * deltaBelowThreshDb;
				}
				else if (deltaBelowThreshDb > -halfKneeDb)
				{
					const float kneePos = deltaBelowThreshDb + halfKneeDb; // 0..kneeDb
					gainDeltaDb = slope * (kneePos * kneePos) / (2.0f * kneeDb);
				}
			}

			gainDeltaDb = juce::jlimit (-120.0f, 120.0f, gainDeltaDb);
			gr = fastDecibelsToGain (-gainDeltaDb);
		}

		for (int ch = 0; ch < chCount; ++ch)
			channelData[ch][i] *= gr;
	}

	state.expScLastGain = detectorGainTarget;
}

void CABTRAudioProcessor::applyMidSideOutputMode (juce::AudioBuffer<float>& buf, int modeVal, int nSamples)
{
	if ((modeVal == 1 || modeVal == 2) && buf.getNumChannels() >= 2)
	{
		auto* L = buf.getWritePointer (0);
		auto* R = buf.getWritePointer (1);
		for (int i = 0; i < nSamples; ++i)
		{
			const float mono = (L[i] + R[i]) * 0.5f;
			if (modeVal == 1) // MID output: dual mono mid
			{
				L[i] = mono;
				R[i] = mono;
			}
			else // SIDE output: stereo-encoded side
			{
				L[i] = mono;
				R[i] = -mono;
			}
		}
	}
}

//==============================================================================
// DELAY: Proper delay line with smoothing to prevent clicks
// Handles any delay length correctly (even > buffer size)
// Used for phase alignment between loaders
//==============================================================================
void CABTRAudioProcessor::applyDelay (juce::AudioBuffer<float>& buffer, float delayMs, int loaderIndex)
{
	const int numSamples = buffer.getNumSamples();
	const int numChannels = buffer.getNumChannels();
	
	// Convert ms to samples
	const float targetDelaySamples = juce::jmax (0.0f, delayMs * 0.001f * static_cast<float> (currentSampleRate));
	
	// Select delay line and smoother for this loader
	auto& delayLine = loaderIndex == 0 ? stateA.delayLine : (loaderIndex == 1 ? stateB.delayLine : stateC.delayLine);
	auto& smoother  = loaderIndex == 0 ? stateA.smoothedDelay : (loaderIndex == 1 ? stateB.smoothedDelay : stateC.smoothedDelay);
	auto* const* writeChannels = buffer.getArrayOfWritePointers();
	
	// Set target delay with smoothing (prevents clicks when changing delay)
	smoother.setTargetValue (targetDelaySamples);

	// Keep the buffer continuously fed and fade the first 0..2 samples into dry.
	// This preserves the intentional "rewind" glide while avoiding a hard switch
	// at very small delays, where interpolation quality and stale-buffer jumps are
	// most likely to click.
	constexpr float kMinInterpDelaySamples = 2.0f;

	if (! smoother.isSmoothing())
	{
		const float currentDelay = smoother.getCurrentValue();
		const float interpDelay = juce::jmax (currentDelay, kMinInterpDelaySamples);
		const float wet = juce::jlimit (0.0f, 1.0f, currentDelay / kMinInterpDelaySamples);
		delayLine.setDelay (interpDelay);

		if (wet <= 0.0f)
		{
			for (int i = 0; i < numSamples; ++i)
			{
				for (int ch = 0; ch < numChannels; ++ch)
				{
					auto* channelData = writeChannels[ch];
					delayLine.pushSample (ch, channelData[i]);
					delayLine.popSample (ch);
				}
			}

			return;
		}

		for (int i = 0; i < numSamples; ++i)
		{
			for (int ch = 0; ch < numChannels; ++ch)
			{
				auto* channelData = writeChannels[ch];
				const float input = channelData[i];
				delayLine.pushSample (ch, input);
				const float delayed = delayLine.popSample (ch);
				channelData[i] = input + (delayed - input) * wet;
			}
		}

		return;
	}

	// Apply smoothed delay sample-by-sample while the target is moving.
	for (int i = 0; i < numSamples; ++i)
	{
		const float currentDelay = smoother.getNextValue();
		const float interpDelay = juce::jmax (currentDelay, kMinInterpDelaySamples);
		const float wet = juce::jlimit (0.0f, 1.0f, currentDelay / kMinInterpDelaySamples);
		delayLine.setDelay (interpDelay);
		
		// Process each channel
		for (int ch = 0; ch < numChannels; ++ch)
		{
			auto* channelData = writeChannels[ch];
			const float input = channelData[i];
			
			// Push input and pop delayed output
			delayLine.pushSample (ch, input);
			const float delayed = delayLine.popSample (ch);
			channelData[i] = input + (delayed - input) * wet;
		}
	}
}

void CABTRAudioProcessor::calculateAutoAlignment()
{
	// Only consider loaders that are both loaded AND enabled
	const bool enabledA = parameters.getRawParameterValue (kParamEnableA)->load() > 0.5f;
	const bool enabledB = parameters.getRawParameterValue (kParamEnableB)->load() > 0.5f;
	const bool enabledC = parameters.getRawParameterValue (kParamEnableC)->load() > 0.5f;

	const bool hasA = enabledA && ! stateA.currentFilePath.isEmpty() && stateA.impulseResponse.getNumSamples() > 0;
	const bool hasB = enabledB && ! stateB.currentFilePath.isEmpty() && stateB.impulseResponse.getNumSamples() > 0;
	const bool hasC = enabledC && ! stateC.currentFilePath.isEmpty() && stateC.impulseResponse.getNumSamples() > 0;

	if (! hasA || (! hasB && ! hasC))
	{
		LOG_IR_EVENT ("ALIGN: skipped - need A enabled + at least B or C enabled & loaded");
		return;
	}

	const auto& irA = stateA.impulseResponse;
	const float* dataA = irA.getReadPointer (0);
	const int lenA = irA.getNumSamples();

	// Max lag for guitar cabs: 5ms (240 samples @ 48kHz)
	const int maxLagSamples = static_cast<int> (currentSampleRate * 0.005);

	// Helper: cross-correlate A vs X, return best lag and signed correlation
	auto xcorr = [&] (const juce::AudioBuffer<float>& irX) -> std::pair<int, float>
	{
		const float* dataX = irX.getReadPointer (0);
		const int lenX = irX.getNumSamples();
		const int maxLag = juce::jmin (maxLagSamples, juce::jmin (lenA, lenX) - 1);
		float bestCorr = 0.0f;
		int bestLag = 0;
		for (int lag = -maxLag; lag <= maxLag; ++lag)
		{
			float sum = 0.0f;
			const int start = juce::jmax (0, -lag);
			const int end = juce::jmin (lenA, lenX - lag);
			for (int n = start; n < end; ++n)
				sum += dataA[n] * dataX[n + lag];
			if (std::abs (sum) > std::abs (bestCorr))
			{
				bestCorr = sum;
				bestLag = lag;
			}
		}
		return { bestLag, bestCorr };
	};

	// Helper: apply alignment result to a loader
	auto applyAlignment = [&] (const char* delayId, const char* invId,
	                            int bestLag, float bestCorr, bool currentInv, bool currentInvA)
	{
		// Compensate for current INV states to find raw correlation
		float rawCorr = bestCorr;
		if (currentInvA) rawCorr = -rawCorr;
		if (currentInv)  rawCorr = -rawCorr;
		const bool needsInvert = (rawCorr < 0.0f);

		const float lagMs = static_cast<float> (std::abs (bestLag)) / static_cast<float> (currentSampleRate) * 1000.0f;
		const float clampedMs = juce::jmin (lagMs, kDelayMax);

		// Apply delay: positive lag means X needs delay, negative means A needs it
		// Since A is reference and may be shared, we only apply delay to the non-A loader.
		// If bestLag < 0, X is ahead of A - we add delay to X by abs(bestLag).
		// If bestLag > 0, A is ahead of X - we add delay to X.
		// Actually the convention: positive lag = B needs to be delayed, negative = A needs delay.
		// For multi-loader: we apply delay only to B/C. If A needs delay, we apply to A separately.
		if (auto* p = parameters.getParameter (delayId))
			p->setValueNotifyingHost (p->convertTo0to1 (bestLag > 0 ? clampedMs : 0.0f));

		if (auto* p = parameters.getParameter (invId))
			p->setValueNotifyingHost (needsInvert ? 1.0f : 0.0f);

		LOG_IR_EVENT ("ALIGN: lag=" + juce::String (bestLag) +
		              " (" + juce::String (lagMs, 3) + "ms), inv=" + juce::String (needsInvert ? "ON" : "OFF"));
	};

	const bool currentInvA = parameters.getRawParameterValue (kParamInvA)->load() > 0.5f;

	// Reset all delays first
	if (auto* p = parameters.getParameter (kParamDelayA))
		p->setValueNotifyingHost (p->convertTo0to1 (0.0f));
	if (auto* p = parameters.getParameter (kParamDelayB))
		p->setValueNotifyingHost (p->convertTo0to1 (0.0f));
	if (auto* p = parameters.getParameter (kParamDelayC))
		p->setValueNotifyingHost (p->convertTo0to1 (0.0f));
	// Reset INV on A
	if (auto* p = parameters.getParameter (kParamInvA))
		p->setValueNotifyingHost (0.0f);

	// For multi-loader alignment: A is reference. We compute delays for B and C.
	// We track the largest negative lag (A ahead -> need delay on A's side).
	int aDelaySamples = 0; // positive = A needs this many samples of delay

	if (hasB)
	{
		auto [lagB, corrB] = xcorr (stateB.impulseResponse);
		const bool currentInvB = parameters.getRawParameterValue (kParamInvB)->load() > 0.5f;
		applyAlignment (kParamDelayB, kParamInvB, lagB, corrB, currentInvB, currentInvA);
		if (lagB < 0) aDelaySamples = juce::jmax (aDelaySamples, -lagB);
	}

	if (hasC)
	{
		auto [lagC, corrC] = xcorr (stateC.impulseResponse);
		const bool currentInvC = parameters.getRawParameterValue (kParamInvC)->load() > 0.5f;
		applyAlignment (kParamDelayC, kParamInvC, lagC, corrC, currentInvC, currentInvA);
		if (lagC < 0) aDelaySamples = juce::jmax (aDelaySamples, -lagC);
	}

	// If A needs delay (was ahead of one or more loaders), apply it
	if (aDelaySamples > 0)
	{
		const float aMsDelay = static_cast<float> (aDelaySamples) / static_cast<float> (currentSampleRate) * 1000.0f;
		if (auto* p = parameters.getParameter (kParamDelayA))
			p->setValueNotifyingHost (p->convertTo0to1 (juce::jmin (aMsDelay, kDelayMax)));
		LOG_IR_EVENT ("ALIGN A: delay=" + juce::String (aMsDelay, 3) + "ms (A was ahead)");
	}
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
// This ensures file I/O never happens in audio thread.
// Debounced: only reload after the IR-affecting parameters have been stable for 300ms.
//==============================================================================
void CABTRAudioProcessor::timerCallback()
{
	const juce::int64 now = juce::Time::currentTimeMillis();
	constexpr juce::int64 minReloadIntervalMs = 300;
	
	// Helper lambda: check if IR-affecting params changed for a loader
	auto checkLoader = [&] (IRLoaderState& state, const char* sizeId, const char* invId,
	                         const char* normId, const char* rvsId, const char* startId,
	                         const char* endId, const char* resoId)
	{
		if (state.currentFilePath.isEmpty())
			return;
		
		const float size = parameters.getRawParameterValue (sizeId)->load();
		const bool inv = parameters.getRawParameterValue (invId)->load() > 0.5f;
		const bool norm = parameters.getRawParameterValue (normId)->load() > 0.5f;
		const bool rvs = parameters.getRawParameterValue (rvsId)->load() > 0.5f;
		const float start = parameters.getRawParameterValue (startId)->load();
		const float end = parameters.getRawParameterValue (endId)->load();
		const float reso = parameters.getRawParameterValue (resoId)->load();
		
		const float epsilon = 0.0001f;
		const bool changed = std::abs (size - state.lastSize.load()) > epsilon ||
		                     inv != state.lastInv.load() ||
		                     norm != state.lastNorm.load() ||
		                     rvs != state.lastRvs.load() ||
		                     std::abs (start - state.lastStart.load()) > epsilon ||
		                     std::abs (end - state.lastEnd.load()) > epsilon ||
		                     std::abs (reso - state.lastReso.load()) > epsilon;
		
		if (changed)
		{
			state.needsUpdate.store (true);
			state.lastChangeTime = now;
		}

		if (state.needsUpdate.load() &&
		    (now - state.lastChangeTime >= minReloadIntervalMs) &&
		    (now - state.lastReloadTime >= minReloadIntervalMs))
		{
			LOG_IR_EVENT ("Loader param change - reloading (size=" +
			              juce::String (size, 3) + ", start=" + juce::String (start, 1) +
			              "ms, end=" + juce::String (end, 1) + "ms, norm=" +
			              juce::String (norm ? "ON" : "OFF") + ", inv=" +
			              juce::String (inv ? "ON" : "OFF") + ", rvs=" +
			              juce::String (rvs ? "ON" : "OFF") + ", reso=" +
			              juce::String (reso * 100.0f, 0) + "%)");

			loadImpulseResponse (state, state.currentFilePath);
			state.lastReloadTime = now;
		}
	};
	
	checkLoader (stateA, kParamSizeA, kParamInvA, kParamNormA, kParamRvsA, kParamStartA, kParamEndA, kParamResoA);
	checkLoader (stateB, kParamSizeB, kParamInvB, kParamNormB, kParamRvsB, kParamStartB, kParamEndB, kParamResoB);
	checkLoader (stateC, kParamSizeC, kParamInvC, kParamNormC, kParamRvsC, kParamStartC, kParamEndC, kParamResoC);
	
	// ALIGN: momentary action - calculate cross-correlation + set delay/inv, then turn off
	{
		const float alignVal = parameters.getRawParameterValue (kParamAlign)->load();
		constexpr juce::int64 alignCooldownMs = 500;
		if (alignVal > 0.5f && (now - lastAlignTime >= alignCooldownMs))
		{
			const bool enabledA = parameters.getRawParameterValue (kParamEnableA)->load() > 0.5f;
			const bool enabledB = parameters.getRawParameterValue (kParamEnableB)->load() > 0.5f;
			const bool enabledC = parameters.getRawParameterValue (kParamEnableC)->load() > 0.5f;
			const bool loadedA = stateA.currentFilePath.isNotEmpty();
			const bool loadedB = stateB.currentFilePath.isNotEmpty();
			const bool loadedC = stateC.currentFilePath.isNotEmpty();

			// Need A + at least one other loader enabled & loaded
			if (enabledA && loadedA && ((enabledB && loadedB) || (enabledC && loadedC)))
			{
				LOG_IR_EVENT ("ALIGN triggered! A_loaded=yes B_loaded=" +
				              juce::String (loadedB ? "yes" : "no") +
				              " C_loaded=" + juce::String (loadedC ? "yes" : "no"));
				calculateAutoAlignment();
				lastAlignTime = now;
			}
			else
			{
				LOG_IR_EVENT ("ALIGN skipped: enableA=" + juce::String (enabledA ? "ON" : "OFF") +
				              " enableB=" + juce::String (enabledB ? "ON" : "OFF") +
				              " enableC=" + juce::String (enabledC ? "ON" : "OFF") +
				              " loadedA=" + juce::String (loadedA ? "yes" : "no") +
				              " loadedB=" + juce::String (loadedB ? "yes" : "no") +
				              " loadedC=" + juce::String (loadedC ? "yes" : "no"));
			}

			if (auto* p = parameters.getParameter (kParamAlign))
				p->setValueNotifyingHost (0.0f);
		}
	}
}

//==============================================================================
// UI state persistence (non-automatable collapse state)
//==============================================================================
void CABTRAudioProcessor::setUiIoExpanded (int loaderIndex, bool expanded)
{
	const char* keys[] = { UiStateKeys::ioExpandedA, UiStateKeys::ioExpandedB, UiStateKeys::ioExpandedC };
	if (loaderIndex >= 0 && loaderIndex < 3)
		parameters.state.setProperty (keys[loaderIndex], expanded, nullptr);
}

bool CABTRAudioProcessor::getUiIoExpanded (int loaderIndex) const noexcept
{
	const char* keys[] = { UiStateKeys::ioExpandedA, UiStateKeys::ioExpandedB, UiStateKeys::ioExpandedC };
	if (loaderIndex >= 0 && loaderIndex < 3)
	{
		const auto fromState = parameters.state.getProperty (keys[loaderIndex]);
		if (! fromState.isVoid()) return (bool) fromState;
	}
	return false;
}

//==============================================================================
void CABTRAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
	// Save parameters (automatic via APVTS)
	auto state = parameters.copyState();
	
	// Add IR file paths to state
	state.setProperty ("irPathA", stateA.currentFilePath, nullptr);
	state.setProperty ("irPathB", stateB.currentFilePath, nullptr);
	state.setProperty ("irPathC", stateC.currentFilePath, nullptr);
	state.setProperty (kVariationInstanceSeedProperty, juce::String (variationInstanceSeed_), nullptr);
	
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

			const juce::String variationSeedString = state.getProperty (kVariationInstanceSeedProperty, {}).toString();
			variationInstanceSeed_ = variationSeedString.isNotEmpty()
			                       ? variationSeedString.getLargeIntValue()
			                       : createVariationInstanceSeed();
			if (variationInstanceSeed_ == 0)
				variationInstanceSeed_ = createVariationInstanceSeed();
			resetAllVariationStates();
			
			// Restore IR file paths
			const juce::String pathA = state.getProperty ("irPathA", "");
			const juce::String pathB = state.getProperty ("irPathB", "");
			const juce::String pathC = state.getProperty ("irPathC", "");
			
			LOG_IR_EVENT ("State loaded: IR_A=" + pathA + ", IR_B=" + pathB + ", IR_C=" + pathC);
			
			auto clearIr = [] (IRLoaderState& loaderState)
			{
				loaderState.convolution.reset();
				loaderState.impulseResponse.setSize (0, 0);
				loaderState.currentFilePath.clear();
				loaderState.needsUpdate.store (false);
				loaderState.irSlopeDbPerOct.store (0.0f);
				loaderState.lastChangeTime = 0;
				loaderState.lastReloadTime = 0;
			};

			auto restoreIrPath = [this, &clearIr] (IRLoaderState& loaderState, const juce::String& path)
			{
				clearIr (loaderState);
				if (path.isNotEmpty())
					loadImpulseResponse (loaderState, path);
			};

			restoreIrPath (stateA, pathA);
			restoreIrPath (stateB, pathB);
			restoreIrPath (stateC, pathC);
			
			// Update UI file display labels
			if (auto* editor = dynamic_cast<CABTRAudioProcessorEditor*> (getActiveEditor()))
				editor->updateFileDisplayLabels (stateA.currentFilePath, stateB.currentFilePath, stateC.currentFilePath);
		}
	}
}

//==============================================================================
// This creates new instances of the plugin..
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new CABTRAudioProcessor();
}
