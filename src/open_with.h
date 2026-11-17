#pragma once

// open_with.h — the "Open with" bar in the preview pane (F9): programs on this PC that can open the file.
// Owned by the "preview" unit (implementation: open_with.cpp). Also used by the test tool ("handlers" and
// "overlay --openwith" commands; the test tool never LAUNCHES any program).
//
// Only shell APIs are used (SHAssocEnumHandlers / IAssocHandler, SHOpenWithDialog).
// No file association is ever CHANGED: the default program opened on double-click stays as it is
// (the "Always use this app" checkbox is hidden in Windows' "Open with" dialog).
// All functions must be called on a COM-initialized (STA) thread. They do not throw.

#include <windows.h>
#include <shobjidl.h>
#include <wrl/client.h>

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "overlay.h"

// Maximum number of programs shown in the bar (plus the "…" button).
inline constexpr size_t kOpenWithMaxApps = 6;

struct OpenWithApp {
    std::wstring uiName;        // IAssocHandler::GetUIName ("Creality Print")
    std::wstring name;          // IAssocHandler::GetName (usually the exe path)
    std::wstring iconPath;      // IAssocHandler::GetIconLocation (may be empty)
    int iconIndex = 0;
    bool recommended = false;   // IAssocHandler::IsRecommended
    bool isDefault = false;     // the extension's default program (first in the list); only if defaultFromAssoc
    Microsoft::WRL::ComPtr<IAssocHandler> handler;
    std::shared_ptr<const OverlayIcon> icon;   // filled by LoadOpenWithIcons; null = no icon
};

struct OpenWithList {
    std::vector<OpenWithApp> apps;     // default program first, unique by UI name, at most maxApps
    size_t totalHandlers = 0;          // count before dedup/truncation
    bool usedRecommendedFilter = true; // false: ASSOC_FILTER_RECOMMENDED was empty, ASSOC_FILTER_NONE was used
    bool defaultFromAssoc = false;     // default found via AssocQueryString (false: no item has isDefault)
    std::wstring defaultExecutable;    // AssocQueryString(ASSOCSTR_EXECUTABLE)
    std::wstring defaultFriendlyName;  // AssocQueryString(ASSOCSTR_FRIENDLYAPPNAME)
};

// extension: ".stl" / ".3mf" (dot included). Lists programs; launches none. May take a few tens of ms:
// the preview calls it once, off the render path (after the model is shown).
// ASSOC_FILTER_RECOMMENDED first; if empty, ASSOC_FILTER_NONE. allHandlers = true (test tool only, to try the
// layout with many buttons): ASSOC_FILTER_NONE directly.
OpenWithList EnumerateOpenWithApps(const wchar_t* extension, size_t maxApps = kOpenWithMaxApps,
                                   bool allHandlers = false);

// Extracts each program's icon at sizePx × sizePx (SHDefExtractIcon / PrivateExtractIcons; for packaged
// apps the "@{...}" resource via SHLoadIndirectString + WIC; for MSIX alias exes the shell item icon via
// IShellItemImageFactory). On failure icon stays null.
void LoadOpenWithIcons(std::vector<OpenWithApp>& apps, int sizePx);

// Opens the file with this program (IShellItem -> BHID_DataObject -> IAssocHandler::Invoke). Calls
// AllowSetForegroundWindow(ASFW_ANY) first so the launched program comes to the front.
HRESULT InvokeOpenWithApp(const OpenWithApp& app, const std::wstring& path);

// Windows' "Open with" dialog (OAIF_EXEC | OAIF_HIDE_REGISTRATION: opens with the chosen program, with no
// option to change the default). Returns HRESULT_FROM_WIN32(ERROR_CANCELLED) if the user cancels.
HRESULT ShowOpenWithDialog(HWND owner, const std::wstring& path);

// Explorer starts the preview handler at low integrity level (Low IL). A program launched from there also
// runs at Low IL (can't write its settings, may crash) and a cross-process modal window deadlocks. So
// programs are launched by Explorer ITSELF, through the site given to the handler via SetSite:
// SID_STopLevelBrowser -> IShellBrowser -> active view -> IShellFolderViewDual::Application ->
// IShellDispatch2::ShellExecute. The call runs in the Explorer process (normal level).
// file: file or exe to open; args: arguments for the exe; verb: nullptr = default,
// L"openas" = Windows' "Open with" window (Explorer opens it; our process does not wait).
HRESULT ShellExecuteViaSite(IUnknown* site, const std::wstring& file, const std::wstring& args, const wchar_t* verb);

// ShellExecuteViaSite arguments for the program: if app.name is a full-path .exe, (exe, "\"path\"").
// Otherwise false (packaged app, etc.).
bool OpenWithAppCommand(const OpenWithApp& app, const std::wstring& path, std::wstring& file, std::wstring& args);

// Is this process running at low integrity level (Low IL)?
bool IsLowIntegrityProcess();

// Whether a string such as a stream's Stat name is a path usable for "Open with": a full path (drive or UNC),
// an existing file (not a folder), with extension .stl / .3mf. The path is never guessed.
bool IsUsableOpenWithPath(const std::wstring& path);
