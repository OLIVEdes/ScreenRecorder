/*
    ScreenRecorder
    Copyright (C) 2026 OLIVEdes

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU Affero General Public License as published
    by the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Affero General Public License for more details.

    You should have received a copy of the GNU Affero General Public License
    along with this program.  If not, see <https://www.gnu.org/licenses/>.

    SPDX-License-Identifier: AGPL-3.0-or-later
*/

#include "DurationStepper.h"

namespace
{
    const juce::Colour accentColour (0xff4f8cff);

    // 文字や線の色は LookAndFeel のテキスト色に合わせる（背景が明るくても暗くても読めるように）
    juce::Colour inkColour (juce::Component& c)   { return c.findColour (juce::Label::textColourId); }
    constexpr int captionHeight = 16, colonWidth = 14;

    juce::FontOptions digitFont()   { return juce::FontOptions (26.0f).withStyleFlags (juce::Font::bold); }
}

//==============================================================================
DurationStepper::StepButton::StepButton (bool isIncrement)
    : juce::Button (isIncrement ? "+" : "-"),
      increment (isIncrement)
{
    setRepeatSpeed (400, 90, 20);   // 長押しで加速しながら連続入力
    setWantsKeyboardFocus (false);  // キー入力で値が変わらないようにする
}

void DurationStepper::StepButton::paintButton (juce::Graphics& g, bool isHighlighted, bool isDown)
{
    const auto bounds = getLocalBounds().toFloat().reduced (2.0f);
    const auto size = juce::jmin (bounds.getWidth(), bounds.getHeight());
    const auto circle = bounds.withSizeKeepingCentre (size, size);
    const auto enabled = isEnabled();

    const auto ink = inkColour (*this);
    auto fill = juce::Colours::white.withAlpha (enabled ? 0.7f : 0.3f);

    if (enabled && isDown)
        fill = accentColour.withAlpha (0.6f);
    else if (enabled && isHighlighted)
        fill = accentColour.withAlpha (0.3f);

    g.setColour (fill);
    g.fillEllipse (circle);

    g.setColour (ink.withAlpha (enabled ? 0.25f : 0.1f));
    g.drawEllipse (circle, 1.0f);

    const auto centre = circle.getCentre();
    const auto radius = size * 0.2f;

    g.setColour (ink.withAlpha (enabled ? 0.9f : 0.25f));
    g.drawLine (centre.x - radius, centre.y, centre.x + radius, centre.y, 2.0f);

    if (increment)
        g.drawLine (centre.x, centre.y - radius, centre.x, centre.y + radius, 2.0f);
}

//==============================================================================
DurationStepper::DurationStepper()
{
    fields.add (new Field ("hours", 3600, 99));
    fields.add (new Field ("min",     60, 59));
    fields.add (new Field ("sec",      1, 59));

    for (auto* field : fields)
    {
        auto& value = field->value;
        value.setFont (digitFont());
        value.setJustificationType (juce::Justification::centred);
        value.setEditable (false, true, false);
        value.setTooltip ("Double-click to type, or scroll to change");
        value.onTextChange = [this, field] { applyTypedValue (*field); };

        field->decrement.setTitle ("Decrease " + field->caption);
        field->increment.setTitle ("Increase " + field->caption);
        field->decrement.onClick = [this, field] { step (*field, -1); };
        field->increment.onClick = [this, field] { step (*field, 1); };

        addAndMakeVisible (value);
        addAndMakeVisible (field->decrement);
        addAndMakeVisible (field->increment);
    }

    updateDisplay();
}

void DurationStepper::setTotalSeconds (int newTotalSeconds, juce::NotificationType notification)
{
    newTotalSeconds = juce::jlimit (0, maxTotalSeconds, newTotalSeconds);
    const auto changed = newTotalSeconds != totalSeconds;

    totalSeconds = newTotalSeconds;
    updateDisplay(); // 直接入力された "7" を "07" に整形するため、変化がなくても更新する

    if (changed && notification != juce::dontSendNotification && onChange != nullptr)
        onChange();
}

void DurationStepper::step (const Field& field, int direction)
{
    setTotalSeconds (totalSeconds + direction * field.unitSeconds);
}

void DurationStepper::applyTypedValue (const Field& field)
{
    const auto typed = field.value.getText().retainCharacters ("0123456789").getIntValue();
    const auto current = totalSeconds / field.unitSeconds % (field.maxValue + 1);

    setTotalSeconds (totalSeconds + (juce::jlimit (0, field.maxValue, typed) - current) * field.unitSeconds);
}

void DurationStepper::updateDisplay()
{
    const int values[] = { totalSeconds / 3600, (totalSeconds / 60) % 60, totalSeconds % 60 };
    const auto textColour = inkColour (*this).withAlpha (totalSeconds == 0 ? 0.35f : 1.0f);

    for (int i = 0; i < fields.size(); ++i)
    {
        auto* field = fields[i];
        field->value.setText (juce::String (values[i]).paddedLeft ('0', 2), juce::dontSendNotification);
        field->value.setColour (juce::Label::textColourId, textColour);
        field->decrement.setEnabled (totalSeconds > 0);
        field->increment.setEnabled (totalSeconds < maxTotalSeconds);
    }

    repaint();
}

//==============================================================================
void DurationStepper::paint (juce::Graphics& g)
{
    const auto card = getLocalBounds().withTrimmedBottom (captionHeight).toFloat().reduced (0.5f);

    const auto ink = inkColour (*this);

    g.setColour (juce::Colours::white.withAlpha (0.55f));
    g.fillRoundedRectangle (card, 10.0f);
    g.setColour (totalSeconds > 0 ? accentColour.withAlpha (0.6f) : ink.withAlpha (0.2f));
    g.drawRoundedRectangle (card, 10.0f, 1.0f);

    g.setColour (ink.withAlpha (0.45f));
    g.setFont (digitFont());

    for (auto& colon : colonAreas)
        g.drawText (":", colon, juce::Justification::centred, false);

    g.setFont (juce::FontOptions (12.0f));

    for (auto* field : fields)
        g.drawText (field->caption, field->area.withY (getHeight() - captionHeight).withHeight (captionHeight),
                    juce::Justification::centred, false);
}

void DurationStepper::resized()
{
    auto area = getLocalBounds().withTrimmedBottom (captionHeight).reduced (6, 4);
    const auto fieldWidth = (area.getWidth() - 2 * colonWidth) / 3;

    for (int i = 0; i < fields.size(); ++i)
    {
        auto* field = fields[i];
        auto bounds = area.removeFromLeft (fieldWidth);
        field->area = bounds;

        const auto buttonSize = bounds.getHeight();
        field->decrement.setBounds (bounds.removeFromLeft (buttonSize));
        field->increment.setBounds (bounds.removeFromRight (buttonSize));
        field->value.setBounds (bounds);

        if (i < 2)
            colonAreas[(size_t) i] = area.removeFromLeft (colonWidth);
    }
}

void DurationStepper::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    const auto position = e.getEventRelativeTo (this).getPosition();

    for (auto* field : fields)
    {
        if (field->area.contains (position))
        {
            // トラックパッド等の細かいホイールイベントは溜めてから 1 段階ずつ動かす
            wheelAccumulator += wheel.isReversed ? -wheel.deltaY : wheel.deltaY;

            if (std::abs (wheelAccumulator) >= 0.05f)
            {
                step (*field, wheelAccumulator > 0.0f ? 1 : -1);
                wheelAccumulator = 0.0f;
            }

            return;
        }
    }

    Component::mouseWheelMove (e, wheel);
}
