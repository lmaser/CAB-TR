// PluginEditor.cpp — CAB-TR
// This file is temporarily simplified. Full implementation with complete
// file explorer will be added in subsequent updates.
#include "PluginEditor.h"
#include "InfoContent.h"
#include <functional>

using namespace TR;

#if JUCE_WINDOWS
 #include <windows.h>
#endif

// ══════════════════════════════════════════════════════════════
//  Timer & display constants
// ══════════════════════════════════════════════════════════════
static constexpr int kCrtTimerHz   = 10;
static constexpr int kIdleTimerHz  = 4;
static constexpr float kSilenceDb  = -80.0f;

// ══════════════════════════════════════════════════════════════
//  Parameter listener IDs
// ══════════════════════════════════════════════════════════════
static constexpr std::array<const char*, 6> kUiMirrorParamIds {
	CABTRAudioProcessor::kParamUiPalette,
	CABTRAudioProcessor::kParamUiFxTail,
	CABTRAudioProcessor::kParamUiColor0,
	CABTRAudioProcessor::kParamUiColor1,
	CABTRAudioProcessor::kParamEnableA,
	CABTRAudioProcessor::kParamEnableB
};

// ══════════════════════════════════════════════════════════════
//  Popup helper classes
// ══════════════════════════════════════════════════════════════
namespace
{
	struct PopupSwatchButton final : public juce::TextButton
	{
		std::function<void()> onLeftClick;
		std::function<void()> onRightClick;

		void clicked() override
		{
			if (onLeftClick)
				onLeftClick();
			else
				juce::TextButton::clicked();
		}

		void mouseUp (const juce::MouseEvent& e) override
		{
			if (e.mods.isPopupMenu())
			{
				if (onRightClick)
					onRightClick();
				return;
			}

			juce::TextButton::mouseUp (e);
		}
	};

	struct PopupClickableLabel final : public juce::Label
	{
		using juce::Label::Label;
		std::function<void()> onClick;

		void mouseUp (const juce::MouseEvent& e) override
		{
			juce::Label::mouseUp (e);
			if (! e.mods.isPopupMenu() && onClick)
				onClick();
		}
	};

	// File browser list model
	class FileListModel : public juce::ListBoxModel
	{
	public:
		FileListModel (const TRScheme& colours) : scheme (colours) {}

		void setCurrentFolder (const juce::File& folder)
		{
			currentFolder = folder;
			refreshFileList();
		}

		void refreshFileList()
		{
			items.clear();

			if (! currentFolder.exists() || ! currentFolder.isDirectory())
				return;

			// Add parent directory option if not at root
			auto parent = currentFolder.getParentDirectory();
			if (parent.exists() && parent.isDirectory())
				items.add ({ true, parent, ".." });

			// Add subdirectories - with validation
			for (const auto& entry : juce::RangedDirectoryIterator (currentFolder, false, "*", juce::File::findDirectories))
			{
				auto dir = entry.getFile();
				// Only add if directory is accessible
				if (dir.exists() && dir.isDirectory())
				{
					items.add ({ true, dir, dir.getFileName() });
				}
			}

			// Add audio files - with validation
			const juce::String pattern = "*.wav;*.aif;*.aiff;*.flac;*.mp3;*.ogg;*.ir";
			juce::StringArray extensions;
			extensions.addTokens (pattern, ";", "");

			for (const auto& ext : extensions)
			{
				for (const auto& entry : juce::RangedDirectoryIterator (currentFolder, false, ext, juce::File::findFiles))
				{
					auto file = entry.getFile();
					// Only add if file exists and has content
					if (file.existsAsFile() && file.getSize() > 0)
					{
						items.add ({ false, file, file.getFileName() });
					}
				}
			}
		}

		int getNumRows() override
		{
			return items.size();
		}

		void paintListBoxItem (int rowNumber, juce::Graphics& g, int width, int height, bool rowIsSelected) override
		{
			if (rowNumber < 0 || rowNumber >= items.size())
				return;

			auto& item = items.getReference (rowNumber);

			// Background
			if (rowIsSelected)
				g.fillAll (scheme.fg.withAlpha (0.3f));
			else
				g.fillAll (scheme.bg);

			// Text
			g.setColour (rowIsSelected ? scheme.fg : scheme.text);
			g.setFont (juce::Font (juce::FontOptions (15.0f)));

			juce::String displayText = item.displayName;
			if (item.isDirectory && item.displayName != "..")
				displayText += "/";

			g.drawText (displayText, 8, 0, width - 16, height, juce::Justification::centredLeft, true);
		}

		void listBoxItemClicked (int row, const juce::MouseEvent&) override
		{
			if (row < 0 || row >= items.size())
				return;

			selectedRow = row;
		}

		void listBoxItemDoubleClicked (int row, const juce::MouseEvent&) override
		{
			if (row < 0 || row >= items.size())
				return;

			auto& item = items.getReference (row);

			// Validate item before proceeding
			if (! item.file.exists())
				return;

			if (item.displayName == "..")
			{
				// Go to parent directory
				if (onNavigateUp)
					onNavigateUp();
			}
			else if (item.isDirectory && item.file.isDirectory())
			{
				// Navigate into directory - double check it's accessible
				// CRITICAL: Copy the File BEFORE passing to callback
				// to avoid reference invalidation if items array is modified
				if (onNavigateInto)
				{
					juce::File folderCopy = item.file;
					onNavigateInto (folderCopy);
				}
			}
			else if (! item.isDirectory && item.file.existsAsFile())
			{
				// Select file - copy before passing
				if (onFileSelected)
				{
					juce::File fileCopy = item.file;
					onFileSelected (fileCopy);
				}
			}
		}

		juce::File getSelectedFile() const
		{
			if (selectedRow >= 0 && selectedRow < items.size())
			{
				auto& item = items.getReference (selectedRow);
				if (! item.isDirectory || item.displayName == "..")
					return item.file;
			}
			return {};
		}

		bool selectedIsDirectory() const
		{
			if (selectedRow >= 0 && selectedRow < items.size())
				return items.getReference (selectedRow).isDirectory;
			return false;
		}

		std::function<void()> onNavigateUp;
		std::function<void(const juce::File&)> onNavigateInto;
		std::function<void(const juce::File&)> onFileSelected;

		juce::File currentFolder;
		int selectedRow = -1;

		struct Item
		{
			bool isDirectory;
			juce::File file;
			juce::String displayName;
		};

		juce::Array<Item> items;

	private:
		TRScheme scheme;
	};
}

// ══════════════════════════════════════════════════════════════
//  Popup static layout helpers
// ══════════════════════════════════════════════════════════════
static void syncGraphicsPopupState (juce::AlertWindow& aw,
                                    const std::array<juce::Colour, 2>& defaultPalette,
                                    const std::array<juce::Colour, 2>& customPalette,
                                    bool useCustomPalette)
{
	if (auto* t = dynamic_cast<juce::ToggleButton*> (aw.findChildWithID ("paletteDefaultToggle")))
		t->setToggleState (! useCustomPalette, juce::dontSendNotification);
	if (auto* t = dynamic_cast<juce::ToggleButton*> (aw.findChildWithID ("paletteCustomToggle")))
		t->setToggleState (useCustomPalette, juce::dontSendNotification);

	for (int i = 0; i < 2; ++i)
	{
		if (auto* dflt = dynamic_cast<juce::TextButton*> (aw.findChildWithID ("defaultSwatch" + juce::String (i))))
			setPaletteSwatchColour (*dflt, defaultPalette[(size_t) i]);
		if (auto* custom = dynamic_cast<juce::TextButton*> (aw.findChildWithID ("customSwatch" + juce::String (i))))
		{
			setPaletteSwatchColour (*custom, customPalette[(size_t) i]);
			custom->setTooltip (colourToHexRgb (customPalette[(size_t) i]));
		}
	}

	auto applyLabelTextColourTo = [] (juce::Label* lbl, juce::Colour col)
	{
		if (lbl != nullptr)
			lbl->setColour (juce::Label::textColourId, col);
	};

	const juce::Colour activeText = useCustomPalette ? customPalette[0] : defaultPalette[0];
	applyLabelTextColourTo (dynamic_cast<juce::Label*> (aw.findChildWithID ("paletteDefaultLabel")), activeText);
	applyLabelTextColourTo (dynamic_cast<juce::Label*> (aw.findChildWithID ("paletteCustomLabel")), activeText);
	applyLabelTextColourTo (dynamic_cast<juce::Label*> (aw.findChildWithID ("paletteTitle")), activeText);
	applyLabelTextColourTo (dynamic_cast<juce::Label*> (aw.findChildWithID ("fxLabel")), activeText);
}

static void layoutGraphicsPopupContent (juce::AlertWindow& aw)
{
	layoutAlertWindowButtons (aw);

	auto snapEven = [] (int v) { return v & ~1; };

	const int contentLeft = kPromptInnerMargin;
	const int contentRight = aw.getWidth() - kPromptInnerMargin;
	const int contentW = juce::jmax (0, contentRight - contentLeft);

	auto* dfltToggle = dynamic_cast<juce::ToggleButton*> (aw.findChildWithID ("paletteDefaultToggle"));
	auto* dfltLabel  = dynamic_cast<juce::Label*> (aw.findChildWithID ("paletteDefaultLabel"));
	auto* customToggle = dynamic_cast<juce::ToggleButton*> (aw.findChildWithID ("paletteCustomToggle"));
	auto* customLabel  = dynamic_cast<juce::Label*> (aw.findChildWithID ("paletteCustomLabel"));
	auto* paletteTitle = dynamic_cast<juce::Label*> (aw.findChildWithID ("paletteTitle"));
	auto* fxToggle = dynamic_cast<juce::ToggleButton*> (aw.findChildWithID ("fxToggle"));
	auto* fxLabel  = dynamic_cast<juce::Label*> (aw.findChildWithID ("fxLabel"));
	auto* okBtn = aw.getNumButtons() > 0 ? aw.getButton (0) : nullptr;

	constexpr int toggleBox = GraphicsPromptLayout::toggleBox;
	constexpr int toggleGap = 4;
	constexpr int toggleVisualInsetLeft = 2;
	constexpr int swatchSize = GraphicsPromptLayout::swatchSize;
	constexpr int swatchGap = GraphicsPromptLayout::swatchGap;
	constexpr int columnGap = GraphicsPromptLayout::columnGap;
	constexpr int titleH = GraphicsPromptLayout::titleHeight;

	const int toggleVisualSide = juce::jlimit (14, juce::jmax (14, toggleBox - 2), (int) std::lround ((double) toggleBox * 0.65));

	const int swatchW = swatchSize;
	const int swatchH = (2 * swatchSize) + swatchGap;
	const int swatchGroupSize = (2 * swatchW) + swatchGap;
	const int swatchesH = swatchH;
	const int modeH = toggleBox;

	const int baseGap1 = GraphicsPromptLayout::titleToModeGap;
	const int baseGap2 = GraphicsPromptLayout::modeToSwatchesGap;

	const int titleY = snapEven (kPromptFooterBottomPad);
	const int footerY = getAlertButtonsTop (aw);

	const int bodyH = modeH + baseGap2 + swatchesH;
	const int bodyZoneTop = titleY + titleH + baseGap1;
	const int bodyZoneBottom = footerY - baseGap1;
	const int bodyZoneH = juce::jmax (0, bodyZoneBottom - bodyZoneTop);
	const int bodyY = snapEven (bodyZoneTop + juce::jmax (0, (bodyZoneH - bodyH) / 2));

	const int modeY = bodyY;
	const int blocksY = snapEven (modeY + modeH + baseGap2);

	const int dfltLabelW = (dfltLabel != nullptr) ? juce::jmax (38, stringWidth (dfltLabel->getFont(), "DFLT") + 2) : 40;
	const int customLabelW = (customLabel != nullptr) ? juce::jmax (38, stringWidth (customLabel->getFont(), "CSTM") + 2) : 40;
	const int fxLabelW = (fxLabel != nullptr)
	                   ? juce::jmax (90, stringWidth (fxLabel->getFont(), fxLabel->getText().toUpperCase()) + 2)
	                   : 96;

	const int toggleLabelStartOffset = toggleVisualInsetLeft + toggleVisualSide + toggleGap;
	const int dfltRowW = toggleLabelStartOffset + dfltLabelW;
	const int customRowW = toggleLabelStartOffset + customLabelW;
	const int fxRowW = toggleLabelStartOffset + fxLabelW;
	const int okBtnW = (okBtn != nullptr) ? okBtn->getWidth() : 96;

	const int leftColumnW = juce::jmax (swatchGroupSize, juce::jmax (dfltRowW, fxRowW));
	const int rightColumnW = juce::jmax (swatchGroupSize, juce::jmax (customRowW, okBtnW));
	const int columnsRowW = leftColumnW + columnGap + rightColumnW;
	const int columnsX = snapEven (contentLeft + juce::jmax (0, (contentW - columnsRowW) / 2));
	const int col0X = columnsX;
	const int col1X = columnsX + leftColumnW + columnGap;

	const int dfltX = col0X;
	const int customX = col1X;

	const int defaultSwatchStartX = col0X;
	const int customSwatchStartX = col1X;

	if (paletteTitle != nullptr)
	{
		const int paletteW = juce::jmax (100, juce::jmin (leftColumnW, contentRight - col0X));
		paletteTitle->setBounds (col0X, titleY, paletteW, titleH);
	}

	if (dfltToggle != nullptr)   dfltToggle->setBounds (dfltX, modeY, toggleBox, toggleBox);
	if (dfltLabel != nullptr)    dfltLabel->setBounds (dfltX + toggleLabelStartOffset, modeY, dfltLabelW, toggleBox);
	if (customToggle != nullptr) customToggle->setBounds (customX, modeY, toggleBox, toggleBox);
	if (customLabel != nullptr)  customLabel->setBounds (customX + toggleLabelStartOffset, modeY, customLabelW, toggleBox);

	auto placeSwatchGroup = [&] (const juce::String& prefix, int startX)
	{
		const int startY = blocksY;
		for (int i = 0; i < 2; ++i)
		{
			if (auto* b = dynamic_cast<juce::TextButton*> (aw.findChildWithID (prefix + juce::String (i))))
			{
				b->setBounds (startX + i * (swatchW + swatchGap), startY, swatchW, swatchH);
			}
		}
	};

	placeSwatchGroup ("defaultSwatch", defaultSwatchStartX);
	placeSwatchGroup ("customSwatch", customSwatchStartX);

	if (okBtn != nullptr)
	{
		auto okR = okBtn->getBounds();
		okR.setX (col1X);
		okR.setY (footerY);
		okBtn->setBounds (okR);

		const int fxY = snapEven (footerY + juce::jmax (0, (okR.getHeight() - toggleBox) / 2));
		const int fxX = col0X;
		if (fxToggle != nullptr) fxToggle->setBounds (fxX, fxY, toggleBox, toggleBox);
		if (fxLabel != nullptr)  fxLabel->setBounds (fxX + toggleLabelStartOffset, fxY, fxLabelW, toggleBox);
	}

	auto updateVisualBounds = [] (juce::Component* c, int& minX, int& maxR)
	{
		if (c == nullptr)
			return;
		const auto r = c->getBounds();
		minX = juce::jmin (minX, r.getX());
		maxR = juce::jmax (maxR, r.getRight());
	};

	int visualMinX = aw.getWidth();
	int visualMaxR = 0;

	updateVisualBounds (paletteTitle, visualMinX, visualMaxR);
	updateVisualBounds (dfltToggle, visualMinX, visualMaxR);
	updateVisualBounds (dfltLabel, visualMinX, visualMaxR);
	updateVisualBounds (customToggle, visualMinX, visualMaxR);
	updateVisualBounds (customLabel, visualMinX, visualMaxR);
	updateVisualBounds (fxToggle, visualMinX, visualMaxR);
	updateVisualBounds (fxLabel, visualMinX, visualMaxR);
	updateVisualBounds (okBtn, visualMinX, visualMaxR);

	for (int i = 0; i < 2; ++i)
	{
		updateVisualBounds (aw.findChildWithID ("defaultSwatch" + juce::String (i)), visualMinX, visualMaxR);
		updateVisualBounds (aw.findChildWithID ("customSwatch" + juce::String (i)), visualMinX, visualMaxR);
	}

	if (visualMaxR > visualMinX)
	{
		const int leftMarginToPrompt = visualMinX;
		const int rightMarginToPrompt = aw.getWidth() - visualMaxR;

		int dx = (rightMarginToPrompt - leftMarginToPrompt) / 2;

		const int minDx = contentLeft - visualMinX;
		const int maxDx = contentRight - visualMaxR;
		dx = juce::jlimit (minDx, maxDx, dx);

		if (dx != 0)
		{
			auto shiftX = [dx] (juce::Component* c)
			{
				if (c == nullptr)
					return;
				auto r = c->getBounds();
				r.setX (r.getX() + dx);
				c->setBounds (r);
			};

			shiftX (paletteTitle);
			shiftX (dfltToggle);
			shiftX (dfltLabel);
			shiftX (customToggle);
			shiftX (customLabel);
			shiftX (fxToggle);
			shiftX (fxLabel);
			shiftX (okBtn);

			for (int i = 0; i < 2; ++i)
			{
				shiftX (aw.findChildWithID ("defaultSwatch" + juce::String (i)));
				shiftX (aw.findChildWithID ("customSwatch" + juce::String (i)));
			}
		}
	}
}

