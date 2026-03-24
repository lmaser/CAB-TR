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
	// Convolution engines (StereoThreadedConvolver) — prepared in prepareToPlay()
	
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
	layout.add (std::make_unique<juce::AudioParameterChoice> (
		kParamModeInA, "Mode In A", juce::StringArray { "L+R", "MID", "SIDE" }, kModeDefault));
	layout.add (std::make_unique<juce::AudioParameterChoice> (
		kParamModeOutA, "Mode Out A", juce::StringArray { "L+R", "MID", "SIDE" }, kModeDefault));
	layout.add (std::make_unique<juce::AudioParameterChoice> (
		kParamSumBusA, "Sum Bus A", juce::StringArray { "ST", u8"\u2192M", u8"\u2192S" }, kSumBusDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamMixA, "Mix A", kGlobalMixMin, kGlobalMixMax, kGlobalMixDefault));

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
	layout.add (std::make_unique<juce::AudioParameterChoice> (
		kParamModeInB, "Mode In B", juce::StringArray { "L+R", "MID", "SIDE" }, kModeDefault));
	layout.add (std::make_unique<juce::AudioParameterChoice> (
		kParamModeOutB, "Mode Out B", juce::StringArray { "L+R", "MID", "SIDE" }, kModeDefault));
	layout.add (std::make_unique<juce::AudioParameterChoice> (
		kParamSumBusB, "Sum Bus B", juce::StringArray { "ST", u8"\u2192M", u8"\u2192S" }, kSumBusDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamMixB, "Mix B", kGlobalMixMin, kGlobalMixMax, kGlobalMixDefault));

	// ══════════════════════════════════════════════════════════════
	//  IR Loader C Parameters
	// ══════════════════════════════════════════════════════════════
	layout.add (std::make_unique<juce::AudioParameterBool> (
		kParamEnableC, "Enable C", false));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamHpFreqC, "HP Freq C", 
		juce::NormalisableRange<float> (kFilterFreqMin, kFilterFreqMax, 1.0f, 0.3f), 
		kFilterHpFreqDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamLpFreqC, "LP Freq C", 
		juce::NormalisableRange<float> (kFilterFreqMin, kFilterFreqMax, 1.0f, 0.3f), 
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
		kParamOutC, "Out C", kOutMin, kOutMax, kOutDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamStartC, "Start C",
		juce::NormalisableRange<float> (kStartMin, kStartMax, 0.1f, 0.15f),
		kStartDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamEndC, "End C",
		juce::NormalisableRange<float> (kEndMin, kEndMax, 0.1f, 0.15f),
		kEndDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamPitchC, "Pitch C", 
		juce::NormalisableRange<float> (kPitchMin, kPitchMax, 0.01f, 0.5f), 
		kPitchDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamDelayC, "Delay C",
		juce::NormalisableRange<float> (kDelayMin, kDelayMax, 0.001f, 0.3f),
		kDelayDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamPanC, "Pan C", kPanMin, kPanMax, kPanDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamFredC, "Angle C", kFredMin, kFredMax, kFredDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamPosC, "Distance C", kPosMin, kPosMax, kPosDefault));
	layout.add (std::make_unique<juce::AudioParameterBool> (
		kParamInvC, "Invert C", false));
	layout.add (std::make_unique<juce::AudioParameterBool> (
		kParamNormC, "Normalize C", false));
	layout.add (std::make_unique<juce::AudioParameterBool> (
		kParamRvsC, "Reverse C", false));
	layout.add (std::make_unique<juce::AudioParameterBool> (
		kParamChaosC, "Chaos C", false));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamChaosAmtC, "Chaos Amount C",
		juce::NormalisableRange<float> (kChaosAmtMin, kChaosAmtMax, 0.1f), kChaosAmtDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamChaosSpdC, "Chaos Speed C",
		juce::NormalisableRange<float> (kChaosSpdMin, kChaosSpdMax, 0.01f, 0.3f), kChaosSpdDefault));
	layout.add (std::make_unique<juce::AudioParameterChoice> (
		kParamModeInC, "Mode In C", juce::StringArray { "L+R", "MID", "SIDE" }, kModeDefault));
	layout.add (std::make_unique<juce::AudioParameterChoice> (
		kParamModeOutC, "Mode Out C", juce::StringArray { "L+R", "MID", "SIDE" }, kModeDefault));
	layout.add (std::make_unique<juce::AudioParameterChoice> (
		kParamSumBusC, "Sum Bus C", juce::StringArray { "ST", u8"\u2192M", u8"\u2192S" }, kSumBusDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamMixC, "Mix C", kGlobalMixMin, kGlobalMixMax, kGlobalMixDefault));

	// ══════════════════════════════════════════════════════════════
	//  Global Parameters
	// ══════════════════════════════════════════════════════════════
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamInput, "Input", kInputMin, kInputMax, kInputDefault));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamOutput, "Output", kOutputMin, kOutputMax, kOutputDefault));
	layout.add (std::make_unique<juce::AudioParameterChoice> (
		kParamRoute, "Route", juce::StringArray { "A>B>C", "A|B|C", "A>B|C", "A|B>C" }, kRouteDefault));
	layout.add (std::make_unique<juce::AudioParameterBool> (
		kParamAlign, "Align", false));
	layout.add (std::make_unique<juce::AudioParameterFloat> (
		kParamMix, "Mix", kGlobalMixMin, kGlobalMixMax, kGlobalMixDefault));
	layout.add (std::make_unique<juce::AudioParameterChoice> (
		kParamMatch, "Match", juce::StringArray { "None", "White", "Pink (-3dB)", "Brown (-6dB)", "Bright (+3dB)", "Bright+ (+6dB)" }, kMatchDefault));
	layout.add (std::make_unique<juce::AudioParameterChoice> (
		kParamTrim, "Norm", juce::StringArray { "Off", "0 dB", "-3 dB", "-6 dB", "-12 dB", "-18 dB" }, kTrimDefault));

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

	stateA.convolution.prepare (sampleRate, samplesPerBlock);
	stateB.convolution.prepare (sampleRate, samplesPerBlock);
	stateC.convolution.prepare (sampleRate, samplesPerBlock);

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
	tempBufferA.setSize (2, samplesPerBlock);
	tempBufferB.setSize (2, samplesPerBlock);
	tempBufferC.setSize (2, samplesPerBlock);
	globalDryBuffer.setSize (2, samplesPerBlock);
	loaderDryBuffer.setSize (2, samplesPerBlock);

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
	pDelayA  = parameters.getRawParameterValue (kParamDelayA);
	pPanA    = parameters.getRawParameterValue (kParamPanA);
	pFredA   = parameters.getRawParameterValue (kParamFredA);
	pPosA    = parameters.getRawParameterValue (kParamPosA);
	pOutA    = parameters.getRawParameterValue (kParamOutA);
	pHpFreqB = parameters.getRawParameterValue (kParamHpFreqB);
	pLpFreqB = parameters.getRawParameterValue (kParamLpFreqB);
	pHpOnB   = parameters.getRawParameterValue (kParamHpOnB);
	pLpOnB   = parameters.getRawParameterValue (kParamLpOnB);
	pHpSlopeB = parameters.getRawParameterValue (kParamHpSlopeB);
	pLpSlopeB = parameters.getRawParameterValue (kParamLpSlopeB);
	pDelayB  = parameters.getRawParameterValue (kParamDelayB);
	pPanB    = parameters.getRawParameterValue (kParamPanB);
	pFredB   = parameters.getRawParameterValue (kParamFredB);
	pPosB    = parameters.getRawParameterValue (kParamPosB);
	pOutB    = parameters.getRawParameterValue (kParamOutB);
	pChaosA    = parameters.getRawParameterValue (kParamChaosA);
	pChaosAmtA = parameters.getRawParameterValue (kParamChaosAmtA);
	pChaosSpdA = parameters.getRawParameterValue (kParamChaosSpdA);
	pModeInA   = parameters.getRawParameterValue (kParamModeInA);
	pModeOutA  = parameters.getRawParameterValue (kParamModeOutA);
	pSumBusA   = parameters.getRawParameterValue (kParamSumBusA);
	pChaosB    = parameters.getRawParameterValue (kParamChaosB);
	pChaosAmtB = parameters.getRawParameterValue (kParamChaosAmtB);
	pChaosSpdB = parameters.getRawParameterValue (kParamChaosSpdB);
	pModeInB   = parameters.getRawParameterValue (kParamModeInB);
	pModeOutB  = parameters.getRawParameterValue (kParamModeOutB);
	pSumBusB   = parameters.getRawParameterValue (kParamSumBusB);
	pMixA      = parameters.getRawParameterValue (kParamMixA);
	pMixB      = parameters.getRawParameterValue (kParamMixB);
	pEnableC   = parameters.getRawParameterValue (kParamEnableC);
	pHpFreqC   = parameters.getRawParameterValue (kParamHpFreqC);
	pLpFreqC   = parameters.getRawParameterValue (kParamLpFreqC);
	pHpOnC     = parameters.getRawParameterValue (kParamHpOnC);
	pLpOnC     = parameters.getRawParameterValue (kParamLpOnC);
	pHpSlopeC  = parameters.getRawParameterValue (kParamHpSlopeC);
	pLpSlopeC  = parameters.getRawParameterValue (kParamLpSlopeC);
	pDelayC    = parameters.getRawParameterValue (kParamDelayC);
	pPanC      = parameters.getRawParameterValue (kParamPanC);
	pFredC     = parameters.getRawParameterValue (kParamFredC);
	pPosC      = parameters.getRawParameterValue (kParamPosC);
	pOutC      = parameters.getRawParameterValue (kParamOutC);
	pChaosC    = parameters.getRawParameterValue (kParamChaosC);
	pChaosAmtC = parameters.getRawParameterValue (kParamChaosAmtC);
	pChaosSpdC = parameters.getRawParameterValue (kParamChaosSpdC);
	pModeInC   = parameters.getRawParameterValue (kParamModeInC);
	pModeOutC  = parameters.getRawParameterValue (kParamModeOutC);
	pSumBusC   = parameters.getRawParameterValue (kParamSumBusC);
	pMixC      = parameters.getRawParameterValue (kParamMixC);
	pMatch     = parameters.getRawParameterValue (kParamMatch);
	pTrim      = parameters.getRawParameterValue (kParamTrim);

	// Reset tilt EQ state
	tiltState_[0] = tiltState_[1] = 0.0f;
	tiltLastProfile_ = -1;

	// Reset wet NORM AGC state
	normPeakFollower_ = 0.0f;
	normSmoothedGain_ = 1.0f;

	// Reset FRED and CHAOS state for all loaders
	for (auto* state : { &stateA, &stateB, &stateC })
	{
		std::memset (state->fredDelayBuffer, 0, sizeof (state->fredDelayBuffer));
		state->fredDelayIndex = 0;
		std::memset (state->chaosDelayBuffer, 0, sizeof (state->chaosDelayBuffer));
		state->chaosDelayWritePos = 0;
		state->chaosCurrentTarget = 0.0f;
		state->chaosSmoothedValue = 0.0f;
		state->chaosPhaseSamples = 0.0f;
		state->chaosGainTarget = 0.0f;
		state->chaosGainSmoothed = 0.0f;
		state->chaosGainPhase = 0.0f;
		state->smoothedPosFreq = 12000.0f;
		state->filterCoeffCountdown = 0;
	}

	// Initialize EMA-smoothed filter frequencies to current parameter values
	std::atomic<float>* hpPtrs[] = { pHpFreqA, pHpFreqB, pHpFreqC };
	std::atomic<float>* lpPtrs[] = { pLpFreqA, pLpFreqB, pLpFreqC };
	IRLoaderState* states[] = { &stateA, &stateB, &stateC };
	for (int i = 0; i < 3; ++i)
	{
		states[i]->smoothedHpFreq = hpPtrs[i]->load();
		states[i]->smoothedLpFreq = lpPtrs[i]->load();
	}
}

