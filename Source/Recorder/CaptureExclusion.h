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

namespace rec
{
/** 呼び出したスレッドが作るトップレベルウィンドウを画面キャプチャから除外する。

    既存のウィンドウに加え、以後このスレッドで作られるウィンドウ（JUCE のポップアップメニュー、
    コンボボックスのリスト、ダイアログ等）にも WM_CREATE の直後に自動で適用する。
    JUCE のメッセージスレッドから、メインウィンドウを作る前に呼ぶこと。
    WDA_EXCLUDEFROMCAPTURE は Windows 10 2004 以降。それ以前は黒塗り (WDA_MONITOR) になる。
*/
void excludeThreadWindowsFromCapture();

void stopExcludingThreadWindowsFromCapture();

} // namespace rec