static void layoutInfoPopupContent (juce::AlertWindow& aw)
{
	layoutAlertWindowButtons (aw);

	const int contentTop = kPromptBodyTopPad;
	const int contentBottom = getAlertButtonsTop (aw) - kPromptBodyBottomPad;
	const int contentH = juce::jmax (0, contentBottom - contentTop);
	const int bodyW = aw.getWidth() - (2 * kPromptInnerMargin);

	auto* viewport = dynamic_cast<juce::Viewport*> (aw.findChildWithID ("bodyViewport"));
	if (viewport == nullptr)
		return;

	viewport->setBounds (kPromptInnerMargin, contentTop, bodyW, contentH);

	auto* content = viewport->getViewedComponent();
	if (content == nullptr)
		return;

	constexpr int kItemGap = 10;
	int y = 0;
	const int innerW = bodyW - 10;

	for (int i = 0; i < content->getNumChildComponents(); ++i)
	{
		auto* child = content->getChildComponent (i);
		if (child == nullptr || ! child->isVisible())
			continue;

		int itemH = 30;
		if (auto* label = dynamic_cast<juce::Label*> (child))
		{
			auto font = label->getFont();
			const auto text = label->getText();
			const auto border = label->getBorderSize();

			if (! text.containsChar ('\n'))
			{
				itemH = (int) std::ceil (font.getHeight()) + border.getTopAndBottom();
			}
			else
			{
				juce::AttributedString as;
				as.append (text, font, label->findColour (juce::Label::textColourId));
				as.setJustification (label->getJustificationType());
				juce::TextLayout layout;
				const int textAreaW = innerW - border.getLeftAndRight();
				layout.createLayout (as, (float) juce::jmax (1, textAreaW));
				itemH = juce::jmax (20, (int) std::ceil (layout.getHeight() + font.getDescent())
				                        + border.getTopAndBottom() + 4);
			}
		}
		else if (dynamic_cast<juce::HyperlinkButton*> (child) != nullptr)
		{
			itemH = 28;
		}

		child->setBounds (0, y, innerW, itemH);

		if (auto* label = dynamic_cast<juce::Label*> (child))
		{
			const auto& props = label->getProperties();
			if (props.contains ("poemPadFraction"))
			{
				const float padFrac = (float) props["poemPadFraction"];
				const int padPx = juce::jmax (4, (int) std::round (innerW * padFrac));
				label->setBorderSize (juce::BorderSize<int> (0, padPx, 0, padPx));

				auto font = label->getFont();
				const int textAreaW = innerW - 2 * padPx;
				for (float scale = 1.0f; scale >= 0.65f; scale -= 0.025f)
				{
					font.setHorizontalScale (scale);
					juce::GlyphArrangement glyphs;
					glyphs.addLineOfText (font, label->getText(), 0.0f, 0.0f);
					if (static_cast<int> (std::ceil (glyphs.getBoundingBox (0, -1, false).getWidth())) <= textAreaW)
						break;
				}
				label->setFont (font);
			}
		}

		y += itemH + kItemGap;
	}

	if (y > kItemGap)
		y -= kItemGap;

	content->setSize (innerW, juce::jmax (contentH, y));
}

//==============================================================================
//  BarSlider::getTextFromValue
//==============================================================================
juce::String CABTRAudioProcessorEditor::BarSlider::getTextFromValue (double v)
{
	if (owner == nullptr)
		return juce::Slider::getTextFromValue (v);

	// Format based on slider type
	// HP/LP frequencies
	if (this == &owner->hpFreqSliderA || this == &owner->lpFreqSliderA ||
	    this == &owner->hpFreqSliderB || this == &owner->lpFreqSliderB)
	{
		return juce::String (v, 1) + " Hz";
	}

	// OUT, START, END, DELAY (dB or ms)
	if (this == &owner->outSliderA || this == &owner->outSliderB)
	{
		return juce::String (v, 1) + " dB";
	}

	if (this == &owner->startSliderA || this == &owner->endSliderA ||
	    this == &owner->startSliderB || this == &owner->endSliderB)
	{
		return juce::String (static_cast<int> (std::round (v))) + " ms";
	}

	if (this == &owner->delaySliderA || this == &owner->delaySliderB)
	{
		return juce::String (static_cast<int> (std::round (v))) + " ms";
	}

	// PITCH (as percentage)
	if (this == &owner->pitchSliderA || this == &owner->pitchSliderB)
	{
		return juce::String (v * 100.0, 1) + "%";
	}

	// PAN (percentage)
	if (this == &owner->panSliderA || this == &owner->panSliderB)
	{
		double percent = v * 100.0;
		if (std::abs (percent - 50.0) < 1.0)
			return "C";
		if (percent < 50.0)
			return "L" + juce::String (50.0 - percent, 0);
		return "R" + juce::String (percent - 50.0, 0);
	}

	// ANGLE, DIST, global MIX (percentage)
	if (this == &owner->fredSliderA || this == &owner->posSliderA ||
	    this == &owner->fredSliderB || this == &owner->posSliderB ||
	    this == &owner->globalMixSlider)
	{
		return juce::String (v * 100.0, 1) + "%";
	}

	return juce::Slider::getTextFromValue (v);
}

//==============================================================================
//  LookAndFeel implementations
//==============================================================================
void CABTRAudioProcessorEditor::MinimalLNF::drawLinearSlider (
	juce::Graphics& g, int x, int y, int width, int height,
	float sliderPos, float /*minSliderPos*/, float /*maxSliderPos*/,
	const juce::Slider::SliderStyle /*style*/, juce::Slider& /*slider*/)
{
	const juce::Rectangle<float> r ((float) x, (float) y, (float) width, (float) height);

	g.setColour (scheme.outline);
	g.drawRect (r, 4.0f);

	const float pad = 7.0f;
	auto inner = r.reduced (pad);

	g.setColour (scheme.bg);
	g.fillRect (inner);

	const float fillW = juce::jlimit (0.0f, inner.getWidth(), sliderPos - inner.getX());
	auto fill = inner.withWidth (fillW);

	g.setColour (scheme.fg);
	g.fillRect (fill);
}

void CABTRAudioProcessorEditor::MinimalLNF::drawTickBox (
	juce::Graphics& g, juce::Component& button,
	float x, float y, float w, float h,
	bool ticked, bool /*isEnabled*/, bool /*highlighted*/, bool /*down*/)
{
	juce::ignoreUnused (x, y, w, h);

	const auto local = button.getLocalBounds().toFloat().reduced (1.0f);
	const float side = juce::jlimit (14.0f,
	                                 juce::jmax (14.0f, local.getHeight() - 2.0f),
	                                 std::round (local.getHeight() * 0.65f));

	auto r = juce::Rectangle<float> (local.getX() + 2.0f,
	                                 local.getCentreY() - (side * 0.5f),
	                                 side, side).getIntersection (local);

	g.setColour (scheme.outline);
	g.drawRect (r, 4.0f);

	const float innerInset = juce::jlimit (1.0f, side * 0.45f, side * UiMetrics::tickBoxInnerInsetRatio);
	auto inner = r.reduced (innerInset);

	if (ticked)
	{
		g.setColour (scheme.fg);
		g.fillRect (inner);
	}
	else
	{
		g.setColour (scheme.bg);
		g.fillRect (inner);
	}
}

void CABTRAudioProcessorEditor::MinimalLNF::drawButtonBackground (
	juce::Graphics& g, juce::Button& button,
	const juce::Colour& backgroundColour,
	bool shouldDrawButtonAsHighlighted,
	bool shouldDrawButtonAsDown)
{
	auto r = button.getLocalBounds();

	auto fill = backgroundColour;
	if (shouldDrawButtonAsDown)
		fill = fill.brighter (0.12f);
	else if (shouldDrawButtonAsHighlighted)
		fill = fill.brighter (0.06f);

	g.setColour (fill);
	g.fillRect (r);

	g.setColour (scheme.outline);
	g.drawRect (r.reduced (1), 3);
}

void CABTRAudioProcessorEditor::MinimalLNF::drawComboBox (
	juce::Graphics& g, int width, int height,
	bool /*isButtonDown*/, int /*buttonX*/, int /*buttonY*/,
	int /*buttonW*/, int /*buttonH*/, juce::ComboBox& /*box*/)
{
	const juce::Rectangle<int> r (0, 0, width, height);

	g.setColour (scheme.bg);
	g.fillRect (r);

	g.setColour (scheme.outline);
	g.drawRect (r, 3);
}

void CABTRAudioProcessorEditor::MinimalLNF::drawPopupMenuBackground (
	juce::Graphics& g, int width, int height)
{
	g.fillAll (scheme.bg);
	g.setColour (scheme.outline);
	g.drawRect (0, 0, width, height, 2);
}

void CABTRAudioProcessorEditor::MinimalLNF::drawScrollbar (
	juce::Graphics& g, juce::ScrollBar&,
	int x, int y, int width, int height,
	bool isScrollbarVertical,
	int thumbStartPosition, int thumbSize,
	bool isMouseOver, bool isMouseDown)
{
	juce::ignoreUnused (x, y, width, height);

	const auto thumbColour = scheme.text.withAlpha (isMouseDown ? 0.7f
	                                                 : isMouseOver ? 0.5f
	                                                               : 0.3f);
	constexpr float barThickness = 7.0f;
	constexpr float cornerRadius = 3.5f;

	if (isScrollbarVertical)
	{
		const float bx = (float) (x + width) - barThickness - 1.0f;
		g.setColour (thumbColour);
		g.fillRoundedRectangle (bx, (float) thumbStartPosition,
		                        barThickness, (float) thumbSize, cornerRadius);
	}
	else
	{
		const float by = (float) (y + height) - barThickness - 1.0f;
		g.setColour (thumbColour);
		g.fillRoundedRectangle ((float) thumbStartPosition, by,
		                        (float) thumbSize, barThickness, cornerRadius);
	}
}

void CABTRAudioProcessorEditor::MinimalLNF::drawAlertBox (juce::Graphics& g,
                                                          juce::AlertWindow& alert,
                                                          const juce::Rectangle<int>& textArea,
                                                          juce::TextLayout& textLayout)
{
	auto bounds = alert.getLocalBounds();

	g.setColour (scheme.bg);
	g.fillRect (bounds);

	g.setColour (scheme.outline);
	g.drawRect (bounds.reduced (1), 3);

	g.setColour (scheme.text);
	textLayout.draw (g, textArea.toFloat());
}

void CABTRAudioProcessorEditor::MinimalLNF::drawBubble (juce::Graphics& g,
                                                        juce::BubbleComponent&,
                                                        const juce::Point<float>&,
                                                        const juce::Rectangle<float>& body)
{
	drawOverlayPanel (g,
	                  body.getSmallestIntegerContainer(),
	                  findColour (juce::TooltipWindow::backgroundColourId),
	                  findColour (juce::TooltipWindow::outlineColourId));
}

juce::Font CABTRAudioProcessorEditor::MinimalLNF::getTextButtonFont (juce::TextButton&, int buttonHeight)
{
	const float h = juce::jlimit (12.0f, 26.0f, buttonHeight * 0.48f);
	return juce::Font (juce::FontOptions (h).withStyle ("Bold"));
}

juce::Font CABTRAudioProcessorEditor::MinimalLNF::getAlertWindowMessageFont()
{
	auto f = juce::LookAndFeel_V4::getAlertWindowMessageFont();
	f.setBold (true);
	return f;
}

juce::Font CABTRAudioProcessorEditor::MinimalLNF::getLabelFont (juce::Label& label)
{
	auto f = label.getFont();
	if (f.getHeight() <= 0.0f)
	{
		const float h = juce::jlimit (12.0f, 40.0f, (float) juce::jmax (12, label.getHeight() - 6));
		f = juce::Font (juce::FontOptions (h).withStyle ("Bold"));
	}
	else
	{
		f.setBold (true);
	}
	return f;
}

juce::Font CABTRAudioProcessorEditor::MinimalLNF::getSliderPopupFont (juce::Slider&)
{
	return makeOverlayDisplayFont();
}

juce::Rectangle<int> CABTRAudioProcessorEditor::MinimalLNF::getTooltipBounds (const juce::String& tipText,
                                                                               juce::Point<int> screenPos,
                                                                               juce::Rectangle<int> parentArea)
{
	const auto f = makeOverlayDisplayFont();
	const int h = juce::jmax (UiMetrics::tooltipMinHeight,
	                          (int) std::ceil (f.getHeight() * UiMetrics::tooltipHeightScale));

	const int anchorOffsetX = juce::jmax (8, (int) std::round ((double) h * UiMetrics::tooltipAnchorXRatio));
	const int anchorOffsetY = juce::jmax (10, (int) std::round ((double) h * UiMetrics::tooltipAnchorYRatio));
	const int parentMargin = juce::jmax (2, (int) std::round ((double) h * UiMetrics::tooltipParentMarginRatio));
	const int widthPad = juce::jmax (16, (int) std::round (f.getHeight() * UiMetrics::tooltipWidthPadFontRatio));

	const int w = juce::jmax (UiMetrics::tooltipMinWidth, stringWidth (f, tipText) + widthPad);
	auto r = juce::Rectangle<int> (screenPos.x + anchorOffsetX, screenPos.y + anchorOffsetY, w, h);
	return r.constrainedWithin (parentArea.reduced (parentMargin));
}

void CABTRAudioProcessorEditor::MinimalLNF::drawTooltip (juce::Graphics& g,
                                                          const juce::String& text,
                                                          int width,
                                                          int height)
{
	const auto f = makeOverlayDisplayFont();
	const int h = juce::jmax (UiMetrics::tooltipMinHeight,
	                          (int) std::ceil (f.getHeight() * UiMetrics::tooltipHeightScale));
	const int textInsetX = juce::jmax (4, (int) std::round ((double) h * UiMetrics::tooltipTextInsetXRatio));
	const int textInsetY = juce::jmax (1, (int) std::round ((double) h * UiMetrics::tooltipTextInsetYRatio));

	drawOverlayPanel (g,
	                  { 0, 0, width, height },
	                  findColour (juce::TooltipWindow::backgroundColourId),
	                  findColour (juce::TooltipWindow::outlineColourId));

	g.setColour (findColour (juce::TooltipWindow::textColourId));
	g.setFont (f);
	g.drawText (text, textInsetX, textInsetY, width - textInsetX * 2, height - textInsetY * 2,
	            juce::Justification::centredLeft, true);
}