void CABTRAudioProcessor::releaseResources()
{
	// Reset delay lines
	stateA.delayLine.reset();
	stateB.delayLine.reset();
	stateC.delayLine.reset();
	
	// Print debug stats
	DBG ("CAB-TR CPU Debug Stats:");
	DBG ("  Loader A reloads: " + juce::String (reloadCountA.load()));
	DBG ("  Loader B reloads: " + juce::String (reloadCountB.load()));
	DBG ("  Loader C reloads: " + juce::String (reloadCountC.load()));
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

static inline void injectMSBus (float l, float r, int bus,
                                float& stL, float& stR,
                                float& midBus, float& sideBus)
{
	if (bus == 0)      { stL += l; stR += r; }
	else if (bus == 1) { midBus += (l + r) * 0.5f; }
	else               { sideBus += (l - r) * 0.5f; }
}

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
	const bool enableC = pEnableC->load() > 0.5f;
	const int route = static_cast<int> (pRoute->load());
	const float globalMix = pMix->load();

	// Per-loader mode parameters
	const int modeInA  = static_cast<int> (pModeInA->load());
	const int modeOutA = static_cast<int> (pModeOutA->load());
	const int modeInB  = static_cast<int> (pModeInB->load());
	const int modeOutB = static_cast<int> (pModeOutB->load());
	const int modeInC  = static_cast<int> (pModeInC->load());
	const int modeOutC = static_cast<int> (pModeOutC->load());
	const int sumBusA  = static_cast<int> (pSumBusA->load());
	const int sumBusB  = static_cast<int> (pSumBusB->load());
	const int sumBusC  = static_cast<int> (pSumBusC->load());
	const float mixA = pMixA->load();
	const float mixB = pMixB->load();
	const float mixC = pMixC->load();

	// Apply input gain (SIMD optimized)
	const float inputGain = juce::Decibels::decibelsToGain (pInput->load());
	buffer.applyGain (inputGain);

	// Peak input measurement (post input gain)
	{
		float inPeak = 0.0f;
		for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
			inPeak = juce::jmax (inPeak, buffer.getMagnitude (ch, 0, numSamples));
		float cur = peakInputLevel.load();
		if (inPeak > cur) peakInputLevel.store (inPeak);
	}

	// Helper: apply M/S mode conversion to a buffer
	auto applyMode = [] (juce::AudioBuffer<float>& buf, int modeVal, int nSamples)
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
	};

	// Capture dry signal AFTER input gain, but BEFORE any loader processing
	// Used for global MIX: dry is unaffected by convolution, filters, mode, etc.
	const bool needsDry = (globalMix < 0.999f);
	if (needsDry)
	{
		for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
			globalDryBuffer.copyFrom (ch, 0, buffer, ch, 0, numSamples);
	}

	// ── Helper lambdas ──
	// Save dry copy before processing a loader (for per-loader mix)
	auto saveDry = [&] (const juce::AudioBuffer<float>& src)
	{
		for (int ch = 0; ch < src.getNumChannels(); ++ch)
			loaderDryBuffer.copyFrom (ch, 0, src, ch, 0, numSamples);
	};

	// Per-loader dry/wet blend (loaderDryBuffer must contain the pre-process signal)
	auto applyLoaderMix = [&] (juce::AudioBuffer<float>& buf, float mixVal)
	{
		if (mixVal < 0.999f)
		{
			const float wet = mixVal;
			const float dry = 1.0f - mixVal;
			for (int ch = 0; ch < buf.getNumChannels(); ++ch)
			{
				auto* wetData = buf.getWritePointer (ch);
				const auto* dryData = loaderDryBuffer.getReadPointer (ch);
				juce::FloatVectorOperations::multiply (wetData, wet, numSamples);
				juce::FloatVectorOperations::addWithMultiply (wetData, dryData, dry, numSamples);
			}
		}
	};

	// Process one loader with mode in/out and per-loader mix
	auto processOne = [&] (IRLoaderState& state, juce::AudioBuffer<float>& buf,
	                        int loaderIndex, int modeIn, int modeOut, float loaderMix)
	{
		const auto loaderStart = juce::Time::getHighResolutionTicks();
		saveDry (buf);
		applyMode (buf, modeIn, numSamples);
		processLoader (state, buf, loaderIndex);
		applyMode (buf, modeOut, numSamples);
		applyLoaderMix (buf, loaderMix);
		const double loaderUs = juce::Time::highResolutionTicksToSeconds (
			juce::Time::getHighResolutionTicks() - loaderStart) * 1e6;
		auto& worst = loaderIndex == 0 ? worstLoaderAUs : (loaderIndex == 1 ? worstLoaderBUs : worstLoaderCUs);
		double cur = worst.load();
		if (loaderUs > cur) worst.store (loaderUs);
	};

	// Count how many loaders are active (for parallel compensation)
	auto countEnabled = [&] (bool a, bool b, bool c) -> int
	{
		return (a ? 1 : 0) + (b ? 1 : 0) + (c ? 1 : 0);
	};

	// ── ROUTING ──
	// Route 0: A→B→C (full series)
	// Route 1: A|B|C  (all parallel)
	// Route 2: A→B|C  (series A→B, C parallel)
	// Route 3: A|B→C  (A parallel, series B→C)

	if (route == 1) // A|B|C — all parallel
	{
		const int numActive = countEnabled (enableA, enableB, enableC);
		if (numActive >= 2)
		{
			// Copy input into each temp buffer for active loaders
			if (enableA)
				for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
					tempBufferA.copyFrom (ch, 0, buffer, ch, 0, numSamples);
			if (enableB)
				for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
					tempBufferB.copyFrom (ch, 0, buffer, ch, 0, numSamples);
			if (enableC)
				for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
					tempBufferC.copyFrom (ch, 0, buffer, ch, 0, numSamples);

			if (enableA) processOne (stateA, tempBufferA, 0, modeInA, modeOutA, mixA);
			if (enableB) processOne (stateB, tempBufferB, 1, modeInB, modeOutB, mixB);
			if (enableC) processOne (stateC, tempBufferC, 2, modeInC, modeOutC, mixC);

			// Sum active buffers — M/S bus-aware
			const bool anyMSBus = (enableA && sumBusA != 0)
			                   || (enableB && sumBusB != 0)
			                   || (enableC && sumBusC != 0);

			if (anyMSBus && buffer.getNumChannels() >= 2)
			{
				// M/S bus routing: each loader contributes to ST, →M, or →S bus
				auto* outL = buffer.getWritePointer (0);
				auto* outR = buffer.getWritePointer (1);

				const float* srcL[3] = { nullptr, nullptr, nullptr };
				const float* srcR[3] = { nullptr, nullptr, nullptr };
				int buses[3] = { sumBusA, sumBusB, sumBusC };
				bool active[3] = { enableA, enableB, enableC };

				if (enableA) { srcL[0] = tempBufferA.getReadPointer (0); srcR[0] = tempBufferA.getReadPointer (1); }
				if (enableB) { srcL[1] = tempBufferB.getReadPointer (0); srcR[1] = tempBufferB.getReadPointer (1); }
				if (enableC) { srcL[2] = tempBufferC.getReadPointer (0); srcR[2] = tempBufferC.getReadPointer (1); }

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
				// Fast path: all ST — simple L+R addition (no M/S overhead)
				buffer.clear();
				if (enableA)
					for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
						juce::FloatVectorOperations::add (buffer.getWritePointer (ch), tempBufferA.getReadPointer (ch), numSamples);
				if (enableB)
					for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
						juce::FloatVectorOperations::add (buffer.getWritePointer (ch), tempBufferB.getReadPointer (ch), numSamples);
				if (enableC)
					for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
						juce::FloatVectorOperations::add (buffer.getWritePointer (ch), tempBufferC.getReadPointer (ch), numSamples);
			}

			// Parallel compensation: 1/sqrt(N)
			buffer.applyGain (1.0f / std::sqrt (static_cast<float> (numActive)));
		}
		else if (enableA)
			processOne (stateA, buffer, 0, modeInA, modeOutA, mixA);
		else if (enableB)
			processOne (stateB, buffer, 1, modeInB, modeOutB, mixB);
		else if (enableC)
			processOne (stateC, buffer, 2, modeInC, modeOutC, mixC);
	}
	else if (route == 2) // A→B|C — series A→B, C parallel
	{
		const bool seriesActive = enableA || enableB;
		if (seriesActive && enableC)
		{
			// Copy input for parallel path C
			for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
				tempBufferC.copyFrom (ch, 0, buffer, ch, 0, numSamples);
			// Series path: A→B stays in buffer
			if (enableA) processOne (stateA, buffer, 0, modeInA, modeOutA, mixA);
			if (enableB) processOne (stateB, buffer, 1, modeInB, modeOutB, mixB);
			// Parallel path C
			processOne (stateC, tempBufferC, 2, modeInC, modeOutC, mixC);
			// Sum both paths and compensate — M/S bus-aware
			// Series path bus = sumBusB (last in chain), parallel path bus = sumBusC
			const int seriesBus = sumBusB;
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
			buffer.applyGain (0.707106781f); // -3dB for 2 parallel paths
		}
		else
		{
			// Only one path active — series or C alone
			if (enableA) processOne (stateA, buffer, 0, modeInA, modeOutA, mixA);
			if (enableB) processOne (stateB, buffer, 1, modeInB, modeOutB, mixB);
			if (enableC) processOne (stateC, buffer, 2, modeInC, modeOutC, mixC);
		}
	}
	else if (route == 3) // A|B→C — A parallel, series B→C
	{
		const bool seriesActive = enableB || enableC;
		if (enableA && seriesActive)
		{
			// Copy input for parallel path A
			for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
				tempBufferA.copyFrom (ch, 0, buffer, ch, 0, numSamples);
			// Parallel path A
			processOne (stateA, tempBufferA, 0, modeInA, modeOutA, mixA);
			// Series path: B→C stays in buffer
			if (enableB) processOne (stateB, buffer, 1, modeInB, modeOutB, mixB);
			if (enableC) processOne (stateC, buffer, 2, modeInC, modeOutC, mixC);
			// Sum both paths and compensate — M/S bus-aware
			// Parallel path bus = sumBusA, series path bus = sumBusC (last in chain)
			const int parallelBus = sumBusA;
			const int seriesBus = sumBusC;
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
			buffer.applyGain (0.707106781f); // -3dB for 2 parallel paths
		}
		else
		{
			// Only one path active
			if (enableA) processOne (stateA, buffer, 0, modeInA, modeOutA, mixA);
			if (enableB) processOne (stateB, buffer, 1, modeInB, modeOutB, mixB);
			if (enableC) processOne (stateC, buffer, 2, modeInC, modeOutC, mixC);
		}
	}
	else // route == 0: A→B→C (full series)
	{
		if (enableA) processOne (stateA, buffer, 0, modeInA, modeOutA, mixA);
		if (enableB) processOne (stateB, buffer, 1, modeInB, modeOutB, mixB);
		if (enableC) processOne (stateC, buffer, 2, modeInC, modeOutC, mixC);
	}

	// ── MATCH: Adaptive tilt EQ based on IR spectral analysis ──
	// Measures the spectral slope of the combined IR (computed at load time),
	// then applies compensating tilt to match the selected target profile.
	// Target slopes: White=0, Pink=-3, Brown=-6, Bright=+3, Bright+=+6 dB/oct
	{
		const int matchProfile = static_cast<int> (pMatch->load());

		if (matchProfile != 0) // 0 = None
		{
			// Target dB/oct per profile
			static constexpr float kTargetSlopes[] = { 0.0f, 0.0f, -3.0f, -6.0f, 3.0f, 6.0f };
			const float targetSlope = kTargetSlopes[matchProfile];

			// Compute combined IR slope based on routing mode
			float combinedSlope = 0.0f;
			if (route == 0) // A→B→C series: slopes add
			{
				if (enableA) combinedSlope += stateA.irSlopeDbPerOct.load();
				if (enableB) combinedSlope += stateB.irSlopeDbPerOct.load();
				if (enableC) combinedSlope += stateC.irSlopeDbPerOct.load();
			}
			else if (route == 1) // A|B|C parallel: weighted average
			{
				float totalWeight = 0.0f;
				if (enableA) { combinedSlope += stateA.irSlopeDbPerOct.load() * mixA; totalWeight += mixA; }
				if (enableB) { combinedSlope += stateB.irSlopeDbPerOct.load() * mixB; totalWeight += mixB; }
				if (enableC) { combinedSlope += stateC.irSlopeDbPerOct.load() * mixC; totalWeight += mixC; }
				if (totalWeight > 0.0f) combinedSlope /= totalWeight;
			}
			else if (route == 2) // A→B | C
			{
				float seriesSlope = 0.0f;
				if (enableA) seriesSlope += stateA.irSlopeDbPerOct.load();
				if (enableB) seriesSlope += stateB.irSlopeDbPerOct.load();
				if (enableC)
					combinedSlope = (seriesSlope + stateC.irSlopeDbPerOct.load()) * 0.5f;
				else
					combinedSlope = seriesSlope;
			}
			else if (route == 3) // A | B→C
			{
				float seriesSlope = 0.0f;
				if (enableB) seriesSlope += stateB.irSlopeDbPerOct.load();
				if (enableC) seriesSlope += stateC.irSlopeDbPerOct.load();
				if (enableA)
					combinedSlope = (stateA.irSlopeDbPerOct.load() + seriesSlope) * 0.5f;
				else
					combinedSlope = seriesSlope;
			}

			// Compensating slope: what tilt we need to get from measured → target
			const float compensatingSlope = targetSlope - combinedSlope;

			// Update target coefficients when profile or slope changes
			if (matchProfile != tiltLastProfile_ || std::abs (compensatingSlope - tiltLastSlope_) > 0.05f)
			{
				tiltLastProfile_ = matchProfile;
				tiltLastSlope_ = compensatingSlope;

				if (std::abs (compensatingSlope) < 0.1f)
				{
					tiltTargetB0_ = 1.0f; tiltTargetB1_ = 0.0f; tiltTargetA1_ = 0.0f;
				}
				else
				{
					const double sr = getSampleRate();
					const double pivot = 1000.0;
					const double octavesToNyquist = std::log2 ((sr * 0.5) / pivot);
					const double gainAtNyquistDb = compensatingSlope * octavesToNyquist;
					const double gNy = std::pow (10.0, gainAtNyquistDb / 20.0);

					const double wc = 2.0 * sr * std::tan (juce::MathConstants<double>::pi * pivot / sr);
					const double K = wc / (2.0 * sr);
					const double g = std::sqrt (gNy);

					const double norm = 1.0 / (1.0 + K * g);
					tiltTargetB0_ = static_cast<float> ((g + K) * norm);
					tiltTargetB1_ = static_cast<float> ((K - g) * norm);
					tiltTargetA1_ = static_cast<float> ((K * g - 1.0) * norm);
				}
			}

			// Smooth coefficients towards target (~30ms ramp) to avoid zipper noise
			const float smoothCoeff = 1.0f - std::exp (-1.0f / (static_cast<float> (getSampleRate()) * 0.03f));

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

	// ── NORM: static peak-normalize wet signal to target level ──
	// Captures maximum peak (no release) and applies fixed gain.
	// Toggling NORM off/on resets the peak measurement.
	{
		const int normIdx = static_cast<int> (pTrim->load());
		if (normIdx > 0)
		{
			// Target peak levels: 0 dB, -3 dB, -6 dB, -12 dB, -18 dB
			static constexpr float kNormTargets[] = { 1.0f, 1.0f, 0.70795f, 0.50119f, 0.25119f, 0.12589f };
			const float target = kNormTargets[normIdx];

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
				const float desiredGain = target / normPeakFollower_;
				const float clampedGain = juce::jlimit (0.01f, 7.94f, desiredGain);

				// Smooth only towards lower gain (new louder peak detected)
				// to avoid click; ramp up is instant since peak only grows
				if (clampedGain < normSmoothedGain_)
				{
					const float coeff = 1.0f - std::exp (-1.0f / (static_cast<float> (getSampleRate()) * 0.01f / static_cast<float> (numSamples)));
					normSmoothedGain_ += (clampedGain - normSmoothedGain_) * coeff;
				}
				else
				{
					normSmoothedGain_ = clampedGain;
				}

				for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
					juce::FloatVectorOperations::multiply (buffer.getWritePointer (ch), normSmoothedGain_, numSamples);
			}
		}
		else
		{
			// Reset state when off — next enable will re-measure
			normPeakFollower_ = 0.0f;
			normSmoothedGain_ = 1.0f;
		}
	}

	// Global MIX: blend unprocessed dry with fully processed wet
	// dry = input after gain, wet = after all loader processing (mode + convolution + effects)
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

	// DC blocking filter (first-order high-pass ~5 Hz)
	// Removes DC offset introduced by convolution, pitch-shifted IRs, or filter artefacts.
	// y[n] = x[n] - x[n-1] + R * y[n-1],  R = 1 - 2*pi*fc/sr
	{
		const float R = 1.0f - (6.2831853f * 5.0f / static_cast<float> (getSampleRate()));
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

	// Apply output gain (SIMD optimized)
	const float outputGain = juce::Decibels::decibelsToGain (pOutput->load());
	buffer.applyGain (outputGain);

	// Safety hard-limiter: prevent catastrophic output only (NaN/Inf runaway).
	// Set very high (+48 dBFS) so it never engages during normal operation.
	{
		constexpr float kSafetyLimit = 251.19f; // +48 dBFS — only catches runaways
		for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
		{
			auto* data = buffer.getWritePointer (ch);
			juce::FloatVectorOperations::clip (data, data, -kSafetyLimit, kSafetyLimit, numSamples);
		}
	}

	// Peak output + clip detection (post output gain)
	{
		float outPeak = 0.0f;
		for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
			outPeak = juce::jmax (outPeak, buffer.getMagnitude (ch, 0, numSamples));
		float cur = peakOutputLevel.load();
		if (outPeak > cur) peakOutputLevel.store (outPeak);
		if (outPeak > 1.0f) clipCount++;
	}
	
	// CPU profiling: track worst-case block time
	const auto blockEndTime = juce::Time::getHighResolutionTicks();
	const double blockTimeUs = juce::Time::highResolutionTicksToSeconds (blockEndTime - blockStartTime) * 1e6;
	double currentWorst = worstBlockTimeUs.load();
	if (blockTimeUs > currentWorst)
		worstBlockTimeUs.store (blockTimeUs);
	blockCount++;
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
			magnitudeDb[static_cast<size_t> (bin)] += (mag > 1e-10f)
				? 20.0f * std::log10 (mag)
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

	// Load audio file (reusable format manager — no re-registration per load)
	std::unique_ptr<juce::AudioFormatReader> reader (
		formatManager.createReaderFor (irFile));

	if (reader == nullptr)
		return;

	// Smart read: only read the samples we actually need (avoids loading 480K samples to discard 99%)
	// 1. Calculate trim boundaries first, then read only that range + margin for pitch
	const int loaderIdx = (&state == &stateA) ? 0 : ((&state == &stateB) ? 1 : 2);
	const char* loaderLabel = loaderIdx == 0 ? "A" : (loaderIdx == 1 ? "B" : "C");
	auto pickParam = [&] (const char* a, const char* b, const char* c) -> const char*
		{ return loaderIdx == 0 ? a : (loaderIdx == 1 ? b : c); };
	const float startMs_pre = parameters.getRawParameterValue (pickParam (kParamStartA, kParamStartB, kParamStartC))->load();
	const float endMs_pre = parameters.getRawParameterValue (pickParam (kParamEndA, kParamEndB, kParamEndC))->load();
	const float pitch_pre = parameters.getRawParameterValue (pickParam (kParamPitchA, kParamPitchB, kParamPitchC))->load();

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
	const char* startParam = pickParam (kParamStartA, kParamStartB, kParamStartC);
	const char* endParam = pickParam (kParamEndA, kParamEndB, kParamEndC);
	const char* pitchParam = pickParam (kParamPitchA, kParamPitchB, kParamPitchC);
	const char* invParam = pickParam (kParamInvA, kParamInvB, kParamInvC);
	const char* normParam = pickParam (kParamNormA, kParamNormB, kParamNormC);
	const char* rvsParam = pickParam (kParamRvsA, kParamRvsB, kParamRvsC);
	
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

	// Measure spectral slope for adaptive Match tilt EQ
	state.irSlopeDbPerOct.store (computeIRSlopeDbPerOct (state.impulseResponse, reader->sampleRate));
	LOG_IR_EVENT ("IR spectral slope: " + juce::String (state.irSlopeDbPerOct.load(), 2) + " dB/oct");

	// Load into convolution engine
	// NonUniform{256} uses partitioned FFT — handles long IRs efficiently
	LOG_IR_EVENT ("IR final length: " + juce::String (state.impulseResponse.getNumSamples()) +
	              " samples (" + juce::String (state.impulseResponse.getNumSamples() / reader->sampleRate * 1000.0, 1) + " ms)");

	// Load IR into two-stage threaded convolver (head on audio thread, tail on background)
	// We pass state.impulseResponse directly — loadIR copies internally
	state.convolution.loadIR (state.impulseResponse, reader->sampleRate);
	
	// Update cached parameter values AFTER successful load
	state.lastPitch.store (pitch);
	state.lastInv.store (invert);
	state.lastNorm.store (normalize);
	state.lastRvs.store (reverse);
	state.lastStart.store (startMs);
	state.lastEnd.store (endMs);
	
	// Increment reload counter
	if (loaderIdx == 0)
		reloadCountA++;
	else if (loaderIdx == 1)
		reloadCountB++;
	else
		reloadCountC++;

	state.currentFilePath = filePath;
	state.irSampleRate = reader->sampleRate;
	state.needsUpdate = false;

	LOG_IR_EVENT ("IR Reload " + juce::String (loaderLabel) + ": " + 
	     juce::String (finalLength) + " samples (" +
	     juce::String (finalLength / reader->sampleRate * 1000.0, 1) + "ms)" +
	     ", pitch=" + juce::String (pitch, 2) +
	     ", inv=" + juce::String (invert ? 1 : 0) + ", rvs=" + juce::String (reverse ? 1 : 0) +
	     ", norm=" + juce::String (normalize ? 1 : 0));
}

//==============================================================================
// EXPORT COMBINED IR — Offline capture through full processing chain
// Generates a unit impulse, processes it through both loaders (routing, filters,
// DIST, PAN, DELAY, ANGLE, mode in/out, mix, gains) except CHAOS.
// formatType: 0=WAV16, 1=WAV24, 2=WAV32f, 3=AIFF24, 4=FLAC24
//==============================================================================

// FFT-based linear convolution (for series routing: A output → B convolution)
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

bool CABTRAudioProcessor::exportCombinedIR (double targetSampleRate,
                                             int formatType, double maxLengthSec,
                                             bool trimSilence,
                                             bool normalizeOutput,
                                             const juce::File& outputFile)
{
	// Read current global parameters
	const bool enableA = pEnableA->load() > 0.5f;
	const bool enableB = pEnableB->load() > 0.5f;
	const bool enableC = pEnableC->load() > 0.5f;
	const int route = static_cast<int> (pRoute->load());
	const float globalMix = pMix->load();
	const float inputGainDb = pInput->load();
	const float outputGainDb = pOutput->load();
	const int modeInA  = static_cast<int> (pModeInA->load());
	const int modeOutA = static_cast<int> (pModeOutA->load());
	const int modeInB  = static_cast<int> (pModeInB->load());
	const int modeOutB = static_cast<int> (pModeOutB->load());
	const int modeInC  = static_cast<int> (pModeInC->load());
	const int modeOutC = static_cast<int> (pModeOutC->load());

	// Need at least one enabled loader with an IR
	const bool hasA = enableA && stateA.impulseResponse.getNumSamples() > 0;
	const bool hasB = enableB && stateB.impulseResponse.getNumSamples() > 0;
	const bool hasC = enableC && stateC.impulseResponse.getNumSamples() > 0;
	if (! hasA && ! hasB && ! hasC)
		return false;

	const double workingSR = currentSampleRate;
	const int maxWorkingSamples = static_cast<int> (maxLengthSec * workingSR) + 1;

	const int irLenA = hasA ? stateA.impulseResponse.getNumSamples() : 0;
	const int irLenB = hasB ? stateB.impulseResponse.getNumSamples() : 0;
	const int irLenC = hasC ? stateC.impulseResponse.getNumSamples() : 0;

	// Helper: prepare a loader's IR into a stereo buffer
	auto prepareLoaderBuf = [&] (IRLoaderState& state, int irLen, int workLen) -> juce::AudioBuffer<float>
	{
		juce::AudioBuffer<float> buf (2, workLen);
		buf.clear();
		for (int ch = 0; ch < juce::jmin (2, state.impulseResponse.getNumChannels()); ++ch)
			buf.copyFrom (ch, 0, state.impulseResponse, ch, 0, juce::jmin (irLen, workLen));
		if (state.impulseResponse.getNumChannels() == 1 && buf.getNumChannels() >= 2)
			buf.copyFrom (1, 0, buf, 0, 0, workLen);
		return buf;
	};

	// Determine working length based on routing
	int workingLen = 0;
	if (route == 0) // A→B→C series: convolution chain
	{
		int len = juce::jmax (irLenA, 1);
		if (hasA && hasB) len = irLenA + irLenB - 1;
		if ((hasA || hasB) && hasC) len = len + irLenC - 1;
		workingLen = juce::jmax (len, juce::jmax (irLenA, juce::jmax (irLenB, irLenC)));
	}
	else if (route == 1) // A|B|C parallel
		workingLen = juce::jmax (irLenA, juce::jmax (irLenB, irLenC));
	else if (route == 2) // A→B|C
	{
		int seriesLen = (hasA && hasB) ? (irLenA + irLenB - 1) : juce::jmax (irLenA, irLenB);
		workingLen = juce::jmax (seriesLen, irLenC);
	}
	else // route == 3: A|B→C
	{
		int seriesLen = (hasB && hasC) ? (irLenB + irLenC - 1) : juce::jmax (irLenB, irLenC);
		workingLen = juce::jmax (irLenA, seriesLen);
	}

	// Add headroom for delay
	const float maxDelayMs = juce::jmax (
		hasA ? std::abs (pDelayA->load()) : 0.0f,
		juce::jmax (
			hasB ? std::abs (pDelayB->load()) : 0.0f,
			hasC ? std::abs (pDelayC->load()) : 0.0f));
	workingLen += static_cast<int> (maxDelayMs * 0.001 * workingSR) + 1;
	workingLen = juce::jmin (workingLen, maxWorkingSamples);

	// ── Build the combined IR offline ──
	juce::AudioBuffer<float> result;

	if (route == 1) // A|B|C parallel
	{
		int numActive = 0;
		result.setSize (2, workingLen);
		result.clear();

		if (hasA)
		{
			auto bufA = prepareLoaderBuf (stateA, irLenA, workingLen);
			offlineProcessLoaderEffects (bufA, 0, workingSR, modeInA, modeOutA);
			for (int ch = 0; ch < 2; ++ch)
				juce::FloatVectorOperations::add (result.getWritePointer (ch), bufA.getReadPointer (ch), workingLen);
			numActive++;
		}
		if (hasB)
		{
			auto bufB = prepareLoaderBuf (stateB, irLenB, workingLen);
			offlineProcessLoaderEffects (bufB, 1, workingSR, modeInB, modeOutB);
			for (int ch = 0; ch < 2; ++ch)
				juce::FloatVectorOperations::add (result.getWritePointer (ch), bufB.getReadPointer (ch), workingLen);
			numActive++;
		}
		if (hasC)
		{
			auto bufC = prepareLoaderBuf (stateC, irLenC, workingLen);
			offlineProcessLoaderEffects (bufC, 2, workingSR, modeInC, modeOutC);
			for (int ch = 0; ch < 2; ++ch)
				juce::FloatVectorOperations::add (result.getWritePointer (ch), bufC.getReadPointer (ch), workingLen);
			numActive++;
		}
		if (numActive >= 2)
			result.applyGain (1.0f / std::sqrt (static_cast<float> (numActive)));
	}
	else if (route == 2) // A→B|C
	{
		// Series path: A→B
		juce::AudioBuffer<float> seriesPath (2, workingLen);
		seriesPath.clear();
		bool seriesHasContent = false;

		if (hasA)
		{
			seriesPath = prepareLoaderBuf (stateA, irLenA, workingLen);
			offlineProcessLoaderEffects (seriesPath, 0, workingSR, modeInA, modeOutA);
			seriesHasContent = true;
		}
		if (hasB)
		{
			if (seriesHasContent)
			{
				auto convolved = fftConvolve (seriesPath, stateB.impulseResponse);
				int convLen = juce::jmin (convolved.getNumSamples(), workingLen);
				seriesPath.clear();
				for (int ch = 0; ch < juce::jmin (2, convolved.getNumChannels()); ++ch)
					seriesPath.copyFrom (ch, 0, convolved, ch, 0, convLen);
				if (convolved.getNumChannels() == 1) seriesPath.copyFrom (1, 0, seriesPath, 0, 0, workingLen);
			}
			else
				seriesPath = prepareLoaderBuf (stateB, irLenB, workingLen);
			offlineProcessLoaderEffects (seriesPath, 1, workingSR, modeInB, modeOutB);
			seriesHasContent = true;
		}

		// Parallel path: C
		if (hasC && seriesHasContent)
		{
			auto bufC = prepareLoaderBuf (stateC, irLenC, workingLen);
			offlineProcessLoaderEffects (bufC, 2, workingSR, modeInC, modeOutC);
			result.setSize (2, workingLen);
			result.clear();
			for (int ch = 0; ch < 2; ++ch)
			{
				juce::FloatVectorOperations::add (result.getWritePointer (ch), seriesPath.getReadPointer (ch), workingLen);
				juce::FloatVectorOperations::add (result.getWritePointer (ch), bufC.getReadPointer (ch), workingLen);
			}
			result.applyGain (0.707106781f);
		}
		else if (hasC)
		{
			result = prepareLoaderBuf (stateC, irLenC, workingLen);
			offlineProcessLoaderEffects (result, 2, workingSR, modeInC, modeOutC);
		}
		else
			result = std::move (seriesPath);
	}
	else if (route == 3) // A|B→C
	{
		// Series path: B→C
		juce::AudioBuffer<float> seriesPath (2, workingLen);
		seriesPath.clear();
		bool seriesHasContent = false;

		if (hasB)
		{
			seriesPath = prepareLoaderBuf (stateB, irLenB, workingLen);
			offlineProcessLoaderEffects (seriesPath, 1, workingSR, modeInB, modeOutB);
			seriesHasContent = true;
		}
		if (hasC)
		{
			if (seriesHasContent)
			{
				auto convolved = fftConvolve (seriesPath, stateC.impulseResponse);
				int convLen = juce::jmin (convolved.getNumSamples(), workingLen);
				seriesPath.clear();
				for (int ch = 0; ch < juce::jmin (2, convolved.getNumChannels()); ++ch)
					seriesPath.copyFrom (ch, 0, convolved, ch, 0, convLen);
				if (convolved.getNumChannels() == 1) seriesPath.copyFrom (1, 0, seriesPath, 0, 0, workingLen);
			}
			else
				seriesPath = prepareLoaderBuf (stateC, irLenC, workingLen);
			offlineProcessLoaderEffects (seriesPath, 2, workingSR, modeInC, modeOutC);
			seriesHasContent = true;
		}

		// Parallel path: A
		if (hasA && seriesHasContent)
		{
			auto bufA = prepareLoaderBuf (stateA, irLenA, workingLen);
			offlineProcessLoaderEffects (bufA, 0, workingSR, modeInA, modeOutA);
			result.setSize (2, workingLen);
			result.clear();
			for (int ch = 0; ch < 2; ++ch)
			{
				juce::FloatVectorOperations::add (result.getWritePointer (ch), bufA.getReadPointer (ch), workingLen);
				juce::FloatVectorOperations::add (result.getWritePointer (ch), seriesPath.getReadPointer (ch), workingLen);
			}
			result.applyGain (0.707106781f);
		}
		else if (hasA)
		{
			result = prepareLoaderBuf (stateA, irLenA, workingLen);
			offlineProcessLoaderEffects (result, 0, workingSR, modeInA, modeOutA);
		}
		else
			result = std::move (seriesPath);
	}
	else // route == 0: A→B→C full series
	{
		result.setSize (2, workingLen);
		result.clear();
		bool hasContent = false;

		if (hasA)
		{
			result = prepareLoaderBuf (stateA, irLenA, workingLen);
			offlineProcessLoaderEffects (result, 0, workingSR, modeInA, modeOutA);
			hasContent = true;
		}
		if (hasB)
		{
			if (hasContent)
			{
				auto convolved = fftConvolve (result, stateB.impulseResponse);
				int convLen = juce::jmin (convolved.getNumSamples(), workingLen);
				result.clear();
				for (int ch = 0; ch < juce::jmin (2, convolved.getNumChannels()); ++ch)
					result.copyFrom (ch, 0, convolved, ch, 0, convLen);
				if (convolved.getNumChannels() == 1) result.copyFrom (1, 0, result, 0, 0, workingLen);
			}
			else
			{
				result = prepareLoaderBuf (stateB, irLenB, workingLen);
			}
			offlineProcessLoaderEffects (result, 1, workingSR, modeInB, modeOutB);
			hasContent = true;
		}
		if (hasC)
		{
			if (hasContent)
			{
				auto convolved = fftConvolve (result, stateC.impulseResponse);
				int convLen = juce::jmin (convolved.getNumSamples(), workingLen);
				result.clear();
				for (int ch = 0; ch < juce::jmin (2, convolved.getNumChannels()); ++ch)
					result.copyFrom (ch, 0, convolved, ch, 0, convLen);
				if (convolved.getNumChannels() == 1) result.copyFrom (1, 0, result, 0, 0, workingLen);
			}
			else
			{
				result = prepareLoaderBuf (stateC, irLenC, workingLen);
			}
			offlineProcessLoaderEffects (result, 2, workingSR, modeInC, modeOutC);
		}
	}

	const int numSamples = result.getNumSamples();

	// ── Apply global processing ──

	// Input gain
	if (std::abs (inputGainDb) > 0.01f)
		result.applyGain (juce::Decibels::decibelsToGain (inputGainDb));

	// ── MATCH tilt EQ (offline) ──
	{
		const int matchProfile = static_cast<int> (pMatch->load());
		if (matchProfile != 0)
		{
			static constexpr float kTargetSlopes[] = { 0.0f, 0.0f, -3.0f, -6.0f, 3.0f, 6.0f };
			const float targetSlope = kTargetSlopes[matchProfile];

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
				const float mA = hasA ? std::abs (pOutA->load()) : 0.0f;
				const float mB = hasB ? std::abs (pOutB->load()) : 0.0f;
				const float mC = hasC ? std::abs (pOutC->load()) : 0.0f;
				if (hasA) { combinedSlope += stateA.irSlopeDbPerOct.load() * mA; totalWeight += mA; }
				if (hasB) { combinedSlope += stateB.irSlopeDbPerOct.load() * mB; totalWeight += mB; }
				if (hasC) { combinedSlope += stateC.irSlopeDbPerOct.load() * mC; totalWeight += mC; }
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

			const float compensatingSlope = targetSlope - combinedSlope;

			if (std::abs (compensatingSlope) >= 0.1f)
			{
				const double pivot = 1000.0;
				const double octavesToNyquist = std::log2 ((workingSR * 0.5) / pivot);
				const double gainAtNyquistDb = compensatingSlope * octavesToNyquist;
				const double gNy = std::pow (10.0, gainAtNyquistDb / 20.0);

				const double wc = 2.0 * workingSR * std::tan (juce::MathConstants<double>::pi * pivot / workingSR);
				const double K = wc / (2.0 * workingSR);
				const double g = std::sqrt (gNy);

				const double norm = 1.0 / (1.0 + K * g);
				const float b0 = static_cast<float> ((g + K) * norm);
				const float b1 = static_cast<float> ((K - g) * norm);
				const float a1 = static_cast<float> ((K * g - 1.0) * norm);

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
		}
	}

	// ── NORM: apply peak normalization to wet signal ──
	{
		const int normIdx = static_cast<int> (pTrim->load());
		if (normIdx > 0)
		{
			static constexpr float kNormTargets[] = { 1.0f, 1.0f, 0.70795f, 0.50119f, 0.25119f, 0.12589f };
			const float target = kNormTargets[normIdx];

			float peak = 0.0f;
			for (int ch = 0; ch < result.getNumChannels(); ++ch)
				peak = std::max (peak, result.getMagnitude (ch, 0, numSamples));

			if (peak > 0.001f)
			{
				const float gain = juce::jlimit (0.01f, 7.94f, target / peak);
				result.applyGain (gain);
			}
		}
	}

	// Global MIX: blend impulse (dry) with processed (wet)
	if (globalMix < 0.999f)
	{
		const float wet = globalMix;
		const float dry = 1.0f - globalMix;

		// Dry = unit impulse scaled by inputGain (already applied above → just a spike at sample 0)
		// Build dry signal: impulse at sample 0 with inputGain already baked
		for (int ch = 0; ch < result.getNumChannels(); ++ch)
		{
			auto* data = result.getWritePointer (ch);
			// Dry signal = input-gained unit impulse at sample 0 (no mode conversion on dry)
			float dryImpulse = 1.0f;
			if (std::abs (inputGainDb) > 0.01f)
				dryImpulse *= juce::Decibels::decibelsToGain (inputGainDb);

			for (int i = 0; i < numSamples; ++i)
				data[i] *= wet;
			if (numSamples > 0)
				data[0] += dry * dryImpulse;
		}
	}

	// Output gain
	if (std::abs (outputGainDb) > 0.01f)
		result.applyGain (juce::Decibels::decibelsToGain (outputGainDb));

	// ── Resample if target SR differs ──
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

	// Normalize to 0dB peak if requested
	if (normalizeOutput)
	{
		float peak = 0.0f;
		for (int ch = 0; ch < result.getNumChannels(); ++ch)
			peak = juce::jmax (peak, result.getMagnitude (ch, 0, result.getNumSamples()));
		if (peak > 0.0001f)
			result.applyGain (1.0f / peak);
	}

	// ── Write to file ──
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
// Offline loader effects: HP → LP → DIST → PAN → DELAY → ANGLE → OUT gain
// Uses temporary filter instances (no audio-thread state mutation).
// CHAOS is intentionally skipped (non-deterministic modulation).
//==============================================================================
void CABTRAudioProcessor::offlineProcessLoaderEffects (juce::AudioBuffer<float>& buffer,
                                                        int loaderIndex, double sampleRate,
                                                        int modeIn, int modeOut)
{
	const int numSamples = buffer.getNumSamples();
	const int numChannels = buffer.getNumChannels();
	const float maxFreq = static_cast<float> (sampleRate) * 0.49f;

	// Apply MODE IN before processing
	if ((modeIn == 1 || modeIn == 2) && numChannels >= 2)
	{
		auto* L = buffer.getWritePointer (0);
		auto* R = buffer.getWritePointer (1);
		for (int i = 0; i < numSamples; ++i)
		{
			const float l = L[i];
			const float r = R[i];
			if (modeIn == 1)
			{
				const float mid = (l + r) * 0.707106781f;
				L[i] = R[i] = mid;
			}
			else
			{
				const float side = (l - r) * 0.707106781f;
				L[i] = R[i] = side;
			}
		}
	}

	auto pick = [&] (std::atomic<float>* a, std::atomic<float>* b, std::atomic<float>* c)
		-> std::atomic<float>* { return loaderIndex == 0 ? a : (loaderIndex == 1 ? b : c); };

	const float hpFreq = pick (pHpFreqA, pHpFreqB, pHpFreqC)->load();
	const float lpFreq = pick (pLpFreqA, pLpFreqB, pLpFreqC)->load();
	const float delayMs = pick (pDelayA, pDelayB, pDelayC)->load();
	const float pan = pick (pPanA, pPanB, pPanC)->load();
	const float fred = pick (pFredA, pFredB, pFredC)->load();
	const float pos = pick (pPosA, pPosB, pPosC)->load();
	const float outDb = pick (pOutA, pOutB, pOutC)->load();

	juce::dsp::ProcessSpec spec;
	spec.sampleRate = sampleRate;
	spec.maximumBlockSize = static_cast<juce::uint32> (numSamples);
	spec.numChannels = static_cast<juce::uint32> (numChannels);

	// HP filter
	if (hpFreq >= 21.0f)
	{
		juce::dsp::ProcessorDuplicator<juce::dsp::IIR::Filter<float>,
		                                juce::dsp::IIR::Coefficients<float>> hp;
		hp.prepare (spec);
		*hp.state = *juce::dsp::IIR::Coefficients<float>::makeHighPass (
			sampleRate, juce::jlimit (20.0f, maxFreq, hpFreq), 0.707f);
		juce::dsp::AudioBlock<float> block (buffer);
		juce::dsp::ProcessContextReplacing<float> ctx (block);
		hp.process (ctx);
	}

	// LP filter
	if (lpFreq <= 19900.0f)
	{
		juce::dsp::ProcessorDuplicator<juce::dsp::IIR::Filter<float>,
		                                juce::dsp::IIR::Coefficients<float>> lp;
		lp.prepare (spec);
		*lp.state = *juce::dsp::IIR::Coefficients<float>::makeLowPass (
			sampleRate, juce::jlimit (20.0f, maxFreq, lpFreq), 0.707f);
		juce::dsp::AudioBlock<float> block (buffer);
		juce::dsp::ProcessContextReplacing<float> ctx (block);
		lp.process (ctx);
	}

	// DIST (exponential LPF + gain attenuation)
	if (pos > 0.01f)
	{
		const float cutoff = 12000.0f * std::exp (-pos * 2.0794f);
		juce::dsp::ProcessorDuplicator<juce::dsp::IIR::Filter<float>,
		                                juce::dsp::IIR::Coefficients<float>> posF;
		posF.prepare (spec);
		*posF.state = *juce::dsp::IIR::Coefficients<float>::makeLowPass (
			sampleRate, juce::jlimit (200.0f, maxFreq, cutoff), 0.707f);
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

	// DELAY (offline: shift samples right)
	if (delayMs > 0.1f)
	{
		const int delaySamples = static_cast<int> (delayMs * 0.001f * static_cast<float> (sampleRate) + 0.5f);
		if (delaySamples > 0 && delaySamples < numSamples)
		{
			for (int ch = 0; ch < numChannels; ++ch)
			{
				auto* data = buffer.getWritePointer (ch);
				std::memmove (data + delaySamples, data,
				              sizeof (float) * static_cast<size_t> (numSamples - delaySamples));
				std::memset (data, 0, sizeof (float) * static_cast<size_t> (delaySamples));
			}
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

	// OUT gain
	if (std::abs (outDb) > 0.01f)
		buffer.applyGain (juce::Decibels::decibelsToGain (outDb));

	// Apply MODE OUT after processing
	if ((modeOut == 1 || modeOut == 2) && numChannels >= 2)
	{
		auto* L = buffer.getWritePointer (0);
		auto* R = buffer.getWritePointer (1);
		for (int i = 0; i < numSamples; ++i)
		{
			const float l = L[i];
			const float r = R[i];
			if (modeOut == 1)
			{
				const float mid = (l + r) * 0.707106781f;
				L[i] = R[i] = mid;
			}
			else
			{
				const float side = (l - r) * 0.707106781f;
				L[i] = R[i] = side;
			}
		}
	}
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
                                          int loaderIndex)
{
	const int numSamples = buffer.getNumSamples();
	const int numChannels = buffer.getNumChannels();
	
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
	// loaderIndex: 0=A, 1=B, 2=C
	auto pick = [&] (std::atomic<float>* a, std::atomic<float>* b, std::atomic<float>* c)
		-> std::atomic<float>* { return loaderIndex == 0 ? a : (loaderIndex == 1 ? b : c); };

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
	const bool chaosEnabled = pick (pChaosA, pChaosB, pChaosC)->load() > 0.5f;
	const float chaosAmt = pick (pChaosAmtA, pChaosAmtB, pChaosAmtC)->load();
	const float chaosSpd = pick (pChaosSpdA, pChaosSpdB, pChaosSpdC)->load();
	
	// (FRED processing happens after convolution + filters)
	
	// 1. CONVOLUTION (TwoStageFFTConvolver — head on audio thread, tail on background)
	{
		const auto convStart = juce::Time::getHighResolutionTicks();
		state.convolution.process (buffer);
		const double convUs = juce::Time::highResolutionTicksToSeconds (
			juce::Time::getHighResolutionTicks() - convStart) * 1e6;
		double curConv = worstConvTimeUs.load();
		if (convUs > curConv) worstConvTimeUs.store (convUs);

		// Sanitise: kill NaN/Inf/denormals that can leak from the engine
		// during IR swaps (crossfade transients) or extreme input.
		for (int ch = 0; ch < numChannels; ++ch)
		{
			auto* d = buffer.getWritePointer (ch);
			for (int i = 0; i < numSamples; ++i)
				if (! std::isfinite (d[i])) d[i] = 0.0f;
		}
	}
	
	// 2-3. HP + LP FILTERS with slope selection (6/12/24 dB/oct)
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

			// HP: recalc if smoothed frequency or slope changed
			const float clampedHp = juce::jlimit (20.0f, maxFreq, state.smoothedHpFreq);
			if (std::abs (clampedHp - state.lastHpFreq) > 0.01f || hpSlope != state.lastHpSlope)
			{
				if (hpSlope == 0) // 6 dB/oct — first-order
				{
					*state.hpFilter.state = *juce::dsp::IIR::Coefficients<float>::makeFirstOrderHighPass (
						currentSampleRate, clampedHp);
				}
				else if (hpSlope == 1) // 12 dB/oct — Butterworth biquad
				{
					*state.hpFilter.state = *juce::dsp::IIR::Coefficients<float>::makeHighPass (
						currentSampleRate, clampedHp, 0.70710678f);
				}
				else // 24 dB/oct — cascaded biquad pair
				{
					*state.hpFilter.state = *juce::dsp::IIR::Coefficients<float>::makeHighPass (
						currentSampleRate, clampedHp, kBW4_Q1);
					*state.hpFilter2.state = *juce::dsp::IIR::Coefficients<float>::makeHighPass (
						currentSampleRate, clampedHp, kBW4_Q2);
				}
				state.lastHpFreq = clampedHp;
				state.lastHpSlope = hpSlope;
			}

			// LP: recalc if smoothed frequency or slope changed
			const float clampedLp = juce::jlimit (20.0f, maxFreq, state.smoothedLpFreq);
			if (std::abs (clampedLp - state.lastLpFreq) > 0.01f || lpSlope != state.lastLpSlope)
			{
				if (lpSlope == 0) // 6 dB/oct — first-order
				{
					*state.lpFilter.state = *juce::dsp::IIR::Coefficients<float>::makeFirstOrderLowPass (
						currentSampleRate, clampedLp);
				}
				else if (lpSlope == 1) // 12 dB/oct — Butterworth biquad
				{
					*state.lpFilter.state = *juce::dsp::IIR::Coefficients<float>::makeLowPass (
						currentSampleRate, clampedLp, 0.70710678f);
				}
				else // 24 dB/oct — cascaded biquad pair
				{
					*state.lpFilter.state = *juce::dsp::IIR::Coefficients<float>::makeLowPass (
						currentSampleRate, clampedLp, kBW4_Q1);
					*state.lpFilter2.state = *juce::dsp::IIR::Coefficients<float>::makeLowPass (
						currentSampleRate, clampedLp, kBW4_Q2);
				}
				state.lastLpFreq = clampedLp;
				state.lastLpSlope = lpSlope;
			}
		}
	}

	// Apply HP filter (1 or 2 stages depending on slope)
	if (hpOn && state.smoothedHpFreq >= 21.0f)
	{
		juce::dsp::AudioBlock<float> block (buffer);
		juce::dsp::ProcessContextReplacing<float> context (block);
		state.hpFilter.process (context);
		if (hpSlope == 2) // 24 dB/oct: second stage
			state.hpFilter2.process (context);
	}

	// Apply LP filter (1 or 2 stages depending on slope)
	if (lpOn && state.smoothedLpFreq <= 19900.0f)
	{
		juce::dsp::AudioBlock<float> block (buffer);
		juce::dsp::ProcessContextReplacing<float> context (block);
		state.lpFilter.process (context);
		if (lpSlope == 2) // 24 dB/oct: second stage
			state.lpFilter2.process (context);
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
		applyDelay (buffer, delayMs, loaderIndex);
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
void CABTRAudioProcessor::applyDelay (juce::AudioBuffer<float>& buffer, float delayMs, int loaderIndex)
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
	auto& delayLine = loaderIndex == 0 ? stateA.delayLine : (loaderIndex == 1 ? stateB.delayLine : stateC.delayLine);
	auto& smoother  = loaderIndex == 0 ? stateA.smoothedDelay : (loaderIndex == 1 ? stateB.smoothedDelay : stateC.smoothedDelay);
	
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
	// Only consider loaders that are both loaded AND enabled
	const bool enabledA = parameters.getRawParameterValue (kParamEnableA)->load() > 0.5f;
	const bool enabledB = parameters.getRawParameterValue (kParamEnableB)->load() > 0.5f;
	const bool enabledC = parameters.getRawParameterValue (kParamEnableC)->load() > 0.5f;

	const bool hasA = enabledA && ! stateA.currentFilePath.isEmpty() && stateA.impulseResponse.getNumSamples() > 0;
	const bool hasB = enabledB && ! stateB.currentFilePath.isEmpty() && stateB.impulseResponse.getNumSamples() > 0;
	const bool hasC = enabledC && ! stateC.currentFilePath.isEmpty() && stateC.impulseResponse.getNumSamples() > 0;

	if (! hasA || (! hasB && ! hasC))
	{
		LOG_IR_EVENT ("ALIGN: skipped — need A enabled + at least B or C enabled & loaded");
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
	                            int bestLag, float bestCorr, bool currentInv, bool currentInvA, const char* label)
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
		// If bestLag < 0, X is ahead of A — we add delay to X by abs(bestLag).
		// If bestLag > 0, A is ahead of X — we add delay to X.
		// Actually the convention: positive lag = B needs to be delayed, negative = A needs delay.
		// For multi-loader: we apply delay only to B/C. If A needs delay, we apply to A separately.
		if (auto* p = parameters.getParameter (delayId))
			p->setValueNotifyingHost (p->convertTo0to1 (bestLag > 0 ? clampedMs : 0.0f));

		if (auto* p = parameters.getParameter (invId))
			p->setValueNotifyingHost (needsInvert ? 1.0f : 0.0f);

		LOG_IR_EVENT ("ALIGN " + juce::String (label) + ": lag=" + juce::String (bestLag) +
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
	// We track the largest negative lag (A ahead → need delay on A's side).
	int aDelaySamples = 0; // positive = A needs this many samples of delay

	if (hasB)
	{
		auto [lagB, corrB] = xcorr (stateB.impulseResponse);
		const bool currentInvB = parameters.getRawParameterValue (kParamInvB)->load() > 0.5f;
		applyAlignment (kParamDelayB, kParamInvB, lagB, corrB, currentInvB, currentInvA, "B");
		if (lagB < 0) aDelaySamples = juce::jmax (aDelaySamples, -lagB);
	}

	if (hasC)
	{
		auto [lagC, corrC] = xcorr (stateC.impulseResponse);
		const bool currentInvC = parameters.getRawParameterValue (kParamInvC)->load() > 0.5f;
		applyAlignment (kParamDelayC, kParamInvC, lagC, corrC, currentInvC, currentInvA, "C");
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
	checkLoader (stateC, kParamPitchC, kParamInvC, kParamNormC, kParamRvsC, kParamStartC, kParamEndC, "C");
	
	// ALIGN: momentary action — calculate cross-correlation + set delay/inv, then turn off
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
	
	// Debug logging every 10 seconds
	if (didReload || (now - lastDebugTime > 10000))
	{
		if (now - lastDebugTime > 10000)
		{
			const double worstUs = worstBlockTimeUs.exchange (0.0);
			const double budgetUs = (currentBlockSize / currentSampleRate) * 1e6;
			const double cpuPct = (worstUs / budgetUs) * 100.0;
			const double loaderAUs = worstLoaderAUs.exchange (0.0);
			const double loaderBUs = worstLoaderBUs.exchange (0.0);
			const double loaderCUs = worstLoaderCUs.exchange (0.0);
			const double convUs = worstConvTimeUs.exchange (0.0);
			const float inPeak  = peakInputLevel.exchange (0.0f);
			const float outPeak = peakOutputLevel.exchange (0.0f);
			const int   clips   = clipCount.exchange (0);
			
			const float inDb  = inPeak  > 0.0f ? juce::Decibels::gainToDecibels (inPeak)  : -100.0f;
			const float outDb = outPeak > 0.0f ? juce::Decibels::gainToDecibels (outPeak) : -100.0f;
			
			LOG_IR_EVENT ("CAB-TR Stats: CPU worst=" + juce::String (worstUs, 0) + "us / " +
			              juce::String (budgetUs, 0) + "us budget (" + 
			              juce::String (cpuPct, 1) + "%) blocks=" + 
			              juce::String (blockCount.load()) +
			              " | loaderA=" + juce::String (loaderAUs, 0) + "us" +
			              " loaderB=" + juce::String (loaderBUs, 0) + "us" +
			              " loaderC=" + juce::String (loaderCUs, 0) + "us" +
			              " convWorst=" + juce::String (convUs, 0) + "us" +
			              " | irA=" + juce::String (stateA.impulseResponse.getNumSamples()) + "smp" +
			              " irB=" + juce::String (stateB.impulseResponse.getNumSamples()) + "smp" +
			              " irC=" + juce::String (stateC.impulseResponse.getNumSamples()) + "smp" +
			              " | peakIn=" + juce::String (inDb, 1) + "dB" +
			              " peakOut=" + juce::String (outDb, 1) + "dB" +
			              " clips=" + juce::String (clips) +
			              " | route=" + juce::String (static_cast<int> (parameters.getRawParameterValue (kParamRoute)->load())) +
			              " mix=" + juce::String (parameters.getRawParameterValue (kParamMix)->load(), 2) +
			              " | A=" + juce::String (parameters.getRawParameterValue (kParamEnableA)->load() > 0.5f ? "ON" : "off") +
			              (stateA.currentFilePath.isNotEmpty() ? "(loaded)" : "(empty)") +
			              " B=" + juce::String (parameters.getRawParameterValue (kParamEnableB)->load() > 0.5f ? "ON" : "off") +
			              (stateB.currentFilePath.isNotEmpty() ? "(loaded)" : "(empty)") +
			              " C=" + juce::String (parameters.getRawParameterValue (kParamEnableC)->load() > 0.5f ? "ON" : "off") +
			              (stateC.currentFilePath.isNotEmpty() ? "(loaded)" : "(empty)") +
			              " | reloads=" + juce::String (reloadCountA.load()) +
			              "/" + juce::String (reloadCountB.load()) +
			              "/" + juce::String (reloadCountC.load()) +
			              " blockSize=" + juce::String (currentBlockSize) +
			              " sr=" + juce::String (static_cast<int> (currentSampleRate)));
			lastDebugTime = now;
		}
	}
}

//==============================================================================
// UI state persistence (non-automatable collapse state)
//==============================================================================
void CABTRAudioProcessor::setUiIoExpanded (bool expanded)
{
	parameters.state.setProperty (UiStateKeys::ioExpanded, expanded, nullptr);
}

bool CABTRAudioProcessor::getUiIoExpanded() const noexcept
{
	const auto fromState = parameters.state.getProperty (UiStateKeys::ioExpanded);
	if (! fromState.isVoid()) return (bool) fromState;
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
			const juce::String pathC = state.getProperty ("irPathC", "");
			
			LOG_IR_EVENT ("State loaded: IR_A=" + pathA + ", IR_B=" + pathB + ", IR_C=" + pathC);
			
			// Reload IRs if paths exist
			if (pathA.isNotEmpty())
				loadImpulseResponse (stateA, pathA);
			if (pathB.isNotEmpty())
				loadImpulseResponse (stateB, pathB);
			if (pathC.isNotEmpty())
				loadImpulseResponse (stateC, pathC);
			
			// Update UI file display labels
			if (auto* editor = dynamic_cast<CABTRAudioProcessorEditor*> (getActiveEditor()))
				editor->updateFileDisplayLabels (pathA, pathB, pathC);
		}
	}
}

//==============================================================================
// This creates new instances of the plugin..
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new CABTRAudioProcessor();
}
