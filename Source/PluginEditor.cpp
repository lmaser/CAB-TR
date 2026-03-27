// PluginEditor.cpp — CAB-TR
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
static constexpr std::array<const char*, 7> kUiMirrorParamIds {
	CABTRAudioProcessor::kParamUiPalette,
	CABTRAudioProcessor::kParamUiFxTail,
	CABTRAudioProcessor::kParamUiColor0,
	CABTRAudioProcessor::kParamUiColor1,
	CABTRAudioProcessor::kParamEnableA,
	CABTRAudioProcessor::kParamEnableB,
	CABTRAudioProcessor::kParamEnableC
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

	switch (type_)
	{
		case Type::HpFreq:
		case Type::LpFreq:
			return juce::String (v, 1) + " Hz";

		case Type::Input:
			if (v <= -80.0) return "-INF";
			return juce::String (v, 1) + " dB";

		case Type::Output:
		case Type::GlobalOutput:
			return juce::String (v, 1) + " dB";

		case Type::Tilt:
			return juce::String (v, 1) + " dB";

		case Type::Start:
		case Type::End:
		case Type::Delay:
			return juce::String (static_cast<int> (std::round (v))) + " ms";

		case Type::Size:
			return juce::String (v * 100.0, 1) + "%";

		case Type::Pan:
		{
			double percent = v * 100.0;
			if (std::abs (percent - 50.0) < 1.0)
				return "C";
			if (percent < 50.0)
				return "L" + juce::String (50.0 - percent, 0);
			return "R" + juce::String (percent - 50.0, 0);
		}

		case Type::Fred:
		case Type::Pos:
		case Type::Reso:
		case Type::Mix:
		case Type::GlobalMix:
			return juce::String (v * 100.0, 1) + "%";

		default:
			break;
	}

	return juce::Slider::getTextFromValue (v);
}

//==============================================================================
//  FilterBarComponent implementations
//==============================================================================
juce::Rectangle<float> CABTRAudioProcessorEditor::FilterBarComponent::getInnerArea() const
{
	return getLocalBounds().toFloat().reduced (kPad);
}

float CABTRAudioProcessorEditor::FilterBarComponent::freqToNormX (float freq) const
{
	const float clamped = juce::jlimit (kMinFreq, kMaxFreq, freq);
	return std::log2 (clamped / kMinFreq) / std::log2 (kMaxFreq / kMinFreq);
}

float CABTRAudioProcessorEditor::FilterBarComponent::normXToFreq (float normX) const
{
	const float n = juce::jlimit (0.0f, 1.0f, normX);
	return kMinFreq * std::pow (2.0f, n * std::log2 (kMaxFreq / kMinFreq));
}

float CABTRAudioProcessorEditor::FilterBarComponent::getMarkerScreenX (float freq) const
{
	const auto inner = getInnerArea();
	return inner.getX() + freqToNormX (freq) * inner.getWidth();
}

CABTRAudioProcessorEditor::FilterBarComponent::DragTarget
CABTRAudioProcessorEditor::FilterBarComponent::hitTestMarker (juce::Point<float> p) const
{
	const float hpX = getMarkerScreenX (hpFreq_);
	const float lpX = getMarkerScreenX (lpFreq_);
	const float distHp = std::abs (p.x - hpX);
	const float distLp = std::abs (p.x - lpX);

	if (distHp <= kMarkerHitPx && distHp <= distLp)
		return HP;
	if (distLp <= kMarkerHitPx)
		return LP;
	if (distHp <= kMarkerHitPx)
		return HP;

	return None;
}

void CABTRAudioProcessorEditor::FilterBarComponent::setFreqFromMouseX (float mouseX, DragTarget target)
{
	if (owner == nullptr || target == None)
		return;

	const auto inner = getInnerArea();
	const float normX = (inner.getWidth() > 0.0f) ? (mouseX - inner.getX()) / inner.getWidth() : 0.0f;
	float freq = normXToFreq (normX);

	auto& proc = owner->audioProcessor;
	auto pick = [this] (const char* a, const char* b, const char* c) { return loaderIndex_ == 0 ? a : (loaderIndex_ == 1 ? b : c); };
	const char* hpId = pick (CABTRAudioProcessor::kParamHpFreqA, CABTRAudioProcessor::kParamHpFreqB, CABTRAudioProcessor::kParamHpFreqC);
	const char* lpId = pick (CABTRAudioProcessor::kParamLpFreqA, CABTRAudioProcessor::kParamLpFreqB, CABTRAudioProcessor::kParamLpFreqC);

	// Clamp so HP never exceeds LP and vice-versa
	if (target == HP)
	{
		const float otherFreq = proc.getValueTreeState().getRawParameterValue (lpId)->load();
		freq = juce::jmin (freq, otherFreq);
	}
	else
	{
		const float otherFreq = proc.getValueTreeState().getRawParameterValue (hpId)->load();
		freq = juce::jmax (freq, otherFreq);
	}

	const char* paramId = (target == HP) ? hpId : lpId;
	if (auto* param = proc.getValueTreeState().getParameter (paramId))
		param->setValueNotifyingHost (param->convertTo0to1 (freq));
}

void CABTRAudioProcessorEditor::FilterBarComponent::updateFromProcessor()
{
	if (owner == nullptr) return;
	auto& proc = owner->audioProcessor;
	auto pick = [this] (const char* a, const char* b, const char* c) { return loaderIndex_ == 0 ? a : (loaderIndex_ == 1 ? b : c); };
	const char* hpId   = pick (CABTRAudioProcessor::kParamHpFreqA, CABTRAudioProcessor::kParamHpFreqB, CABTRAudioProcessor::kParamHpFreqC);
	const char* lpId   = pick (CABTRAudioProcessor::kParamLpFreqA, CABTRAudioProcessor::kParamLpFreqB, CABTRAudioProcessor::kParamLpFreqC);
	const char* hpOnId = pick (CABTRAudioProcessor::kParamHpOnA,   CABTRAudioProcessor::kParamHpOnB,   CABTRAudioProcessor::kParamHpOnC);
	const char* lpOnId = pick (CABTRAudioProcessor::kParamLpOnA,   CABTRAudioProcessor::kParamLpOnB,   CABTRAudioProcessor::kParamLpOnC);

	const float newHpFreq = proc.getValueTreeState().getRawParameterValue (hpId)->load();
	const float newLpFreq = proc.getValueTreeState().getRawParameterValue (lpId)->load();
	const bool  newHpOn   = proc.getValueTreeState().getRawParameterValue (hpOnId)->load() > 0.5f;
	const bool  newLpOn   = proc.getValueTreeState().getRawParameterValue (lpOnId)->load() > 0.5f;

	if (newHpFreq == hpFreq_ && newLpFreq == lpFreq_ && newHpOn == hpOn_ && newLpOn == lpOn_)
		return;

	hpFreq_ = newHpFreq;
	lpFreq_ = newLpFreq;
	hpOn_   = newHpOn;
	lpOn_   = newLpOn;
	repaint();
}

void CABTRAudioProcessorEditor::FilterBarComponent::paint (juce::Graphics& g)
{
	const auto r = getLocalBounds().toFloat();

	// Outline
	g.setColour (scheme.outline);
	g.drawRect (r, 4.0f);

	// Background
	const auto inner = getInnerArea();
	g.setColour (scheme.bg);
	g.fillRect (inner);

	// Pass-band fill (between HP and LP)
	if (hpOn_ || lpOn_)
	{
		const float hpX = hpOn_ ? getMarkerScreenX (hpFreq_) : inner.getX();
		const float lpX = lpOn_ ? getMarkerScreenX (lpFreq_) : inner.getRight();

		if (lpX > hpX)
		{
			const auto band = juce::Rectangle<float> (hpX, inner.getY(), lpX - hpX, inner.getHeight());
			g.setColour (scheme.fg.withAlpha (0.12f));
			g.fillRect (band.getIntersection (inner));
		}
	}

	// HP marker
	{
		const float mx = getMarkerScreenX (hpFreq_);
		if (mx >= inner.getX() && mx <= inner.getRight())
		{
			const float alpha = hpOn_ ? 1.0f : 0.25f;
			g.setColour (scheme.fg.withAlpha (alpha));
			const float hw = 2.5f;
			const float overshoot = 3.0f;
			g.fillRoundedRectangle (mx - hw, inner.getY() - overshoot, hw * 2.0f,
			                        inner.getHeight() + overshoot * 2.0f, 2.0f);
		}
	}

	// LP marker
	{
		const float mx = getMarkerScreenX (lpFreq_);
		if (mx >= inner.getX() && mx <= inner.getRight())
		{
			const float alpha = lpOn_ ? 1.0f : 0.25f;
			g.setColour (scheme.fg.withAlpha (alpha));
			const float hw = 2.5f;
			const float overshoot = 3.0f;
			g.fillRoundedRectangle (mx - hw, inner.getY() - overshoot, hw * 2.0f,
			                        inner.getHeight() + overshoot * 2.0f, 2.0f);
		}
	}
}

void CABTRAudioProcessorEditor::FilterBarComponent::mouseDown (const juce::MouseEvent& e)
{
	if (e.mods.isPopupMenu())
	{
		if (owner != nullptr)
			owner->openFilterPrompt (loaderIndex_);
		return;
	}

	currentDrag_ = hitTestMarker (e.position);
	if (currentDrag_ != None)
	{
		setFreqFromMouseX (e.position.x, currentDrag_);
		updateFromProcessor();
	}
}

void CABTRAudioProcessorEditor::FilterBarComponent::mouseDrag (const juce::MouseEvent& e)
{
	if (currentDrag_ != None)
	{
		setFreqFromMouseX (e.position.x, currentDrag_);
		updateFromProcessor();
	}
}

void CABTRAudioProcessorEditor::FilterBarComponent::mouseUp (const juce::MouseEvent&)
{
	currentDrag_ = None;
}

void CABTRAudioProcessorEditor::FilterBarComponent::mouseMove (const juce::MouseEvent& e)
{
	const auto target = hitTestMarker (e.position);
	if (target == HP)
	{
		const int hz = juce::roundToInt (hpFreq_);
		setTooltip ("HP: " + juce::String (hz) + " Hz");
	}
	else if (target == LP)
	{
		const int hz = juce::roundToInt (lpFreq_);
		setTooltip ("LP: " + juce::String (hz) + " Hz");
	}
	else
	{
		setTooltip ({});
	}
}

void CABTRAudioProcessorEditor::FilterBarComponent::mouseDoubleClick (const juce::MouseEvent& e)
{
	if (owner == nullptr) return;
	auto& proc = owner->audioProcessor;
	auto pick = [this] (const char* a, const char* b, const char* c) { return loaderIndex_ == 0 ? a : (loaderIndex_ == 1 ? b : c); };

	const auto target = hitTestMarker (e.position);
	if (target == HP)
	{
		const char* paramId = pick (CABTRAudioProcessor::kParamHpOnA, CABTRAudioProcessor::kParamHpOnB, CABTRAudioProcessor::kParamHpOnC);
		if (auto* param = proc.getValueTreeState().getParameter (paramId))
		{
			const bool current = param->getValue() > 0.5f;
			param->setValueNotifyingHost (current ? 0.0f : 1.0f);
		}
	}
	else if (target == LP)
	{
		const char* paramId = pick (CABTRAudioProcessor::kParamLpOnA, CABTRAudioProcessor::kParamLpOnB, CABTRAudioProcessor::kParamLpOnC);
		if (auto* param = proc.getValueTreeState().getParameter (paramId))
		{
			const bool current = param->getValue() > 0.5f;
			param->setValueNotifyingHost (current ? 0.0f : 1.0f);
		}
	}
	else
	{
		owner->openFilterPrompt (loaderIndex_);
	}
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

	if (ticked)
	{
		g.setColour (scheme.outline);
		g.fillRect (r);
	}
	else
	{
		g.setColour (scheme.outline);
		g.drawRect (r, 4.0f);

		const float innerInset = juce::jlimit (1.0f, side * 0.45f, side * UiMetrics::tickBoxInnerInsetRatio);
		auto inner = r.reduced (innerInset);
		g.setColour (scheme.bg);
		g.fillRect (inner);
	}
}

void CABTRAudioProcessorEditor::MinimalLNF::drawToggleButton (
	juce::Graphics& g, juce::ToggleButton& button,
	bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown)
{
	const auto local = button.getLocalBounds().toFloat().reduced (1.0f);
	const float side = juce::jlimit (14.0f,
	                                 juce::jmax (14.0f, local.getHeight() - 2.0f),
	                                 std::round (local.getHeight() * 0.65f));

	drawTickBox (g, button, 0, 0, 0, 0,
	             button.getToggleState(), button.isEnabled(),
	             shouldDrawButtonAsHighlighted, shouldDrawButtonAsDown);

	const float textX = local.getX() + 2.0f + side + 2.0f;
	auto textArea = button.getLocalBounds().toFloat();
	textArea.removeFromLeft (textX);

	g.setColour (button.findColour (juce::ToggleButton::textColourId));

	float fontSize = juce::jlimit (12.0f, 40.0f, (float) button.getHeight() - 6.0f);

	// Shrink font if text would overflow available width
	const auto text = button.getButtonText();
	const float availW = textArea.getWidth();
	if (availW > 0)
	{
		juce::Font testFont (juce::FontOptions (fontSize).withStyle ("Bold"));
		juce::GlyphArrangement ga;
		ga.addLineOfText (testFont, text, 0.0f, 0.0f);
		const float neededW = ga.getBoundingBox (0, -1, false).getWidth();
		if (neededW > availW)
			fontSize = juce::jmax (8.0f, fontSize * (availW / neededW));
	}

	g.setFont (juce::Font (juce::FontOptions (fontSize).withStyle ("Bold")));

	g.drawText (text, textArea,
	            juce::Justification::centredLeft, false);
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
	g.drawFittedText (text,
	                  textInsetX,
	                  textInsetY,
	                  juce::jmax (1, width - (textInsetX * 2)),
	                  juce::jmax (1, height - (textInsetY * 2)),
	                  juce::Justification::centred,
	                  1);
}