//==============================================================================
//  Constructor
//==============================================================================
CABTRAudioProcessorEditor::CABTRAudioProcessorEditor (CABTRAudioProcessor& p)
	: AudioProcessorEditor (&p), audioProcessor (p)
{
	setLookAndFeel (&lnf);

	// Initialize current folders to Documents
	currentFolderA = juce::File::getSpecialLocation (juce::File::userDocumentsDirectory);
	currentFolderB = juce::File::getSpecialLocation (juce::File::userDocumentsDirectory);

	// Setup IR Loader A components
	addAndMakeVisible (enableButtonA);
	enableButtonA.setButtonText ("ENABLE");
	enableButtonA.addListener (this);

	addAndMakeVisible (browseButtonA);
	browseButtonA.addListener (this);
	browseButtonA.setColour (juce::TextButton::buttonColourId, activeScheme.bg);

	addAndMakeVisible (fileDisplayA);
	fileDisplayA.setText ("No file loaded", juce::dontSendNotification);
	fileDisplayA.setJustificationType (juce::Justification::centred);

	// Setup sliders A
	auto setupSliderWithTooltip = [this] (BarSlider& slider, const juce::String& tooltip) {
		addAndMakeVisible (slider);
		slider.setOwner (this);
		setupBar (slider);
		slider.addListener (this);
		slider.setTooltip (tooltip);
	};

	setupSliderWithTooltip (hpFreqSliderA, "HP Filter A");
	setupSliderWithTooltip (lpFreqSliderA, "LP Filter A");
	setupSliderWithTooltip (outSliderA, "Output Gain A");
	setupSliderWithTooltip (startSliderA, "IR Start Time A");
	setupSliderWithTooltip (endSliderA, "IR End Time A");
	setupSliderWithTooltip (pitchSliderA, "Pitch Shift A (25%-400%)");
	setupSliderWithTooltip (delaySliderA, "Delay A (0-1000ms)");
	setupSliderWithTooltip (panSliderA, "Pan A (L-R)");
	setupSliderWithTooltip (fredSliderA, "Angle A (off-axis mic simulation)");
	setupSliderWithTooltip (posSliderA, "Distance A (proximity/distance)");

	addAndMakeVisible (invButtonA);
	invButtonA.setButtonText ("INV");
	invButtonA.addListener (this);

	addAndMakeVisible (normButtonA);
	normButtonA.setButtonText ("NORM");
	normButtonA.addListener (this);

	addAndMakeVisible (rvsButtonA);
	rvsButtonA.setButtonText ("RVS");
	rvsButtonA.addListener (this);

	addAndMakeVisible (chaosButtonA);
	chaosButtonA.setButtonText ("CHAOS");
	chaosButtonA.addListener (this);

	// CHAOS tooltip overlay — invisible label over the CHAOS checkbox.
	// Provides tooltip on hover; right-click forwarded to editor opens the config prompt.
	{
		const float savedAmt = audioProcessor.getValueTreeState().getRawParameterValue (CABTRAudioProcessor::kParamChaosAmtA)->load();
		const float savedSpd = audioProcessor.getValueTreeState().getRawParameterValue (CABTRAudioProcessor::kParamChaosSpdA)->load();
		chaosDisplayA.setText ("", juce::dontSendNotification);
		chaosDisplayA.setInterceptsMouseClicks (true, false);
		chaosDisplayA.addMouseListener (this, false);
		chaosDisplayA.setTooltip (juce::String (juce::roundToInt (savedAmt)) + "% | " + juce::String (juce::roundToInt (savedSpd)) + " Hz");
		chaosDisplayA.setColour (juce::Label::backgroundColourId, juce::Colours::transparentBlack);
		chaosDisplayA.setColour (juce::Label::outlineColourId, juce::Colours::transparentBlack);
		chaosDisplayA.setOpaque (false);
		addAndMakeVisible (chaosDisplayA);
	}

	// Setup IR Loader B components (identical to A)
	addAndMakeVisible (enableButtonB);
	enableButtonB.setButtonText ("ENABLE");
	enableButtonB.addListener (this);

	addAndMakeVisible (browseButtonB);
	browseButtonB.addListener (this);
	browseButtonB.setColour (juce::TextButton::buttonColourId, activeScheme.bg);

	addAndMakeVisible (fileDisplayB);
	fileDisplayB.setText ("No file loaded", juce::dontSendNotification);
	fileDisplayB.setJustificationType (juce::Justification::centred);

	setupSliderWithTooltip (hpFreqSliderB, "HP Filter B");
	setupSliderWithTooltip (lpFreqSliderB, "LP Filter B");
	setupSliderWithTooltip (outSliderB, "Output Gain B");
	setupSliderWithTooltip (startSliderB, "IR Start Time B");
	setupSliderWithTooltip (endSliderB, "IR End Time B");
	setupSliderWithTooltip (pitchSliderB, "Pitch Shift B (25%-400%)");
	setupSliderWithTooltip (delaySliderB, "Delay B (0-1000ms)");
	setupSliderWithTooltip (panSliderB, "Pan B (L-R)");
	setupSliderWithTooltip (fredSliderB, "Angle B (off-axis mic simulation)");
	setupSliderWithTooltip (posSliderB, "Distance B (proximity/distance)");

	addAndMakeVisible (invButtonB);
	invButtonB.setButtonText ("INV");
	invButtonB.addListener (this);

	addAndMakeVisible (normButtonB);
	normButtonB.setButtonText ("NORM");
	normButtonB.addListener (this);

	addAndMakeVisible (rvsButtonB);
	rvsButtonB.setButtonText ("RVS");
	rvsButtonB.addListener (this);

	addAndMakeVisible (chaosButtonB);
	chaosButtonB.setButtonText ("CHAOS");
	chaosButtonB.addListener (this);

	{
		const float savedAmt = audioProcessor.getValueTreeState().getRawParameterValue (CABTRAudioProcessor::kParamChaosAmtB)->load();
		const float savedSpd = audioProcessor.getValueTreeState().getRawParameterValue (CABTRAudioProcessor::kParamChaosSpdB)->load();
		chaosDisplayB.setText ("", juce::dontSendNotification);
		chaosDisplayB.setInterceptsMouseClicks (true, false);
		chaosDisplayB.addMouseListener (this, false);
		chaosDisplayB.setTooltip (juce::String (juce::roundToInt (savedAmt)) + "% | " + juce::String (juce::roundToInt (savedSpd)) + " Hz");
		chaosDisplayB.setColour (juce::Label::backgroundColourId, juce::Colours::transparentBlack);
		chaosDisplayB.setColour (juce::Label::outlineColourId, juce::Colours::transparentBlack);
		chaosDisplayB.setOpaque (false);
		addAndMakeVisible (chaosDisplayB);
	}

	// Setup global controls
	addAndMakeVisible (modeInCombo);
	modeInCombo.addItem ("L+R", 1);
	modeInCombo.addItem ("MID", 2);
	modeInCombo.addItem ("SIDE", 3);
	modeInCombo.setJustificationType (juce::Justification::centred);
	modeInCombo.setLookAndFeel (&lnf);
	modeInCombo.addListener (this);

	addAndMakeVisible (modeCombo);
	modeCombo.addItem ("L+R", 1);
	modeCombo.addItem ("MID", 2);
	modeCombo.addItem ("SIDE", 3);
	modeCombo.setJustificationType (juce::Justification::centred);
	modeCombo.setLookAndFeel (&lnf);
	modeCombo.addListener (this);

	addAndMakeVisible (routeCombo);
	routeCombo.addItem ("A->B", 1);
	routeCombo.addItem ("A|B", 2);
	routeCombo.setJustificationType (juce::Justification::centred);
	routeCombo.setLookAndFeel (&lnf);
	routeCombo.addListener (this);

	addAndMakeVisible (alignButton);
	alignButton.setButtonText ("ALIGN");
	alignButton.addListener (this);

	// Global MIX slider (dry/wet)
	setupSliderWithTooltip (globalMixSlider, "Global Mix (Dry/Wet)");

	// Create parameter attachments
	auto& params = audioProcessor.getValueTreeState();

	enableAttachA = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (
		params, CABTRAudioProcessor::kParamEnableA, enableButtonA);
	hpFreqAttachA = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (
		params, CABTRAudioProcessor::kParamHpFreqA, hpFreqSliderA);
	lpFreqAttachA = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (
		params, CABTRAudioProcessor::kParamLpFreqA, lpFreqSliderA);
	outAttachA = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (
		params, CABTRAudioProcessor::kParamOutA, outSliderA);
	startAttachA = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (
		params, CABTRAudioProcessor::kParamStartA, startSliderA);
	endAttachA = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (
		params, CABTRAudioProcessor::kParamEndA, endSliderA);
	pitchAttachA = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (
		params, CABTRAudioProcessor::kParamPitchA, pitchSliderA);
	delayAttachA = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (
		params, CABTRAudioProcessor::kParamDelayA, delaySliderA);
	panAttachA = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (
		params, CABTRAudioProcessor::kParamPanA, panSliderA);
	fredAttachA = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (
		params, CABTRAudioProcessor::kParamFredA, fredSliderA);
	posAttachA = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (
		params, CABTRAudioProcessor::kParamPosA, posSliderA);
	invAttachA = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (
		params, CABTRAudioProcessor::kParamInvA, invButtonA);
	normAttachA = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (
		params, CABTRAudioProcessor::kParamNormA, normButtonA);
	rvsAttachA = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (
		params, CABTRAudioProcessor::kParamRvsA, rvsButtonA);
	chaosAttachA = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (
		params, CABTRAudioProcessor::kParamChaosA, chaosButtonA);

	enableAttachB = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (
		params, CABTRAudioProcessor::kParamEnableB, enableButtonB);
	hpFreqAttachB = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (
		params, CABTRAudioProcessor::kParamHpFreqB, hpFreqSliderB);
	lpFreqAttachB = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (
		params, CABTRAudioProcessor::kParamLpFreqB, lpFreqSliderB);
	outAttachB = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (
		params, CABTRAudioProcessor::kParamOutB, outSliderB);
	startAttachB = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (
		params, CABTRAudioProcessor::kParamStartB, startSliderB);
	endAttachB = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (
		params, CABTRAudioProcessor::kParamEndB, endSliderB);
	pitchAttachB = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (
		params, CABTRAudioProcessor::kParamPitchB, pitchSliderB);
	delayAttachB = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (
		params, CABTRAudioProcessor::kParamDelayB, delaySliderB);
	panAttachB = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (
		params, CABTRAudioProcessor::kParamPanB, panSliderB);
	fredAttachB = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (
		params, CABTRAudioProcessor::kParamFredB, fredSliderB);
	posAttachB = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (
		params, CABTRAudioProcessor::kParamPosB, posSliderB);
	invAttachB = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (
		params, CABTRAudioProcessor::kParamInvB, invButtonB);
	normAttachB = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (
		params, CABTRAudioProcessor::kParamNormB, normButtonB);
	rvsAttachB = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (
		params, CABTRAudioProcessor::kParamRvsB, rvsButtonB);
	chaosAttachB = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (
		params, CABTRAudioProcessor::kParamChaosB, chaosButtonB);

	modeInAttach = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (
		params, CABTRAudioProcessor::kParamModeIn, modeInCombo);
	modeAttach = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (
		params, CABTRAudioProcessor::kParamMode, modeCombo);
	routeAttach = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (
		params, CABTRAudioProcessor::kParamRoute, routeCombo);
	alignAttach = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (
		params, CABTRAudioProcessor::kParamAlign, alignButton);
	globalMixAttach = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (
		params, CABTRAudioProcessor::kParamMix, globalMixSlider);

	// Setup tooltip window
	tooltipWindow = std::make_unique<juce::TooltipWindow> (this, 700);

	// Setup prompt overlay
	addChildComponent (promptOverlay);
	promptOverlay.setInterceptsMouseClicks (true, true);

	// Setup CRT effect
	setComponentEffect (&crtEffect);
	crtEffect.setEnabled (false);

	// Setup parameter listeners for UI state
	for (auto* paramId : kUiMirrorParamIds)
		params.addParameterListener (paramId, this);

	// Initialize palette
	applyActivePalette();

	// Initial refresh of cached text
	refreshLegendTextCache();
	legendDirty = false;

	// Initial size
	setSize (1200, 600);
	setResizable (true, true);
	setResizeLimits (800, 500, 2000, 1200);

	// Start timer
	startTimer (kIdleTimerHz);

	// Initialize loader enabled/disabled visual state
	updateLoaderEnabledState (true);
	updateLoaderEnabledState (false);
}

CABTRAudioProcessorEditor::~CABTRAudioProcessorEditor()
{
	TR::dismissEditorOwnedModalPrompts (lnf);
	setPromptOverlayActive (false);

	setComponentEffect (nullptr);
	setLookAndFeel (nullptr);

	auto& params = audioProcessor.getValueTreeState();
	for (auto* paramId : kUiMirrorParamIds)
		params.removeParameterListener (paramId, this);
}

//==============================================================================
//  Paint
//==============================================================================
void CABTRAudioProcessorEditor::paint (juce::Graphics& g)
{
	using namespace TR;

	g.fillAll (activeScheme.bg);
	g.setColour (activeScheme.text);

	if (legendDirty)
	{
		refreshLegendTextCache();
		legendDirty = false;
	}

	// Helper lambda for drawing legend text with fallback
	auto tryDrawLegend = [&] (const juce::Rectangle<int>& area,
	                          const juce::String& fullText,
	                          const juce::String& shortText,
	                          const juce::String& intText) -> bool {
		constexpr float baseFontPx = 32.0f;
		constexpr float minFontPx = 14.0f;

		if (area.isEmpty())
			return false;

		g.setFont (kBoldFont40());
		const float shrinkFloor = baseFontPx * 0.75f;

		if (drawIfFitsWithOptionalShrink (g, area, fullText, baseFontPx, shrinkFloor))
			return true;

		if (drawIfFitsWithOptionalShrink (g, area, shortText, baseFontPx, minFontPx))
			return true;

		drawValueNoEllipsis (g, area, intText, juce::String(), intText, baseFontPx, minFontPx);
		return true;
	};

	// Title & version
	{
		constexpr int titleX = 16;
		constexpr int titleY = 12;
		constexpr int titleH = 32;
		g.setFont (juce::Font (juce::FontOptions (28.0f).withStyle ("Bold")));
		g.drawText ("CAB-TR", titleX, titleY, 200, titleH, juce::Justification::left);

		// Draw version near the gear icon (scaled like other TR plugins)
		const auto iconArea = getInfoIconArea();
		constexpr int kVersionGapPx = 8;
		auto versionFont = juce::Font (juce::FontOptions (
		    juce::jmax (10.0f, (float) titleH * UiMetrics::versionFontRatio)).withStyle ("Bold"));
		g.setFont (versionFont);

		const int versionH = juce::jlimit (10, iconArea.getHeight(),
		    (int) std::round ((double) iconArea.getHeight() * UiMetrics::versionHeightRatio));
		const int versionY = iconArea.getBottom() - versionH;
		const int desiredVersionW = juce::jlimit (28, 64,
		    (int) std::round ((double) iconArea.getWidth() * UiMetrics::versionDesiredWidthRatio));
		const int versionRight = iconArea.getX() - kVersionGapPx;
		const int versionLeftLimit = titleX;
		const int versionX = juce::jmax (versionLeftLimit, versionRight - desiredVersionW);
		const int versionW = juce::jmax (0, versionRight - versionX);

		if (versionW > 0)
			g.drawText (juce::String ("v") + InfoContent::version, versionX, versionY, versionW, versionH,
			            juce::Justification::bottomRight, false);
		const auto modeInArea = modeInCombo.getBounds().withHeight (16).translated (0, -18);
		const auto modeArea = modeCombo.getBounds().withHeight (16).translated (0, -18);
		const auto routeArea = routeCombo.getBounds().withHeight (16).translated (0, -18);
		g.drawText ("MODE IN", modeInArea, juce::Justification::centred);
		g.drawText ("MODE OUT", modeArea, juce::Justification::centred);
		g.drawText ("ROUTE", routeArea, juce::Justification::centred);

		const auto mixArea = globalMixSlider.getBounds().withHeight (16).translated (0, -18);
		g.drawText ("MIX", mixArea, juce::Justification::centred);
	}

	// Draw gear icon (in paint, like other TR plugins)
	{
		if (cachedInfoGearPath.isEmpty())
			updateInfoIconCache();

		g.setColour (activeScheme.text);
		g.fillPath (cachedInfoGearPath);
		g.strokePath (cachedInfoGearPath, juce::PathStrokeType (1.0f));

		g.setColour (activeScheme.bg);
		g.fillEllipse (cachedInfoGearHole);
	}

	// Draw export icon (download arrow next to title)
	{
		if (cachedExportIconPath.isEmpty())
			updateExportIconCache();

		g.setColour (activeScheme.text);
		g.fillPath (cachedExportIconPath);
	}

	// Draw value legends for all bar sliders
	{
		g.setFont (kBoldFont40());

		const bool enabledA = enableButtonA.getToggleState();
		const bool enabledB = enableButtonB.getToggleState();
		const auto textColourA = enabledA ? activeScheme.text : activeScheme.text.withAlpha (0.35f);
		const auto textColourB = enabledB ? activeScheme.text : activeScheme.text.withAlpha (0.35f);

		// Loader A value areas
		g.setColour (textColourA);
		const juce::String* fullTextsA[] = { &cachedHpFreqTextAFull, &cachedLpFreqTextAFull,
		                                      &cachedOutTextAFull, &cachedStartTextAFull,
		                                      &cachedEndTextAFull, &cachedPitchTextAFull,
		                                      &cachedDelayTextAFull, &cachedPanTextAFull,
		                                      &cachedFredTextAFull, &cachedPosTextAFull };

		const juce::String* shortTextsA[] = { &cachedHpFreqTextAShort, &cachedLpFreqTextAShort,
		                                       &cachedOutTextAShort, &cachedStartTextAShort,
		                                       &cachedEndTextAShort, &cachedPitchTextAShort,
		                                       &cachedDelayTextAShort, &cachedPanTextAShort,
		                                       &cachedFredTextAShort, &cachedPosTextAShort };

		const juce::String* intTextsA[] = { &cachedHpFreqTextAInt, &cachedLpFreqTextAInt,
		                                     &cachedOutTextAInt, &cachedStartTextAInt,
		                                     &cachedEndTextAInt, &cachedPitchTextAInt,
		                                     &cachedDelayTextAInt, &cachedPanTextAInt,
		                                     &cachedFredTextAInt, &cachedPosTextAInt };

		const juce::Slider* slidersA[] = { &hpFreqSliderA, &lpFreqSliderA, &outSliderA,
		                                    &startSliderA, &endSliderA, &pitchSliderA,
		                                    &delaySliderA, &panSliderA, &fredSliderA, &posSliderA };

		for (int i = 0; i < 10; ++i)
		{
			if (slidersA[i]->isVisible())
			{
				const auto valueArea = getValueAreaFor (slidersA[i]->getBounds());
				cachedValueAreas_[(size_t) i] = valueArea;
				tryDrawLegend (valueArea, *fullTextsA[i], *shortTextsA[i], *intTextsA[i]);
			}
		}

		// Loader B value areas
		g.setColour (textColourB);
		const juce::String* fullTextsB[] = { &cachedHpFreqTextBFull, &cachedLpFreqTextBFull,
		                                      &cachedOutTextBFull, &cachedStartTextBFull,
		                                      &cachedEndTextBFull, &cachedPitchTextBFull,
		                                      &cachedDelayTextBFull, &cachedPanTextBFull,
		                                      &cachedFredTextBFull, &cachedPosTextBFull };

		const juce::String* shortTextsB[] = { &cachedHpFreqTextBShort, &cachedLpFreqTextBShort,
		                                       &cachedOutTextBShort, &cachedStartTextBShort,
		                                       &cachedEndTextBShort, &cachedPitchTextBShort,
		                                       &cachedDelayTextBShort, &cachedPanTextBShort,
		                                       &cachedFredTextBShort, &cachedPosTextBShort };

		const juce::String* intTextsB[] = { &cachedHpFreqTextBInt, &cachedLpFreqTextBInt,
		                                     &cachedOutTextBInt, &cachedStartTextBInt,
		                                     &cachedEndTextBInt, &cachedPitchTextBInt,
		                                     &cachedDelayTextBInt, &cachedPanTextBInt,
		                                     &cachedFredTextBInt, &cachedPosTextBInt };

		const juce::Slider* slidersB[] = { &hpFreqSliderB, &lpFreqSliderB, &outSliderB,
		                                    &startSliderB, &endSliderB, &pitchSliderB,
		                                    &delaySliderB, &panSliderB, &fredSliderB, &posSliderB };

		for (int i = 0; i < 10; ++i)
		{
			if (slidersB[i]->isVisible())
			{
				const auto valueArea = getValueAreaFor (slidersB[i]->getBounds());
				cachedValueAreas_[(size_t) (10 + i)] = valueArea;
				tryDrawLegend (valueArea, *fullTextsB[i], *shortTextsB[i], *intTextsB[i]);
			}
		}
	}
}

void CABTRAudioProcessorEditor::paintOverChildren (juce::Graphics& g)
{
	juce::ignoreUnused (g);
}

//==============================================================================
//  Resized & Layout
//==============================================================================
void CABTRAudioProcessorEditor::resized()
{
	auto bounds = getLocalBounds();
	
	// Header (title area + buttons)
	auto header = bounds.removeFromTop (40);

	// Footer for global controls
	auto footer = bounds.removeFromBottom (50);
	
	// Split remaining area into two halves for A and B
	const int halfWidth = bounds.getWidth() / 2;
	auto leftArea = bounds.removeFromLeft (halfWidth);
	auto rightArea = bounds;

	// Layout IR Loader A
	layoutIRSection (leftArea, true);

	// Layout IR Loader B
	layoutIRSection (rightArea, false);

	// Layout footer: MODE IN | MODE OUT | ROUTE | MIX | ALIGN
	const int comboW = 80;
	const int btnW = 70;
	const int mixW = 100;
	const int gap = 12;
	const int totalFooterW = comboW * 3 + mixW + btnW + gap * 4;
	
	auto footerCenter = footer.withSizeKeepingCentre (totalFooterW, 30);
	
	modeInCombo.setBounds (footerCenter.removeFromLeft (comboW));
	footerCenter.removeFromLeft (gap);
	modeCombo.setBounds (footerCenter.removeFromLeft (comboW));
	footerCenter.removeFromLeft (gap);
	routeCombo.setBounds (footerCenter.removeFromLeft (comboW));
	footerCenter.removeFromLeft (gap);
	globalMixSlider.setBounds (footerCenter.removeFromLeft (mixW));
	footerCenter.removeFromLeft (gap);
	alignButton.setBounds (footerCenter.removeFromLeft (btnW));

	promptOverlay.setBounds (getLocalBounds());

	updateInfoIconCache();
	updateExportIconCache();
}

