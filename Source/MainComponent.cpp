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

#include "MainComponent.h"

namespace
{
    juce::String formatTime (int seconds)
    {
        return juce::String::formatted ("%02d:%02d:%02d", seconds / 3600, (seconds / 60) % 60, seconds % 60);
    }
}

//==============================================================================
MainComponent::MainComponent()
{
    for (auto* component : std::initializer_list<juce::Component*> { &monitorLabel, &frameRateLabel, &bitrateLabel, &folderLabel,
                                                                     &folderPathLabel, &statusLabel, &monitorBox, &frameRateBox,
                                                                     &bitrateBox, &audioToggle, &refreshButton, &chooseFolderButton,
                                                                     &openFolderButton, &recordButton, &autoStopLabel, &autoStopEditor,
                                                                     &clearAutoStopButton })
        addAndMakeVisible (component);

    // ComboBox の ID にそのまま fps / Mbps の値を使う
    for (auto fps : { 30, 60 })
        frameRateBox.addItem (juce::String (fps) + " fps", fps);

    for (auto mbps : { 4, 8, 16, 32, 50 })
        bitrateBox.addItem (juce::String (mbps) + " Mbps", mbps);

    // 自動停止の時間は録画中も変更できる（延長・短縮したいとき用）。00:00:00 は手動停止
    autoStopEditor.onChange     = [this] { autoStopEdited(); };
    clearAutoStopButton.onClick = [this] { autoStopEditor.setTotalSeconds (0); };
    clearAutoStopButton.setEnabled (false);

    frameRateBox.setSelectedId (60);
    bitrateBox.setSelectedId (16);
    audioToggle.setToggleState (true, juce::dontSendNotification);

    folderPathLabel.setText (outputFolder.getFullPathName(), juce::dontSendNotification);
    folderPathLabel.setColour (juce::Label::outlineColourId, juce::Colours::grey.withAlpha (0.5f));
    statusLabel.setJustificationType (juce::Justification::topLeft);

    refreshButton.onClick      = [this] { refreshMonitorList(); };
    chooseFolderButton.onClick = [this] { chooseOutputFolder(); };
    recordButton.onClick       = [this] { recorder.isRecording() ? stopRecording() : startRecording(); };

    openFolderButton.onClick = [this]
    {
        if (currentFile.existsAsFile())
            currentFile.revealToUser();
        else
            outputFolder.startAsProcess();
    };

    refreshMonitorList();
    updateControls();

    setSize (620, 354);
}

MainComponent::~MainComponent()
{
    stopTimer();
    recorder.stop();
}

//==============================================================================
void MainComponent::paint (juce::Graphics& g)
{
    // (Our component is opaque, so we must completely fill the background with a solid colour)
    g.fillAll (getLookAndFeel().findColour (juce::ResizableWindow::backgroundColourId));
}

void MainComponent::resized()
{
    constexpr int rowHeight = 28, gap = 8, labelWidth = 90;

    auto area = getLocalBounds().reduced (16);

    auto nextRow = [&]
    {
        auto row = area.removeFromTop (rowHeight);
        area.removeFromTop (gap);
        return row;
    };

    auto row = nextRow();
    monitorLabel.setBounds (row.removeFromLeft (labelWidth));
    refreshButton.setBounds (row.removeFromRight (80));
    row.removeFromRight (gap);
    monitorBox.setBounds (row);

    row = nextRow();
    frameRateLabel.setBounds (row.removeFromLeft (labelWidth));
    frameRateBox.setBounds (row.removeFromLeft (110));
    row.removeFromLeft (24);
    bitrateLabel.setBounds (row.removeFromLeft (70));
    bitrateBox.setBounds (row.removeFromLeft (110));

    row = nextRow();
    row.removeFromLeft (labelWidth);
    audioToggle.setBounds (row);

    // 時間入力欄は数字・ボタン (40px) の下に単位の見出し (16px) が付く
    row = area.removeFromTop (56);
    area.removeFromTop (gap);
    auto controls = row.withHeight (40);
    autoStopLabel.setBounds (controls.removeFromLeft (labelWidth));
    autoStopEditor.setBounds (row.withLeft (controls.getX()).withWidth (370));
    controls.removeFromLeft (370 + gap);
    clearAutoStopButton.setBounds (controls.removeFromLeft (70).withSizeKeepingCentre (70, rowHeight));

    row = nextRow();
    folderLabel.setBounds (row.removeFromLeft (labelWidth));
    openFolderButton.setBounds (row.removeFromRight (80));
    row.removeFromRight (gap);
    chooseFolderButton.setBounds (row.removeFromRight (90));
    row.removeFromRight (gap);
    folderPathLabel.setBounds (row);

    area.removeFromTop (gap);
    recordButton.setBounds (area.removeFromTop (44));
    area.removeFromTop (gap);
    statusLabel.setBounds (area);
}

//==============================================================================
void MainComponent::timerCallback()
{
    if (! recorder.isRecording())
        return;

    if (const auto error = recorder.getError(); ! error.empty())
    {
        stopRecording(); // エラーの表示は stopRecording() が行う
        return;
    }

    const auto stats = recorder.getStats();
    const auto seconds = static_cast<int> (stats.seconds);

    countdownBase = seconds;

    if (autoStopLimit > 0)
    {
        if (stats.seconds >= autoStopLimit)
        {
            stopRecording ("Stopped automatically after " + formatTime (autoStopLimit) + ".");
            return;
        }

        // 残り時間をカウントダウン表示（onChange は呼ばない）
        autoStopEditor.setTotalSeconds (autoStopLimit - seconds, juce::dontSendNotification);
    }

    statusLabel.setText ("REC  " + formatTime (seconds)
                           + "    frames " + juce::String (stats.framesWritten)
                           + "    dropped " + juce::String (stats.framesDropped)
                           + "\nEncoder: " + encoderDescription,
                         juce::dontSendNotification);
}

