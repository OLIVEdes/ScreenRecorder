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

#include "DurationStepper.h"
#include "Recorder/ScreenRecorder.h"

//==============================================================================
/*
    録画設定（モニター / フレームレート / ビットレート / システム音 / 保存先）と
    録画の開始・停止を行うメイン画面。
*/
class MainComponent  : public juce::Component,
                       private juce::Timer
{
public:
    //==============================================================================
    MainComponent();
    ~MainComponent() override;

    //==============================================================================
    void paint (juce::Graphics&) override;
    void resized() override;

private:
    //==============================================================================
    void timerCallback() override;

    void refreshMonitorList();
    void chooseOutputFolder();
    void startRecording();
    void stopRecording (const juce::String& note = {});
    void updateControls();
    void autoStopEdited();
    void showError (const juce::String& message);

    //==============================================================================
    rec::ScreenRecorder recorder;
    std::vector<rec::MonitorInfo> monitors;

    juce::File outputFolder { juce::File::getSpecialLocation (juce::File::userMoviesDirectory) };
    juce::File currentFile;
    juce::String encoderDescription;

    juce::Label monitorLabel   { {}, "Monitor" },
                frameRateLabel { {}, "Frame rate" },
                bitrateLabel   { {}, "Bitrate" },
                folderLabel    { {}, "Save to" },
                autoStopLabel  { {}, "Stop after" },
                folderPathLabel, statusLabel;

    juce::ComboBox monitorBox, frameRateBox, bitrateBox;
    DurationStepper autoStopEditor;

    // 録画中、入力欄は残り時間を表示する。停止条件は「経過時間 >= autoStopLimit」（0 は手動停止）
    // countdownBase は、入力欄の表示を最後に更新したときの経過秒（表示 = autoStopLimit - countdownBase）
    int autoStopSetting = 0, autoStopLimit = 0, countdownBase = 0;
    juce::ToggleButton audioToggle { "Record system audio (WASAPI loopback)" };

    juce::TextButton refreshButton      { "Refresh" },
                     chooseFolderButton { "Choose..." },
                     openFolderButton   { "Open" },
                     clearAutoStopButton { "Clear" },
                     recordButton       { "Start Recording" };

    std::unique_ptr<juce::FileChooser> folderChooser;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};