void CABTRAudioProcessorEditor::layoutIRSection (juce::Rectangle<int> area, bool isA)
{
	const int margin = 10;
	const int sliderH = 30;
	const int buttonH = 30;
	const int gap = 5;

	area.reduce (margin, margin);

	// Enable checkbox at top
	auto& enableBtn = isA ? enableButtonA : enableButtonB;
	enableBtn.setBounds (area.removeFromTop (buttonH));
	area.removeFromTop (gap);

	// File explorer section
	auto fileArea = area.removeFromTop (80);
	auto& browseBtn = isA ? browseButtonA : browseButtonB;
	auto& fileDisp = isA ? fileDisplayA : fileDisplayB;
	
	browseBtn.setBounds (fileArea.removeFromTop (buttonH));
	fileArea.removeFromTop (gap);
	fileDisp.setBounds (fileArea);
	area.removeFromTop (gap * 2);

	// Control sliders
	auto& hp = isA ? hpFreqSliderA : hpFreqSliderB;
	auto& lp = isA ? lpFreqSliderA : lpFreqSliderB;
	auto& out = isA ? outSliderA : outSliderB;
	auto& start = isA ? startSliderA : startSliderB;
	auto& end = isA ? endSliderA : endSliderB;
	auto& pitch = isA ? pitchSliderA : pitchSliderB;
	auto& delay = isA ? delaySliderA : delaySliderB;
	auto& pan = isA ? panSliderA : panSliderB;
	auto& fred = isA ? fredSliderA : fredSliderB;
	auto& pos = isA ? posSliderA : posSliderB;
	auto& inv = isA ? invButtonA : invButtonB;
	auto& norm = isA ? normButtonA : normButtonB;
	auto& rvs = isA ? rvsButtonA : rvsButtonB;
	auto& chaos = isA ? chaosButtonA : chaosButtonB;

	// Sliders take 55% width to leave space for value labels on right
	const int sliderW = static_cast<int> (area.getWidth() * 0.55f);
	
	auto sliderRow = area.removeFromTop (sliderH);
	lp.setBounds (sliderRow.removeFromLeft (sliderW));
	area.removeFromTop (gap);
	
	sliderRow = area.removeFromTop (sliderH);
	hp.setBounds (sliderRow.removeFromLeft (sliderW));
	area.removeFromTop (gap);
	
	sliderRow = area.removeFromTop (sliderH);
	out.setBounds (sliderRow.removeFromLeft (sliderW));
	area.removeFromTop (gap);
	
	sliderRow = area.removeFromTop (sliderH);
	start.setBounds (sliderRow.removeFromLeft (sliderW));
	area.removeFromTop (gap);
	
	sliderRow = area.removeFromTop (sliderH);
	end.setBounds (sliderRow.removeFromLeft (sliderW));
	area.removeFromTop (gap);
	
	sliderRow = area.removeFromTop (sliderH);
	pitch.setBounds (sliderRow.removeFromLeft (sliderW));
	area.removeFromTop (gap);
	
	sliderRow = area.removeFromTop (sliderH);
	delay.setBounds (sliderRow.removeFromLeft (sliderW));
	area.removeFromTop (gap);
	
	sliderRow = area.removeFromTop (sliderH);
	pan.setBounds (sliderRow.removeFromLeft (sliderW));
	area.removeFromTop (gap);
	
	sliderRow = area.removeFromTop (sliderH);
	fred.setBounds (sliderRow.removeFromLeft (sliderW));
	area.removeFromTop (gap);
	
	sliderRow = area.removeFromTop (sliderH);
	pos.setBounds (sliderRow.removeFromLeft (sliderW));
	area.removeFromTop (gap * 2);

	// Checkboxes at bottom (INV, NORM, RVS, CHAOS) - alineados con las barras
	auto checkArea = area.removeFromTop (buttonH);
	
	// Calculate checkbox width to fit text labels
	const int checkboxW = 55;
	const int numButtons = 4;
	const int spacing = juce::jmax (0, (sliderW - numButtons * checkboxW) / (numButtons - 1));
	
	int bx = checkArea.getX();
	inv.setBounds  (bx, checkArea.getY(), checkboxW, buttonH);
	bx += checkboxW + spacing;
	norm.setBounds (bx, checkArea.getY(), checkboxW, buttonH);
	bx += checkboxW + spacing;
	rvs.setBounds  (bx, checkArea.getY(), checkboxW, buttonH);
	bx += checkboxW + spacing;
	chaos.setBounds (bx, checkArea.getY(), checkboxW, buttonH);

	// Position invisible CHAOS display overlay on top of the CHAOS checkbox
	auto& chaosDisp = isA ? chaosDisplayA : chaosDisplayB;
	chaosDisp.setBounds (bx, checkArea.getY(), checkboxW, buttonH);
}

//==============================================================================
//  Loader enabled/disabled visual state
//==============================================================================
void CABTRAudioProcessorEditor::updateLoaderEnabledState (bool isA)
{
	const bool enabled = isA
		? enableButtonA.getToggleState()
		: enableButtonB.getToggleState();

	const float alpha = enabled ? 1.0f : 0.35f;
	const bool interactive = enabled && ! promptOverlayActive;

	juce::Component* components[] = {
		isA ? &browseButtonA   : &browseButtonB,
		isA ? &fileDisplayA    : &fileDisplayB,
		isA ? &hpFreqSliderA   : &hpFreqSliderB,
		isA ? &lpFreqSliderA   : &lpFreqSliderB,
		isA ? &outSliderA      : &outSliderB,
		isA ? &startSliderA    : &startSliderB,
		isA ? &endSliderA      : &endSliderB,
		isA ? &pitchSliderA    : &pitchSliderB,
		isA ? &delaySliderA    : &delaySliderB,
		isA ? &panSliderA      : &panSliderB,
		isA ? &fredSliderA     : &fredSliderB,
		isA ? &posSliderA      : &posSliderB,
		isA ? &invButtonA      : &invButtonB,
		isA ? &normButtonA     : &normButtonB,
		isA ? &rvsButtonA      : &rvsButtonB,
		isA ? &chaosButtonA    : &chaosButtonB,
		isA ? &chaosDisplayA   : &chaosDisplayB
	};

	for (auto* c : components)
	{
		c->setAlpha (alpha);
		c->setEnabled (interactive);
	}

	repaint();
}

//==============================================================================
//  Callbacks
//==============================================================================
void CABTRAudioProcessorEditor::timerCallback()
{
	// Update file display labels if paths exist but labels show "No file loaded"
	if (!audioProcessor.stateA.currentFilePath.isEmpty() && 
	    fileDisplayA.getText() == "No file loaded")
	{
		juce::File fileA (audioProcessor.stateA.currentFilePath);
		if (fileA.existsAsFile())
		{
			currentFileA = fileA.getFileName();
			currentFolderA = fileA.getParentDirectory();
			fileDisplayA.setText (currentFileA, juce::dontSendNotification);
		}
	}
	
	if (!audioProcessor.stateB.currentFilePath.isEmpty() && 
	    fileDisplayB.getText() == "No file loaded")
	{
		juce::File fileB (audioProcessor.stateB.currentFilePath);
		if (fileB.existsAsFile())
		{
			currentFileB = fileB.getFileName();
			currentFolderB = fileB.getParentDirectory();
			fileDisplayB.setText (currentFileB, juce::dontSendNotification);
		}
	}
	
	// CRT effect animation
	if (crtEnabled)
	{
		crtTime += 1.0f / (float) kCrtTimerHz;
		crtEffect.setTime (crtTime);
		repaint();
	}
}

void CABTRAudioProcessorEditor::sliderValueChanged (juce::Slider* slider)
{
	juce::ignoreUnused (slider);
	legendDirty = true;
	repaint();
}

void CABTRAudioProcessorEditor::buttonClicked (juce::Button* button)
{
	if (button == &browseButtonA)
	{
		openFileExplorer (true);
	}
	else if (button == &browseButtonB)
	{
		openFileExplorer (false);
	}
}

void CABTRAudioProcessorEditor::comboBoxChanged (juce::ComboBox* combo)
{
	juce::ignoreUnused (combo);
}

void CABTRAudioProcessorEditor::parameterChanged (const juce::String& paramID, float newValue)
{
	if (paramID == CABTRAudioProcessor::kParamUiFxTail)
	{
		crtEnabled = newValue > 0.5f;
		crtEffect.setEnabled (crtEnabled);
	}
	else if (paramID == CABTRAudioProcessor::kParamEnableA)
	{
		juce::MessageManager::callAsync ([safeThis = juce::Component::SafePointer<CABTRAudioProcessorEditor> (this)] ()
		{
			if (safeThis != nullptr)
				safeThis->updateLoaderEnabledState (true);
		});
	}
	else if (paramID == CABTRAudioProcessor::kParamEnableB)
	{
		juce::MessageManager::callAsync ([safeThis = juce::Component::SafePointer<CABTRAudioProcessorEditor> (this)] ()
		{
			if (safeThis != nullptr)
				safeThis->updateLoaderEnabledState (false);
		});
	}
}

//==============================================================================
//  File Operations
//==============================================================================
bool CABTRAudioProcessorEditor::isInterestedInFileDrag (const juce::StringArray& files)
{
	for (const auto& file : files)
		if (file.endsWithIgnoreCase (".wav") || file.endsWithIgnoreCase (".aif") ||
		    file.endsWithIgnoreCase (".aiff") || file.endsWithIgnoreCase (".flac") ||
		    file.endsWithIgnoreCase (".mp3") || file.endsWithIgnoreCase (".ogg"))
			return true;
	return false;
}

void CABTRAudioProcessorEditor::filesDropped (const juce::StringArray& files, int x, int y)
{
	juce::ignoreUnused (y);
	if (files.isEmpty())
		return;

	// Determine which IR loader based on x position
	const bool dropOnA = x < getWidth() / 2;
	loadIRFile (files[0], dropOnA);
}

void CABTRAudioProcessorEditor::updateFileDisplayLabels (const juce::String& pathA, const juce::String& pathB)
{
	// Update loader A
	if (pathA.isNotEmpty())
	{
		juce::File fileA (pathA);
		if (fileA.existsAsFile())
		{
			currentFileA = fileA.getFileName();
			currentFolderA = fileA.getParentDirectory();
			fileDisplayA.setText (currentFileA, juce::dontSendNotification);
		}
	}
	else
	{
		currentFileA = "";
		fileDisplayA.setText ("No file loaded", juce::dontSendNotification);
	}
	
	// Update loader B
	if (pathB.isNotEmpty())
	{
		juce::File fileB (pathB);
		if (fileB.existsAsFile())
		{
			currentFileB = fileB.getFileName();
			currentFolderB = fileB.getParentDirectory();
			fileDisplayB.setText (currentFileB, juce::dontSendNotification);
		}
	}
	else
	{
		currentFileB = "";
		fileDisplayB.setText ("No file loaded", juce::dontSendNotification);
	}
}

void CABTRAudioProcessorEditor::openFileExplorer (bool forLoaderA)
{
	using namespace TR;

	lnf.setScheme (activeScheme);
	setPromptOverlayActive (true);

	auto* aw = new juce::AlertWindow ("", "", juce::MessageBoxIconType::NoIcon);
	juce::Component::SafePointer<CABTRAudioProcessorEditor> safeThis (this);
	juce::Component::SafePointer<juce::AlertWindow> safeAw (aw);
	aw->setLookAndFeel (&lnf);
	
	// Set background from palette
	aw->setColour (juce::AlertWindow::backgroundColourId, activeScheme.bg);

	// Size for file browser
	constexpr int browserWidth = 520;
	constexpr int browserHeight = 420;
	aw->setSize (browserWidth, browserHeight);

	// Create file list model and ListBox
	auto* fileModel = new FileListModel (activeScheme);
	auto* listBox = new juce::ListBox ("", fileModel); // No label

	// Add current path label at the top
	auto* pathLabel = new juce::Label ("Path", (forLoaderA ? currentFolderA : currentFolderB).getFullPathName());
	pathLabel->setFont (juce::Font (juce::FontOptions (13.0f)));
	pathLabel->setColour (juce::Label::textColourId, activeScheme.text);
	pathLabel->setJustificationType (juce::Justification::centredLeft);

	// Add drive selection dropdown
	auto* driveCombo = new juce::ComboBox ("Drives");
	driveCombo->setLookAndFeel (&lnf);
	
	// Create SafePointers for all UI components to prevent use-after-free
	juce::Component::SafePointer<juce::ListBox> safeListBox (listBox);
	juce::Component::SafePointer<juce::Label> safePathLabel (pathLabel);
	juce::Component::SafePointer<juce::ComboBox> safeDriveCombo (driveCombo);
	
	// Populate with available drives (Windows A: to Z:)
	int driveIdx = 1;
	for (char driveLetter = 'A'; driveLetter <= 'Z'; ++driveLetter)
	{
		juce::String drivePath = juce::String::charToString (driveLetter) + ":/";
		juce::File driveFile (drivePath);
		if (driveFile.exists())
		{
			driveCombo->addItem (drivePath, driveIdx++);
		}
	}
	
	// Set current drive based on folder path
	auto currentFolder = forLoaderA ? currentFolderA : currentFolderB;
	juce::String folderPath = currentFolder.getFullPathName().replaceCharacter ('\\', '/');
	int selectedDriveIdx = -1;
	for (int i = 0; i < driveCombo->getNumItems(); ++i)
	{
		if (folderPath.startsWithIgnoreCase (driveCombo->getItemText (i)))
		{
			selectedDriveIdx = i;
			break;
		}
	}
	if (selectedDriveIdx >= 0)
		driveCombo->setSelectedItemIndex (selectedDriveIdx, juce::dontSendNotification);
	else
		driveCombo->setTextWhenNothingSelected ("C:/");

	fileModel->setCurrentFolder (currentFolder);

	// Drive selection callback
	driveCombo->onChange = [fileModel, safeListBox, safePathLabel, safeDriveCombo]() mutable
	{
		if (safeDriveCombo == nullptr)
			return;
			
		auto selectedDrive = safeDriveCombo->getText();
		auto driveFile = juce::File (selectedDrive);
		
		if (driveFile.exists() && driveFile.isDirectory())
		{
			fileModel->setCurrentFolder (driveFile);
			
			// Check UI components are still valid before using them
			if (safeListBox != nullptr)
			{
				safeListBox->updateContent();
				safeListBox->repaint();
			}
			if (safePathLabel != nullptr)
				safePathLabel->setText (driveFile.getFullPathName(), juce::dontSendNotification);
		}
	};

	// Callbacks for navigation
	fileModel->onNavigateUp = [fileModel, safeListBox, safePathLabel, safeDriveCombo]() mutable
	{
		auto parent = fileModel->currentFolder.getParentDirectory();
		if (parent.exists() && parent.isDirectory())
		{
			fileModel->setCurrentFolder (parent);
			
			// Check UI components are still valid before using them
			if (safeListBox != nullptr)
			{
				safeListBox->updateContent();
				safeListBox->repaint();
			}
			if (safePathLabel != nullptr)
				safePathLabel->setText (parent.getFullPathName(), juce::dontSendNotification);
			
			// Update drive combo if we've changed drives
			if (safeDriveCombo != nullptr)
			{
				for (int i = 0; i < safeDriveCombo->getNumItems(); ++i)
				{
					if (parent.getFullPathName().startsWith (safeDriveCombo->getItemText (i)))
					{
						safeDriveCombo->setSelectedItemIndex (i, juce::dontSendNotification);
						break;
					}
				}
			}
		}
	};

fileModel->onNavigateInto = [fileModel, safeListBox, safePathLabel] (const juce::File& folder) mutable
	{
		// Safety check: ensure it's a valid directory
		if (! folder.exists() || ! folder.isDirectory())
			return;
			
		fileModel->setCurrentFolder (folder);
		
		// Check UI components are still valid before using them
		if (safeListBox != nullptr)
		{
			safeListBox->updateContent();
			safeListBox->repaint();
		}
		if (safePathLabel != nullptr)
			safePathLabel->setText (folder.getFullPathName(), juce::dontSendNotification);
	};

	fileModel->onFileSelected = [safeThis, safeAw, forLoaderA] (const juce::File& file) mutable
	{
		if (safeThis == nullptr || safeAw == nullptr)
			return;
			
		// Only load valid audio files
		if (file.existsAsFile() && file.getSize() > 0)
		{
			safeThis->loadIRFile (file.getFullPathName(), forLoaderA);
			safeAw->exitModalState (1);
		}
	};

	// Style the ListBox with green border
	listBox->setColour (juce::ListBox::backgroundColourId, activeScheme.bg);
	listBox->setColour (juce::ListBox::outlineColourId, activeScheme.fg);
	listBox->setOutlineThickness (1);
	listBox->setRowHeight (24);
	listBox->setMultipleSelectionEnabled (false);

	// Add components to AlertWindow
	constexpr int margin = 24;
	constexpr int driveComboH = 28;
	constexpr int pathLabelH = 24;
	constexpr int componentGap = 8;
	constexpr int buttonAreaH = 92;  // More bottom margin

	const int contentTop = 12;  // More reduced top margin
	const int contentW = browserWidth - (margin * 2);
	
	driveCombo->setBounds (margin, contentTop, contentW, driveComboH);
	pathLabel->setBounds (margin, contentTop + driveComboH + componentGap, contentW, pathLabelH);
	
	const int listBoxY = contentTop + driveComboH + componentGap + pathLabelH + componentGap;
	const int listBoxH = browserHeight - listBoxY - buttonAreaH - margin;
	listBox->setBounds (margin, listBoxY, contentW, listBoxH);

	aw->addCustomComponent (driveCombo);
	aw->addCustomComponent (pathLabel);
	aw->addCustomComponent (listBox);

	// Buttons
	aw->addButton ("SELECT", 1, juce::KeyPress (juce::KeyPress::returnKey));
	aw->addButton ("CANCEL", 0, juce::KeyPress (juce::KeyPress::escapeKey));

	styleAlertButtons (*aw, lnf);
	layoutAlertWindowButtons (*aw);

	aw->setEscapeKeyCancels (true);

	// Embed in overlay instead of modal
	if (safeThis != nullptr)
	{
		embedAlertWindowInOverlay (safeThis.getComponent(), aw);
	}

	aw->enterModalState (true, juce::ModalCallbackFunction::create ([safeThis, aw, fileModel, listBox, pathLabel, driveCombo, safeListBox, safePathLabel, safeDriveCombo, forLoaderA] (int result) mutable
	{
		std::unique_ptr<juce::AlertWindow> killer (aw);
		
		if (safeThis == nullptr)
			return;
		
		const bool shouldSelect = (result == 1);

		if (shouldSelect && fileModel->selectedRow >= 0 && fileModel->selectedRow < fileModel->items.size())
		{
			auto& item = fileModel->items.getReference (fileModel->selectedRow);
			
			// Only load valid files, not directories or empty files
			if (! item.isDirectory && item.file.existsAsFile() && item.file.getSize() > 0 && item.displayName != "..")
			{
				safeThis->loadIRFile (item.file.getFullPathName(), forLoaderA);
			}
		}
		
		// Update currentFolder ONLY here in modal callback (safe, synchronous)
		if (forLoaderA)
			safeThis->currentFolderA = fileModel->currentFolder;
		else
			safeThis->currentFolderB = fileModel->currentFolder;

		// CRITICAL: Clear all callbacks BEFORE deleting objects to prevent use-after-free
		fileModel->onNavigateUp = nullptr;
		fileModel->onNavigateInto = nullptr;
		fileModel->onFileSelected = nullptr;
		driveCombo->onChange = nullptr;

		// Clean up
		delete driveCombo;
		delete pathLabel;
		delete listBox;
		delete fileModel;

		safeThis->setPromptOverlayActive (false);
	}), true);
}