void MainComponent::refreshMonitorList()
{
    monitors = rec::getMonitors();
    monitorBox.clear (juce::dontSendNotification);

    int selectedId = 1;

    for (size_t i = 0; i < monitors.size(); ++i)
    {
        const auto& monitor = monitors[i];
        const auto id = static_cast<int> (i) + 1;

        monitorBox.addItem (juce::String (monitor.name.c_str()) + "  " + juce::String (monitor.width) + " x " + juce::String (monitor.height)
                              + (monitor.isPrimary ? "  (primary)" : "") + "  - " + juce::String (monitor.adapterName.c_str()),
                            id);

        if (monitor.isPrimary)
            selectedId = id;
    }

    monitorBox.setSelectedId (selectedId, juce::dontSendNotification);
}

void MainComponent::chooseOutputFolder()
{
    folderChooser = std::make_unique<juce::FileChooser> ("Choose a folder for recordings", outputFolder);

    folderChooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                                [this] (const juce::FileChooser& chooser)
                                {
                                    const auto folder = chooser.getResult();

                                    if (folder != juce::File())
                                    {
                                        outputFolder = folder;
                                        folderPathLabel.setText (outputFolder.getFullPathName(), juce::dontSendNotification);
                                    }
                                });
}

void MainComponent::startRecording()
{
    const auto index = monitorBox.getSelectedItemIndex();

    if (! juce::isPositiveAndBelow (index, static_cast<int> (monitors.size())))
        return;

    if (const auto result = outputFolder.createDirectory(); result.failed())
    {
        showError ("Cannot create the output folder:\n" + result.getErrorMessage());
        return;
    }

    currentFile = outputFolder.getNonexistentChildFile ("Recording_" + juce::Time::getCurrentTime().formatted ("%Y-%m-%d_%H-%M-%S"),
                                                        ".mp4", false);

    const auto& monitor = monitors[static_cast<size_t> (index)];

    rec::RecorderSettings settings;
    settings.outputFile        = currentFile.getFullPathName().toWideCharPointer();
    settings.adapterIndex      = monitor.adapterIndex;
    settings.outputIndex       = monitor.outputIndex;
    settings.frameRate         = frameRateBox.getSelectedId();
    settings.videoBitrate      = bitrateBox.getSelectedId() * 1'000'000;
    settings.recordSystemAudio = audioToggle.getToggleState();

    try
    {
        recorder.start (settings);
    }
    catch (const std::exception& e)
    {
        showError ("Could not start recording:\n" + juce::String::fromUTF8 (e.what()));
        return;
    }

    autoStopSetting = autoStopLimit = autoStopEditor.getTotalSeconds();
    countdownBase = 0;

    encoderDescription = juce::String (recorder.getVideoEncoderName().c_str())
                           + (recorder.isHardwareEncoder() ? " (hardware)" : " (software)");

    updateControls();
    timerCallback();
    startTimerHz (4);
}

void MainComponent::stopRecording (const juce::String& note)
{
    stopTimer();
    recorder.stop();

    // カウントダウンしていた入力欄を、録画前に設定した時間へ戻す
    autoStopLimit = 0;
    autoStopEditor.setTotalSeconds (autoStopSetting, juce::dontSendNotification);
    updateControls();

    if (const auto error = recorder.getError(); ! error.empty())
    {
        statusLabel.setText ("Recording failed.", juce::dontSendNotification);
        showError ("Recording stopped because of an error:\n" + juce::String::fromUTF8 (error.c_str()));
        return;
    }

    statusLabel.setText ((note.isNotEmpty() ? note + "\n" : juce::String()) + "Saved: " + currentFile.getFullPathName(),
                         juce::dontSendNotification);
}

void MainComponent::autoStopEdited()
{
    // 録画中の ± 操作は「残り時間」の変更として扱い、停止する経過時間を付け替える
    if (recorder.isRecording())
    {
        const auto remaining = autoStopEditor.getTotalSeconds();
        autoStopLimit = remaining > 0 ? countdownBase + remaining : 0;
    }

    clearAutoStopButton.setEnabled (autoStopEditor.getTotalSeconds() > 0);
}

void MainComponent::updateControls()
{
    const auto recording = recorder.isRecording();

    autoStopLabel.setText (recording ? "Stops in" : "Stop after", juce::dontSendNotification);
    clearAutoStopButton.setEnabled (autoStopEditor.getTotalSeconds() > 0);

    for (auto* component : std::initializer_list<juce::Component*> { &monitorBox, &frameRateBox, &bitrateBox, &audioToggle,
                                                                     &refreshButton, &chooseFolderButton })
        component->setEnabled (! recording);

    recordButton.setButtonText (recording ? "Stop Recording" : "Start Recording");

    if (recording)
    {
        recordButton.setColour (juce::TextButton::buttonColourId, juce::Colour (0xffe8868e));
        recordButton.setColour (juce::TextButton::textColourOffId, juce::Colours::white);
    }
    else
    {
        recordButton.removeColour (juce::TextButton::buttonColourId);
        recordButton.removeColour (juce::TextButton::textColourOffId);
    }
}

void MainComponent::showError (const juce::String& message)
{
    juce::AlertWindow::showAsync (juce::MessageBoxOptions::makeOptionsOk (juce::MessageBoxIconType::WarningIcon,
                                                                          "Screen Recorder", message),
                                  [] (int) {});
}
