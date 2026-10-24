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

#pragma once

#include <JuceHeader.h>

//==============================================================================
/*
    時・分・秒をそれぞれ − / + ボタンで入力する時間入力欄。

    - ボタンは長押しで連続して増減する。桁は繰り上がり・繰り下がりする（00:00:59 の秒 + → 00:01:00）
    - 数字の上でのマウスホイールでも増減でき、数字をダブルクリックすると直接入力できる
    - 00:00:00 は「未設定」として数字を薄く表示する
*/
class DurationStepper  : public juce::Component
{
public:
    DurationStepper();

    static constexpr int maxTotalSeconds = 99 * 3600 + 59 * 60 + 59;

    int getTotalSeconds() const noexcept    { return totalSeconds; }
    void setTotalSeconds (int newTotalSeconds, juce::NotificationType notification = juce::sendNotificationSync);

    /** 値が変わったときに呼ばれる */
    std::function<void()> onChange;

    //==============================================================================
    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

private:
    //==============================================================================
    /** 丸い − / + ボタン（記号はフォントに頼らず線で描く） */
    class StepButton  : public juce::Button
    {
    public:
        explicit StepButton (bool isIncrement);
        void paintButton (juce::Graphics&, bool isHighlighted, bool isDown) override;

    private:
        const bool increment;
    };

    struct Field
    {
        Field (const juce::String& captionText, int secondsPerUnit, int maximum)
            : caption (captionText), unitSeconds (secondsPerUnit), maxValue (maximum) {}

        const juce::String caption;
        const int unitSeconds, maxValue;

        juce::Label value;
        StepButton decrement { false }, increment { true };
        juce::Rectangle<int> area;
    };

    void step (const Field& field, int direction);
    void applyTypedValue (const Field& field);
    void updateDisplay();

    juce::OwnedArray<Field> fields;
    std::array<juce::Rectangle<int>, 2> colonAreas;
    int totalSeconds = 0;
    float wheelAccumulator = 0.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DurationStepper)
};