//==============================================================================
//  Static loader param-ID table
//==============================================================================
const CABTRAudioProcessorEditor::LoaderParamIds CABTRAudioProcessorEditor::kLoaderParams[3] =
{
	{ // A
		CABTRAudioProcessor::kParamEnableA,
		CABTRAudioProcessor::kParamHpFreqA, CABTRAudioProcessor::kParamLpFreqA, CABTRAudioProcessor::kParamInA, CABTRAudioProcessor::kParamOutA, CABTRAudioProcessor::kParamTiltA,
		CABTRAudioProcessor::kParamStartA,  CABTRAudioProcessor::kParamEndA,    CABTRAudioProcessor::kParamSizeA,
		CABTRAudioProcessor::kParamDelayA,  CABTRAudioProcessor::kParamPanA,    CABTRAudioProcessor::kParamFredA, CABTRAudioProcessor::kParamPosA, CABTRAudioProcessor::kParamResoA,
		CABTRAudioProcessor::kParamInvA,    CABTRAudioProcessor::kParamNormA,   CABTRAudioProcessor::kParamRvsA,  CABTRAudioProcessor::kParamChaosA, CABTRAudioProcessor::kParamChaosFilterA,
		CABTRAudioProcessor::kParamChaosAmtA, CABTRAudioProcessor::kParamChaosSpdA,
		CABTRAudioProcessor::kParamChaosAmtFilterA, CABTRAudioProcessor::kParamChaosSpdFilterA,
		CABTRAudioProcessor::kParamModeInA, CABTRAudioProcessor::kParamModeOutA, CABTRAudioProcessor::kParamSumBusA, CABTRAudioProcessor::kParamMixA
	},
	{ // B
		CABTRAudioProcessor::kParamEnableB,
		CABTRAudioProcessor::kParamHpFreqB, CABTRAudioProcessor::kParamLpFreqB, CABTRAudioProcessor::kParamInB, CABTRAudioProcessor::kParamOutB, CABTRAudioProcessor::kParamTiltB,
		CABTRAudioProcessor::kParamStartB,  CABTRAudioProcessor::kParamEndB,    CABTRAudioProcessor::kParamSizeB,
		CABTRAudioProcessor::kParamDelayB,  CABTRAudioProcessor::kParamPanB,    CABTRAudioProcessor::kParamFredB, CABTRAudioProcessor::kParamPosB, CABTRAudioProcessor::kParamResoB,
		CABTRAudioProcessor::kParamInvB,    CABTRAudioProcessor::kParamNormB,   CABTRAudioProcessor::kParamRvsB,  CABTRAudioProcessor::kParamChaosB, CABTRAudioProcessor::kParamChaosFilterB,
		CABTRAudioProcessor::kParamChaosAmtB, CABTRAudioProcessor::kParamChaosSpdB,
		CABTRAudioProcessor::kParamChaosAmtFilterB, CABTRAudioProcessor::kParamChaosSpdFilterB,
		CABTRAudioProcessor::kParamModeInB, CABTRAudioProcessor::kParamModeOutB, CABTRAudioProcessor::kParamSumBusB, CABTRAudioProcessor::kParamMixB
	},
	{ // C
		CABTRAudioProcessor::kParamEnableC,
		CABTRAudioProcessor::kParamHpFreqC, CABTRAudioProcessor::kParamLpFreqC, CABTRAudioProcessor::kParamInC, CABTRAudioProcessor::kParamOutC, CABTRAudioProcessor::kParamTiltC,
		CABTRAudioProcessor::kParamStartC,  CABTRAudioProcessor::kParamEndC,    CABTRAudioProcessor::kParamSizeC,
		CABTRAudioProcessor::kParamDelayC,  CABTRAudioProcessor::kParamPanC,    CABTRAudioProcessor::kParamFredC, CABTRAudioProcessor::kParamPosC, CABTRAudioProcessor::kParamResoC,
		CABTRAudioProcessor::kParamInvC,    CABTRAudioProcessor::kParamNormC,   CABTRAudioProcessor::kParamRvsC,  CABTRAudioProcessor::kParamChaosC, CABTRAudioProcessor::kParamChaosFilterC,
		CABTRAudioProcessor::kParamChaosAmtC, CABTRAudioProcessor::kParamChaosSpdC,
		CABTRAudioProcessor::kParamChaosAmtFilterC, CABTRAudioProcessor::kParamChaosSpdFilterC,
		CABTRAudioProcessor::kParamModeInC, CABTRAudioProcessor::kParamModeOutC, CABTRAudioProcessor::kParamSumBusC, CABTRAudioProcessor::kParamMixC
	}
};

//==============================================================================
//  Loader ref accessors
//==============================================================================
CABTRAudioProcessorEditor::LoaderRefs CABTRAudioProcessorEditor::getLoaderRefs (int i)
{
	static const char* kLabels[] = { "A", "B", "C" };
	switch (i)
	{
		case 1: return { enableButtonB, browseButtonB, fileDisplayB,
		                 hpFreqSliderB, lpFreqSliderB, inSliderB, outSliderB, tiltSliderB, startSliderB, endSliderB,
		                 sizeSliderB, delaySliderB, panSliderB, fredSliderB, posSliderB, resoSliderB,
		                 invButtonB, normButtonB, rvsButtonB, chaosButtonB, chaosFilterButtonB, chaosDisplayB,
		                 modeInComboB, modeOutComboB, sumBusComboB, filterBarB_, mixSliderB };
		case 2: return { enableButtonC, browseButtonC, fileDisplayC,
		                 hpFreqSliderC, lpFreqSliderC, inSliderC, outSliderC, tiltSliderC, startSliderC, endSliderC,
		                 sizeSliderC, delaySliderC, panSliderC, fredSliderC, posSliderC, resoSliderC,
		                 invButtonC, normButtonC, rvsButtonC, chaosButtonC, chaosFilterButtonC, chaosDisplayC,
		                 modeInComboC, modeOutComboC, sumBusComboC, filterBarC_, mixSliderC };
		default: return { enableButtonA, browseButtonA, fileDisplayA,
		                  hpFreqSliderA, lpFreqSliderA, inSliderA, outSliderA, tiltSliderA, startSliderA, endSliderA,
		                  sizeSliderA, delaySliderA, panSliderA, fredSliderA, posSliderA, resoSliderA,
		                  invButtonA, normButtonA, rvsButtonA, chaosButtonA, chaosFilterButtonA, chaosDisplayA,
		                  modeInComboA, modeOutComboA, sumBusComboA, filterBarA_, mixSliderA };
	}
}

CABTRAudioProcessorEditor::AttachRefs CABTRAudioProcessorEditor::getAttachRefs (int i)
{
	switch (i)
	{
		case 1: return { enableAttachB,
		                 hpFreqAttachB, lpFreqAttachB, inAttachB, outAttachB, tiltAttachB, startAttachB, endAttachB,
		                 sizeAttachB, delayAttachB, panAttachB, fredAttachB, posAttachB, resoAttachB,
		                 invAttachB, normAttachB, rvsAttachB, chaosAttachB, chaosFilterAttachB,
		                 modeInAttachB, modeOutAttachB, sumBusAttachB, mixAttachB };
		case 2: return { enableAttachC,
		                 hpFreqAttachC, lpFreqAttachC, inAttachC, outAttachC, tiltAttachC, startAttachC, endAttachC,
		                 sizeAttachC, delayAttachC, panAttachC, fredAttachC, posAttachC, resoAttachC,
		                 invAttachC, normAttachC, rvsAttachC, chaosAttachC, chaosFilterAttachC,
		                 modeInAttachC, modeOutAttachC, sumBusAttachC, mixAttachC };
		default: return { enableAttachA,
		                  hpFreqAttachA, lpFreqAttachA, inAttachA, outAttachA, tiltAttachA, startAttachA, endAttachA,
		                  sizeAttachA, delayAttachA, panAttachA, fredAttachA, posAttachA, resoAttachA,
		                  invAttachA, normAttachA, rvsAttachA, chaosAttachA, chaosFilterAttachA,
		                  modeInAttachA, modeOutAttachA, sumBusAttachA, mixAttachA };
	}
}

//==============================================================================
//  setupLoaderUI — unified per-loader component initialisation
//==============================================================================
void CABTRAudioProcessorEditor::setupLoaderUI (int loaderIndex, LoaderRefs r,
                                                const char* chaosAmtId, const char* chaosSpdId)
{
	const juce::String suffix = juce::String (loaderIndex == 0 ? "A" : loaderIndex == 1 ? "B" : "C");

	addAndMakeVisible (r.enableBtn);
	r.enableBtn.setButtonText ("ENABLE " + suffix);
	r.enableBtn.addListener (this);

	addAndMakeVisible (r.browseBtn);
	r.browseBtn.setOwner (this, loaderIndex);
	r.browseBtn.addListener (this);
	r.browseBtn.setColour (juce::TextButton::buttonColourId, activeScheme.bg);

	addAndMakeVisible (r.fileDisp);
	r.fileDisp.setText ("No file loaded", juce::dontSendNotification);
	r.fileDisp.setJustificationType (juce::Justification::centred);
	r.fileDisp.setInterceptsMouseClicks (false, false);

	using ST = BarSlider::Type;
	auto setupSlider = [this] (BarSlider& slider, const juce::String& /*tooltip*/, ST type) {
		addAndMakeVisible (slider);
		slider.setOwner (this);
		slider.setType (type);
		setupBar (slider);
		slider.addListener (this);
	};

	setupSlider (r.hp,    "HP Filter " + suffix,                       ST::HpFreq);
	setupSlider (r.lp,    "LP Filter " + suffix,                       ST::LpFreq);
	setupSlider (r.in,    "Input Gain " + suffix,                      ST::Input);
	setupSlider (r.out,   "Output Gain " + suffix,                     ST::Output);
	setupSlider (r.tilt,  "Tilt EQ " + suffix + " (-6/+6 dB)",    ST::Tilt);
	setupSlider (r.start, "IR Start Time " + suffix,                   ST::Start);
	setupSlider (r.end,   "IR End Time " + suffix,                     ST::End);
	setupSlider (r.size,  "Size " + suffix + " (25%-400%)",            ST::Size);
	setupSlider (r.delay, "Delay " + suffix + " (0-1000ms)",           ST::Delay);
	setupSlider (r.pan,   "Pan " + suffix + " (L-R)",                  ST::Pan);
	setupSlider (r.fred,  "Angle " + suffix + " (off-axis mic simulation)", ST::Fred);
	setupSlider (r.pos,   "Distance " + suffix + " (proximity/distance)",   ST::Pos);
	setupSlider (r.reso,  "Resonance " + suffix + " (0%-200%)",        ST::Reso);

	addAndMakeVisible (r.inv);   r.inv.setButtonText ("INV");            r.inv.addListener (this);
	addAndMakeVisible (r.norm);  r.norm.setButtonText ("NRM");           r.norm.addListener (this);
	addAndMakeVisible (r.rvs);   r.rvs.setButtonText ("RVS");           r.rvs.addListener (this);
	addAndMakeVisible (r.chaos); r.chaos.setButtonText ("CHSD"); r.chaos.addListener (this);
	r.chaos.addMouseListener (this, false);
	addAndMakeVisible (r.chaosFilter); r.chaosFilter.setButtonText ("CHSF"); r.chaosFilter.addListener (this);
	r.chaosFilter.addMouseListener (this, false);

	{
		const float savedAmt = audioProcessor.getValueTreeState().getRawParameterValue (chaosAmtId)->load();
		const float savedSpd = audioProcessor.getValueTreeState().getRawParameterValue (chaosSpdId)->load();
		r.chaosDisp.setText ("", juce::dontSendNotification);
		r.chaosDisp.setInterceptsMouseClicks (true, false);
		r.chaosDisp.addMouseListener (this, false);
		r.chaosDisp.setTooltip (juce::String (juce::roundToInt (savedAmt)) + "% | " + juce::String (juce::roundToInt (savedSpd)) + " Hz");
		r.chaosDisp.setColour (juce::Label::backgroundColourId, juce::Colours::transparentBlack);
		r.chaosDisp.setColour (juce::Label::outlineColourId, juce::Colours::transparentBlack);
		r.chaosDisp.setOpaque (false);
		addAndMakeVisible (r.chaosDisp);
	}

	auto setupModeCombo = [this] (juce::ComboBox& combo) {
		addAndMakeVisible (combo);
		combo.addItem ("L+R", 1);
		combo.addItem ("MID", 2);
		combo.addItem ("SIDE", 3);
		combo.setJustificationType (juce::Justification::centred);
		combo.setLookAndFeel (&lnf);
		combo.addListener (this);
	};
	setupModeCombo (r.modeIn);
	setupModeCombo (r.modeOut);

	{
		addAndMakeVisible (r.sumBus);
		r.sumBus.addItem ("ST",  1);
		r.sumBus.addItem (juce::String (juce::CharPointer_UTF8 ("\xe2\x86\x92")) + "M", 2);
		r.sumBus.addItem (juce::String (juce::CharPointer_UTF8 ("\xe2\x86\x92")) + "S", 3);
		r.sumBus.setJustificationType (juce::Justification::centred);
		r.sumBus.setLookAndFeel (&lnf);
		r.sumBus.addListener (this);
	}

	r.filterBar.setOwner (this, loaderIndex);
	r.filterBar.setScheme (activeScheme);
	addAndMakeVisible (r.filterBar);

	setupSlider (r.mix, "Mix " + suffix + " (Dry/Wet)", ST::Mix);
}