void CABTRAudioProcessorEditor::browseToParentFolder (bool forLoaderA)
{
	auto& currentFolder = forLoaderA ? currentFolderA : currentFolderB;
	auto parent = currentFolder.getParentDirectory();
	
	if (parent.exists())
		currentFolder = parent;

	// TODO: Update file list display in custom file browser
}

void CABTRAudioProcessorEditor::loadIRFile (const juce::String& path, bool forLoaderA)
{
	// Safety check: ensure file exists and is not a directory
	juce::File file (path);
	if (! file.existsAsFile() || file.isDirectory() || file.getSize() == 0)
		return;
	
	if (forLoaderA)
	{
		currentFileA = path;
		fileDisplayA.setText (file.getFileName(), juce::dontSendNotification);
		audioProcessor.loadImpulseResponse (audioProcessor.stateA, path);
	}
	else
	{
		currentFileB = path;
		fileDisplayB.setText (file.getFileName(), juce::dontSendNotification);
		audioProcessor.loadImpulseResponse (audioProcessor.stateB, path);
	}
}

//==============================================================================
//  Mouse Events
//==============================================================================
void CABTRAudioProcessorEditor::mouseDown (const juce::MouseEvent& e)
{
	const auto p = e.getEventRelativeTo (this).getPosition();

	// Click on gear icon opens info popup (with Graphics button inside)
	if (getInfoIconArea().contains (p))
	{
		openInfoPopup();
		return;
	}

	// Click on export icon opens export prompt
	if (getExportIconArea().contains (p))
	{
		openExportPrompt();
		return;
	}

	// CHAOS display overlay: left-click toggles, right-click opens prompt
	if (chaosDisplayA.getBounds().contains (p) && enableButtonA.getToggleState())
	{
		if (e.mods.isPopupMenu())
			openChaosPrompt (true);
		else
			chaosButtonA.setToggleState (! chaosButtonA.getToggleState(), juce::sendNotificationSync);
		return;
	}
	if (chaosDisplayB.getBounds().contains (p) && enableButtonB.getToggleState())
	{
		if (e.mods.isPopupMenu())
			openChaosPrompt (false);
		else
			chaosButtonB.setToggleState (! chaosButtonB.getToggleState(), juce::sendNotificationSync);
		return;
	}

	// Right-click on value area opens numeric entry popup
	if (e.mods.isPopupMenu())
	{
		if (auto* slider = getSliderForValueAreaPoint (p))
		{
			openNumericEntryPopupForSlider (*slider);
			return;
		}
	}

	// Window dragging (if applicable)
	if (e.mods.isMiddleButtonDown() || (e.mods.isLeftButtonDown() && e.mods.isAltDown()))
	{
		isDraggingWindow = true;
		dragStartPos = e.getScreenPosition();
	}
}

void CABTRAudioProcessorEditor::mouseDoubleClick (const juce::MouseEvent& e)
{
	juce::ignoreUnused (e);
}

void CABTRAudioProcessorEditor::mouseDrag (const juce::MouseEvent& e)
{
	if (isDraggingWindow && e.mods.isLeftButtonDown())
	{
		// Window dragging handled by OS in plugin contexts
	}
}

//==============================================================================
//  TR-style label/value system helpers
//==============================================================================

void CABTRAudioProcessorEditor::setupBar (juce::Slider& s)
{
	s.setSliderStyle (juce::Slider::LinearBar);
	s.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
	s.setPopupDisplayEnabled (false, false, this);
	s.setTooltip (juce::String());
	s.setPopupMenuEnabled (false);
	s.setColour (juce::Slider::trackColourId, juce::Colours::transparentBlack);
	s.setColour (juce::Slider::backgroundColourId, juce::Colours::transparentBlack);
	s.setColour (juce::Slider::thumbColourId, juce::Colours::transparentBlack);
}

bool CABTRAudioProcessorEditor::refreshLegendTextCache()
{
	using namespace TR;

	// Helper lambda for formatting frequency
	auto formatFreq = [] (double hz) {
		if (hz < 1000.0)
			return juce::String (hz, 1) + " Hz";
		return juce::String (hz / 1000.0, 2) + " kHz";
	};

	// Helper lambda for dB display
	auto formatDb = [] (float db) -> juce::String {
		if (db <= -80.0f)
			return "-INF";
		return juce::String (db, 1) + " dB";
	};

	// Helper lambda for pan (L/C/R)
	auto formatPan = [] (float pan01) -> juce::String {
		const int pct = juce::roundToInt ((pan01 - 0.5f) * 200.0f);  // -100 to +100
		if (pct == 0)
			return "C";
		else if (pct < 0)
			return "L" + juce::String (-pct);
		else
			return "R" + juce::String (pct);
	};

	// Loader A
	cachedHpFreqTextAFull = formatFreq (hpFreqSliderA.getValue()) + " HP";
	cachedHpFreqTextAShort = formatFreq (hpFreqSliderA.getValue());
	cachedHpFreqTextAInt = juce::String (juce::roundToInt (hpFreqSliderA.getValue()));

	cachedLpFreqTextAFull = formatFreq (lpFreqSliderA.getValue()) + " LP";
	cachedLpFreqTextAShort = formatFreq (lpFreqSliderA.getValue());
	cachedLpFreqTextAInt = juce::String (juce::roundToInt (lpFreqSliderA.getValue()));

	cachedOutTextAFull = formatDb ((float) outSliderA.getValue()) + " OUT";
	cachedOutTextAShort = formatDb ((float) outSliderA.getValue());
	cachedOutTextAInt = juce::String (juce::roundToInt (outSliderA.getValue()));

	cachedStartTextAFull = juce::String (juce::roundToInt (startSliderA.getValue())) + " ms START";
	cachedStartTextAShort = juce::String (juce::roundToInt (startSliderA.getValue())) + " ms";
	cachedStartTextAInt = juce::String (juce::roundToInt (startSliderA.getValue()));

	cachedEndTextAFull = juce::String (juce::roundToInt (endSliderA.getValue())) + " ms END";
	cachedEndTextAShort = juce::String (juce::roundToInt (endSliderA.getValue())) + " ms";
	cachedEndTextAInt = juce::String (juce::roundToInt (endSliderA.getValue()));

	const int pitchPctA = juce::roundToInt (pitchSliderA.getValue() * 100.0);
	cachedPitchTextAFull = juce::String (pitchPctA) + "% PITCH";
	cachedPitchTextAShort = juce::String (pitchPctA) + "%";
	cachedPitchTextAInt = juce::String (pitchPctA);

	cachedDelayTextAFull = juce::String (juce::roundToInt (delaySliderA.getValue())) + " ms DELAY";
	cachedDelayTextAShort = juce::String (juce::roundToInt (delaySliderA.getValue())) + " ms";
	cachedDelayTextAInt = juce::String (juce::roundToInt (delaySliderA.getValue()));

	cachedPanTextAFull = formatPan ((float) panSliderA.getValue()) + " PAN";
	cachedPanTextAShort = formatPan ((float) panSliderA.getValue());
	cachedPanTextAInt = formatPan ((float) panSliderA.getValue());

	const int fredPctA = juce::roundToInt (fredSliderA.getValue() * 100.0);
	cachedFredTextAFull = juce::String (fredPctA) + "% ANGLE";
	cachedFredTextAShort = juce::String (fredPctA) + "%";
	cachedFredTextAInt = juce::String (fredPctA);

	const int posPctA = juce::roundToInt (posSliderA.getValue() * 100.0);
	cachedPosTextAFull = juce::String (posPctA) + "% DIST";
	cachedPosTextAShort = juce::String (posPctA) + "%";
	cachedPosTextAInt = juce::String (posPctA);

	// Loader B
	cachedHpFreqTextBFull = formatFreq (hpFreqSliderB.getValue()) + " HP";
	cachedHpFreqTextBShort = formatFreq (hpFreqSliderB.getValue());
	cachedHpFreqTextBInt = juce::String (juce::roundToInt (hpFreqSliderB.getValue()));

	cachedLpFreqTextBFull = formatFreq (lpFreqSliderB.getValue()) + " LP";
	cachedLpFreqTextBShort = formatFreq (lpFreqSliderB.getValue());
	cachedLpFreqTextBInt = juce::String (juce::roundToInt (lpFreqSliderB.getValue()));

	cachedOutTextBFull = formatDb ((float) outSliderB.getValue()) + " OUT";
	cachedOutTextBShort = formatDb ((float) outSliderB.getValue());
	cachedOutTextBInt = juce::String (juce::roundToInt (outSliderB.getValue()));

	cachedStartTextBFull = juce::String (juce::roundToInt (startSliderB.getValue())) + " ms START";
	cachedStartTextBShort = juce::String (juce::roundToInt (startSliderB.getValue())) + " ms";
	cachedStartTextBInt = juce::String (juce::roundToInt (startSliderB.getValue()));

	cachedEndTextBFull = juce::String (juce::roundToInt (endSliderB.getValue())) + " ms END";
	cachedEndTextBShort = juce::String (juce::roundToInt (endSliderB.getValue())) + " ms";
	cachedEndTextBInt = juce::String (juce::roundToInt (endSliderB.getValue()));

	const int pitchPctB = juce::roundToInt (pitchSliderB.getValue() * 100.0);
	cachedPitchTextBFull = juce::String (pitchPctB) + "% PITCH";
	cachedPitchTextBShort = juce::String (pitchPctB) + "%";
	cachedPitchTextBInt = juce::String (pitchPctB);

	cachedDelayTextBFull = juce::String (juce::roundToInt (delaySliderB.getValue())) + " ms DELAY";
	cachedDelayTextBShort = juce::String (juce::roundToInt (delaySliderB.getValue())) + " ms";
	cachedDelayTextBInt = juce::String (juce::roundToInt (delaySliderB.getValue()));

	cachedPanTextBFull = formatPan ((float) panSliderB.getValue()) + " PAN";
	cachedPanTextBShort = formatPan ((float) panSliderB.getValue());
	cachedPanTextBInt = formatPan ((float) panSliderB.getValue());

	const int fredPctB = juce::roundToInt (fredSliderB.getValue() * 100.0);
	cachedFredTextBFull = juce::String (fredPctB) + "% ANGLE";
	cachedFredTextBShort = juce::String (fredPctB) + "%";
	cachedFredTextBInt = juce::String (fredPctB);

	const int posPctB = juce::roundToInt (posSliderB.getValue() * 100.0);
	cachedPosTextBFull = juce::String (posPctB) + "% DIST";
	cachedPosTextBShort = juce::String (posPctB) + "%";
	cachedPosTextBInt = juce::String (posPctB);

	return false;  // Return true if layout needs updating due to text length change
}

juce::Rectangle<int> CABTRAudioProcessorEditor::getValueAreaFor (const juce::Rectangle<int>& barBounds) const
{
	constexpr int valuePadPx = 8;
	constexpr int valueWidthPx = 200;
	constexpr int valueHeightPx = 24;
	constexpr int rightMarginPx = 10;

	const int valueX = barBounds.getRight() + valuePadPx;
	const int maxW = juce::jmax (0, getWidth() - valueX - rightMarginPx);
	const int valueW = juce::jmin (valueWidthPx, maxW);
	const int y = barBounds.getCentreY() - (valueHeightPx / 2);

	return { valueX, y, juce::jmax (0, valueW), valueHeightPx };
}

juce::Slider* CABTRAudioProcessorEditor::getSliderForValueAreaPoint (juce::Point<int> p)
{
	// Check Loader A sliders
	if (getValueAreaFor (hpFreqSliderA.getBounds()).contains (p))   return &hpFreqSliderA;
	if (getValueAreaFor (lpFreqSliderA.getBounds()).contains (p))   return &lpFreqSliderA;
	if (getValueAreaFor (outSliderA.getBounds()).contains (p))      return &outSliderA;
	if (getValueAreaFor (startSliderA.getBounds()).contains (p))    return &startSliderA;
	if (getValueAreaFor (endSliderA.getBounds()).contains (p))      return &endSliderA;
	if (getValueAreaFor (pitchSliderA.getBounds()).contains (p))    return &pitchSliderA;
	if (getValueAreaFor (delaySliderA.getBounds()).contains (p))    return &delaySliderA;
	if (getValueAreaFor (panSliderA.getBounds()).contains (p))      return &panSliderA;
	if (getValueAreaFor (fredSliderA.getBounds()).contains (p))      return &fredSliderA;
	if (getValueAreaFor (posSliderA.getBounds()).contains (p))      return &posSliderA;

	// Check Loader B sliders
	if (getValueAreaFor (hpFreqSliderB.getBounds()).contains (p))   return &hpFreqSliderB;
	if (getValueAreaFor (lpFreqSliderB.getBounds()).contains (p))   return &lpFreqSliderB;
	if (getValueAreaFor (outSliderB.getBounds()).contains (p))      return &outSliderB;
	if (getValueAreaFor (startSliderB.getBounds()).contains (p))    return &startSliderB;
	if (getValueAreaFor (endSliderB.getBounds()).contains (p))      return &endSliderB;
	if (getValueAreaFor (pitchSliderB.getBounds()).contains (p))    return &pitchSliderB;
	if (getValueAreaFor (delaySliderB.getBounds()).contains (p))    return &delaySliderB;
	if (getValueAreaFor (panSliderB.getBounds()).contains (p))      return &panSliderB;
	if (getValueAreaFor (fredSliderB.getBounds()).contains (p))      return &fredSliderB;
	if (getValueAreaFor (posSliderB.getBounds()).contains (p))      return &posSliderB;

	// Global
	if (getValueAreaFor (globalMixSlider.getBounds()).contains (p)) return &globalMixSlider;

	return nullptr;
}

juce::Rectangle<int> CABTRAudioProcessorEditor::getInfoIconArea() const
{
	constexpr int size = 32;
	constexpr int margin = 10;
	const int x = getWidth() - size - margin;
	const int y = margin;
	return { x, y, size, size };
}

void CABTRAudioProcessorEditor::updateInfoIconCache()
{
	const auto iconArea = getInfoIconArea();
	const auto iconF = iconArea.toFloat();
	const auto center = iconF.getCentre();
	const float toothTipR = (float) iconArea.getWidth() * 0.47f;
	const float toothRootR = toothTipR * 0.78f;
	const float holeR = toothTipR * 0.40f;
	constexpr int teeth = 8;

	cachedInfoGearPath.clear();
	for (int i = 0; i < teeth * 2; ++i)
	{
		const float a = -juce::MathConstants<float>::halfPi
		              + (juce::MathConstants<float>::pi * (float) i / (float) teeth);
		const float r = (i % 2 == 0) ? toothTipR : toothRootR;
		const float x = center.x + std::cos (a) * r;
		const float y = center.y + std::sin (a) * r;

		if (i == 0)
			cachedInfoGearPath.startNewSubPath (x, y);
		else
			cachedInfoGearPath.lineTo (x, y);
	}
	cachedInfoGearPath.closeSubPath();
	cachedInfoGearHole = { center.x - holeR, center.y - holeR, holeR * 2.0f, holeR * 2.0f };
}

juce::Rectangle<int> CABTRAudioProcessorEditor::getExportIconArea() const
{
	constexpr int size = 24;
	constexpr int x = 134;
	constexpr int y = 16;
	return { x, y, size, size };
}

void CABTRAudioProcessorEditor::updateExportIconCache()
{
	const auto area = getExportIconArea().toFloat();
	cachedExportIconPath.clear();

	const float cx = area.getCentreX();
	const float top = area.getY();
	const float w = area.getWidth();
	const float h = area.getHeight();

	// Downward arrow: shaft + triangular head
	const float shaftW = w * 0.12f;
	const float shaftTop = top + h * 0.08f;
	const float shaftBot = top + h * 0.46f;
	const float arrowW = w * 0.34f;
	const float arrowTip = top + h * 0.70f;

	cachedExportIconPath.startNewSubPath (cx - shaftW, shaftTop);
	cachedExportIconPath.lineTo (cx + shaftW, shaftTop);
	cachedExportIconPath.lineTo (cx + shaftW, shaftBot);
	cachedExportIconPath.lineTo (cx + arrowW, shaftBot);
	cachedExportIconPath.lineTo (cx, arrowTip);
	cachedExportIconPath.lineTo (cx - arrowW, shaftBot);
	cachedExportIconPath.lineTo (cx - shaftW, shaftBot);
	cachedExportIconPath.closeSubPath();

	// Bottom tray
	const float barY = top + h * 0.80f;
	const float barH = h * 0.10f;
	const float barW = w * 0.72f;
	cachedExportIconPath.addRectangle (cx - barW / 2.0f, barY, barW, barH);
}

//==============================================================================
void CABTRAudioProcessorEditor::setPromptOverlayActive (bool shouldBeActive)
{
	if (promptOverlayActive == shouldBeActive)
		return;

	promptOverlayActive = shouldBeActive;

	promptOverlay.setBounds (getLocalBounds());
	promptOverlay.setVisible (shouldBeActive);
	if (shouldBeActive)
		promptOverlay.toFront (false);

	if (shouldBeActive)
	{
		// Disable ALL interactive controls while prompt is open
		const std::array<juce::Component*, 10> globalControls {
			&globalMixSlider, &enableButtonA, &enableButtonB, &alignButton,
			&browseButtonA, &browseButtonB,
			&modeInCombo, &modeCombo, &routeCombo,
			&invButtonA
		};
		for (auto* c : globalControls)
			c->setEnabled (false);

		// Disable loader subsection controls
		updateLoaderEnabledState (true);
		updateLoaderEnabledState (false);
	}
	else
	{
		// Re-enable global controls
		const std::array<juce::Component*, 10> globalControls {
			&globalMixSlider, &enableButtonA, &enableButtonB, &alignButton,
			&browseButtonA, &browseButtonB,
			&modeInCombo, &modeCombo, &routeCombo,
			&invButtonA
		};
		for (auto* c : globalControls)
			c->setEnabled (true);

		// Re-apply loader enabled state (respects per-loader enable toggle)
		updateLoaderEnabledState (true);
		updateLoaderEnabledState (false);
	}

	repaint();

	if (promptOverlayActive)
		promptOverlay.toFront (false);

	TR::anchorEditorOwnedPromptWindows (*this, lnf);
}

void CABTRAudioProcessorEditor::moved()
{
	if (promptOverlayActive)
		promptOverlay.toFront (false);

	TR::anchorEditorOwnedPromptWindows (*this, lnf);
}

void CABTRAudioProcessorEditor::parentHierarchyChanged()
{
	if (promptOverlayActive)
		promptOverlay.toFront (false);
}

