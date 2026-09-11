#pragma once
#include <windows.h>
#include <cwctype>
#include <string>
#include <string_view>

namespace DXL {
// The launcher and the game accept either a bare key or a modifier combination.
inline bool ParseHotkey(std::wstring_view text, UINT& modifiers, UINT& virtualKey) {
    modifiers = 0;
    virtualKey = 0;
    size_t start = 0;
    while (start <= text.size()) {
        const size_t plus = text.find(L'+', start);
        const auto part = text.substr(start, plus == text.npos ? text.size() - start : plus - start);
        std::wstring token;
        for (wchar_t ch : part) if (!iswspace(ch)) token.push_back(static_cast<wchar_t>(towupper(ch)));
        if (token == L"CTRL" || token == L"CONTROL") modifiers |= MOD_CONTROL;
        else if (token == L"ALT") modifiers |= MOD_ALT;
        else if (token == L"SHIFT") modifiers |= MOD_SHIFT;
        else if (token == L"WIN" || token == L"META") modifiers |= MOD_WIN;
        else {
            UINT key = 0;
            if (token.size() == 1 && ((token[0] >= L'A' && token[0] <= L'Z') ||
                (token[0] >= L'0' && token[0] <= L'9'))) key = token[0];
            else if (token == L"DEL" || token == L"DELETE") key = VK_DELETE;
            else if (token == L"END") key = VK_END;
            else if (token == L"INS" || token == L"INSERT") key = VK_INSERT;
            else if (token == L"HOME") key = VK_HOME;
            else if (token == L"PAGEUP" || token == L"PGUP") key = VK_PRIOR;
            else if (token == L"PAGEDOWN" || token == L"PGDN") key = VK_NEXT;
            else if (token == L"SPACE") key = VK_SPACE;
            else if (token == L"TAB") key = VK_TAB;
            else if (token == L"ENTER") key = VK_RETURN;
            else if (token == L"BACKSPACE") key = VK_BACK;
            else if (token == L"ARROWLEFT" || token == L"LEFT") key = VK_LEFT;
            else if (token == L"ARROWRIGHT" || token == L"RIGHT") key = VK_RIGHT;
            else if (token == L"ARROWUP" || token == L"UP") key = VK_UP;
            else if (token == L"ARROWDOWN" || token == L"DOWN") key = VK_DOWN;
            else if (token.size() >= 2 && token.size() <= 3 && token[0] == L'F') {
                UINT number = 0;
                for (size_t i = 1; i < token.size(); ++i) {
                    if (token[i] < L'0' || token[i] > L'9') return false;
                    number = number * 10 + token[i] - L'0';
                }
                if (number >= 1 && number <= 24) key = VK_F1 + number - 1;
            }
            if (!key || virtualKey) return false;
            virtualKey = key;
        }
        if (plus == text.npos) break;
        start = plus + 1;
    }
    return virtualKey != 0;
}
}