//==============================================================================
//  createLoaderAttachments — unified per-loader param attachment wiring
//==============================================================================
void CABTRAudioProcessorEditor::createLoaderAttachments (juce::AudioProcessorValueTreeState& params,
                                                          int loaderIndex,
                                                          LoaderRefs ui, AttachRefs a)
{
	const auto& ids = kLoaderParams[loaderIndex];

	a.enableAtt  = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>   (params, ids.enable,  ui.enableBtn);
	a.hpAtt      = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>   (params, ids.hpFreq,  ui.hp);
	a.lpAtt      = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>   (params, ids.lpFreq,  ui.lp);
	a.inAtt      = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>   (params, ids.in,      ui.in);
	a.outAtt     = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>   (params, ids.out,     ui.out);
	a.tiltAtt    = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>   (params, ids.tilt,    ui.tilt);
	a.startAtt   = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>   (params, ids.start,   ui.start);
	a.endAtt     = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>   (params, ids.end,     ui.end);
	a.sizeAtt    = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>   (params, ids.size,    ui.size);
	a.delayAtt   = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>   (params, ids.delay,   ui.delay);
	a.panAtt     = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>   (params, ids.pan,     ui.pan);
	a.fredAtt    = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>   (params, ids.fred,    ui.fred);
	a.posAtt     = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>   (params, ids.pos,     ui.pos);
	a.resoAtt    = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>   (params, ids.reso,    ui.reso);
	a.invAtt     = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>   (params, ids.inv,     ui.inv);
	a.normAtt    = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>   (params, ids.norm,    ui.norm);
	a.rvsAtt     = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>   (params, ids.rvs,     ui.rvs);
	a.chaosAtt   = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>   (params, ids.chaos,   ui.chaos);
	a.chaosFilterAtt = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (params, ids.chaosFilter, ui.chaosFilter);
	a.modeInAtt  = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (params, ids.modeIn,  ui.modeIn);
	a.modeOutAtt = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (params, ids.modeOut, ui.modeOut);
	a.sumBusAtt  = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (params, ids.sumBus, ui.sumBus);
	a.mixAtt     = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>   (params, ids.mix,     ui.mix);

	// UI-only skew: changes slider feel without altering VST3 parameter normalization
	ui.hp.setSkewFactor (0.35);
	ui.lp.setSkewFactor (0.35);
	ui.out.setSkewFactor (3.23);
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
	currentFolderC = juce::File::getSpecialLocation (juce::File::userDocumentsDirectory);

	// Setup IR Loader A/B/C components (unified)
	for (int i = 0; i < 3; ++i)
		setupLoaderUI (i, getLoaderRefs (i), kLoaderParams[i].chaosAmt, kLoaderParams[i].chaosSpd);

	// Setup global controls
	addAndMakeVisible (routeCombo);
	routeCombo.addItem ("A>B>C", 1);
	routeCombo.addItem ("A|B|C", 2);
	routeCombo.addItem ("A>B|C", 3);
	routeCombo.addItem ("A|B>C", 4);
	routeCombo.setJustificationType (juce::Justification::centred);
	routeCombo.setLookAndFeel (&lnf);
	routeCombo.addListener (this);

	addAndMakeVisible (alignButton);
	alignButton.setButtonText ("ALIGN");
	alignButton.addListener (this);

	addAndMakeVisible (matchCombo);
	matchCombo.addItem ("None", 1);
	matchCombo.addItem ("White", 2);
	matchCombo.addItem ("Pink (-3dB)", 3);
	matchCombo.addItem ("Brown (-6dB)", 4);
	matchCombo.addItem ("Bright (+3dB)", 5);
	matchCombo.addItem ("Bright+ (+6dB)", 6);
	matchCombo.setJustificationType (juce::Justification::centred);
	matchCombo.setLookAndFeel (&lnf);

	addAndMakeVisible (trimCombo);
	trimCombo.addItem ("Off", 1);
	trimCombo.addItem ("0 dB", 2);
	trimCombo.addItem ("-3 dB", 3);
	trimCombo.addItem ("-6 dB", 4);
	trimCombo.addItem ("-12 dB", 5);
	trimCombo.addItem ("-18 dB", 6);
	trimCombo.setJustificationType (juce::Justification::centred);
	trimCombo.setLookAndFeel (&lnf);

	// Global MIX bar slider (footer)
	addAndMakeVisible (globalMixSlider);
	globalMixSlider.setOwner (this);
	globalMixSlider.setType (BarSlider::Type::GlobalMix);
	setupBar (globalMixSlider);
	globalMixSlider.addListener (this);

	// Global OUTPUT bar slider (footer)
	addAndMakeVisible (globalOutputSlider);
	globalOutputSlider.setOwner (this);
	globalOutputSlider.setType (BarSlider::Type::GlobalOutput);
	setupBar (globalOutputSlider);
	globalOutputSlider.addListener (this);

	// Create parameter attachments
	auto& params = audioProcessor.getValueTreeState();

	// Create per-loader parameter attachments
	for (int i = 0; i < 3; ++i)
		createLoaderAttachments (params, i, getLoaderRefs (i), getAttachRefs (i));

	// Global parameter attachments
	routeAttach = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (
		params, CABTRAudioProcessor::kParamRoute, routeCombo);
	alignAttach = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (
		params, CABTRAudioProcessor::kParamAlign, alignButton);
	matchAttach = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (
		params, CABTRAudioProcessor::kParamMatch, matchCombo);
	trimAttach = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (
		params, CABTRAudioProcessor::kParamTrim, trimCombo);
	globalMixAttach = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (
		params, CABTRAudioProcessor::kParamMix, globalMixSlider);
	globalOutputAttach = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (
		params, CABTRAudioProcessor::kParamOutput, globalOutputSlider);

	// Initialize collapse state from processor
	ioSectionExpanded_ = audioProcessor.getUiIoExpanded();

	// Setup tooltip window
	tooltipWindow = std::make_unique<juce::TooltipWindow> (this, 250);
	tooltipWindow->setLookAndFeel (&lnf);
	tooltipWindow->setAlwaysOnTop (true);
	tooltipWindow->setInterceptsMouseClicks (false, false);

	// Setup prompt overlay
	addChildComponent (promptOverlay);
	promptOverlay.setInterceptsMouseClicks (true, true);

	// Setup CRT effect
	setComponentEffect (&crtEffect);

	// Setup parameter listeners for UI state
	for (auto* paramId : kUiMirrorParamIds)
		params.addParameterListener (paramId, this);

	// Restore persisted UI state from processor (palette, CRT, colors)
	useCustomPalette = audioProcessor.getUiUseCustomPalette();
	crtEnabled       = audioProcessor.getUiFxTailEnabled();
	crtEffect.setEnabled (crtEnabled);
	for (int i = 0; i < 2; ++i)
		customPalette[(size_t) i] = audioProcessor.getUiCustomPaletteColour (i);

	// Initialize palette
	applyActivePalette();

	// Initial refresh of cached text
	refreshLegendTextCache();
	legendDirty = false;

	// Restore persisted window size
	const int restoredW = juce::jlimit (800, 2000, audioProcessor.getUiEditorWidth());
	const int restoredH = juce::jlimit (500, 1200, audioProcessor.getUiEditorHeight());
	setSize (restoredW, restoredH);
	setResizable (true, true);
	setResizeLimits (800, 500, 2000, 1200);

	// Start timer
	startTimer (kIdleTimerHz);

	// Initialize loader enabled/disabled visual state
	updateLoaderEnabledState (0);
	updateLoaderEnabledState (1);
	updateLoaderEnabledState (2);
}

CABTRAudioProcessorEditor::~CABTRAudioProcessorEditor()
{
	TR::dismissEditorOwnedModalPrompts (lnf);
	setPromptOverlayActive (false);

	// Persist UI state to processor before teardown
	audioProcessor.setUiUseCustomPalette (useCustomPalette);
	audioProcessor.setUiFxTailEnabled (crtEnabled);

	setComponentEffect (nullptr);

	if (tooltipWindow != nullptr)
		tooltipWindow->setLookAndFeel (nullptr);

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

		// Footer combo labels — explicit font + colour
		g.setColour (activeScheme.text);
		g.setFont (juce::Font (juce::FontOptions (14.0f).withStyle ("Bold")));

		const auto routeArea = routeCombo.getBounds().withHeight (16).translated (0, -18);
		g.drawText ("ROUTE", routeArea, juce::Justification::centred);

		const auto matchArea = matchCombo.getBounds().withHeight (16).translated (0, -18);
		g.drawText ("MATCH", matchArea, juce::Justification::centred);

		const auto trimArea = trimCombo.getBounds().withHeight (16).translated (0, -18);
		g.drawText ("NORM", trimArea, juce::Justification::centred);

		// Global MIX label + value (right of bar)
		if (globalMixSlider.isVisible())
		{
			const auto mixBounds = globalMixSlider.getBounds();
			const auto mixArea = mixBounds.withHeight (16).translated (0, -18);
			g.drawText ("MIX", mixArea, juce::Justification::centred);

			const int gMixPct = juce::roundToInt (globalMixSlider.getValue() * 100.0);
			const auto valArea = juce::Rectangle<int> (mixBounds.getRight() + 4, mixBounds.getY(), 56, mixBounds.getHeight());
			g.drawText (juce::String (gMixPct) + "%", valArea, juce::Justification::centredLeft);
		}

		// Global OUTPUT label + value (right of bar)
		if (globalOutputSlider.isVisible())
		{
			const auto outBounds = globalOutputSlider.getBounds();
			const auto outArea = outBounds.withHeight (16).translated (0, -18);
			g.drawText ("OUTPUT", outArea, juce::Justification::centred);

			const float gOutDb = (float) globalOutputSlider.getValue();
			juce::String outTxt = (gOutDb <= -80.0f) ? "-INF" : juce::String (gOutDb, 1) + " dB";
			const auto valArea = juce::Rectangle<int> (outBounds.getRight() + 4, outBounds.getY(), 66, outBounds.getHeight());
			g.drawText (outTxt, valArea, juce::Justification::centredLeft);
		}
	}

	// Per-loader MODE IN / MODE OUT labels (only when expanded/visible)
	if (ioSectionExpanded_)
	{
		auto drawModeLabels = [&] (juce::ComboBox& modeIn, juce::ComboBox& modeOut, juce::ComboBox& sumBus, juce::ToggleButton& enableBtn)
		{
			if (! modeIn.isVisible()) return;
			const float alpha = enableBtn.getToggleState() ? 1.0f : 0.35f;
			g.setColour (activeScheme.text.withAlpha (alpha));
			const auto font = juce::Font (juce::FontOptions (11.0f).withStyle ("Bold"));
			g.setFont (font);
			const auto miArea = modeIn.getBounds().withHeight (14).translated (0, -15);
			const auto moArea = modeOut.getBounds().withHeight (14).translated (0, -15);
			const auto sbArea = sumBus.getBounds().withHeight (14).translated (0, -15);
			const float comboW = (float) modeIn.getWidth();
			juce::GlyphArrangement ga;
			ga.addLineOfText (font, "MODE OUT", 0.0f, 0.0f);
			const bool useShort = ga.getBoundingBox (0, -1, false).getWidth() > comboW;
			g.drawText (useShort ? "IN"  : "MODE IN",  miArea, juce::Justification::centred);
			g.drawText (useShort ? "OUT" : "MODE OUT", moArea, juce::Justification::centred);
			g.drawText (useShort ? "SUM" : "SUM BUS",  sbArea, juce::Justification::centred);
		};
		drawModeLabels (modeInComboA, modeOutComboA, sumBusComboA, enableButtonA);
		drawModeLabels (modeInComboB, modeOutComboB, sumBusComboB, enableButtonB);
		drawModeLabels (modeInComboC, modeOutComboC, sumBusComboC, enableButtonC);
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

		for (int loader = 0; loader < 3; ++loader)
		{
			auto refs = getLoaderRefs (loader);
			const bool enabled = refs.enableBtn.getToggleState();
			g.setColour (enabled ? activeScheme.text : activeScheme.text.withAlpha (0.35f));
			const int colR = columnRight_[loader];

			juce::Slider* loaderSliders[kNumCachedParams] = {
				&refs.hp, &refs.lp, &refs.in, &refs.out, &refs.tilt,
				&refs.start, &refs.end, &refs.size, &refs.delay,
				&refs.pan, &refs.fred, &refs.pos, &refs.reso, &refs.mix
			};

			for (int i = 0; i < kNumCachedParams; ++i)
			{
				if (loaderSliders[i]->isVisible())
				{
					const auto valueArea = getValueAreaFor (loaderSliders[i]->getBounds(), colR);
					cachedValueAreas_[(size_t) (loader * kNumCachedParams + i)] = valueArea;
					tryDrawLegend (valueArea, cachedTexts[loader][i].full,
					               cachedTexts[loader][i].short_, cachedTexts[loader][i].intOnly);
				}
			}

			if (refs.filterBar.isVisible())
			{
				const auto filterValueArea = getValueAreaFor (refs.filterBar.getBounds(), colR);
				g.drawText ("FILTER", filterValueArea, juce::Justification::centredLeft);
			}
		}
	}
}

void CABTRAudioProcessorEditor::paintOverChildren (juce::Graphics& g)
{
	// Skip toggle bar when prompt overlay is active (it would paint over the prompt)
	if (promptOverlayActive)
		return;

	// ── Toggle bars (triangle + rounded horizontal bar) ──
	auto drawToggleBar = [&] (const juce::Rectangle<int>& area)
	{
		if (area.isEmpty()) return;
		const float barRadius = (float) area.getHeight() * 0.3f;
		g.setColour (activeScheme.fg.withAlpha (0.25f));
		g.fillRoundedRectangle (area.toFloat(), barRadius);

		const float triH = (float) area.getHeight() * 0.8f;
		const float triW = triH * 1.125f;
		const float cx = (float) area.getCentreX();
		const float cy = (float) area.getCentreY();

		juce::Path tri;
		if (ioSectionExpanded_)
		{
			tri.addTriangle (cx - triW * 0.5f, cy + triH * 0.35f,
			                 cx + triW * 0.5f, cy + triH * 0.35f,
			                 cx,               cy - triH * 0.35f);
		}
		else
		{
			tri.addTriangle (cx - triW * 0.5f, cy - triH * 0.35f,
			                 cx + triW * 0.5f, cy - triH * 0.35f,
			                 cx,               cy + triH * 0.35f);
		}
		g.setColour (activeScheme.text);
		g.fillPath (tri);
	};

	drawToggleBar (cachedToggleBarArea_);
}