//==============================================================================
//  Prompts
//==============================================================================
void CABTRAudioProcessorEditor::openNumericEntryPopupForSlider (juce::Slider& s)
{
	using namespace TR;
	lnf.setScheme (activeScheme);
	const auto scheme = activeScheme;

	// ── Suffix determination ──
	juce::String suffix;
	juce::String suffixShort;
	const bool isHpLp   = (&s == &hpFreqSliderA || &s == &hpFreqSliderB
	                     || &s == &lpFreqSliderA || &s == &lpFreqSliderB);
	const bool isOut     = (&s == &outSliderA || &s == &outSliderB);
	const bool isStart   = (&s == &startSliderA || &s == &startSliderB);
	const bool isEnd     = (&s == &endSliderA || &s == &endSliderB);
	const bool isPitch   = (&s == &pitchSliderA || &s == &pitchSliderB);
	const bool isDelay   = (&s == &delaySliderA || &s == &delaySliderB);
	const bool isPan     = (&s == &panSliderA || &s == &panSliderB);
	const bool isFred    = (&s == &fredSliderA || &s == &fredSliderB);
	const bool isPos     = (&s == &posSliderA || &s == &posSliderB);
	const bool isMix     = (&s == &globalMixSlider);

	if (isHpLp)             { suffix = " Hz";          suffixShort = " Hz"; }
	else if (isOut)         { suffix = " dB OUTPUT";   suffixShort = " dB"; }
	else if (isStart)       { suffix = " ms START";    suffixShort = " ms"; }
	else if (isEnd)         { suffix = " ms END";      suffixShort = " ms"; }
	else if (isPitch)       { suffix = " % PITCH";     suffixShort = " %"; }
	else if (isDelay)       { suffix = " ms DELAY";    suffixShort = " ms"; }
	else if (isPan)         { suffix = " % PAN";       suffixShort = " %"; }
	else if (isFred)        { suffix = " % ANGLE";    suffixShort = " %"; }
	else if (isPos)         { suffix = " % DIST";     suffixShort = " %"; }
	else if (isMix)         { suffix = " % MIX";       suffixShort = " %"; }

	const juce::String suffixText      = suffix.trimStart();
	const juce::String suffixTextShort = suffixShort.trimStart();

	auto* aw = new juce::AlertWindow ("", "", juce::AlertWindow::NoIcon);
	aw->setLookAndFeel (&lnf);

	// ── Initial display value ──
	juce::String currentDisplay;
	if (isHpLp)
		currentDisplay = juce::String (s.getValue(), 3);
	else if (isOut)
		currentDisplay = juce::String (s.getValue(), 1);
	else if (isStart || isEnd || isDelay)
		currentDisplay = juce::String (s.getValue(), 3);
	else if (isPitch)
		currentDisplay = juce::String (juce::jlimit (25.0, 400.0, s.getValue() * 100.0), 1);
	else if (isPan)
		currentDisplay = juce::String (juce::jlimit (0.0, 100.0, s.getValue() * 100.0), 0);
	else if (isFred || isPos || isMix)
		currentDisplay = juce::String (juce::jlimit (0.0, 100.0, s.getValue() * 100.0), 1);
	else
		currentDisplay = s.getTextFromValue (s.getValue());

	aw->addTextEditor ("val", currentDisplay, juce::String());

	juce::Label* suffixLabel = nullptr;
	juce::Rectangle<int> editorBaseBounds;
	std::function<void()> layoutValueAndSuffix;

	if (auto* te = aw->getTextEditor ("val"))
	{
		const auto& f = kBoldFont40();
		te->setFont (f);
		te->applyFontToAllText (f);

		auto r = te->getBounds();
		r.setHeight ((int) (f.getHeight() * kPromptEditorHeightScale) + kPromptEditorHeightPadPx);
		r.setY (juce::jmax (kPromptEditorMinTopPx, r.getY() - kPromptEditorRaiseYPx));
		editorBaseBounds = r;

		suffixLabel = new juce::Label ("suffix", suffixText);
		suffixLabel->setComponentID (kPromptSuffixLabelId);
		suffixLabel->setJustificationType (juce::Justification::centredLeft);
		applyLabelTextColour (*suffixLabel, scheme.text);
		suffixLabel->setBorderSize (juce::BorderSize<int> (0));
		suffixLabel->setFont (f);
		aw->addAndMakeVisible (suffixLabel);

		// Worst-case widths for layout stability
		juce::String worstCaseText;
		if (isHpLp)              worstCaseText = "20000.000";
		else if (isOut)          worstCaseText = "-100.0";
		else if (isStart||isEnd) worstCaseText = "10000.000";
		else if (isPitch)        worstCaseText = "400.0";
		else if (isDelay)        worstCaseText = "1000.000";
		else if (isPan)          worstCaseText = "100";
		else if (isFred||isPos)  worstCaseText = "100.0";
		else if (isMix)          worstCaseText = "100.0";
		else                     worstCaseText = "999.99";

		const int maxInputTextW = juce::jmax (1, stringWidth (f, worstCaseText));

		layoutValueAndSuffix = [aw, te, suffixLabel, editorBaseBounds,
		                        suffixText, suffixTextShort, maxInputTextW]()
		{
			const int contentPad = kPromptInlineContentPadPx;
			const int contentLeft = contentPad;
			const int contentRight = (aw != nullptr ? aw->getWidth() - contentPad : editorBaseBounds.getRight());
			const int availableW = contentRight - contentLeft;
			const int contentCenter = (contentLeft + contentRight) / 2;

			const int fullLabelW = stringWidth (suffixLabel->getFont(), suffixText) + 2;
			const bool stickPercentFull = suffixText.containsChar ('%');
			const int spaceWFull = stickPercentFull ? 0 : juce::jmax (2, stringWidth (suffixLabel->getFont(), " "));
			const int worstCaseFullW = maxInputTextW + spaceWFull + fullLabelW;

			const bool useShort = (worstCaseFullW > availableW) && suffixTextShort != suffixText;
			const juce::String& activeSuffix = useShort ? suffixTextShort : suffixText;
			suffixLabel->setText (activeSuffix, juce::dontSendNotification);

			const auto txt = te->getText();
			const int textW = juce::jmax (1, stringWidth (te->getFont(), txt));
			int labelW = stringWidth (suffixLabel->getFont(), activeSuffix) + 2;

			const bool stickPercent = activeSuffix.containsChar ('%');
			const int spaceW = stickPercent ? 0 : juce::jmax (2, stringWidth (te->getFont(), " "));
			const int minGapPx = juce::jmax (1, spaceW);

			constexpr int kEditorTextPadPx = 12;
			constexpr int kMinEditorWidthPx = 24;
			const int editorW = juce::jlimit (kMinEditorWidthPx,
			                                  editorBaseBounds.getWidth(),
			                                  textW + kEditorTextPadPx * 2);
			auto er = te->getBounds();
			er.setWidth (editorW);

			const int combinedW = textW + minGapPx + labelW;
			int blockLeft = contentCenter - combinedW / 2;
			blockLeft = juce::jlimit (contentLeft,
			                          juce::jmax (contentLeft, contentRight - combinedW),
			                          blockLeft);

			int teX = blockLeft - (editorW - textW) / 2;
			teX = juce::jlimit (contentLeft,
			                    juce::jmax (contentLeft, contentRight - editorW), teX);
			er.setX (teX);
			te->setBounds (er);

			const int textLeftActual = er.getX() + (er.getWidth() - textW) / 2;
			int labelX = textLeftActual + textW + minGapPx;
			labelX = juce::jlimit (contentLeft,
			                       juce::jmax (contentLeft, contentRight - labelW), labelX);
			suffixLabel->setBounds (labelX, er.getY(), labelW, juce::jmax (1, er.getHeight()));
		};

		te->setBounds (editorBaseBounds);
		int labelW0 = stringWidth (suffixLabel->getFont(), suffixText) + 2;
		suffixLabel->setBounds (r.getRight() + 2, r.getY() + 1, labelW0, juce::jmax (1, r.getHeight() - 2));

		if (layoutValueAndSuffix)
			layoutValueAndSuffix();

		// ── Per-slider input constraints ──
		double minVal = 0.0, maxVal = 1.0;
		int maxLen = 0, maxDecs = 4;

		if (isHpLp)
		{
			minVal = 20.0;   maxVal = 20000.0;
			maxDecs = 3;     maxLen = 9;     // "20000.000"
		}
		else if (isOut)
		{
			minVal = -100.0; maxVal = 24.0;
			maxDecs = 1;     maxLen = 6;     // "-100.0"
		}
		else if (isStart || isEnd)
		{
			minVal = 0.0;    maxVal = 10000.0;
			maxDecs = 3;     maxLen = 9;     // "10000.000"
		}
		else if (isPitch)
		{
			minVal = 25.0;   maxVal = 400.0;
			maxDecs = 1;     maxLen = 5;     // "400.0"
		}
		else if (isDelay)
		{
			minVal = 0.0;    maxVal = 1000.0;
			maxDecs = 3;     maxLen = 8;     // "1000.000"
		}
		else if (isPan)
		{
			minVal = 0.0;    maxVal = 100.0;
			maxDecs = 0;     maxLen = 3;     // "100"
		}
		else if (isFred || isPos || isMix)
		{
			minVal = 0.0;    maxVal = 100.0;
			maxDecs = 1;     maxLen = 5;     // "100.0"
		}

		te->setInputFilter (new NumericInputFilter (minVal, maxVal, maxLen, maxDecs), true);

		const int localMaxDecs = maxDecs;
		te->onTextChange = [te, layoutValueAndSuffix, localMaxDecs]() mutable
		{
			auto txt = te->getText();
			int dot = txt.indexOfChar ('.');
			if (dot >= 0)
			{
				int decimals = txt.length() - dot - 1;
				if (decimals > localMaxDecs)
					te->setText (txt.substring (0, dot + 1 + localMaxDecs), juce::dontSendNotification);
			}
			if (layoutValueAndSuffix)
				layoutValueAndSuffix();
		};
	}

	aw->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
	aw->addButton ("CANCEL", 0, juce::KeyPress (juce::KeyPress::escapeKey));
	applyPromptShellSize (*aw);
	layoutAlertWindowButtons (*aw);

	const juce::Font& kPromptFont = kBoldFont40();

	preparePromptTextEditor (*aw, "val", scheme.bg, scheme.text, scheme.fg, kPromptFont, false);

	if (suffixLabel != nullptr && ! editorBaseBounds.isEmpty())
	{
		if (auto* te = aw->getTextEditor ("val"))
			suffixLabel->setFont (te->getFont());
		if (layoutValueAndSuffix)
			layoutValueAndSuffix();
	}

	styleAlertButtons (*aw, lnf);

	juce::Component::SafePointer<CABTRAudioProcessorEditor> safeThis (this);
	juce::Slider* sliderPtr = &s;

	setPromptOverlayActive (true);

	if (safeThis != nullptr)
	{
		fitAlertWindowToEditor (*aw, safeThis.getComponent(), [layoutValueAndSuffix, scheme, &kPromptFont] (juce::AlertWindow& a)
		{
			if (layoutValueAndSuffix)
				layoutValueAndSuffix();
			layoutAlertWindowButtons (a);
			preparePromptTextEditor (a, "val", scheme.bg, scheme.text, scheme.fg, kPromptFont, false);
		});

		embedAlertWindowInOverlay (safeThis.getComponent(), aw);
	}
	else
	{
		aw->centreAroundComponent (this, aw->getWidth(), aw->getHeight());
		bringPromptWindowToFront (*aw);
		aw->repaint();
	}

	// Final styling pass
	{
		preparePromptTextEditor (*aw, "val", scheme.bg, scheme.text, scheme.fg, kPromptFont, false);
		if (auto* suffixLbl = dynamic_cast<juce::Label*> (aw->findChildWithID (kPromptSuffixLabelId)))
		{
			if (auto* te = aw->getTextEditor ("val"))
				suffixLbl->setFont (te->getFont());
		}
		if (layoutValueAndSuffix)
			layoutValueAndSuffix();

		juce::Component::SafePointer<juce::AlertWindow> safeAw (aw);
		juce::MessageManager::callAsync ([safeAw]()
		{
			if (safeAw == nullptr) return;
			bringPromptWindowToFront (*safeAw);
			safeAw->repaint();
		});
	}

	aw->enterModalState (true,
		juce::ModalCallbackFunction::create ([safeThis, sliderPtr, aw] (int result) mutable
		{
			std::unique_ptr<juce::AlertWindow> killer (aw);

			if (safeThis != nullptr)
				safeThis->setPromptOverlayActive (false);

			if (safeThis == nullptr || sliderPtr == nullptr)
				return;

			if (result != 1)
				return;

			const auto txt = aw->getTextEditorContents ("val").trim();
			auto normalised = txt.replaceCharacter (',', '.');

			juce::String t = normalised.trimStart();
			while (t.startsWithChar ('+'))
				t = t.substring (1).trimStart();
			const juce::String numericToken = t.initialSectionContainingOnly ("0123456789.,-");
			double v = numericToken.getDoubleValue();

			// Percent-based sliders: user typed 0-100, slider stores 0-1
			const bool isPitchSlider  = (sliderPtr == &safeThis->pitchSliderA  || sliderPtr == &safeThis->pitchSliderB);
			const bool isPanSlider    = (sliderPtr == &safeThis->panSliderA    || sliderPtr == &safeThis->panSliderB);
			const bool isFredSlider   = (sliderPtr == &safeThis->fredSliderA   || sliderPtr == &safeThis->fredSliderB);
			const bool isPosSlider    = (sliderPtr == &safeThis->posSliderA    || sliderPtr == &safeThis->posSliderB);
			const bool isMixSlider    = (sliderPtr == &safeThis->globalMixSlider);

			if (isPitchSlider || isPanSlider || isFredSlider || isPosSlider || isMixSlider)
				v *= 0.01;

			const auto range = sliderPtr->getRange();
			double clamped = juce::jlimit (range.getStart(), range.getEnd(), v);

			sliderPtr->setValue (clamped, juce::sendNotificationSync);
		}));
}

