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

#include "CaptureExclusion.h"

#ifndef NOMINMAX
 #define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
 #define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>

#pragma comment (lib, "user32.lib")

#ifndef WDA_EXCLUDEFROMCAPTURE
 #define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif

namespace rec
{
namespace
{
    HHOOK hook = nullptr;

    void exclude (HWND window)
    {
        if ((GetWindowLongPtrW (window, GWL_STYLE) & WS_CHILD) != 0)
            return; // 子ウィンドウは親ごと除外される（そもそも指定できない）

        if (! SetWindowDisplayAffinity (window, WDA_EXCLUDEFROMCAPTURE))
            SetWindowDisplayAffinity (window, WDA_MONITOR);
    }

    // WM_CREATE が処理された直後（＝表示される前）に呼ばれる
    LRESULT CALLBACK afterWindowMessage (int code, WPARAM wParam, LPARAM lParam)
    {
        if (code == HC_ACTION)
        {
            const auto& message = *reinterpret_cast<const CWPRETSTRUCT*> (lParam);

            if (message.message == WM_CREATE && message.lResult != -1)
                exclude (message.hwnd);
        }

        return CallNextHookEx (hook, code, wParam, lParam);
    }

    BOOL CALLBACK excludeExistingWindow (HWND window, LPARAM)
    {
        exclude (window);
        return TRUE;
    }
}

void excludeThreadWindowsFromCapture()
{
    if (hook != nullptr)
        return;

    EnumThreadWindows (GetCurrentThreadId(), excludeExistingWindow, 0);
    hook = SetWindowsHookExW (WH_CALLWNDPROCRET, afterWindowMessage, nullptr, GetCurrentThreadId());
}

void stopExcludingThreadWindowsFromCapture()
{
    if (hook != nullptr)
    {
        UnhookWindowsHookEx (hook);
        hook = nullptr;
    }
}

} // namespace rec
