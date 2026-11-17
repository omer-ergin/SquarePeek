#pragma once

// i18n.h — UI language selection (header only). For text the user SEES in the preview pane / thumbnail:
// Turkish if the Windows UI language is Turkish, otherwise English.
// For testing, the environment variable STLPREVIEW_LANG=tr|en always takes precedence.
// The result is computed once per process and cached.

#include <windows.h>

#include <cwchar>

inline bool UiTurkish() {
    static const bool turkish = [] {
        wchar_t value[8] = {};
        const DWORD length = GetEnvironmentVariableW(L"STLPREVIEW_LANG", value, ARRAYSIZE(value));
        if (length > 0 && length < ARRAYSIZE(value)) {
            if (_wcsicmp(value, L"tr") == 0) return true;
            if (_wcsicmp(value, L"en") == 0) return false;
        }
        return PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_TURKISH;
    }();
    return turkish;
}

// Turkish or English text depending on the UI language.
inline const wchar_t* L10n(const wchar_t* tr, const wchar_t* en) {
    return UiTurkish() ? tr : en;
}