//==============================================================================
//  CHAOS prompt (AMOUNT + SPEED)
//==============================================================================
void CABTRAudioProcessorEditor::openChaosPrompt (bool forLoaderA)
{
	using namespace TR;
	lnf.setScheme (activeScheme);
	const auto scheme = activeScheme;

	const auto& amtId = forLoaderA ? CABTRAudioProcessor::kParamChaosAmtA
	                                : CABTRAudioProcessor::kParamChaosAmtB;
	const auto& spdId = forLoaderA ? CABTRAudioProcessor::kParamChaosSpdA
	                                : CABTRAudioProcessor::kParamChaosSpdB;

	const float currentAmt = audioProcessor.getValueTreeState().getRawParameterValue (amtId)->load();
	const float currentSpd = audioProcessor.getValueTreeState().getRawParameterValue (spdId)->load();

	auto* aw = new juce::AlertWindow ("", "", juce::AlertWindow::NoIcon);
	aw->setLookAndFeel (&lnf);

	aw->addTextEditor ("amt", juce::String (juce::roundToInt (currentAmt)), juce::String());
	aw->addTextEditor ("spd", juce::String (currentSpd, 2), juce::String());

	// ── Inline bar component ──
	struct PromptBar : public juce::Component
	{
		TRScheme colours;
		float value      = 0.5f;
		float defaultVal = 0.5f;
		std::function<void (float)> onValueChanged;

		PromptBar (const TRScheme& s, float initial01, float default01)
			: colours (s), value (initial01), defaultVal (default01) {}

		void paint (juce::Graphics& g) override
		{
			const auto r = getLocalBounds().toFloat();
			g.setColour (colours.outline);
			g.drawRect (r, 4.0f);

			const float pad = 7.0f;
			auto inner = r.reduced (pad);

			g.setColour (colours.bg);
			g.fillRect (inner);

			const float fillW = juce::jlimit (0.0f, inner.getWidth(), inner.getWidth() * value);
			g.setColour (colours.fg);
			g.fillRect (inner.withWidth (fillW));
		}

		void mouseDown (const juce::MouseEvent& e) override  { updateFromMouse (e); }
		void mouseDrag (const juce::MouseEvent& e) override  { updateFromMouse (e); }
		void mouseDoubleClick (const juce::MouseEvent&) override { setValue (defaultVal); }

		void setValue (float v01)
		{
			value = juce::jlimit (0.0f, 1.0f, v01);
			repaint();
			if (onValueChanged)
				onValueChanged (value);
		}

	private:
		void updateFromMouse (const juce::MouseEvent& e)
		{
			const float pad = 7.0f;
			const float innerX = pad;
			const float innerW = (float) getWidth() - pad * 2.0f;
			const float v = (innerW > 0.0f) ? ((float) e.x - innerX) / innerW : 0.0f;
			setValue (v);
		}
	};

	struct ResetLabel : public juce::Label
	{
		PromptBar* pairedBar = nullptr;
		void mouseDoubleClick (const juce::MouseEvent&) override
		{
			if (pairedBar != nullptr)
				pairedBar->setValue (pairedBar->defaultVal);
		}
	};

	const auto& f = kBoldFont40();

	ResetLabel* amtSuffix  = nullptr;
	ResetLabel* spdSuffix  = nullptr;
	juce::Label* amtUnitLabel = nullptr;
	juce::Label* spdUnitLabel = nullptr;

	auto setupField = [&] (const char* editorId, const juce::String& suffixText,
	                       const juce::String& unitText, bool useDecimalFilter,
	                       ResetLabel*& suffixOut, juce::Label*& unitOut)
	{
		if (auto* te = aw->getTextEditor (editorId))
		{
			te->setFont (f);
			te->applyFontToAllText (f);

			if (useDecimalFilter)
			{
				// Allow digits and one decimal point, max 6 chars
				te->setInputRestrictions (6, "0123456789.");
			}
			else
			{
				te->setInputFilter (new PctInputFilter(), true);
			}

			auto r = te->getBounds();
			r.setHeight ((int) (f.getHeight() * kPromptEditorHeightScale) + kPromptEditorHeightPadPx);
			te->setBounds (r);

			suffixOut = new ResetLabel();
			suffixOut->setText (suffixText, juce::dontSendNotification);
			suffixOut->setJustificationType (juce::Justification::centredLeft);
			applyLabelTextColour (*suffixOut, scheme.text);
			suffixOut->setBorderSize (juce::BorderSize<int> (0));
			suffixOut->setFont (f);
			aw->addAndMakeVisible (suffixOut);

			unitOut = new juce::Label ("", unitText);
			unitOut->setJustificationType (juce::Justification::centredLeft);
			applyLabelTextColour (*unitOut, scheme.text);
			unitOut->setBorderSize (juce::BorderSize<int> (0));
			unitOut->setFont (f);
			aw->addAndMakeVisible (unitOut);
		}
	};

	setupField ("amt", "AMT", "%",  false, amtSuffix, amtUnitLabel);
	setupField ("spd", "SPD", "Hz", true,  spdSuffix, spdUnitLabel);

	// Bars: AMOUNT 0-100 → 0..1, SPEED 0.01-100 Hz → 0..1 (logarithmic)
	const float spdLogMin = std::log (CABTRAudioProcessor::kChaosSpdMin);
	const float spdLogMax = std::log (CABTRAudioProcessor::kChaosSpdMax);
	const float spdLogRange = spdLogMax - spdLogMin;

	auto hzToBar = [spdLogMin, spdLogRange] (float hz) -> float
	{
		if (hz <= CABTRAudioProcessor::kChaosSpdMin) return 0.0f;
		if (hz >= CABTRAudioProcessor::kChaosSpdMax) return 1.0f;
		return (std::log (hz) - spdLogMin) / spdLogRange;
	};

	auto barToHz = [spdLogMin, spdLogRange] (float v01) -> float
	{
		return std::exp (spdLogMin + v01 * spdLogRange);
	};

	auto* amtBar = new PromptBar (scheme, currentAmt * 0.01f,
	                              CABTRAudioProcessor::kChaosAmtDefault * 0.01f);
	auto* spdBar = new PromptBar (scheme,
	                              hzToBar (currentSpd),
	                              hzToBar (CABTRAudioProcessor::kChaosSpdDefault));
	aw->addAndMakeVisible (amtBar);
	aw->addAndMakeVisible (spdBar);

	if (amtSuffix != nullptr) amtSuffix->pairedBar = amtBar;
	if (spdSuffix != nullptr) spdSuffix->pairedBar = spdBar;

	auto syncing = std::make_shared<bool> (false);

	auto* amtApvts = audioProcessor.getValueTreeState().getParameter (amtId);
	auto* spdApvts = audioProcessor.getValueTreeState().getParameter (spdId);

	// Bar → text + APVTS
	auto barToTextAmt = [aw, syncing, amtApvts] (float v01)
	{
		if (*syncing) return;
		*syncing = true;
		if (auto* te = aw->getTextEditor ("amt"))
		{
			te->setText (juce::String (juce::roundToInt (v01 * 100.0f)), juce::sendNotification);
			te->selectAll();
		}
		if (amtApvts != nullptr)
			amtApvts->setValueNotifyingHost (amtApvts->convertTo0to1 (v01 * 100.0f));
		*syncing = false;
	};

	auto barToTextSpd = [aw, syncing, spdApvts, barToHz] (float v01)
	{
		if (*syncing) return;
		*syncing = true;
		const float hz = juce::jlimit (CABTRAudioProcessor::kChaosSpdMin,
		                               CABTRAudioProcessor::kChaosSpdMax, barToHz (v01));
		if (auto* te = aw->getTextEditor ("spd"))
		{
			te->setText (juce::String (hz, 2), juce::sendNotification);
			te->selectAll();
		}
		if (spdApvts != nullptr)
			spdApvts->setValueNotifyingHost (spdApvts->convertTo0to1 (hz));
		*syncing = false;
	};

	amtBar->onValueChanged = barToTextAmt;
	spdBar->onValueChanged = barToTextSpd;

	// Layout helper
	auto layoutRows = [aw, amtSuffix, spdSuffix, amtUnitLabel, spdUnitLabel, amtBar, spdBar] ()
	{
		auto* amtTe = aw->getTextEditor ("amt");
		auto* spdTe = aw->getTextEditor ("spd");
		if (amtTe == nullptr || spdTe == nullptr)
			return;

		const int buttonsTop = getAlertButtonsTop (*aw);
		const int rowH = amtTe->getHeight();
		const int barH = juce::jmax (10, rowH / 2);
		const int barGap = juce::jmax (2, rowH / 6);
		const int rowTotal = rowH + barGap + barH;
		const int gap = juce::jmax (4, rowH / 3);
		const int totalH = rowTotal * 2 + gap;
		const int startY = juce::jmax (kPromptEditorMinTopPx, (buttonsTop - totalH) / 2);

		const int contentPad = kPromptInlineContentPadPx;
		const int contentW = aw->getWidth() - contentPad * 2;
		const auto& font = amtTe->getFont();
		const int spaceW = juce::jmax (2, stringWidth (font, " "));

		auto placeRow = [&] (juce::TextEditor* te, juce::Label* suffix,
		                     juce::Label* unitLabel, PromptBar* bar, int y)
		{
			if (te == nullptr || suffix == nullptr || bar == nullptr)
				return;

			const int labelW  = stringWidth (suffix->getFont(), suffix->getText()) + 2;
			const auto txt    = te->getText();
			const int textW   = juce::jmax (1, stringWidth (font, txt));
			const int unitW   = (unitLabel != nullptr)
			                  ? stringWidth (font, unitLabel->getText()) + 2 : 0;

			constexpr int kEditorTextPadPx = 12;
			constexpr int kMinEditorWidthPx = 24;
			const int editorW = juce::jlimit (kMinEditorWidthPx, 80,
			                                  textW + kEditorTextPadPx * 2);

			const int visualW = labelW + spaceW + textW + unitW;
			const int centerX = contentPad + contentW / 2;
			int blockLeft = juce::jlimit (contentPad,
			                              juce::jmax (contentPad, contentPad + contentW - visualW),
			                              centerX - visualW / 2);

			suffix->setBounds (blockLeft, y, labelW, rowH);

			int teX = blockLeft + labelW + spaceW - (editorW - textW) / 2;
			teX = juce::jlimit (contentPad,
			                    juce::jmax (contentPad, contentPad + contentW - editorW), teX);
			te->setBounds (teX, y, editorW, rowH);

			if (unitLabel != nullptr)
			{
				const int textRightX = blockLeft + labelW + spaceW + textW;
				unitLabel->setBounds (textRightX, y, unitW, rowH);
			}

			const int barX = kPromptInnerMargin;
			const int barW = juce::jmax (60, aw->getWidth() - kPromptInnerMargin * 2);
			bar->setBounds (barX, y + rowH + barGap, barW, barH);
		};

		placeRow (amtTe, amtSuffix, amtUnitLabel, amtBar, startY);
		placeRow (spdTe, spdSuffix, spdUnitLabel, spdBar, startY + rowTotal + gap);
	};

	// Text → bar + APVTS
	auto textToBar = [syncing, hzToBar] (juce::TextEditor* te, PromptBar* bar,
	                            juce::RangedAudioParameter* param, bool isSpeed)
	{
		if (*syncing || te == nullptr || bar == nullptr) return;
		*syncing = true;
		const float raw = juce::jlimit (0.0f, 100.0f, te->getText().getFloatValue());
		if (isSpeed)
		{
			const float hz = juce::jlimit (CABTRAudioProcessor::kChaosSpdMin,
			                               CABTRAudioProcessor::kChaosSpdMax, raw);
			bar->value = hzToBar (hz);
			if (param != nullptr)
				param->setValueNotifyingHost (param->convertTo0to1 (hz));
		}
		else
		{
			bar->value = raw * 0.01f;
			if (param != nullptr)
				param->setValueNotifyingHost (param->convertTo0to1 (raw));
		}
		bar->repaint();
		*syncing = false;
	};

	if (auto* amtTe = aw->getTextEditor ("amt"))
		amtTe->onTextChange = [layoutRows, amtTe, amtBar, textToBar, amtApvts] () mutable
		{
			textToBar (amtTe, amtBar, amtApvts, false);
			layoutRows();
		};
	if (auto* spdTe = aw->getTextEditor ("spd"))
		spdTe->onTextChange = [layoutRows, spdTe, spdBar, textToBar, spdApvts] () mutable
		{
			textToBar (spdTe, spdBar, spdApvts, true);
			layoutRows();
		};

	aw->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
	aw->addButton ("CANCEL", 0, juce::KeyPress (juce::KeyPress::escapeKey));
	applyPromptShellSize (*aw);
	layoutAlertWindowButtons (*aw);
	layoutRows();

	const auto& kChaosFont = kBoldFont40();
	preparePromptTextEditor (*aw, "amt", scheme.bg, scheme.text, scheme.fg, kChaosFont, false);
	preparePromptTextEditor (*aw, "spd", scheme.bg, scheme.text, scheme.fg, kChaosFont, false);
	layoutRows();

	styleAlertButtons (*aw, lnf);

	juce::Component::SafePointer<CABTRAudioProcessorEditor> safeThis (this);

	if (safeThis != nullptr)
	{
		fitAlertWindowToEditor (*aw, safeThis.getComponent(), [layoutRows] (juce::AlertWindow& a)
		{
			juce::ignoreUnused (a);
			layoutAlertWindowButtons (a);
			layoutRows();
		});

		embedAlertWindowInOverlay (safeThis.getComponent(), aw);
	}
	else
	{
		aw->centreAroundComponent (this, aw->getWidth(), aw->getHeight());
		bringPromptWindowToFront (*aw);
	}

	// Final styling pass
	{
		preparePromptTextEditor (*aw, "amt", scheme.bg, scheme.text, scheme.fg, kChaosFont, false);
		preparePromptTextEditor (*aw, "spd", scheme.bg, scheme.text, scheme.fg, kChaosFont, false);
		layoutRows();

		if (amtSuffix != nullptr)
		{
			if (auto* te = aw->getTextEditor ("amt"))
			{
				amtSuffix->setFont (te->getFont());
				if (amtUnitLabel != nullptr) amtUnitLabel->setFont (te->getFont());
			}
		}
		if (spdSuffix != nullptr)
		{
			if (auto* te = aw->getTextEditor ("spd"))
			{
				spdSuffix->setFont (te->getFont());
				if (spdUnitLabel != nullptr) spdUnitLabel->setFont (te->getFont());
			}
		}

		layoutRows();

		juce::Component::SafePointer<juce::AlertWindow> safeAw (aw);
		juce::MessageManager::callAsync ([safeAw]()
		{
			if (safeAw == nullptr) return;
			bringPromptWindowToFront (*safeAw);
			safeAw->repaint();
		});
	}

	aw->enterModalState (true,
		juce::ModalCallbackFunction::create (
			[safeThis, aw, amtBar, spdBar,
			 savedAmt = currentAmt, savedSpd = currentSpd,
			 amtId, spdId, forLoaderA, spdLogMin, spdLogRange] (int result) mutable
		{
			std::unique_ptr<juce::AlertWindow> killer (aw);

			if (safeThis != nullptr)
				safeThis->setPromptOverlayActive (false);

			if (safeThis == nullptr)
				return;

			if (result != 1)
			{
				// CANCEL: revert to original values
				if (auto* p = safeThis->audioProcessor.getValueTreeState().getParameter (amtId))
					p->setValueNotifyingHost (p->convertTo0to1 (savedAmt));
				if (auto* p = safeThis->audioProcessor.getValueTreeState().getParameter (spdId))
					p->setValueNotifyingHost (p->convertTo0to1 (savedSpd));
				return;
			}

			// OK: update tooltip
			const float newAmt = juce::jlimit (0.0f, 100.0f, amtBar->value * 100.0f);
			const float newSpd = juce::jlimit (CABTRAudioProcessor::kChaosSpdMin,
			                                    CABTRAudioProcessor::kChaosSpdMax,
			                                    std::exp (spdLogMin + juce::jlimit (0.0f, 1.0f, spdBar->value) * spdLogRange));
			auto tip = juce::String (juce::roundToInt (newAmt)) + "% | "
			         + juce::String (juce::roundToInt (newSpd)) + " Hz";
			auto& disp = forLoaderA ? safeThis->chaosDisplayA : safeThis->chaosDisplayB;
			disp.setTooltip (tip);
		}),
		false);
}

//==============================================================================
//  Export Prompt
//==============================================================================
void CABTRAudioProcessorEditor::openExportPrompt()
{
	using namespace TR;

	lnf.setScheme (activeScheme);
	setPromptOverlayActive (true);

	auto* aw = new juce::AlertWindow ("", "", juce::MessageBoxIconType::NoIcon);
	juce::Component::SafePointer<CABTRAudioProcessorEditor> safeThis (this);
	aw->setLookAndFeel (&lnf);
	aw->setColour (juce::AlertWindow::backgroundColourId, activeScheme.bg);

	auto labelFont = lnf.getAlertWindowMessageFont();
	labelFont.setHeight (labelFont.getHeight() * 1.20f);
	auto titleFont = labelFont;
	titleFont.setBold (true);
	titleFont.setHeight (labelFont.getHeight() * 1.15f);

	// Build form panel — all controls live inside this container
	// so AlertWindow can correctly size itself via addCustomComponent
	auto* formPanel = new juce::Component();

	constexpr int formW = 380;
	constexpr int rowH = 28;
	constexpr int labelW = 90;
	constexpr int controlX = labelW + 8;
	constexpr int controlW = formW - controlX;
	constexpr int gap = 6;
	int fy = 0;

	auto addFormLabel = [&] (const juce::String& text, int yPos)
	{
		auto* l = new juce::Label ("", text);
		l->setFont (labelFont);
		l->setColour (juce::Label::textColourId, activeScheme.text);
		l->setJustificationType (juce::Justification::centredRight);
		l->setBounds (0, yPos, labelW, rowH);
		formPanel->addAndMakeVisible (l);
		return l;
	};

	// Title
	auto* titleLabel = new juce::Label ("title", "EXPORT IR");
	titleLabel->setFont (titleFont);
	titleLabel->setColour (juce::Label::textColourId, activeScheme.text);
	titleLabel->setJustificationType (juce::Justification::centred);
	titleLabel->setBounds (0, fy, formW, rowH);
	formPanel->addAndMakeVisible (titleLabel);
	fy += rowH + gap + 4;

	// RATE
	addFormLabel ("RATE", fy);
	auto* rateCombo = new juce::ComboBox ("rate");
	rateCombo->setLookAndFeel (&lnf);
	rateCombo->addItem ("44100 Hz", 1);
	rateCombo->addItem ("48000 Hz", 2);
	rateCombo->addItem ("88200 Hz", 3);
	rateCombo->addItem ("96000 Hz", 4);
	rateCombo->addItem ("176400 Hz", 5);
	rateCombo->addItem ("192000 Hz", 6);
	const double hostSR = audioProcessor.getSampleRate();
	int defaultRateId = 2;
	if      (std::abs (hostSR - 44100.0)  < 1.0) defaultRateId = 1;
	else if (std::abs (hostSR - 48000.0)  < 1.0) defaultRateId = 2;
	else if (std::abs (hostSR - 88200.0)  < 1.0) defaultRateId = 3;
	else if (std::abs (hostSR - 96000.0)  < 1.0) defaultRateId = 4;
	else if (std::abs (hostSR - 176400.0) < 1.0) defaultRateId = 5;
	else if (std::abs (hostSR - 192000.0) < 1.0) defaultRateId = 6;
	rateCombo->setSelectedId (defaultRateId, juce::dontSendNotification);
	rateCombo->setBounds (controlX, fy, controlW, rowH);
	formPanel->addAndMakeVisible (rateCombo);
	fy += rowH + gap;

	// FORMAT
	addFormLabel ("FORMAT", fy);
	auto* formatCombo = new juce::ComboBox ("format");
	formatCombo->setLookAndFeel (&lnf);
	formatCombo->addItem ("WAV 16-bit", 1);
	formatCombo->addItem ("WAV 24-bit", 2);
	formatCombo->addItem ("WAV 32-bit float", 3);
	formatCombo->addItem ("AIFF 24-bit", 4);
	formatCombo->addItem ("FLAC 24-bit", 5);
	formatCombo->setSelectedId (2, juce::dontSendNotification);
	formatCombo->setBounds (controlX, fy, controlW, rowH);
	formPanel->addAndMakeVisible (formatCombo);
	fy += rowH + gap;

	// LENGTH
	addFormLabel ("LENGTH", fy);
	auto* lengthEditor = new juce::TextEditor ("length");
	lengthEditor->setFont (labelFont);
	lengthEditor->setJustification (juce::Justification::centred);
	lengthEditor->setText ("10.0", juce::dontSendNotification);
	lengthEditor->setInputFilter (new NumericInputFilter (0.01, 10.0, 6, 2), true);
	lengthEditor->setColour (juce::TextEditor::backgroundColourId, activeScheme.bg);
	lengthEditor->setColour (juce::TextEditor::textColourId, activeScheme.text);
	lengthEditor->setColour (juce::TextEditor::outlineColourId, activeScheme.text.withAlpha (0.5f));
	lengthEditor->setColour (juce::TextEditor::focusedOutlineColourId, activeScheme.text);
	lengthEditor->setBounds (controlX, fy, controlW - 30, rowH);
	formPanel->addAndMakeVisible (lengthEditor);

	auto* secLabel = new juce::Label ("sec", "s");
	secLabel->setFont (labelFont);
	secLabel->setColour (juce::Label::textColourId, activeScheme.text);
	secLabel->setBounds (controlX + controlW - 26, fy, 26, rowH);
	formPanel->addAndMakeVisible (secLabel);
	fy += rowH + gap;

	// TRIM SILENCE
	auto* trimToggle = new juce::ToggleButton ("TRIM SILENCE");
	trimToggle->setToggleState (true, juce::dontSendNotification);
	trimToggle->setLookAndFeel (&lnf);
	trimToggle->setColour (juce::ToggleButton::textColourId, activeScheme.text);
	trimToggle->setColour (juce::ToggleButton::tickColourId, activeScheme.text);
	trimToggle->setBounds (controlX, fy, controlW, rowH);
	formPanel->addAndMakeVisible (trimToggle);
	fy += rowH;

	// Register form panel as custom component so AlertWindow sizes correctly
	formPanel->setSize (formW, fy);
	aw->addCustomComponent (formPanel);

	// Buttons
	aw->addButton ("EXPORT", 1, juce::KeyPress (juce::KeyPress::returnKey));
	aw->addButton ("CANCEL", 0, juce::KeyPress (juce::KeyPress::escapeKey));
	aw->setEscapeKeyCancels (true);

	styleAlertButtons (*aw, lnf);
	layoutAlertWindowButtons (*aw);

	if (safeThis != nullptr)
	{
		fitAlertWindowToEditor (*aw, safeThis.getComponent(), nullptr);
		embedAlertWindowInOverlay (safeThis.getComponent(), aw);
	}

	aw->enterModalState (true,
		juce::ModalCallbackFunction::create (
			[safeThis, aw, formPanel, rateCombo, formatCombo, lengthEditor, trimToggle] (int result) mutable
		{
			const int rateId = (rateCombo != nullptr) ? rateCombo->getSelectedId() : 2;
			const int formatId = (formatCombo != nullptr) ? formatCombo->getSelectedId() : 2;
			const float maxLen = (lengthEditor != nullptr)
			    ? juce::jlimit (0.01f, 10.0f, lengthEditor->getText().getFloatValue()) : 10.0f;
			const bool trim = (trimToggle != nullptr) ? trimToggle->getToggleState() : true;

			// Delete all form children, then form panel, then AlertWindow
			while (formPanel->getNumChildComponents() > 0)
				delete formPanel->getChildComponent (0);
			delete formPanel;

			std::unique_ptr<juce::AlertWindow> killer (aw);

			if (safeThis == nullptr)
				return;

			safeThis->setPromptOverlayActive (false);

			if (result != 1)
				return;

			double targetRate = 48000.0;
			switch (rateId)
			{
				case 1: targetRate = 44100.0;  break;
				case 2: targetRate = 48000.0;  break;
				case 3: targetRate = 88200.0;  break;
				case 4: targetRate = 96000.0;  break;
				case 5: targetRate = 176400.0; break;
				case 6: targetRate = 192000.0; break;
			}

			const int formatType = formatId - 1;
			juce::String ext = ".wav";
			if (formatId == 4) ext = ".aiff";
			if (formatId == 5) ext = ".flac";

			juce::String baseName = "CAB-TR_export";

			safeThis->exportChooser = std::make_unique<juce::FileChooser> (
				"Export Impulse Response",
				juce::File::getSpecialLocation (juce::File::userDesktopDirectory)
					.getChildFile (baseName + ext),
				"*" + ext);

			auto safeThis2 = safeThis;
			safeThis->exportChooser->launchAsync (
				juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles,
				[safeThis2, targetRate, formatType, maxLen, trim, ext] (const juce::FileChooser& fc)
				{
					if (safeThis2 == nullptr)
						return;

					auto file = fc.getResult();
					if (file == juce::File())
						return;

					if (! file.hasFileExtension (ext.substring (1)))
						file = file.withFileExtension (ext.substring (1));

					const bool ok = safeThis2->audioProcessor.exportCombinedIR (
						targetRate, formatType, maxLen, trim, file);

					if (! ok)
					{
						juce::AlertWindow::showMessageBoxAsync (
							juce::MessageBoxIconType::WarningIcon,
							"Export Failed",
							"Could not export the impulse response.\n"
							"Make sure at least one IR is loaded and enabled.");
					}
				});
		}),
		true);
}

