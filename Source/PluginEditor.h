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
	void updateFileDisplayLabels (const juce::String& pathA, const juce::String& pathB);

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
	void openFileExplorer (bool forLoaderA);
	void browseToParentFolder (bool forLoaderA);
	void loadIRFile (const juce::String& path, bool forLoaderA);
	void openInfoPopup();
	void openGraphicsPopup();
	void applyLabelTextColour (juce::Label& label, juce::Colour colour);
	void layoutIRSection (juce::Rectangle<int> area, bool isA);

	// TR-style label/value display system
	bool refreshLegendTextCache();
	juce::Rectangle<int> getValueAreaFor (const juce::Rectangle<int>& barBounds) const;
	juce::Slider* getSliderForValueAreaPoint (juce::Point<int> p);
	juce::Rectangle<int> getInfoIconArea() const;
	void updateInfoIconCache();
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
	//  File Explorer State
	// ══════════════════════════════════════════════════════════════
	juce::File currentFolderA;
	juce::File currentFolderB;
	juce::String currentFileA;
	juce::String currentFileB;

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

	// ══════════════════════════════════════════════════════════════
	//  UI Components — Global
	// ══════════════════════════════════════════════════════════════
	juce::ComboBox modeInCombo;
	juce::ComboBox modeCombo;
	juce::ComboBox routeCombo;
	BarSlider globalMixSlider;
	juce::ToggleButton alignButton;

	std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> modeInAttach;
	std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> modeAttach;
	std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> routeAttach;
	std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> globalMixAttach;
	std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> alignAttach;

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
		browseButtonA.setColour (juce::TextButton::buttonColourId, activeScheme.bg);
		browseButtonB.setColour (juce::TextButton::buttonColourId, activeScheme.bg);
		fileDisplayA.setColour (juce::Label::textColourId, activeScheme.text);
		fileDisplayB.setColour (juce::Label::textColourId, activeScheme.text);
		modeInCombo.setColour (juce::ComboBox::textColourId,       activeScheme.text);
		modeInCombo.setColour (juce::ComboBox::backgroundColourId, activeScheme.bg);
		modeInCombo.setColour (juce::ComboBox::outlineColourId,    activeScheme.outline);
		modeCombo.setColour   (juce::ComboBox::textColourId,       activeScheme.text);
		modeCombo.setColour   (juce::ComboBox::backgroundColourId, activeScheme.bg);
		modeCombo.setColour   (juce::ComboBox::outlineColourId,    activeScheme.outline);
		routeCombo.setColour  (juce::ComboBox::textColourId,       activeScheme.text);
		routeCombo.setColour  (juce::ComboBox::backgroundColourId, activeScheme.bg);
		routeCombo.setColour  (juce::ComboBox::outlineColourId,    activeScheme.outline);
	}

	// ══════════════════════════════════════════════════════════════
	//  Misc UI State
	// ══════════════════════════════════════════════════════════════
	bool isDraggingWindow = false;
	juce::Point<int> dragStartPos;

	// ══════════════════════════════════════════════════════════════
	//  TR-style legend text cache (for value display)
	// ══════════════════════════════════════════════════════════════
	// Loader A parameter texts (Full, Short, IntOnly versions)
	juce::String cachedHpFreqTextAFull, cachedHpFreqTextAShort, cachedHpFreqTextAInt;
	juce::String cachedLpFreqTextAFull, cachedLpFreqTextAShort, cachedLpFreqTextAInt;
	juce::String cachedOutTextAFull, cachedOutTextAShort, cachedOutTextAInt;
	juce::String cachedStartTextAFull, cachedStartTextAShort, cachedStartTextAInt;
	juce::String cachedEndTextAFull, cachedEndTextAShort, cachedEndTextAInt;
	juce::String cachedPitchTextAFull, cachedPitchTextAShort, cachedPitchTextAInt;
	juce::String cachedDelayTextAFull, cachedDelayTextAShort, cachedDelayTextAInt;
	juce::String cachedPanTextAFull, cachedPanTextAShort, cachedPanTextAInt;
	juce::String cachedFredTextAFull, cachedFredTextAShort, cachedFredTextAInt;
	juce::String cachedPosTextAFull, cachedPosTextAShort, cachedPosTextAInt;

	// Loader B parameter texts
	juce::String cachedHpFreqTextBFull, cachedHpFreqTextBShort, cachedHpFreqTextBInt;
	juce::String cachedLpFreqTextBFull, cachedLpFreqTextBShort, cachedLpFreqTextBInt;
	juce::String cachedOutTextBFull, cachedOutTextBShort, cachedOutTextBInt;
	juce::String cachedStartTextBFull, cachedStartTextBShort, cachedStartTextBInt;
	juce::String cachedEndTextBFull, cachedEndTextBShort, cachedEndTextBInt;
	juce::String cachedPitchTextBFull, cachedPitchTextBShort, cachedPitchTextBInt;
	juce::String cachedDelayTextBFull, cachedDelayTextBShort, cachedDelayTextBInt;
	juce::String cachedPanTextBFull, cachedPanTextBShort, cachedPanTextBInt;
	juce::String cachedFredTextBFull, cachedFredTextBShort, cachedFredTextBInt;
	juce::String cachedPosTextBFull, cachedPosTextBShort, cachedPosTextBInt;

	// Value display areas (calculated in paint(), used for click detection)
	std::array<juce::Rectangle<int>, 20> cachedValueAreas_;  // 10 per loader

	// Gear icon for info button
	juce::Path cachedInfoGearPath;
	juce::Rectangle<float> cachedInfoGearHole;

public:
	// Public for template friend access from TRSharedUI.h
	juce::Component promptOverlay;
	std::unique_ptr<juce::TooltipWindow> tooltipWindow;
	void setPromptOverlayActive (bool shouldBeActive)
	{
		promptOverlay.setVisible (shouldBeActive);
		if (shouldBeActive)
			promptOverlay.toFront (false);
	}

private:
	JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CABTRAudioProcessorEditor)
};