//==============================================================================
//  Resized & Layout
//==============================================================================
void CABTRAudioProcessorEditor::resized()
{
	// Persist window size to processor
	audioProcessor.setUiEditorSize (getWidth(), getHeight());

	auto bounds = getLocalBounds();
	
	// Header (title area + buttons)
	auto header = bounds.removeFromTop (40);

	// Footer for global controls
	auto footer = bounds.removeFromBottom (50);
	
	// Split remaining area into three columns for A, B and C
	const int colWidth = bounds.getWidth() / 3;
	auto leftArea = bounds.removeFromLeft (colWidth);
	auto midArea = bounds.removeFromLeft (colWidth);
	auto rightArea = bounds;

	// Store column right edges for value area clamping
	columnRight_[0] = leftArea.getRight();
	columnRight_[1] = midArea.getRight();
	columnRight_[2] = rightArea.getRight();

	// Layout IR Loader A
	layoutIRSection (leftArea, 0);

	// Layout IR Loader B
	layoutIRSection (midArea, 1);

	// Layout IR Loader C
	layoutIRSection (rightArea, 2);

	// Compute single continuous toggle bar from all column areas
	cachedToggleBarArea_ = cachedToggleBarAreaA_.getUnion (cachedToggleBarAreaB_).getUnion (cachedToggleBarAreaC_);

	// Layout footer: aligned to 3-column grid
	// ┌──── col A ─────┐┌──── col B ─────┐┌──── col C ─────┐
	// │ ROUTE  ALIGN    ││  MIX  [===] val ││ OUTPUT [===] val│
	// └─────────────────┘└─────────────────┘└─────────────────┘
	const int footerMargin = 10;
	const int footerGap    = 8;
	const int barH         = 22;
	const int colW         = getWidth() / 3;

	// Column A area: ROUTE + ALIGN + MATCH + TRIM
	{
		auto colA = footer.withWidth (colW).reduced (footerMargin, 0);
		auto row = colA.withSizeKeepingCentre (colA.getWidth(), 30);
		const int comboW = 80;
		const int btnW   = 70;
		const int matchW = 105;
		const int trimW  = 70;
		routeCombo.setBounds (row.removeFromLeft (comboW));
		row.removeFromLeft (footerGap);
		alignButton.setBounds (row.removeFromLeft (btnW));
		row.removeFromLeft (footerGap);
		matchCombo.setBounds (row.removeFromLeft (matchW));
		row.removeFromLeft (footerGap);
		trimCombo.setBounds (row.removeFromLeft (trimW));
	}

	// Column B area: MIX bar + value text to the right
	{
		auto colB = footer.withX (colW).withWidth (colW).reduced (footerMargin, 0);
		auto row = colB.withSizeKeepingCentre (colB.getWidth(), barH);
		row.removeFromRight (60);  // reserve space for value text
		globalMixSlider.setBounds (row);
	}

	// Column C area: OUTPUT bar + value text to the right
	{
		auto colC = footer.withX (colW * 2).withWidth (getWidth() - colW * 2).reduced (footerMargin, 0);
		auto row = colC.withSizeKeepingCentre (colC.getWidth(), barH);
		row.removeFromRight (70);  // reserve space for value text
		globalOutputSlider.setBounds (row);
	}

	promptOverlay.setBounds (getLocalBounds());

	legendDirty = true;
	updateInfoIconCache();
	updateExportIconCache();
}

void CABTRAudioProcessorEditor::layoutIRSection (juce::Rectangle<int> area, int loaderIndex)
{
	const int margin = 10;
	const int buttonH = 30;
	const int gap = 5;
	const int toggleBarH = 20;

	auto pick = [&] (auto& a, auto& b, auto& c) -> auto& { return loaderIndex == 0 ? a : (loaderIndex == 1 ? b : c); };

	area.reduce (margin, margin);

	// Enable checkbox at top
	auto& enableBtn = pick (enableButtonA, enableButtonB, enableButtonC);
	enableBtn.setBounds (area.removeFromTop (buttonH));
	area.removeFromTop (gap);

	// File explorer section
	auto fileArea = area.removeFromTop (80);
	auto& browseBtn = pick (browseButtonA, browseButtonB, browseButtonC);
	auto& fileDisp = pick (fileDisplayA, fileDisplayB, fileDisplayC);
	
	browseBtn.setBounds (fileArea.removeFromTop (buttonH));
	fileArea.removeFromTop (gap);
	fileDisp.setBounds (fileArea);
	area.removeFromTop (gap);

	// Toggle bar area — full column width (union computed in resized)
	auto toggleBarArea = area.removeFromTop (toggleBarH);
	if (loaderIndex == 0)
		cachedToggleBarAreaA_ = toggleBarArea;
	else if (loaderIndex == 1)
		cachedToggleBarAreaB_ = toggleBarArea;
	else
		cachedToggleBarAreaC_ = toggleBarArea;
	area.removeFromTop (gap);

	const int sliderW = static_cast<int> (area.getWidth() * 0.50f);

	// Component references
	auto& hp    = pick (hpFreqSliderA,  hpFreqSliderB,  hpFreqSliderC);
	auto& lp    = pick (lpFreqSliderA,  lpFreqSliderB,  lpFreqSliderC);
	auto& in_   = pick (inSliderA,      inSliderB,      inSliderC);
	auto& out   = pick (outSliderA,     outSliderB,     outSliderC);
	auto& tilt  = pick (tiltSliderA,    tiltSliderB,    tiltSliderC);
	auto& start = pick (startSliderA,   startSliderB,   startSliderC);
	auto& end   = pick (endSliderA,     endSliderB,     endSliderC);
	auto& size  = pick (sizeSliderA,    sizeSliderB,    sizeSliderC);
	auto& delay = pick (delaySliderA,   delaySliderB,   delaySliderC);
	auto& pan   = pick (panSliderA,     panSliderB,     panSliderC);
	auto& fred  = pick (fredSliderA,    fredSliderB,    fredSliderC);
	auto& pos   = pick (posSliderA,     posSliderB,     posSliderC);
	auto& reso  = pick (resoSliderA,    resoSliderB,    resoSliderC);
	auto& mix   = pick (mixSliderA,     mixSliderB,     mixSliderC);
	auto& inv   = pick (invButtonA,     invButtonB,     invButtonC);
	auto& norm  = pick (normButtonA,    normButtonB,    normButtonC);
	auto& rvs   = pick (rvsButtonA,     rvsButtonB,     rvsButtonC);
	auto& chaos = pick (chaosButtonA,   chaosButtonB,   chaosButtonC);
	auto& chaosFilter = pick (chaosFilterButtonA, chaosFilterButtonB, chaosFilterButtonC);
	auto& filterBar  = pick (filterBarA_,      filterBarB_,      filterBarC_);
	auto& modeInCmb  = pick (modeInComboA,     modeInComboB,     modeInComboC);
	auto& modeOutCmb = pick (modeOutComboA,    modeOutComboB,    modeOutComboC);
	auto& sumBusCmb  = pick (sumBusComboA,     sumBusComboB,     sumBusComboC);
	auto& chaosDisp  = pick (chaosDisplayA,    chaosDisplayB,    chaosDisplayC);

	if (ioSectionExpanded_)
	{
		// ── Expanded IO view: IN, OUT, TILT, FILTER, MIX, MODE IN/OUT, CHAOS ──
		// 5 sliders fill remaining space; mode + chaos get fixed height
		const int modeLabelGap = gap * 2;
		const int comboH = 30;
		const int fixedH = comboH * 2 + modeLabelGap + gap * 5 + gap * 2;
		const int sliderH = juce::jmax (20, (area.getHeight() - fixedH) / 5);

		auto sliderRow = area.removeFromTop (sliderH);
		in_.setBounds (sliderRow.removeFromLeft (sliderW));
		in_.setVisible (true);
		area.removeFromTop (gap);

		sliderRow = area.removeFromTop (sliderH);
		out.setBounds (sliderRow.removeFromLeft (sliderW));
		out.setVisible (true);
		area.removeFromTop (gap);

		sliderRow = area.removeFromTop (sliderH);
		tilt.setBounds (sliderRow.removeFromLeft (sliderW));
		tilt.setVisible (true);
		area.removeFromTop (gap);

		sliderRow = area.removeFromTop (sliderH);
		filterBar.setBounds (sliderRow.removeFromLeft (sliderW));
		filterBar.setVisible (true);
		area.removeFromTop (gap);

		sliderRow = area.removeFromTop (sliderH);
		mix.setBounds (sliderRow.removeFromLeft (sliderW));
		mix.setVisible (true);
		area.removeFromTop (gap);

		// MODE IN / MODE OUT / SUM BUS combos
		area.removeFromTop (modeLabelGap);
		auto modeRow = area.removeFromTop (comboH);
		const int modeComboW = (sliderW - gap * 2) / 3;
		modeInCmb.setBounds  (modeRow.getX(), modeRow.getY(), modeComboW, comboH);
		modeOutCmb.setBounds (modeRow.getX() + modeComboW + gap, modeRow.getY(), modeComboW, comboH);
		sumBusCmb.setBounds  (modeRow.getX() + (modeComboW + gap) * 2, modeRow.getY(), modeComboW, comboH);
		modeInCmb.setVisible (true);
		modeOutCmb.setVisible (true);
		sumBusCmb.setVisible (true);
		area.removeFromTop (gap * 2);

		// CHSF + CHSD checkboxes — CHSF aligned with sliders, CHSD aligned with value column
		auto checkArea = area.removeFromTop (comboH);
		constexpr int valuePadPx = 8;
		const int chsfW = sliderW;
		const int chsdX = checkArea.getX() + sliderW + valuePadPx;
		const int chsdW = checkArea.getRight() - chsdX;
		chaosFilter.setBounds (checkArea.getX(), checkArea.getY(), chsfW, comboH);
		chaos.setBounds (chsdX, checkArea.getY(), chsdW, comboH);
		chaosFilter.setVisible (true);
		chaos.setVisible (true);
		chaosDisp.setBounds (checkArea.getX(), checkArea.getY(), checkArea.getWidth(), comboH);
		chaosDisp.setVisible (true);

		// Hide collapsed-only controls
		hp.setVisible (false);     lp.setVisible (false);
		start.setVisible (false);  end.setVisible (false);
		size.setVisible (false);   delay.setVisible (false);
		pan.setVisible (false);    fred.setVisible (false);
		pos.setVisible (false);    reso.setVisible (false);
		inv.setVisible (false);    norm.setVisible (false);
		rvs.setVisible (false);
	}
	else
	{
		// ── Collapsed main view: 8 sliders + INV/NORM/RVS checkboxes ──
		// Checkboxes get fixed height; sliders fill remaining space
		const int checkH = 30;
		const int totalGap = gap * 7 + gap * 2; // 7 gaps between 8 sliders + extra gap before checkboxes
		const int sliderH = juce::jmax (20, (area.getHeight() - totalGap - checkH) / 8);

		auto sliderRow = area.removeFromTop (sliderH);
		start.setBounds (sliderRow.removeFromLeft (sliderW));
		start.setVisible (true);
		area.removeFromTop (gap);

		sliderRow = area.removeFromTop (sliderH);
		end.setBounds (sliderRow.removeFromLeft (sliderW));
		end.setVisible (true);
		area.removeFromTop (gap);

		sliderRow = area.removeFromTop (sliderH);
		size.setBounds (sliderRow.removeFromLeft (sliderW));
		size.setVisible (true);
		area.removeFromTop (gap);

		sliderRow = area.removeFromTop (sliderH);
		delay.setBounds (sliderRow.removeFromLeft (sliderW));
		delay.setVisible (true);
		area.removeFromTop (gap);

		sliderRow = area.removeFromTop (sliderH);
		pan.setBounds (sliderRow.removeFromLeft (sliderW));
		pan.setVisible (true);
		area.removeFromTop (gap);

		sliderRow = area.removeFromTop (sliderH);
		fred.setBounds (sliderRow.removeFromLeft (sliderW));
		fred.setVisible (true);
		area.removeFromTop (gap);

		sliderRow = area.removeFromTop (sliderH);
		pos.setBounds (sliderRow.removeFromLeft (sliderW));
		pos.setVisible (true);
		area.removeFromTop (gap);

		sliderRow = area.removeFromTop (sliderH);
		reso.setBounds (sliderRow.removeFromLeft (sliderW));
		reso.setVisible (true);
		area.removeFromTop (gap * 2);

		// Checkboxes: INV, NORM, RVS — distribute evenly across sliderW
		auto checkArea = area.removeFromTop (checkH);
		const int numButtons = 3;
		const int checkboxW = sliderW / numButtons;

		int bx = checkArea.getX();
		inv.setBounds  (bx, checkArea.getY(), checkboxW, checkH);  inv.setVisible (true);
		bx += checkboxW;
		norm.setBounds (bx, checkArea.getY(), checkboxW, checkH);  norm.setVisible (true);
		bx += checkboxW;
		rvs.setBounds  (bx, checkArea.getY(), checkboxW, checkH);  rvs.setVisible (true);

		// Hide expanded-only controls
		in_.setVisible (false);        out.setVisible (false);     tilt.setVisible (false);
		filterBar.setVisible (false);
		mix.setVisible (false);
		modeInCmb.setVisible (false);    modeOutCmb.setVisible (false);    sumBusCmb.setVisible (false);
		chaos.setVisible (false);      chaosFilter.setVisible (false);  chaosDisp.setVisible (false);
		hp.setVisible (false);         lp.setVisible (false);
	}
}