void CABTRAudioProcessorEditor::openInfoPopup()
{
	lnf.setScheme (activeScheme);

	setPromptOverlayActive (true);

	auto* aw = new juce::AlertWindow ("", "", juce::AlertWindow::NoIcon);
	juce::Component::SafePointer<juce::AlertWindow> safeAw (aw);
	juce::Component::SafePointer<CABTRAudioProcessorEditor> safeThis (this);
	aw->setLookAndFeel (&lnf);
	aw->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
	aw->addButton ("GRAPHICS", 2);

	applyPromptShellSize (*aw);

	// Body content: parsed from InfoContent.h XML
	auto* bodyContent = new juce::Component();
	bodyContent->setComponentID ("bodyContent");

	auto infoFont = lnf.getAlertWindowMessageFont();
	infoFont.setHeight (infoFont.getHeight() * 1.45f);

	auto headingFont = infoFont;
	headingFont.setBold (true);
	headingFont.setHeight (infoFont.getHeight() * 1.25f);

	auto linkFont = infoFont;
	linkFont.setHeight (infoFont.getHeight() * 1.08f);

	auto poemFont = infoFont;
	poemFont.setItalic (true);

	auto xmlDoc = juce::XmlDocument::parse (InfoContent::xml);
	auto* contentNode = xmlDoc != nullptr ? xmlDoc->getChildByName ("content") : nullptr;

	if (contentNode != nullptr)
	{
		int elemIdx = 0;
		for (auto* node : contentNode->getChildIterator())
		{
			const auto tag  = node->getTagName();
			const auto text = node->getAllSubText().trim();
			const auto id   = tag + juce::String (elemIdx++);

			if (tag == "heading")
			{
				auto* l = new juce::Label (id, text);
				l->setComponentID (id);
				l->setJustificationType (juce::Justification::centred);
				applyLabelTextColour (*l, activeScheme.text);
				l->setFont (headingFont);
				bodyContent->addAndMakeVisible (l);
			}
			else if (tag == "text" || tag == "separator")
			{
				auto* l = new juce::Label (id, text);
				l->setComponentID (id);
				l->setJustificationType (juce::Justification::centred);
				applyLabelTextColour (*l, activeScheme.text);
				l->setFont (infoFont);
				l->setBorderSize (juce::BorderSize<int> (0));
				bodyContent->addAndMakeVisible (l);
			}
			else if (tag == "link")
			{
				const auto url = node->getStringAttribute ("url");
				auto* lnk = new juce::HyperlinkButton (text, juce::URL (url));
				lnk->setComponentID (id);
				lnk->setJustificationType (juce::Justification::centred);
				lnk->setColour (juce::HyperlinkButton::textColourId, activeScheme.text);
				lnk->setFont (linkFont, false, juce::Justification::centred);
				lnk->setTooltip ("");
				bodyContent->addAndMakeVisible (lnk);
			}
			else if (tag == "poem")
			{
				auto* l = new juce::Label (id, text);
				l->setComponentID (id);
				l->setJustificationType (juce::Justification::centred);
				applyLabelTextColour (*l, activeScheme.text);
				l->setFont (poemFont);
				l->setBorderSize (juce::BorderSize<int> (0, 0, 0, 0));
				l->getProperties().set ("poemPadFraction", 0.12f);
				bodyContent->addAndMakeVisible (l);
			}
			else if (tag == "spacer")
			{
				auto* l = new juce::Label (id, "");
				l->setComponentID (id);
				l->setFont (infoFont);
				l->setBorderSize (juce::BorderSize<int> (0));
				bodyContent->addAndMakeVisible (l);
			}
		}
	}

	auto* viewport = new juce::Viewport();
	viewport->setComponentID ("bodyViewport");
	viewport->setViewedComponent (bodyContent, true);
	viewport->setScrollBarsShown (true, false);
	viewport->setScrollBarThickness (8);
	viewport->setLookAndFeel (&lnf);
	aw->addAndMakeVisible (viewport);

	layoutInfoPopupContent (*aw);

	if (safeThis != nullptr)
	{
		fitAlertWindowToEditor (*aw, safeThis.getComponent(), [] (juce::AlertWindow& a)
		{
			layoutInfoPopupContent (a);
		});

		embedAlertWindowInOverlay (safeThis.getComponent(), aw);
	}
	else
	{
		aw->centreAroundComponent (this, aw->getWidth(), aw->getHeight());
		bringPromptWindowToFront (*aw);
		aw->repaint();
	}

	juce::MessageManager::callAsync ([safeAw, safeThis]()
	{
		if (safeAw == nullptr || safeThis == nullptr)
			return;

		bringPromptWindowToFront (*safeAw);
		safeAw->repaint();
	});

	aw->enterModalState (true,
		juce::ModalCallbackFunction::create ([safeThis = juce::Component::SafePointer<CABTRAudioProcessorEditor> (this), aw] (int result) mutable
		{
			std::unique_ptr<juce::AlertWindow> killer (aw);

			if (safeThis == nullptr)
				return;

			if (result == 2)
			{
				safeThis->openGraphicsPopup();
				return;
			}

			safeThis->setPromptOverlayActive (false);
		}));
}

void CABTRAudioProcessorEditor::openGraphicsPopup()
{
	lnf.setScheme (activeScheme);

	useCustomPalette = audioProcessor.getUiUseCustomPalette();
	crtEnabled = audioProcessor.getUiFxTailEnabled();
	crtEffect.setEnabled (crtEnabled);
	applyActivePalette();

	setPromptOverlayActive (true);

	auto* aw = new juce::AlertWindow ("", "", juce::AlertWindow::NoIcon);
	juce::Component::SafePointer<CABTRAudioProcessorEditor> safeThis (this);
	juce::Component::SafePointer<juce::AlertWindow> safeAw (aw);
	aw->setLookAndFeel (&lnf);
	aw->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));

	auto labelFont = lnf.getAlertWindowMessageFont();
	labelFont.setHeight (labelFont.getHeight() * 1.20f);

	auto addPopupLabel = [this, aw] (const juce::String& id,
	                                 const juce::String& text,
	                                 juce::Font font,
	                                 juce::Justification justification = juce::Justification::centredLeft)
	{
		auto* label = new PopupClickableLabel (id, text);
		label->setComponentID (id);
		label->setJustificationType (justification);
		applyLabelTextColour (*label, activeScheme.text);
		label->setBorderSize (juce::BorderSize<int> (0));
		label->setFont (font);
		label->setMouseCursor (juce::MouseCursor::PointingHandCursor);
		aw->addAndMakeVisible (label);
		return label;
	};

	auto* defaultToggle = new juce::ToggleButton ("");
	defaultToggle->setComponentID ("paletteDefaultToggle");
	aw->addAndMakeVisible (defaultToggle);

	auto* defaultLabel = addPopupLabel ("paletteDefaultLabel", "DFLT", labelFont);

	auto* customToggle = new juce::ToggleButton ("");
	customToggle->setComponentID ("paletteCustomToggle");
	aw->addAndMakeVisible (customToggle);

	auto* customLabel = addPopupLabel ("paletteCustomLabel", "CSTM", labelFont);

	auto paletteTitleFont = labelFont;
	paletteTitleFont.setHeight (paletteTitleFont.getHeight() * 1.30f);
	addPopupLabel ("paletteTitle", "PALETTE", paletteTitleFont, juce::Justification::centredLeft);

	for (int i = 0; i < 2; ++i)
	{
		auto* dflt = new juce::TextButton();
		dflt->setComponentID ("defaultSwatch" + juce::String (i));
		dflt->setTooltip ("Default palette colour " + juce::String (i + 1));
		aw->addAndMakeVisible (dflt);

		auto* custom = new PopupSwatchButton();
		custom->setComponentID ("customSwatch" + juce::String (i));
		custom->setTooltip (colourToHexRgb (customPalette[(size_t) i]));
		aw->addAndMakeVisible (custom);
	}

	auto* fxToggle = new juce::ToggleButton ("");
	fxToggle->setComponentID ("fxToggle");
	fxToggle->setToggleState (crtEnabled, juce::dontSendNotification);
	fxToggle->onClick = [safeThis, fxToggle]()
	{
		if (safeThis == nullptr || fxToggle == nullptr)
			return;

		safeThis->applyCrtState (fxToggle->getToggleState());
		safeThis->audioProcessor.setUiFxTailEnabled (safeThis->crtEnabled);
		safeThis->repaint();
	};
	aw->addAndMakeVisible (fxToggle);

	auto* fxLabel = addPopupLabel ("fxLabel", "GRAPHIC FX", labelFont);

	auto syncAndRepaintPopup = [safeThis, safeAw]()
	{
		if (safeThis == nullptr || safeAw == nullptr)
			return;

		syncGraphicsPopupState (*safeAw, safeThis->defaultPalette, safeThis->customPalette, safeThis->useCustomPalette);
		layoutGraphicsPopupContent (*safeAw);
		safeAw->repaint();
	};

	auto applyPaletteAndRepaint = [safeThis]()
	{
		if (safeThis == nullptr)
			return;

		safeThis->applyActivePalette();
		safeThis->repaint();
	};

	defaultToggle->onClick = [safeThis, defaultToggle, customToggle, applyPaletteAndRepaint, syncAndRepaintPopup]() mutable
	{
		if (safeThis == nullptr || defaultToggle == nullptr || customToggle == nullptr)
			return;

		safeThis->useCustomPalette = false;
		safeThis->audioProcessor.setUiUseCustomPalette (safeThis->useCustomPalette);
		defaultToggle->setToggleState (true, juce::dontSendNotification);
		customToggle->setToggleState (false, juce::dontSendNotification);
		applyPaletteAndRepaint();
		syncAndRepaintPopup();
	};

	customToggle->onClick = [safeThis, defaultToggle, customToggle, applyPaletteAndRepaint, syncAndRepaintPopup]() mutable
	{
		if (safeThis == nullptr || defaultToggle == nullptr || customToggle == nullptr)
			return;

		safeThis->useCustomPalette = true;
		safeThis->audioProcessor.setUiUseCustomPalette (safeThis->useCustomPalette);
		defaultToggle->setToggleState (false, juce::dontSendNotification);
		customToggle->setToggleState (true, juce::dontSendNotification);
		applyPaletteAndRepaint();
		syncAndRepaintPopup();
	};

	if (defaultLabel != nullptr && defaultToggle != nullptr)
		defaultLabel->onClick = [defaultToggle]() { defaultToggle->triggerClick(); };

	if (customLabel != nullptr && customToggle != nullptr)
		customLabel->onClick = [customToggle]() { customToggle->triggerClick(); };

	if (fxLabel != nullptr && fxToggle != nullptr)
		fxLabel->onClick = [fxToggle]() { fxToggle->triggerClick(); };

	for (int i = 0; i < 2; ++i)
	{
		if (auto* customSwatch = dynamic_cast<PopupSwatchButton*> (aw->findChildWithID ("customSwatch" + juce::String (i))))
		{
			customSwatch->onLeftClick = [safeThis, safeAw, i]()
			{
				if (safeThis == nullptr)
					return;

				auto& rng = juce::Random::getSystemRandom();
				const auto randomColour = juce::Colour::fromRGB ((juce::uint8) rng.nextInt (256),
				                                                 (juce::uint8) rng.nextInt (256),
				                                                 (juce::uint8) rng.nextInt (256));

				safeThis->customPalette[(size_t) i] = randomColour;
				safeThis->audioProcessor.setUiCustomPaletteColour (i, randomColour);
				if (safeThis->useCustomPalette)
				{
					safeThis->applyActivePalette();
					safeThis->repaint();
				}

				if (safeAw != nullptr)
				{
					syncGraphicsPopupState (*safeAw, safeThis->defaultPalette, safeThis->customPalette, safeThis->useCustomPalette);
					layoutGraphicsPopupContent (*safeAw);
					safeAw->repaint();
				}
			};

			customSwatch->onRightClick = [safeThis, safeAw, i]()
			{
				if (safeThis == nullptr)
					return;

				const auto scheme = safeThis->activeScheme;

				auto* colorAw = new juce::AlertWindow ("", "", juce::AlertWindow::NoIcon);
				colorAw->setLookAndFeel (&safeThis->lnf);
				colorAw->addTextEditor ("hex", colourToHexRgb (safeThis->customPalette[(size_t) i]), juce::String());

				if (auto* te = colorAw->getTextEditor ("hex"))
					te->setInputFilter (new HexInputFilter(), true);

				colorAw->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
				colorAw->addButton ("CANCEL", 0, juce::KeyPress (juce::KeyPress::escapeKey));

				styleAlertButtons (*colorAw, safeThis->lnf);

				applyPromptShellSize (*colorAw);
				layoutAlertWindowButtons (*colorAw);

				const juce::Font& kHexPromptFont = kBoldFont40();

				preparePromptTextEditor (*colorAw, "hex", scheme.bg, scheme.text, scheme.fg, kHexPromptFont, true, 6);

				if (safeThis != nullptr)
				{
					fitAlertWindowToEditor (*colorAw, safeThis.getComponent(), [&] (juce::AlertWindow& a)
					{
						layoutAlertWindowButtons (a);
						preparePromptTextEditor (a, "hex", scheme.bg, scheme.text, scheme.fg, kHexPromptFont, true, 6);
					});

					embedAlertWindowInOverlay (safeThis.getComponent(), colorAw, true);
				}
				else
				{
					colorAw->centreAroundComponent (safeThis.getComponent(), colorAw->getWidth(), colorAw->getHeight());
					bringPromptWindowToFront (*colorAw);
					if (safeThis != nullptr && safeThis->tooltipWindow)
						safeThis->tooltipWindow->toFront (true);
					colorAw->repaint();
				}

				preparePromptTextEditor (*colorAw, "hex", scheme.bg, scheme.text, scheme.fg, kHexPromptFont, true, 6);

				juce::Component::SafePointer<juce::AlertWindow> safeColorAw (colorAw);
				juce::MessageManager::callAsync ([safeColorAw]()
				{
					if (safeColorAw == nullptr)
						return;
					bringPromptWindowToFront (*safeColorAw);
					safeColorAw->repaint();
				});

				colorAw->enterModalState (true,
					juce::ModalCallbackFunction::create ([safeThis, safeAw, colorAw, i] (int result) mutable
					{
						std::unique_ptr<juce::AlertWindow> killer (colorAw);
						if (safeThis == nullptr)
							return;

						if (result != 1)
							return;

						juce::Colour parsed;
						if (! tryParseHexColour (killer->getTextEditorContents ("hex"), parsed))
							return;

						safeThis->customPalette[(size_t) i] = parsed;
						safeThis->audioProcessor.setUiCustomPaletteColour (i, parsed);
						if (safeThis->useCustomPalette)
						{
							safeThis->applyActivePalette();
							safeThis->repaint();
						}

						if (safeAw != nullptr)
						{
							syncGraphicsPopupState (*safeAw, safeThis->defaultPalette, safeThis->customPalette, safeThis->useCustomPalette);
							layoutGraphicsPopupContent (*safeAw);
							safeAw->repaint();
						}
					}));
			};
		}
	}

	applyPromptShellSize (*aw);
	syncGraphicsPopupState (*aw, defaultPalette, customPalette, useCustomPalette);
	layoutGraphicsPopupContent (*aw);

	if (safeThis != nullptr)
	{
		fitAlertWindowToEditor (*aw, safeThis.getComponent(), [&] (juce::AlertWindow& a)
		{
			syncGraphicsPopupState (a, defaultPalette, customPalette, useCustomPalette);
			layoutGraphicsPopupContent (a);
		});
	}
	if (safeThis != nullptr)
	{
		embedAlertWindowInOverlay (safeThis.getComponent(), aw);

		juce::MessageManager::callAsync ([safeAw, safeThis]()
		{
			if (safeAw == nullptr || safeThis == nullptr)
				return;

			safeAw->toFront (false);
			safeAw->repaint();
		});
	}
	else
	{
		aw->centreAroundComponent (this, aw->getWidth(), aw->getHeight());
		bringPromptWindowToFront (*aw);
		aw->repaint();
	}

	aw->enterModalState (true,
		juce::ModalCallbackFunction::create ([safeThis, aw] (int) mutable
		{
			std::unique_ptr<juce::AlertWindow> killer (aw);
			if (safeThis != nullptr)
				safeThis->setPromptOverlayActive (false);
		}));
}

void CABTRAudioProcessorEditor::applyLabelTextColour (juce::Label& label, juce::Colour colour)
{
	label.setColour (juce::Label::textColourId, colour);
}
