#pragma once

#include <cstdint>
#include <atomic>
#include <JuceHeader.h>
#include "PluginProcessor.h"
#include "CrtEffect.h"
#include "TRSharedUI.h"

class CABTRAudioProcessorEditor : public juce::AudioProcessorEditor,
                                   private juce::Slider::Listener,
                                   private juce::Button::Listener,
                                   private juce::ComboBox::Listener,
                                   private juce::AudioProcessorValueTreeState::Listener,
                                   private juce::Timer,
                                   private juce::FileDragAndDropTarget
{
public:
	explicit CABTRAudioProcessorEditor (CABTRAudioProcessor&);
	~CABTRAudioProcessorEditor() override;

	void paint (juce::Graphics&) override;
	void paintOverChildren (juce::Graphics&) override;
	void resized() override;
	void moved() override;
	void parentHierarchyChanged() override;

	// FileDragAndDropTarget
	bool isInterestedInFileDrag (const juce::StringArray& files) override;
	void filesDropped (const juce::StringArray& files, int x, int y) override;
	
	// Public API for updating UI from processor state
	void updateFileDisplayLabels (const juce::String& pathA, const juce::String& pathB, const juce::String& pathC);

private:
	void mouseDown (const juce::MouseEvent& e) override;
	void mouseDoubleClick (const juce::MouseEvent& e) override;
	void mouseDrag (const juce::MouseEvent& e) override;

	void timerCallback() override;
	void sliderValueChanged (juce::Slider* slider) override;
	void buttonClicked (juce::Button* button) override;
	void comboBoxChanged (juce::ComboBox* combo) override;
	void parameterChanged (const juce::String& paramID, float newValue) override;

	void openNumericEntryPopupForSlider (juce::Slider& s);
	void openFileExplorer (int loaderIndex);
	void browseToParentFolder (int loaderIndex);
	void loadIRFile (const juce::String& path, int loaderIndex);
	void openInfoPopup();
	void openGraphicsPopup();
	void openChaosPrompt (int loaderIndex);
	void openFilterPrompt (int loaderIndex);
	void openExportPrompt();
	void applyLabelTextColour (juce::Label& label, juce::Colour colour);
	void layoutIRSection (juce::Rectangle<int> area, int loaderIndex);
	void updateLoaderEnabledState (int loaderIndex);

	// TR-style label/value display system
	bool legendDirty = true;
	bool refreshLegendTextCache();
	juce::Rectangle<int> getValueAreaFor (const juce::Rectangle<int>& barBounds, int columnRight) const;
	juce::Slider* getSliderForValueAreaPoint (juce::Point<int> p);
	juce::Rectangle<int> getInfoIconArea() const;
	void updateInfoIconCache();
	juce::Rectangle<int> getExportIconArea() const;
	void updateExportIconCache();
	void setupBar (juce::Slider& s);

	CABTRAudioProcessor& audioProcessor;

	// ══════════════════════════════════════════════════════════════
	//  Custom Slider with right-click popup
	// ══════════════════════════════════════════════════════════════
	class BarSlider : public juce::Slider
	{
	public:
		using juce::Slider::Slider;

		void setOwner (CABTRAudioProcessorEditor* o) { owner = o; }
		void setAllowNumericPopup (bool allow) { allowNumericPopup = allow; }

		void mouseDown (const juce::MouseEvent& e) override
		{
			if (e.mods.isPopupMenu() && allowNumericPopup)
			{
				if (owner != nullptr)
					owner->openNumericEntryPopupForSlider (*this);
				return;
			}
			juce::Slider::mouseDown (e);
		}

		juce::String getTextFromValue (double v) override;

	private:
		CABTRAudioProcessorEditor* owner = nullptr;
		bool allowNumericPopup = true;
	};

	// ══════════════════════════════════════════════════════════════
	//  Filter bar (dual HP/LP marker component, replaces separate sliders)
	// ══════════════════════════════════════════════════════════════
	class FilterBarComponent : public juce::Component,
	                           public juce::SettableTooltipClient
	{
	public:
		void setOwner (CABTRAudioProcessorEditor* o, int loaderIdx) { owner = o; loaderIndex_ = loaderIdx; }
		void setScheme (const TR::TRScheme& s) { scheme = s; repaint(); }

		void paint (juce::Graphics& g) override;
		void mouseDown (const juce::MouseEvent& e) override;
		void mouseDrag (const juce::MouseEvent& e) override;
		void mouseUp (const juce::MouseEvent& e) override;
		void mouseMove (const juce::MouseEvent& e) override;
		void mouseDoubleClick (const juce::MouseEvent& e) override;

		void updateFromProcessor();

	private:
		CABTRAudioProcessorEditor* owner = nullptr;
		int loaderIndex_ = 0;
		TR::TRScheme scheme {};

		float hpFreq_ = 80.0f;
		float lpFreq_ = 12000.0f;
		bool  hpOn_   = true;
		bool  lpOn_   = true;

		enum DragTarget { None, HP, LP };
		DragTarget currentDrag_ = None;

		static constexpr float kMinFreq = 20.0f;
		static constexpr float kMaxFreq = 20000.0f;
		static constexpr float kPad     = 7.0f;
		static constexpr int   kMarkerHitPx = 10;

		juce::Rectangle<float> getInnerArea() const;
		float freqToNormX (float freq) const;
		float normXToFreq (float normX) const;
		float getMarkerScreenX (float freq) const;
		DragTarget hitTestMarker (juce::Point<float> p) const;
		void  setFreqFromMouseX (float mouseX, DragTarget target);
	};

	// ══════════════════════════════════════════════════════════════
	//  Look and Feel
	// ══════════════════════════════════════════════════════════════
	class MinimalLNF : public juce::LookAndFeel_V4
	{
	public:
		MinimalLNF()
		{
			scheme = { juce::Colours::black, juce::Colours::white,
			           juce::Colours::white, juce::Colours::white };
			TR::applySchemeToLookAndFeel (*this, scheme);
		}

		void setScheme (const TR::TRScheme& s)
		{
			scheme = s;
			TR::applySchemeToLookAndFeel (*this, scheme);
		}

		void drawLinearSlider (juce::Graphics&, int x, int y, int width, int height,
		                       float sliderPos, float minSliderPos, float maxSliderPos,
		                       const juce::Slider::SliderStyle, juce::Slider&) override;

		void drawTickBox (juce::Graphics&, juce::Component&,
		                  float x, float y, float w, float h,
		                  bool ticked, bool isEnabled,
		                  bool highlighted, bool down) override;

		void drawToggleButton (juce::Graphics&, juce::ToggleButton&,
		                      bool shouldDrawButtonAsHighlighted,
		                      bool shouldDrawButtonAsDown) override;

		void drawButtonBackground (juce::Graphics&, juce::Button&,
		                           const juce::Colour& backgroundColour,
		                           bool shouldDrawButtonAsHighlighted,
		                           bool shouldDrawButtonAsDown) override;

		void drawComboBox (juce::Graphics&, int width, int height,
		                   bool isButtonDown, int buttonX, int buttonY,
		                   int buttonW, int buttonH, juce::ComboBox&) override;

		void positionComboBoxText (juce::ComboBox& box, juce::Label& label) override
		{
			label.setBounds (0, 0, box.getWidth(), box.getHeight());
			label.setJustificationType (juce::Justification::centred);
		}

		void drawPopupMenuBackground (juce::Graphics&, int width, int height) override;

		void drawAlertBox (juce::Graphics&, juce::AlertWindow&,
		                   const juce::Rectangle<int>& textArea,
		                   juce::TextLayout& textLayout) override;

		void drawBubble (juce::Graphics&, juce::BubbleComponent&,
		                 const juce::Point<float>& tip,
		                 const juce::Rectangle<float>& body) override;

		juce::Font getTextButtonFont (juce::TextButton&, int buttonHeight) override;
		juce::Font getAlertWindowMessageFont() override;
		juce::Font getLabelFont (juce::Label& label) override;
		juce::Font getSliderPopupFont (juce::Slider&) override;
		juce::Rectangle<int> getTooltipBounds (const juce::String& tipText,
		                                       juce::Point<int> screenPos,
		                                       juce::Rectangle<int> parentArea) override;
		void drawTooltip (juce::Graphics&, const juce::String& text, int width, int height) override;

		void drawScrollbar (juce::Graphics&, juce::ScrollBar&,
		                    int x, int y, int width, int height,
		                    bool isScrollbarVertical,
		                    int thumbStartPosition, int thumbSize,
		                    bool isMouseOver, bool isMouseDown) override;

		int getMinimumScrollbarThumbSize (juce::ScrollBar&) override { return 16; }
		int getScrollbarButtonSize (juce::ScrollBar&) override      { return 0; }

		TR::TRScheme scheme;
	};

	MinimalLNF lnf;

	// ══════════════════════════════════════════════════════════════
	//  DRY helpers for tripled loader A/B/C setup
	// ══════════════════════════════════════════════════════════════
	struct LoaderRefs
	{
		juce::ToggleButton &enableBtn;  juce::TextButton &browseBtn;  juce::Label &fileDisp;
		BarSlider &hp, &lp, &out, &start, &end, &pitch, &delay, &pan, &fred, &pos;
		juce::ToggleButton &inv, &norm, &rvs, &chaos;  juce::Label &chaosDisp;
		juce::ComboBox &modeIn, &modeOut;
		FilterBarComponent &filterBar;  BarSlider &mix;
	};
	struct AttachRefs
	{
		std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment>   &enableAtt;
		std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>   &hpAtt, &lpAtt, &outAtt, &startAtt, &endAtt, &pitchAtt, &delayAtt, &panAtt, &fredAtt, &posAtt;
		std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment>   &invAtt, &normAtt, &rvsAtt, &chaosAtt;
		std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> &modeInAtt, &modeOutAtt;
		std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>   &mixAtt;
	};
	LoaderRefs  getLoaderRefs (int index);
	AttachRefs  getAttachRefs (int index);
	void setupLoaderUI (int loaderIndex, LoaderRefs refs, const char* chaosAmtId, const char* chaosSpdId);
	void createLoaderAttachments (juce::AudioProcessorValueTreeState& params, int loaderIndex,
	                              LoaderRefs ui, AttachRefs att);

	struct LoaderParamIds
	{
		const char* enable;
		const char* hpFreq;  const char* lpFreq;  const char* out;
		const char* start;   const char* end;     const char* pitch;
		const char* delay;   const char* pan;     const char* fred;   const char* pos;
		const char* inv;     const char* norm;    const char* rvs;    const char* chaos;
		const char* chaosAmt; const char* chaosSpd;
		const char* modeIn;  const char* modeOut; const char* mix;
	};
	static const LoaderParamIds kLoaderParams[3];

	// ══════════════════════════════════════════════════════════════
	//  File Explorer State
	// ══════════════════════════════════════════════════════════════
	juce::File currentFolderA;
	juce::File currentFolderB;
	juce::File currentFolderC;
	juce::String currentFileA;
	juce::String currentFileB;
	juce::String currentFileC;

	// ══════════════════════════════════════════════════════════════
	//  UI Components — IR Loader A
	// ══════════════════════════════════════════════════════════════
	juce::ToggleButton enableButtonA;
	juce::TextButton browseButtonA { "..." };
	juce::Label fileDisplayA;

	BarSlider hpFreqSliderA;
	BarSlider lpFreqSliderA;
	BarSlider outSliderA;
	BarSlider startSliderA;
	BarSlider endSliderA;
	BarSlider pitchSliderA;
	BarSlider delaySliderA;
	BarSlider panSliderA;
	BarSlider fredSliderA;
	BarSlider posSliderA;

	juce::ToggleButton invButtonA;
	juce::ToggleButton normButtonA;
	juce::ToggleButton rvsButtonA;
	juce::ToggleButton chaosButtonA;
	juce::Label chaosDisplayA;

	std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> enableAttachA;
	std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> hpFreqAttachA;
	std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> lpFreqAttachA;
	std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> outAttachA;
	std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> startAttachA;
	std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> endAttachA;
	std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> pitchAttachA;
	std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> delayAttachA;
	std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> panAttachA;
	std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> fredAttachA;
	std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> posAttachA;
	std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> invAttachA;
	std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> normAttachA;
	std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> rvsAttachA;
	std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> chaosAttachA;
	juce::ComboBox modeInComboA;
	juce::ComboBox modeOutComboA;
	std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> modeInAttachA;
	std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> modeOutAttachA;

	// ══════════════════════════════════════════════════════════════
	//  UI Components — IR Loader B
	// ══════════════════════════════════════════════════════════════
	juce::ToggleButton enableButtonB;
	juce::TextButton browseButtonB { "..." };
	juce::Label fileDisplayB;

	BarSlider hpFreqSliderB;
	BarSlider lpFreqSliderB;
	BarSlider outSliderB;
	BarSlider startSliderB;
	BarSlider endSliderB;
	BarSlider pitchSliderB;
	BarSlider delaySliderB;
	BarSlider panSliderB;
	BarSlider fredSliderB;
	BarSlider posSliderB;

	juce::ToggleButton invButtonB;
	juce::ToggleButton normButtonB;
	juce::ToggleButton rvsButtonB;
	juce::ToggleButton chaosButtonB;
	juce::Label chaosDisplayB;

	std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> enableAttachB;
	std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> hpFreqAttachB;
	std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> lpFreqAttachB;
	std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> outAttachB;
	std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> startAttachB;
	std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> endAttachB;
	std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> pitchAttachB;
	std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> delayAttachB;
	std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> panAttachB;
	std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> fredAttachB;
	std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> posAttachB;
	std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> invAttachB;
	std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> normAttachB;
	std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> rvsAttachB;
	std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> chaosAttachB;
	juce::ComboBox modeInComboB;
	juce::ComboBox modeOutComboB;
	std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> modeInAttachB;
	std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> modeOutAttachB;

	// ══════════════════════════════════════════════════════════════
	//  UI Components — IR Loader C
	// ══════════════════════════════════════════════════════════════
	juce::ToggleButton enableButtonC;
	juce::TextButton browseButtonC { "..." };
	juce::Label fileDisplayC;

	BarSlider hpFreqSliderC;
	BarSlider lpFreqSliderC;
	BarSlider outSliderC;
	BarSlider startSliderC;
	BarSlider endSliderC;
	BarSlider pitchSliderC;
	BarSlider delaySliderC;
	BarSlider panSliderC;
	BarSlider fredSliderC;
	BarSlider posSliderC;

	juce::ToggleButton invButtonC;
	juce::ToggleButton normButtonC;
	juce::ToggleButton rvsButtonC;
	juce::ToggleButton chaosButtonC;
	juce::Label chaosDisplayC;

	std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> enableAttachC;
	std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> hpFreqAttachC;
	std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> lpFreqAttachC;
	std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> outAttachC;
	std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> startAttachC;
	std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> endAttachC;
	std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> pitchAttachC;
	std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> delayAttachC;
	std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> panAttachC;
	std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> fredAttachC;
	std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> posAttachC;
	std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> invAttachC;
	std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> normAttachC;
	std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> rvsAttachC;
	std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> chaosAttachC;
	juce::ComboBox modeInComboC;
	juce::ComboBox modeOutComboC;
	std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> modeInAttachC;
	std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> modeOutAttachC;

	// ══════════════════════════════════════════════════════════════
	//  UI Components — Filter Bars & per-loader MIX
	// ══════════════════════════════════════════════════════════════
	FilterBarComponent filterBarA_;
	FilterBarComponent filterBarB_;
	FilterBarComponent filterBarC_;

	BarSlider mixSliderA;   // per-loader MIX A (kParamMixA)
	BarSlider mixSliderB;   // per-loader MIX B (kParamMixB)
	BarSlider mixSliderC;   // per-loader MIX C (kParamMixC)
	std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> mixAttachA;
	std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> mixAttachB;
	std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> mixAttachC;

	// ══════════════════════════════════════════════════════════════
	//  Collapse/Expand state
	// ══════════════════════════════════════════════════════════════
	bool ioSectionExpanded_ = false;
	juce::Rectangle<int> cachedToggleBarAreaA_;
	juce::Rectangle<int> cachedToggleBarAreaB_;
	juce::Rectangle<int> cachedToggleBarAreaC_;
	juce::Rectangle<int> cachedToggleBarArea_;     // continuous bar (used for drawing/clicks)

	// ══════════════════════════════════════════════════════════════
	//  UI Components — Global
	// ══════════════════════════════════════════════════════════════
	juce::ComboBox routeCombo;
	juce::ToggleButton alignButton;
	juce::ComboBox matchCombo;
	juce::ComboBox trimCombo;

	BarSlider globalMixSlider;   // Global dry/wet MIX (kParamMix)
	BarSlider globalOutputSlider; // Global output gain (kParamOutput)

	std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> routeAttach;
	std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> alignAttach;
	std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> matchAttach;
	std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> trimAttach;
	std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> globalMixAttach;
	std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> globalOutputAttach;

	// ══════════════════════════════════════════════════════════════
	//  CRT Effect
	// ══════════════════════════════════════════════════════════════
	CrtEffect crtEffect;
	float crtTime = 0.0f;
	bool crtEnabled = false;

	void applyCrtState (bool enabled)
	{
		crtEnabled = enabled;
		crtEffect.setEnabled (enabled);
		crtTime = 0.0f;
	}

	// ══════════════════════════════════════════════════════════════
	//  Palette & Colour Scheme
	// ══════════════════════════════════════════════════════════════
	bool useCustomPalette = false;
	std::array<juce::Colour, 2> defaultPalette { juce::Colours::white, juce::Colours::black };
	std::array<juce::Colour, 2> customPalette  { juce::Colours::white, juce::Colours::black };
	TR::TRScheme activeScheme;

	void applyActivePalette()
	{
		const auto& palette = useCustomPalette ? customPalette : defaultPalette;

		TR::TRScheme scheme;
		scheme.bg      = palette[1];
		scheme.fg      = palette[0];
		scheme.outline = palette[0];
		scheme.text    = palette[0];

		activeScheme = scheme;
		lnf.setScheme (activeScheme);

		auto applyComboScheme = [this] (juce::ComboBox& c) {
			c.setColour (juce::ComboBox::textColourId,       activeScheme.text);
			c.setColour (juce::ComboBox::backgroundColourId, activeScheme.bg);
			c.setColour (juce::ComboBox::outlineColourId,    activeScheme.outline);
		};

		for (int i = 0; i < 3; ++i)
		{
			auto r = getLoaderRefs (i);
			r.browseBtn.setColour (juce::TextButton::buttonColourId, activeScheme.bg);
			r.fileDisp.setColour (juce::Label::textColourId, activeScheme.text);
			applyComboScheme (r.modeIn);
			applyComboScheme (r.modeOut);
			r.filterBar.setScheme (activeScheme);
		}

		applyComboScheme (routeCombo);
		applyComboScheme (matchCombo);
		applyComboScheme (trimCombo);
	}

	// ══════════════════════════════════════════════════════════════
	//  Misc UI State
	// ══════════════════════════════════════════════════════════════
	bool isDraggingWindow = false;
	juce::Point<int> dragStartPos;

	// ══════════════════════════════════════════════════════════════
	//  TR-style legend text cache (for value display)
	// ══════════════════════════════════════════════════════════════
	struct CachedParamText { juce::String full, short_, intOnly; };
	// Param indices: HP=0, LP=1, OUT=2, START=3, END=4, PITCH=5, DELAY=6, PAN=7, FRED=8, POS=9, MIX=10
	static constexpr int kNumCachedParams = 11;
	CachedParamText cachedTexts[3][kNumCachedParams];  // [loader][param]

	// Column right edges (set in resized(), used by getValueAreaFor())
	int columnRight_[3] = {};

	// Value display areas (calculated in paint(), used for click detection)
	std::array<juce::Rectangle<int>, 33> cachedValueAreas_;  // 11 per loader × 3

	// Gear icon for info button
	juce::Path cachedInfoGearPath;
	juce::Rectangle<float> cachedInfoGearHole;

	// Export icon (download arrow)
	juce::Path cachedExportIconPath;
	std::unique_ptr<juce::FileChooser> exportChooser;

public:
	// Public for template friend access from TRSharedUI.h
	using PromptOverlay = TR::PromptOverlay;
	PromptOverlay promptOverlay;
	std::unique_ptr<juce::TooltipWindow> tooltipWindow;
	void setPromptOverlayActive (bool shouldBeActive);
	bool promptOverlayActive = false;

private:
	JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CABTRAudioProcessorEditor)
};