//==============================================================================
//  Loader enabled/disabled visual state
//==============================================================================
void CABTRAudioProcessorEditor::updateLoaderEnabledState (int loaderIndex)
{
	auto r = getLoaderRefs (loaderIndex);

	const bool enabled = r.enableBtn.getToggleState();
	const float alpha = enabled ? 1.0f : 0.35f;
	const bool interactive = enabled && ! promptOverlayActive;

	juce::Component* components[] = {
		&r.browseBtn, &r.fileDisp,
		&r.hp, &r.lp, &r.in, &r.out, &r.tilt,
		&r.start, &r.end, &r.size, &r.delay, &r.pan, &r.fred, &r.pos, &r.reso,
		&r.inv, &r.norm, &r.rvs, &r.chaos, &r.chaosFilter, &r.chaosDisp,
		&r.modeIn, &r.modeOut, &r.sumBus,
		&r.filterBar, &r.mix
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
	juce::Label*  fileDisps[]    = { &fileDisplayA, &fileDisplayB, &fileDisplayC };
	juce::String* currentFiles[] = { &currentFileA, &currentFileB, &currentFileC };
	juce::File*   currentDirs[]  = { &currentFolderA, &currentFolderB, &currentFolderC };
	const juce::String* paths[]  = { &audioProcessor.stateA.currentFilePath,
	                                  &audioProcessor.stateB.currentFilePath,
	                                  &audioProcessor.stateC.currentFilePath };

	for (int i = 0; i < 3; ++i)
	{
		if (paths[i]->isNotEmpty() && fileDisps[i]->getText() == "No file loaded")
		{
			juce::File f (*paths[i]);
			if (f.existsAsFile())
			{
				*currentFiles[i] = f.getFileName();
				*currentDirs[i]  = f.getParentDirectory();
				fileDisps[i]->setText (*currentFiles[i], juce::dontSendNotification);
			}
		}
	}
	
	// Sync filter bars from processor
	filterBarA_.updateFromProcessor();
	filterBarB_.updateFromProcessor();
	filterBarC_.updateFromProcessor();

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
	juce::TextButton* browseBtns[] = { &browseButtonA, &browseButtonB, &browseButtonC };
	for (int i = 0; i < 3; ++i)
	{
		if (button == browseBtns[i])
		{
			openFileExplorer (i);
			return;
		}
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
		return;
	}

	const char* enableIds[] = { CABTRAudioProcessor::kParamEnableA,
	                            CABTRAudioProcessor::kParamEnableB,
	                            CABTRAudioProcessor::kParamEnableC };
	for (int i = 0; i < 3; ++i)
	{
		if (paramID == enableIds[i])
		{
			const int idx = i;
			juce::MessageManager::callAsync ([safeThis = juce::Component::SafePointer<CABTRAudioProcessorEditor> (this), idx] ()
			{
				if (safeThis != nullptr)
					safeThis->updateLoaderEnabledState (idx);
			});
			return;
		}
	}
}

//==============================================================================
//  File Operations
//==============================================================================
void CABTRAudioProcessorEditor::updateFileDisplayLabels (const juce::String& pathA, const juce::String& pathB, const juce::String& pathC)
{
	const juce::String* paths[]      = { &pathA,        &pathB,        &pathC };
	juce::String*       curFiles[]   = { &currentFileA, &currentFileB, &currentFileC };
	juce::File*         curFolders[] = { &currentFolderA, &currentFolderB, &currentFolderC };
	juce::Label*        displays[]   = { &fileDisplayA, &fileDisplayB, &fileDisplayC };

	for (int i = 0; i < 3; ++i)
	{
		if (paths[i]->isNotEmpty())
		{
			juce::File f (*paths[i]);
			if (f.existsAsFile())
			{
				*curFiles[i]   = f.getFileName();
				*curFolders[i] = f.getParentDirectory();
				displays[i]->setText (*curFiles[i], juce::dontSendNotification);
			}
		}
		else
		{
			*curFiles[i] = "";
			displays[i]->setText ("No file loaded", juce::dontSendNotification);
		}
	}
}

void CABTRAudioProcessorEditor::openFileExplorer (int loaderIndex)
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

	// Use standard prompt width, taller height for file list
	constexpr int browserHeight = 420;

	// Create file list model and ListBox
	auto* fileModel = new FileListModel (activeScheme);
	auto* listBox = new juce::ListBox ("", fileModel); // No label

	// Add current path label at the top
	auto& startFolder = loaderIndex == 0 ? currentFolderA : (loaderIndex == 1 ? currentFolderB : currentFolderC);
	auto* pathLabel = new juce::Label ("Path", startFolder.getFullPathName());
	pathLabel->setFont (juce::Font (juce::FontOptions (13.0f)));
	pathLabel->setColour (juce::Label::textColourId, activeScheme.text);
	pathLabel->setJustificationType (juce::Justification::centredLeft);
	pathLabel->setMinimumHorizontalScale (0.5f);

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
	auto currentFolder = startFolder;
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
				safeListBox->selectRow (0);
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
			safeListBox->selectRow (0);
			safeListBox->repaint();
		}
		if (safePathLabel != nullptr)
			safePathLabel->setText (folder.getFullPathName(), juce::dontSendNotification);
	};

	fileModel->onFileSelected = [safeThis, safeAw, loaderIndex] (const juce::File& file) mutable
	{
		if (safeThis == nullptr || safeAw == nullptr)
			return;
			
		// Only close dialog for valid audio files — actual load happens in modal callback
		if (file.existsAsFile() && file.getSize() > 0)
			safeAw->exitModalState (1);
	};

	// Style the ListBox with green border
	listBox->setColour (juce::ListBox::backgroundColourId, activeScheme.bg);
	listBox->setColour (juce::ListBox::outlineColourId, activeScheme.fg);
	listBox->setOutlineThickness (3);
	listBox->setRowHeight (24);
	listBox->setMultipleSelectionEnabled (false);

	// Build browser panel — same container pattern as export prompt
	auto* browserPanel = new juce::Component();
	constexpr int margin = TR::kPromptInnerMargin;
	constexpr int driveLabelH = 18;
	constexpr int driveComboH = 28;
	constexpr int pathLabelH = 24;
	constexpr int componentGap = 4;
	const int panelW = TR::kPromptWidth - (margin * 2);

	auto labelFont = lnf.getAlertWindowMessageFont();
	labelFont.setHeight (labelFont.getHeight() * 1.20f);
	auto boldFont = labelFont;
	boldFont.setBold (true);

	int py = 0;

	auto* drivesLabel = new juce::Label ("", "Drives");
	drivesLabel->setFont (boldFont);
	drivesLabel->setColour (juce::Label::textColourId, activeScheme.text);
	drivesLabel->setJustificationType (juce::Justification::centredLeft);
	drivesLabel->setBounds (0, py, panelW, driveLabelH);
	browserPanel->addAndMakeVisible (drivesLabel);
	py += driveLabelH;

	driveCombo->setBounds (0, py, panelW, driveComboH);
	browserPanel->addAndMakeVisible (driveCombo);
	py += driveComboH + componentGap;

	auto* pathTitleLabel = new juce::Label ("", "Path");
	pathTitleLabel->setFont (boldFont);
	pathTitleLabel->setColour (juce::Label::textColourId, activeScheme.text);
	pathTitleLabel->setJustificationType (juce::Justification::centredLeft);
	pathTitleLabel->setBounds (0, py, panelW, driveLabelH);
	browserPanel->addAndMakeVisible (pathTitleLabel);
	py += driveLabelH;

	pathLabel->setBounds (0, py, panelW, pathLabelH);
	browserPanel->addAndMakeVisible (pathLabel);
	py += pathLabelH + componentGap;

	const int listBoxStartY = py;
	listBox->setBounds (0, py, panelW, 200);
	browserPanel->addAndMakeVisible (listBox);
	browserPanel->setSize (panelW, py + 200);

	aw->addCustomComponent (browserPanel);

	// Buttons
	aw->addButton ("SELECT", 1, juce::KeyPress (juce::KeyPress::returnKey));
	aw->addButton ("CANCEL", 0, juce::KeyPress (juce::KeyPress::escapeKey));

	aw->setSize (TR::kPromptWidth, browserHeight);
	finalizePromptButtons (*aw, lnf);

	// Reposition browser panel between top and button row
	{
		const int buttonsTop = TR::getAlertButtonsTop (*aw);
		const int bodyTop    = TR::kPromptBodyTopPad;
		const int bodyBottom = buttonsTop - TR::kPromptBodyBottomPad;
		const int headerH    = listBoxStartY;  // everything above the listBox
		const int listBoxH   = juce::jmax (60, bodyBottom - bodyTop - headerH);
		const int panelX     = (TR::kPromptWidth - panelW) / 2;

		listBox->setBounds (0, listBoxStartY, panelW, listBoxH);
		browserPanel->setSize (panelW, headerH + listBoxH);
		browserPanel->setBounds (panelX, bodyTop, panelW, headerH + listBoxH);
	}

	// Embed in overlay instead of modal
	if (safeThis != nullptr)
	{
		fitAlertWindowToEditor (*aw, safeThis.getComponent(), nullptr);
		embedAlertWindowInOverlay (safeThis.getComponent(), aw);
	}

	aw->enterModalState (true, juce::ModalCallbackFunction::create ([safeThis, aw, fileModel, listBox, pathLabel, driveCombo, browserPanel, safeListBox, safePathLabel, safeDriveCombo, loaderIndex] (int result) mutable
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
				safeThis->loadIRFile (item.file.getFullPathName(), loaderIndex);
			}
		}
		
		// Update currentFolder ONLY here in modal callback (safe, synchronous)
		if (loaderIndex == 0)
			safeThis->currentFolderA = fileModel->currentFolder;
		else if (loaderIndex == 1)
			safeThis->currentFolderB = fileModel->currentFolder;
		else
			safeThis->currentFolderC = fileModel->currentFolder;

		// CRITICAL: Clear all callbacks BEFORE deleting objects to prevent use-after-free
		fileModel->onNavigateUp = nullptr;
		fileModel->onNavigateInto = nullptr;
		fileModel->onFileSelected = nullptr;
		driveCombo->onChange = nullptr;

		// Clean up — remove children from panel before deleting individually
		browserPanel->removeAllChildren();
		delete driveCombo;
		delete pathLabel;
		delete listBox;
		delete fileModel;
		delete browserPanel;

		safeThis->setPromptOverlayActive (false);
	}), true);
}

void CABTRAudioProcessorEditor::browseToParentFolder (int loaderIndex)
{
	auto& currentFolder = loaderIndex == 0 ? currentFolderA : (loaderIndex == 1 ? currentFolderB : currentFolderC);
	auto parent = currentFolder.getParentDirectory();
	
	if (parent.exists())
		currentFolder = parent;

}

void CABTRAudioProcessorEditor::loadIRFile (const juce::String& path, int loaderIndex)
{
	// Safety check: ensure file exists and is not a directory
	juce::File file (path);
	if (! file.existsAsFile() || file.isDirectory() || file.getSize() == 0)
		return;
	
	juce::String*    curFiles[]  = { &currentFileA,  &currentFileB,  &currentFileC };
	juce::Label*     displays[]  = { &fileDisplayA,  &fileDisplayB,  &fileDisplayC };
	CABTRAudioProcessor::IRLoaderState* states[] = { &audioProcessor.stateA, &audioProcessor.stateB, &audioProcessor.stateC };

	const int idx = juce::jlimit (0, 2, loaderIndex);
	*curFiles[idx] = path;
	displays[idx]->setText (file.getFileName(), juce::dontSendNotification);
	audioProcessor.loadImpulseResponse (*states[idx], path);
}

//==============================================================================
//  Mouse Events
//==============================================================================
void CABTRAudioProcessorEditor::mouseDown (const juce::MouseEvent& e)
{
	const auto p = e.getEventRelativeTo (this).getPosition();

	// Toggle IO section expand/collapse
	if (cachedToggleBarArea_.contains (p))
	{
		ioSectionExpanded_ = ! ioSectionExpanded_;
		audioProcessor.setUiIoExpanded (ioSectionExpanded_);
		resized();
		repaint();
		return;
	}

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

	// CHAOS checkboxes: left-click toggles, right-click opens chaos amount/speed prompt
	{
		juce::ToggleButton* enableBtns[]      = { &enableButtonA,  &enableButtonB,  &enableButtonC };
		juce::ToggleButton* chaosBtns[]       = { &chaosButtonA,   &chaosButtonB,   &chaosButtonC };
		juce::ToggleButton* chaosFilterBtns[] = { &chaosFilterButtonA, &chaosFilterButtonB, &chaosFilterButtonC };
		juce::Label*        chaosDisps[]      = { &chaosDisplayA,  &chaosDisplayB,  &chaosDisplayC };

		for (int i = 0; i < 3; ++i)
		{
			if (! enableBtns[i]->getToggleState())
				continue;

			// Hit-test chaos filter button or its display overlay half
			const bool hitFilter = chaosFilterBtns[i]->getBounds().contains (p)
				|| (chaosDisps[i]->isVisible() && chaosDisps[i]->getBounds().contains (p)
					&& p.x < chaosFilterBtns[i]->getBounds().getRight());

			// Hit-test chaos delay button or its display overlay half
			const bool hitDelay = !hitFilter
				&& (chaosBtns[i]->getBounds().contains (p)
					|| (chaosDisps[i]->isVisible() && chaosDisps[i]->getBounds().contains (p)
						&& p.x >= chaosBtns[i]->getBounds().getX()));

			if (hitFilter)
			{
				if (e.mods.isPopupMenu())
					openChaosPrompt (i, true);
				else
					chaosFilterBtns[i]->setToggleState (! chaosFilterBtns[i]->getToggleState(), juce::sendNotificationSync);
				return;
			}
			if (hitDelay)
			{
				if (e.mods.isPopupMenu())
					openChaosPrompt (i, false);
				else
					chaosBtns[i]->setToggleState (! chaosBtns[i]->getToggleState(), juce::sendNotificationSync);
				return;
			}
		}
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
	// Double-click on file display label → reset that loader's IR
	const auto pos = e.getEventRelativeTo (this).getPosition();
	juce::Label* displays[] = { &fileDisplayA, &fileDisplayB, &fileDisplayC };
	for (int i = 0; i < 3; ++i)
	{
		if (displays[i]->getBounds().contains (pos))
		{
			// Clear convolver and IR buffer
			CABTRAudioProcessor::IRLoaderState* states[] = {
				&audioProcessor.stateA, &audioProcessor.stateB, &audioProcessor.stateC };
			states[i]->convolution.reset();
			states[i]->impulseResponse.setSize (0, 0);
			states[i]->currentFilePath.clear();
			states[i]->needsUpdate.store (false);

			// Clear editor state
			juce::String* curFiles[] = { &currentFileA, &currentFileB, &currentFileC };
			*curFiles[i] = juce::String();
			displays[i]->setText ("No file loaded", juce::dontSendNotification);

			legendDirty = true;
			repaint();
			return;
		}
	}
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

	auto formatFreq = [] (double hz) {
		if (hz < 1000.0)
			return juce::String (hz, 1) + " Hz";
		return juce::String (hz / 1000.0, 2) + " kHz";
	};

	auto formatDb = [] (float db) -> juce::String {
		if (db <= -80.0f)
			return "-INF";
		return juce::String (db, 1) + " dB";
	};

	auto formatPan = [] (float pan01) -> juce::String {
		const int pct = juce::roundToInt ((pan01 - 0.5f) * 200.0f);
		if (pct == 0)  return "C";
		if (pct < 0)   return "L" + juce::String (-pct);
		return "R" + juce::String (pct);
	};

	// Labels and format types: 0=freq, 1=dB, 2=ms, 3=percent, 4=pan, 5=tilt(dB)
	struct ParamFmt { int type; const char* label; };
	const ParamFmt fmts[kNumCachedParams] = {
		{0,"HP"}, {0,"LP"}, {1,"IN"}, {1,"OUT"}, {5,"TILT"}, {2,"START"}, {2,"END"},
		{3,"SIZE"}, {2,"DELAY"}, {4,"PAN"}, {3,"ANGLE"}, {3,"DIST"}, {3,"RESO"}, {3,"MIX"}
	};

	for (int loader = 0; loader < 3; ++loader)
	{
		auto refs = getLoaderRefs (loader);
		juce::Slider* loaderSliders[kNumCachedParams] = {
			&refs.hp, &refs.lp, &refs.in, &refs.out, &refs.tilt,
			&refs.start, &refs.end, &refs.size, &refs.delay,
			&refs.pan, &refs.fred, &refs.pos, &refs.reso, &refs.mix
		};

		for (int p = 0; p < kNumCachedParams; ++p)
		{
			auto& ct = cachedTexts[loader][p];
			const double val = loaderSliders[p]->getValue();
			const auto& fmt = fmts[p];

			switch (fmt.type)
			{
				case 0: // Frequency
					ct.full    = formatFreq (val) + " " + fmt.label;
					ct.short_  = formatFreq (val);
					ct.intOnly = juce::String (juce::roundToInt (val));
					break;
				case 1: // dB
					ct.full    = formatDb ((float) val) + " " + fmt.label;
					ct.short_  = formatDb ((float) val);
					ct.intOnly = juce::String (juce::roundToInt (val));
					break;
				case 2: // ms
					ct.full    = juce::String (juce::roundToInt (val)) + " ms " + fmt.label;
					ct.short_  = juce::String (juce::roundToInt (val)) + " ms";
					ct.intOnly = juce::String (juce::roundToInt (val));
					break;
				case 3: // Percent (value is 0..1 range → display as %)
				{
					const int pct = juce::roundToInt (val * 100.0);
					ct.full    = juce::String (pct) + "% " + fmt.label;
					ct.short_  = juce::String (pct) + "%";
					ct.intOnly = juce::String (pct);
					break;
				}
				case 4: // Pan
					ct.full    = formatPan ((float) val) + " " + fmt.label;
					ct.short_  = formatPan ((float) val);
					ct.intOnly = formatPan ((float) val);
					break;
				case 5: // dB (Tilt)
					ct.full    = juce::String (val, 1) + " dB " + fmt.label;
					ct.short_  = juce::String (val, 1) + " dB";
					ct.intOnly = juce::String (juce::roundToInt (val));
					break;
			}
		}
	}

	return false;
}

juce::Rectangle<int> CABTRAudioProcessorEditor::getValueAreaFor (const juce::Rectangle<int>& barBounds,
                                                                  int columnRight) const
{
	constexpr int valuePadPx    = 8;
	constexpr int valueHeightPx = 24;
	constexpr int rightMarginPx = 6;

	const int valueX = barBounds.getRight() + valuePadPx;
	const int maxW   = juce::jmax (0, columnRight - valueX - rightMarginPx);
	const int y      = barBounds.getCentreY() - (valueHeightPx / 2);

	return { valueX, y, maxW, valueHeightPx };
}

juce::Slider* CABTRAudioProcessorEditor::getSliderForValueAreaPoint (juce::Point<int> p)
{
	for (int i = 0; i < 3; ++i)
	{
		auto r = getLoaderRefs (i);
		const int colR = columnRight_[i];

		BarSlider* sliders[] = { &r.hp, &r.lp, &r.in, &r.out, &r.tilt,
		                         &r.start, &r.end, &r.size, &r.delay,
		                         &r.pan, &r.fred, &r.pos, &r.reso };

		for (auto* s : sliders)
			if (getValueAreaFor (s->getBounds(), colR).contains (p))
				return s;

		if (r.mix.isVisible() && getValueAreaFor (r.mix.getBounds(), colR).contains (p))
			return &r.mix;
	}

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
		const std::array<juce::Component*, 9> globalControls {
			&enableButtonA, &enableButtonB, &enableButtonC, &alignButton,
			&browseButtonA, &browseButtonB, &browseButtonC,
			&routeCombo,
			&invButtonA
		};
		for (auto* c : globalControls)
			c->setEnabled (false);

		// Disable loader subsection controls
		updateLoaderEnabledState (0);
		updateLoaderEnabledState (1);
		updateLoaderEnabledState (2);
	}
	else
	{
		// Re-enable global controls
		const std::array<juce::Component*, 9> globalControls {
			&enableButtonA, &enableButtonB, &enableButtonC, &alignButton,
			&browseButtonA, &browseButtonB, &browseButtonC,
			&routeCombo,
			&invButtonA
		};
		for (auto* c : globalControls)
			c->setEnabled (true);

		// Re-apply loader enabled state (respects per-loader enable toggle)
		updateLoaderEnabledState (0);
		updateLoaderEnabledState (1);
		updateLoaderEnabledState (2);
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

	// ── Suffix determination via slider type ──
	juce::String suffix;
	juce::String suffixShort;
	auto* bar = dynamic_cast<BarSlider*> (&s);
	const auto stype = bar ? bar->getType() : BarSlider::Type::Unknown;

	const bool isHpLp  = (stype == BarSlider::Type::HpFreq || stype == BarSlider::Type::LpFreq);
	const bool isIn    = (stype == BarSlider::Type::Input);
	const bool isOut   = (stype == BarSlider::Type::Output || stype == BarSlider::Type::GlobalOutput);
	const bool isTilt  = (stype == BarSlider::Type::Tilt);
	const bool isStart = (stype == BarSlider::Type::Start);
	const bool isEnd   = (stype == BarSlider::Type::End);
	const bool isSize  = (stype == BarSlider::Type::Size);
	const bool isDelay = (stype == BarSlider::Type::Delay);
	const bool isPan   = (stype == BarSlider::Type::Pan);
	const bool isFred  = (stype == BarSlider::Type::Fred);
	const bool isPos   = (stype == BarSlider::Type::Pos);
	const bool isReso  = (stype == BarSlider::Type::Reso);
	const bool isMix   = (stype == BarSlider::Type::Mix || stype == BarSlider::Type::GlobalMix);

	if (isHpLp)             { suffix = " Hz";          suffixShort = " Hz"; }
	else if (isIn)          { suffix = " dB INPUT";    suffixShort = " dB"; }
	else if (isOut)         { suffix = " dB OUTPUT";   suffixShort = " dB"; }
	else if (isTilt)        { suffix = " dB TILT"; suffixShort = " dB"; }
	else if (isStart)       { suffix = " ms START";    suffixShort = " ms"; }
	else if (isEnd)         { suffix = " ms END";      suffixShort = " ms"; }
	else if (isSize)        { suffix = " % SIZE";      suffixShort = " %"; }
	else if (isDelay)       { suffix = " ms DELAY";    suffixShort = " ms"; }
	else if (isPan)         { suffix = " % PAN";       suffixShort = " %"; }
	else if (isFred)        { suffix = " % ANGLE";    suffixShort = " %"; }
	else if (isPos)         { suffix = " % DIST";     suffixShort = " %"; }
	else if (isReso)        { suffix = " % RESO";     suffixShort = " %"; }
	else if (isMix)         { suffix = " % MIX";       suffixShort = " %"; }

	const juce::String suffixText      = suffix.trimStart();
	const juce::String suffixTextShort = suffixShort.trimStart();

	auto* aw = new juce::AlertWindow ("", "", juce::AlertWindow::NoIcon);
	aw->setLookAndFeel (&lnf);

	// ── Initial display value ──
	juce::String currentDisplay;
	if (isHpLp)
		currentDisplay = juce::String (s.getValue(), 3);
	else if (isIn || isOut)
		currentDisplay = juce::String (s.getValue(), 1);
	else if (isTilt)
		currentDisplay = juce::String (s.getValue(), 2);
	else if (isStart || isEnd || isDelay)
		currentDisplay = juce::String (s.getValue(), 3);
	else if (isSize)
		currentDisplay = juce::String (juce::jlimit (25.0, 400.0, s.getValue() * 100.0), 1);
	else if (isPan)
		currentDisplay = juce::String (juce::jlimit (0.0, 100.0, s.getValue() * 100.0), 0);
	else if (isReso)
		currentDisplay = juce::String (juce::jlimit (0.0, 200.0, s.getValue() * 100.0), 0);
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
		else if (isIn)           worstCaseText = "-100.0";
		else if (isOut)          worstCaseText = "-100.0";
		else if (isTilt)         worstCaseText = "-6.00";
		else if (isStart||isEnd) worstCaseText = "10000.000";
		else if (isSize)         worstCaseText = "400.0";
		else if (isDelay)        worstCaseText = "1000.000";
		else if (isPan)          worstCaseText = "100";
		else if (isFred||isPos)  worstCaseText = "100.0";
		else if (isReso)         worstCaseText = "200";
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
			maxDecs = 0;     maxLen = 5;     // "20000"
		}
		else if (isIn)
		{
			minVal = -100.0; maxVal = 0.0;
			maxDecs = 1;     maxLen = 6;     // "-100.0"
		}
		else if (isOut)
		{
			minVal = -100.0; maxVal = 24.0;
			maxDecs = 1;     maxLen = 6;     // "-100.0"
		}
		else if (isTilt)
		{
			minVal = -6.0;   maxVal = 6.0;
			maxDecs = 2;     maxLen = 5;     // "-6.00"
		}
		else if (isStart || isEnd)
		{
			minVal = 0.0;    maxVal = 10000.0;
			maxDecs = 3;     maxLen = 9;     // "10000.000"
		}
		else if (isSize)
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
		else if (isReso)
		{
			minVal = 0.0;    maxVal = 200.0;
			maxDecs = 0;     maxLen = 3;     // "200"
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
	aw->setEscapeKeyCancels (true);
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

			// Percent-based sliders: user typed 0-100/200, slider stores 0-1/2
			auto* barPtr = dynamic_cast<BarSlider*> (sliderPtr);
			const auto st = barPtr ? barPtr->getType() : BarSlider::Type::Unknown;
			const bool needsPercentConvert = (st == BarSlider::Type::Size || st == BarSlider::Type::Pan ||
			                                  st == BarSlider::Type::Fred  || st == BarSlider::Type::Pos  ||
			                                  st == BarSlider::Type::Reso  || st == BarSlider::Type::Mix  ||
			                                  st == BarSlider::Type::GlobalMix);

			if (needsPercentConvert)
				v *= 0.01;

			const auto range = sliderPtr->getRange();
			double clamped = juce::jlimit (range.getStart(), range.getEnd(), v);

			sliderPtr->setValue (clamped, juce::sendNotificationSync);
		}));
}

//==============================================================================
//  FILTER prompt (HP + LP frequencies, on/off, slope)
//==============================================================================
void CABTRAudioProcessorEditor::openFilterPrompt (int loaderIndex)
{
	using namespace TR;
	lnf.setScheme (activeScheme);
	const auto scheme = activeScheme;

	auto& proc = audioProcessor;
	auto& vts = proc.getValueTreeState();

	auto pickId = [&] (const char* a, const char* b, const char* c) -> const char* {
		return loaderIndex == 0 ? a : (loaderIndex == 1 ? b : c);
	};
	const char* hpFreqId  = pickId (CABTRAudioProcessor::kParamHpFreqA,  CABTRAudioProcessor::kParamHpFreqB,  CABTRAudioProcessor::kParamHpFreqC);
	const char* lpFreqId  = pickId (CABTRAudioProcessor::kParamLpFreqA,  CABTRAudioProcessor::kParamLpFreqB,  CABTRAudioProcessor::kParamLpFreqC);
	const char* hpOnId    = pickId (CABTRAudioProcessor::kParamHpOnA,    CABTRAudioProcessor::kParamHpOnB,    CABTRAudioProcessor::kParamHpOnC);
	const char* lpOnId    = pickId (CABTRAudioProcessor::kParamLpOnA,    CABTRAudioProcessor::kParamLpOnB,    CABTRAudioProcessor::kParamLpOnC);
	const char* hpSlopeId = pickId (CABTRAudioProcessor::kParamHpSlopeA, CABTRAudioProcessor::kParamHpSlopeB, CABTRAudioProcessor::kParamHpSlopeC);
	const char* lpSlopeId = pickId (CABTRAudioProcessor::kParamLpSlopeA, CABTRAudioProcessor::kParamLpSlopeB, CABTRAudioProcessor::kParamLpSlopeC);

	const float hpFreq  = vts.getRawParameterValue (hpFreqId)->load();
	const float lpFreq  = vts.getRawParameterValue (lpFreqId)->load();
	const bool  hpOn    = vts.getRawParameterValue (hpOnId)->load() > 0.5f;
	const bool  lpOn    = vts.getRawParameterValue (lpOnId)->load() > 0.5f;
	const int   hpSlope = juce::jlimit (0, 2, (int) vts.getRawParameterValue (hpSlopeId)->load());
	const int   lpSlope = juce::jlimit (0, 2, (int) vts.getRawParameterValue (lpSlopeId)->load());

	auto* aw = new juce::AlertWindow ("", "", juce::AlertWindow::NoIcon);
	aw->setLookAndFeel (&lnf);

	// ── Inline PromptBar for freq dragging ──
	struct PromptBar : public juce::Component
	{
		TRScheme colours;
		float value01    = 0.5f;
		float default01  = 0.5f;
		std::function<void (float)> onValueChanged;

		PromptBar (const TRScheme& s, float initial01, float def01)
			: colours (s), value01 (initial01), default01 (def01) {}

		void paint (juce::Graphics& g) override
		{
			const auto r = getLocalBounds().toFloat();
			g.setColour (colours.outline);
			g.drawRect (r, 4.0f);
			const float pad = 7.0f;
			auto inner = r.reduced (pad);
			g.setColour (colours.bg);
			g.fillRect (inner);
			const float fillW = juce::jlimit (0.0f, inner.getWidth(), inner.getWidth() * value01);
			g.setColour (colours.fg);
			g.fillRect (inner.withWidth (fillW));
		}

		void mouseDown (const juce::MouseEvent& e) override { updateFromMouse (e); }
		void mouseDrag (const juce::MouseEvent& e) override { updateFromMouse (e); }
		void mouseDoubleClick (const juce::MouseEvent&) override { setValue (default01); }

		void setValue (float v)
		{
			value01 = juce::jlimit (0.0f, 1.0f, v);
			repaint();
			if (onValueChanged)
				onValueChanged (value01);
		}

	private:
		void updateFromMouse (const juce::MouseEvent& e)
		{
			const float pad = 7.0f;
			const float innerW = (float) getWidth() - pad * 2.0f;
			setValue (innerW > 0.0f ? ((float) e.x - pad) / innerW : 0.0f);
		}
	};

	// Freq normalisation helpers (log scale 20..20000)
	auto freqToNorm = [] (float freq) -> float
	{
		constexpr float minF = 20.0f, maxF = 20000.0f;
		return std::log2 (juce::jlimit (minF, maxF, freq) / minF) / std::log2 (maxF / minF);
	};
	auto normToFreq = [] (float n) -> float
	{
		constexpr float minF = 20.0f, maxF = 20000.0f;
		return minF * std::pow (2.0f, juce::jlimit (0.0f, 1.0f, n) * std::log2 (maxF / minF));
	};

	// HP section
	aw->addTextEditor ("hpFreq", juce::String (juce::roundToInt (hpFreq)), juce::String());
	auto* hpBar = new PromptBar (scheme, freqToNorm (hpFreq), freqToNorm (CABTRAudioProcessor::kFilterHpFreqDefault));
	aw->addAndMakeVisible (hpBar);

	// LP section
	aw->addTextEditor ("lpFreq", juce::String (juce::roundToInt (lpFreq)), juce::String());
	auto* lpBar = new PromptBar (scheme, freqToNorm (lpFreq), freqToNorm (CABTRAudioProcessor::kFilterLpFreqDefault));
	aw->addAndMakeVisible (lpBar);

	// HP on/off toggle
	auto* hpToggle = new juce::ToggleButton ("");
	hpToggle->setToggleState (hpOn, juce::dontSendNotification);
	hpToggle->setLookAndFeel (&lnf);
	aw->addAndMakeVisible (hpToggle);

	// LP on/off toggle
	auto* lpToggle = new juce::ToggleButton ("");
	lpToggle->setToggleState (lpOn, juce::dontSendNotification);
	lpToggle->setLookAndFeel (&lnf);
	aw->addAndMakeVisible (lpToggle);

	// ── Clickable slope labels (cycle 6→12→24→6 on click) ──
	auto slopeToText = [] (int s) -> juce::String
	{
		if (s == 0) return "6dB";
		if (s == 1) return "12dB";
		return "24dB";
	};

	const juce::Font slopeFont (juce::FontOptions (34.0f).withStyle ("Bold"));

	auto* hpSlopeLabel = new juce::Label ("", slopeToText (hpSlope));
	hpSlopeLabel->setJustificationType (juce::Justification::centredRight);
	hpSlopeLabel->setColour (juce::Label::textColourId, scheme.text);
	hpSlopeLabel->setBorderSize (juce::BorderSize<int> (0));
	hpSlopeLabel->setFont (slopeFont);
	aw->addAndMakeVisible (hpSlopeLabel);

	auto* lpSlopeLabel = new juce::Label ("", slopeToText (lpSlope));
	lpSlopeLabel->setJustificationType (juce::Justification::centredRight);
	lpSlopeLabel->setColour (juce::Label::textColourId, scheme.text);
	lpSlopeLabel->setBorderSize (juce::BorderSize<int> (0));
	lpSlopeLabel->setFont (slopeFont);
	aw->addAndMakeVisible (lpSlopeLabel);

	// Shared state
	auto hpSlopeVal = std::make_shared<int> (hpSlope);
	auto lpSlopeVal = std::make_shared<int> (lpSlope);
	auto syncing    = std::make_shared<bool> (false);
	auto layoutFn   = std::make_shared<std::function<void()>> ([] {});

	juce::Component::SafePointer<CABTRAudioProcessorEditor> safeThis (this);

	// Capture param IDs for lambda usage
	const juce::String hpFreqIdStr (hpFreqId);
	const juce::String lpFreqIdStr (lpFreqId);
	const juce::String hpOnIdStr (hpOnId);
	const juce::String lpOnIdStr (lpOnId);
	const juce::String hpSlopeIdStr (hpSlopeId);
	const juce::String lpSlopeIdStr (lpSlopeId);

	auto pushParams = [safeThis, hpToggle, lpToggle, hpSlopeVal, lpSlopeVal, normToFreq, aw,
	                    hpFreqIdStr, lpFreqIdStr, hpOnIdStr, lpOnIdStr,
	                    hpSlopeIdStr, lpSlopeIdStr, loaderIndex] ()
	{
		if (safeThis == nullptr) return;
		auto& p = safeThis->audioProcessor;
		auto& vts = p.getValueTreeState();
		auto setP = [&vts] (const juce::String& id, float plain)
		{
			if (auto* param = vts.getParameter (id))
				param->setValueNotifyingHost (param->convertTo0to1 (plain));
		};

		auto* hpTe = aw->getTextEditor ("hpFreq");
		auto* lpTe = aw->getTextEditor ("lpFreq");
		float hpF = hpTe ? juce::jlimit (20.0f, 20000.0f, (float) hpTe->getText().getIntValue()) : 20.0f;
		float lpF = lpTe ? juce::jlimit (20.0f, 20000.0f, (float) lpTe->getText().getIntValue()) : 20000.0f;
		if (hpF > lpF) { const float mid = (hpF + lpF) * 0.5f; hpF = mid; lpF = mid; }
		if (hpTe) setP (hpFreqIdStr, hpF);
		if (lpTe) setP (lpFreqIdStr, lpF);
		setP (hpSlopeIdStr, (float) *hpSlopeVal);
		setP (lpSlopeIdStr, (float) *lpSlopeVal);

		if (auto* hpOnParam = vts.getParameter (hpOnIdStr))
			hpOnParam->setValueNotifyingHost (hpToggle->getToggleState() ? 1.0f : 0.0f);
		if (auto* lpOnParam = vts.getParameter (lpOnIdStr))
			lpOnParam->setValueNotifyingHost (lpToggle->getToggleState() ? 1.0f : 0.0f);

		auto& fb = loaderIndex == 0 ? safeThis->filterBarA_ : (loaderIndex == 1 ? safeThis->filterBarB_ : safeThis->filterBarC_);
		fb.updateFromProcessor();
	};

	// Slope label click → cycle value and push
	struct SlopeCycler : public juce::MouseListener
	{
		std::shared_ptr<int> val;
		juce::Label* label;
		std::function<juce::String (int)> toText;
		std::function<void()> push;
		std::shared_ptr<std::function<void()>> layout;
		void mouseDown (const juce::MouseEvent&) override
		{
			*val = (*val + 1) % 3;
			label->setText (toText (*val), juce::dontSendNotification);
			push();
			if (layout && *layout) (*layout)();
		}
	};

	hpSlopeLabel->setInterceptsMouseClicks (true, false);
	auto* hpCycler = new SlopeCycler();
	hpCycler->val = hpSlopeVal;
	hpCycler->label = hpSlopeLabel;
	hpCycler->toText = slopeToText;
	hpCycler->push = pushParams;
	hpCycler->layout = layoutFn;
	hpSlopeLabel->addMouseListener (hpCycler, false);

	lpSlopeLabel->setInterceptsMouseClicks (true, false);
	auto* lpCycler = new SlopeCycler();
	lpCycler->val = lpSlopeVal;
	lpCycler->label = lpSlopeLabel;
	lpCycler->toText = slopeToText;
	lpCycler->push = pushParams;
	lpCycler->layout = layoutFn;
	lpSlopeLabel->addMouseListener (lpCycler, false);

	auto hpCyclerGuard = std::shared_ptr<SlopeCycler> (hpCycler);
	auto lpCyclerGuard = std::shared_ptr<SlopeCycler> (lpCycler);

	// Wire toggle real-time
	hpToggle->onClick = pushParams;
	lpToggle->onClick = pushParams;

	// Wire bar ↔ text sync
	auto barToText = [aw, syncing, normToFreq, freqToNorm, pushParams, hpBar, lpBar] (const char* editorId, float v01, bool isHp)
	{
		if (*syncing) return;
		*syncing = true;
		if (isHp)
			v01 = juce::jmin (v01, lpBar->value01);
		else
			v01 = juce::jmax (v01, hpBar->value01);

		if (isHp) { hpBar->value01 = v01; hpBar->repaint(); }
		else      { lpBar->value01 = v01; lpBar->repaint(); }

		if (auto* te = aw->getTextEditor (editorId))
		{
			te->setText (juce::String (juce::roundToInt (normToFreq (v01))), juce::sendNotification);
			te->selectAll();
		}
		*syncing = false;
		pushParams();
	};

	hpBar->onValueChanged = [barToText] (float v) { barToText ("hpFreq", v, true); };
	lpBar->onValueChanged = [barToText] (float v) { barToText ("lpFreq", v, false); };

	auto textToBar = [syncing, freqToNorm, normToFreq, pushParams, aw, hpBar, lpBar] (juce::TextEditor* te, PromptBar* bar, bool isHp)
	{
		if (*syncing || te == nullptr || bar == nullptr) return;
		*syncing = true;
		float freq = juce::jlimit (20.0f, 20000.0f, (float) te->getText().getIntValue());
		auto* otherTe = aw->getTextEditor (isHp ? "lpFreq" : "hpFreq");
		const float otherFreq = otherTe ? juce::jlimit (20.0f, 20000.0f, (float) otherTe->getText().getIntValue()) : (isHp ? 20000.0f : 20.0f);
		if (isHp) freq = juce::jmin (freq, otherFreq);
		else      freq = juce::jmax (freq, otherFreq);
		te->setText (juce::String (juce::roundToInt (freq)), juce::dontSendNotification);
		bar->value01 = freqToNorm (freq);
		bar->repaint();
		*syncing = false;
		pushParams();
	};

	auto* hpTe = aw->getTextEditor ("hpFreq");
	auto* lpTe = aw->getTextEditor ("lpFreq");

	if (hpTe != nullptr)
		hpTe->onTextChange = [syncing, textToBar, hpTe, hpBar, layoutFn] () { textToBar (hpTe, hpBar, true); if (*layoutFn) (*layoutFn)(); };
	if (lpTe != nullptr)
		lpTe->onTextChange = [syncing, textToBar, lpTe, lpBar, layoutFn] () { textToBar (lpTe, lpBar, false); if (*layoutFn) (*layoutFn)(); };

	// Buttons
	aw->addButton ("OK",     1, juce::KeyPress (juce::KeyPress::returnKey));
	aw->addButton ("CANCEL", 0, juce::KeyPress (juce::KeyPress::escapeKey));
	aw->setEscapeKeyCancels (true);

	applyPromptShellSize (*aw);
	layoutAlertWindowButtons (*aw);

	const int margin     = kPromptInnerMargin;
	const int toggleSide = 26;
	const juce::Font promptFont (juce::FontOptions (34.0f).withStyle ("Bold"));

	// Labels: HP name, LP name, Hz labels
	auto* hpNameLabel = new juce::Label ("", "HP");
	hpNameLabel->setJustificationType (juce::Justification::centredLeft);
	hpNameLabel->setColour (juce::Label::textColourId, scheme.text);
	hpNameLabel->setBorderSize (juce::BorderSize<int> (0));
	hpNameLabel->setFont (promptFont);
	aw->addAndMakeVisible (hpNameLabel);

	auto* lpNameLabel = new juce::Label ("", "LP");
	lpNameLabel->setJustificationType (juce::Justification::centredLeft);
	lpNameLabel->setColour (juce::Label::textColourId, scheme.text);
	lpNameLabel->setBorderSize (juce::BorderSize<int> (0));
	lpNameLabel->setFont (promptFont);
	aw->addAndMakeVisible (lpNameLabel);

	auto* hpHzLabel = new juce::Label ("", "Hz");
	hpHzLabel->setJustificationType (juce::Justification::centredLeft);
	hpHzLabel->setColour (juce::Label::textColourId, scheme.text);
	hpHzLabel->setBorderSize (juce::BorderSize<int> (0));
	hpHzLabel->setFont (promptFont);
	aw->addAndMakeVisible (hpHzLabel);

	auto* lpHzLabel = new juce::Label ("", "Hz");
	lpHzLabel->setJustificationType (juce::Justification::centredLeft);
	lpHzLabel->setColour (juce::Label::textColourId, scheme.text);
	lpHzLabel->setBorderSize (juce::BorderSize<int> (0));
	lpHzLabel->setFont (promptFont);
	aw->addAndMakeVisible (lpHzLabel);

	preparePromptTextEditor (*aw, "hpFreq", scheme.bg, scheme.text, scheme.fg, promptFont, false);
	preparePromptTextEditor (*aw, "lpFreq", scheme.bg, scheme.text, scheme.fg, promptFont, false);

	// Toggle forwarder: clicking HP/LP label toggles checkboxes
	struct ToggleForwarder : public juce::MouseListener
	{
		juce::ToggleButton* toggle = nullptr;
		void mouseDown (const juce::MouseEvent&) override
		{
			if (toggle != nullptr)
				toggle->setToggleState (! toggle->getToggleState(), juce::sendNotification);
		}
	};
	hpNameLabel->setInterceptsMouseClicks (true, false);
	auto* hpFwd = new ToggleForwarder();
	hpFwd->toggle = hpToggle;
	hpNameLabel->addMouseListener (hpFwd, false);

	lpNameLabel->setInterceptsMouseClicks (true, false);
	auto* lpFwd = new ToggleForwarder();
	lpFwd->toggle = lpToggle;
	lpNameLabel->addMouseListener (lpFwd, false);

	auto hpFwdGuard = std::shared_ptr<ToggleForwarder> (hpFwd);
	auto lpFwdGuard = std::shared_ptr<ToggleForwarder> (lpFwd);

	// ── Layout function (with slope labels) ──
	auto layoutRows = [aw, hpToggle, lpToggle,
	                    hpNameLabel, lpNameLabel, hpHzLabel, lpHzLabel,
	                    hpSlopeLabel, lpSlopeLabel,
	                    hpBar, lpBar, promptFont, slopeFont, toggleSide, margin] ()
	{
		auto* hpTe = aw->getTextEditor ("hpFreq");
		auto* lpTe = aw->getTextEditor ("lpFreq");
		if (hpTe == nullptr || lpTe == nullptr) return;

		const int buttonsTop = getAlertButtonsTop (*aw);
		const int rowH       = hpTe->getHeight();
		const int barH       = juce::jmax (10, rowH / 2);
		const int barGap     = juce::jmax (2, rowH / 6);
		const int gap        = juce::jmax (4, rowH / 3);
		const int rowTotal   = rowH + barGap + barH;
		const int totalH     = rowTotal * 2 + gap;
		const int startY     = juce::jmax (kPromptEditorMinTopPx, (buttonsTop - totalH) / 2);

		const int barX = margin;
		const int barR = aw->getWidth() - margin;

		constexpr int toggleVisualInsetLeft = 2;
		constexpr int tglGap = 4;
		const int toggleVisualSide = juce::jlimit (14,
		                                           juce::jmax (14, toggleSide - 2),
		                                           (int) std::lround ((double) toggleSide * 0.65));
		const int labelOffset = toggleVisualInsetLeft + toggleVisualSide + tglGap;

		const int nameW  = stringWidth (slopeFont, "LP") + 2;
		const int slopeW = stringWidth (slopeFont, "24dB") + 4;
		const int hzGap  = 2;
		const int hzW    = stringWidth (promptFont, "Hz") + 2;

		auto placeRow = [&] (juce::ToggleButton* toggle, juce::Label* nameLabel,
		                     juce::TextEditor* te, juce::Label* hzLabel,
		                     juce::Label* slopeLabel, PromptBar* bar, int y)
		{
			nameLabel->setFont (slopeFont);
			hzLabel->setFont (promptFont);
			slopeLabel->setFont (slopeFont);

			toggle->setBounds (barX, y + (rowH - toggleSide) / 2, toggleSide, toggleSide);
			const int nameX = barX + labelOffset;
			nameLabel->setBounds (nameX, y, nameW, rowH);

			const int slopeX = barR - slopeW;
			slopeLabel->setBounds (slopeX, y, slopeW, rowH);

			const int midL = nameX + nameW;
			const int midR = slopeX;
			const int midW = midR - midL;

			const auto txt   = te->getText();
			const int textW  = juce::jmax (1, stringWidth (promptFont, txt));
			constexpr int kEditorPad = 6;
			const int editorW = textW + kEditorPad * 2;
			const int groupW  = editorW + hzGap + hzW;
			const int groupX  = midL + juce::jmax (0, (midW - groupW) / 2);

			te->setBounds (groupX, y, editorW, rowH);
			hzLabel->setBounds (groupX + editorW + hzGap, y, hzW, rowH);

			const int barW = juce::jmax (60, barR - barX);
			bar->setBounds (barX, y + rowH + barGap, barW, barH);
		};

		placeRow (hpToggle, hpNameLabel, hpTe, hpHzLabel, hpSlopeLabel, hpBar, startY);
		placeRow (lpToggle, lpNameLabel, lpTe, lpHzLabel, lpSlopeLabel, lpBar, startY + rowTotal + gap);
	};

	layoutRows();
	*layoutFn = layoutRows;

	preparePromptTextEditor (*aw, "hpFreq", scheme.bg, scheme.text, scheme.fg, promptFont, false);
	preparePromptTextEditor (*aw, "lpFreq", scheme.bg, scheme.text, scheme.fg, promptFont, false);
	layoutRows();

	styleAlertButtons (*aw, lnf);

	// Original values for CANCEL restore
	const float origHpFreq  = hpFreq;
	const float origLpFreq  = lpFreq;
	const bool  origHpOn    = hpOn;
	const bool  origLpOn    = lpOn;
	const int   origHpSlope = hpSlope;
	const int   origLpSlope = lpSlope;

	setPromptOverlayActive (true);

	fitAlertWindowToEditor (*aw, safeThis.getComponent(), [layoutRows] (juce::AlertWindow& a)
	{
		layoutAlertWindowButtons (a);
		layoutRows();
	});

	embedAlertWindowInOverlay (safeThis.getComponent(), aw);

	aw->enterModalState (true,
		juce::ModalCallbackFunction::create (
			[safeThis, aw, origHpFreq, origLpFreq, origHpOn, origLpOn,
			 origHpSlope, origLpSlope,
			 hpCyclerGuard, lpCyclerGuard, hpFwdGuard, lpFwdGuard, loaderIndex,
			 hpFreqIdStr, lpFreqIdStr, hpOnIdStr, lpOnIdStr,
			 hpSlopeIdStr, lpSlopeIdStr] (int result)
		{
			std::unique_ptr<juce::AlertWindow> killer (aw);

			if (safeThis == nullptr)
				return;

			if (result != 1)
			{
				// CANCEL — restore original values
				auto& vts = safeThis->audioProcessor.getValueTreeState();
				auto setP = [&vts] (const juce::String& id, float plain)
				{
					if (auto* param = vts.getParameter (id))
						param->setValueNotifyingHost (param->convertTo0to1 (plain));
				};
				setP (hpFreqIdStr, origHpFreq);
				setP (lpFreqIdStr, origLpFreq);
				setP (hpSlopeIdStr, (float) origHpSlope);
				setP (lpSlopeIdStr, (float) origLpSlope);
				if (auto* hpOnParam = vts.getParameter (hpOnIdStr))
					hpOnParam->setValueNotifyingHost (origHpOn ? 1.0f : 0.0f);
				if (auto* lpOnParam = vts.getParameter (lpOnIdStr))
					lpOnParam->setValueNotifyingHost (origLpOn ? 1.0f : 0.0f);

				auto& fb = loaderIndex == 0 ? safeThis->filterBarA_ : (loaderIndex == 1 ? safeThis->filterBarB_ : safeThis->filterBarC_);
				fb.updateFromProcessor();
			}

			safeThis->setPromptOverlayActive (false);
		}),
		false);
}

//==============================================================================
//  CHAOS prompt (AMOUNT + SPEED)
//==============================================================================
void CABTRAudioProcessorEditor::openChaosPrompt (int loaderIndex, bool isFilter)
{
	using namespace TR;
	lnf.setScheme (activeScheme);
	const auto scheme = activeScheme;

	const auto& amtId = isFilter
	    ? (loaderIndex == 0 ? CABTRAudioProcessor::kParamChaosAmtFilterA
	       : (loaderIndex == 1 ? CABTRAudioProcessor::kParamChaosAmtFilterB
	                           : CABTRAudioProcessor::kParamChaosAmtFilterC))
	    : (loaderIndex == 0 ? CABTRAudioProcessor::kParamChaosAmtA
	       : (loaderIndex == 1 ? CABTRAudioProcessor::kParamChaosAmtB
	                           : CABTRAudioProcessor::kParamChaosAmtC));
	const auto& spdId = isFilter
	    ? (loaderIndex == 0 ? CABTRAudioProcessor::kParamChaosSpdFilterA
	       : (loaderIndex == 1 ? CABTRAudioProcessor::kParamChaosSpdFilterB
	                           : CABTRAudioProcessor::kParamChaosSpdFilterC))
	    : (loaderIndex == 0 ? CABTRAudioProcessor::kParamChaosSpdA
	       : (loaderIndex == 1 ? CABTRAudioProcessor::kParamChaosSpdB
	                           : CABTRAudioProcessor::kParamChaosSpdC));

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
	aw->setEscapeKeyCancels (true);
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
			 amtId, spdId, loaderIndex, spdLogMin, spdLogRange] (int result) mutable
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
			auto& disp = loaderIndex == 0 ? safeThis->chaosDisplayA : (loaderIndex == 1 ? safeThis->chaosDisplayB : safeThis->chaosDisplayC);
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
	auto* lengthLabel = new juce::Label ("length", "10.000s");
	lengthLabel->setFont (labelFont);
	lengthLabel->setColour (juce::Label::textColourId, activeScheme.text);
	lengthLabel->setColour (juce::Label::backgroundColourId, juce::Colours::transparentBlack);
	lengthLabel->setColour (juce::Label::outlineColourId, juce::Colours::transparentBlack);
	lengthLabel->setJustificationType (juce::Justification::centred);
	lengthLabel->setEditable (true, true, false);
	lengthLabel->setBounds (controlX, fy, controlW, rowH);
	formPanel->addAndMakeVisible (lengthLabel);

	lengthLabel->onEditorShow = [lengthLabel, this, labelFont]()
	{
		if (auto* ed = lengthLabel->getCurrentTextEditor())
		{
			ed->setInputFilter (new NumericInputFilter (0.01, 10.0, 7, 3), true);
			ed->setJustification (juce::Justification::centred);
			ed->setFont (labelFont);
			ed->setBorder (juce::BorderSize<int> (0));
			ed->setIndents (0, 0);
			ed->setColour (juce::TextEditor::outlineColourId,        juce::Colours::transparentBlack);
			ed->setColour (juce::TextEditor::focusedOutlineColourId, juce::Colours::transparentBlack);
			ed->setColour (juce::TextEditor::backgroundColourId,     juce::Colours::transparentBlack);
			auto numText = lengthLabel->getText().trimCharactersAtEnd ("s");
			ed->setText (numText, false);
			ed->selectAll();
		}
	};

	lengthLabel->onTextChange = [lengthLabel]()
	{
		auto raw = lengthLabel->getText().trimCharactersAtEnd ("s ");
		double val = juce::jlimit (0.01, 10.0, raw.getDoubleValue());
		int decimals = 1;
		auto dotIdx = raw.indexOfChar ('.');
		if (dotIdx >= 0)
			decimals = juce::jlimit (1, 3, raw.length() - dotIdx - 1);
		lengthLabel->setText (juce::String (val, decimals) + "s", juce::dontSendNotification);
	};
	fy += rowH + gap;

	// TRIM SILENCE
	auto* trimToggle = new juce::ToggleButton ("TRIM SILENCE");
	trimToggle->setToggleState (true, juce::dontSendNotification);
	trimToggle->setLookAndFeel (&lnf);
	trimToggle->setColour (juce::ToggleButton::textColourId, activeScheme.text);
	trimToggle->setColour (juce::ToggleButton::tickColourId, activeScheme.text);
	{
		const int toggleW = juce::jmin (controlW, 200);
		const int toggleX = (formW - toggleW) / 2;
		trimToggle->setBounds (toggleX, fy, toggleW, rowH);
	}
	formPanel->addAndMakeVisible (trimToggle);
	fy += rowH + gap;

	// NORMALIZE 0dB
	auto* normalizeToggle = new juce::ToggleButton ("NORMALIZE 0dB");
	normalizeToggle->setToggleState (false, juce::dontSendNotification);
	normalizeToggle->setLookAndFeel (&lnf);
	normalizeToggle->setColour (juce::ToggleButton::textColourId, activeScheme.text);
	normalizeToggle->setColour (juce::ToggleButton::tickColourId, activeScheme.text);
	{
		const int toggleW = juce::jmin (controlW, 200);
		const int toggleX = (formW - toggleW) / 2;
		normalizeToggle->setBounds (toggleX, fy, toggleW, rowH);
	}
	formPanel->addAndMakeVisible (normalizeToggle);
	fy += rowH;

	// Register form panel as custom component so AlertWindow sizes correctly
	formPanel->setSize (formW, fy);
	aw->addCustomComponent (formPanel);

	// Buttons
	aw->addButton ("EXPORT", 1, juce::KeyPress (juce::KeyPress::returnKey));
	aw->addButton ("CANCEL", 0, juce::KeyPress (juce::KeyPress::escapeKey));

	finalizePromptButtons (*aw, lnf, [] (juce::AlertWindow& a) { applyPromptShellSize (a); });

	// Centre formPanel vertically between top and button row
	{
		const int buttonsTop = TR::getAlertButtonsTop (*aw);
		const int availH     = buttonsTop - TR::kPromptBodyBottomPad;
		const int panelY     = juce::jmax (TR::kPromptEditorMinTopPx,
		                                   (availH - formPanel->getHeight()) / 2);
		const int panelX     = (aw->getWidth() - formPanel->getWidth()) / 2;
		formPanel->setBounds (panelX, panelY, formPanel->getWidth(), formPanel->getHeight());
	}

	if (safeThis != nullptr)
	{
		fitAlertWindowToEditor (*aw, safeThis.getComponent(), nullptr);
		embedAlertWindowInOverlay (safeThis.getComponent(), aw);
	}

	aw->enterModalState (true,
		juce::ModalCallbackFunction::create (
			[safeThis, aw, formPanel, rateCombo, formatCombo, lengthLabel, trimToggle, normalizeToggle] (int result) mutable
		{
			const int rateId = (rateCombo != nullptr) ? rateCombo->getSelectedId() : 2;
			const int formatId = (formatCombo != nullptr) ? formatCombo->getSelectedId() : 2;
			const float maxLen = (lengthLabel != nullptr)
			    ? juce::jlimit (0.01f, 10.0f, lengthLabel->getText().trimCharactersAtEnd ("s").getFloatValue()) : 10.0f;
			const bool trim = (trimToggle != nullptr) ? trimToggle->getToggleState() : true;
			const bool normalize = (normalizeToggle != nullptr) ? normalizeToggle->getToggleState() : false;

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
				[safeThis2, targetRate, formatType, maxLen, trim, normalize, ext] (const juce::FileChooser& fc)
				{
					if (safeThis2 == nullptr)
						return;

					auto file = fc.getResult();
					if (file == juce::File())
						return;

					if (! file.hasFileExtension (ext.substring (1)))
						file = file.withFileExtension (ext.substring (1));

					const bool ok = safeThis2->audioProcessor.exportCombinedIR (
						targetRate, formatType, maxLen, trim, normalize, file);

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
	aw->setEscapeKeyCancels (true);

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
	aw->setEscapeKeyCancels (true);

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

				finalizePromptButtons (*colorAw, safeThis->lnf,
				                       [] (juce::AlertWindow& a) { applyPromptShellSize (a); });

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
