// Registration / unregistration self-test. For this process HKLM, HKCU, HKCR and HKEY_USERS are redirected with
// RegOverridePredefKey to empty keys under HKCU\Software\STLPreviewRegTest: nothing is written to the real registry
// (except that test key). Checks that a foreign handler (such as Creality's) is backed up and restored, that unregister
// leaves a value taken over by another program alone, and that a ProgID known only from HKEY_USERS gets linked.
#include "com_ids.h"
#include "com_util.h"
#include "registration.h"

#include <windows.h>

#include <string>

namespace {
constexpr wchar_t kSandbox[] = L"Software\\STLPreviewRegTest";
constexpr wchar_t kForeign[] = L"{11111111-2222-3333-4444-555555555555}";
constexpr wchar_t kOther[] = L"{66666666-7777-8888-9999-000000000000}";

void Set(HKEY root, const std::wstring& key, const std::wstring& value) {
    RegSetKeyValueW(root, key.c_str(), nullptr, REG_SZ, value.c_str(),
                    static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
}

std::wstring Get(HKEY root, const std::wstring& key, const wchar_t* name = nullptr) {
    wchar_t buffer[512] = {};
    DWORD size = sizeof(buffer);
    if (RegGetValueW(root, key.c_str(), name, RRF_RT_REG_SZ, nullptr, buffer, &size) != ERROR_SUCCESS) return L"<none>";
    return buffer;
}

bool Exists(HKEY root, const std::wstring& key) {
    HKEY handle = nullptr;
    if (RegOpenKeyExW(root, key.c_str(), 0, KEY_READ, &handle) != ERROR_SUCCESS) return false;
    RegCloseKey(handle);
    return true;
}

void Check(std::wstring& report, bool& ok, bool pass, const std::wstring& what) {
    report += (pass ? L"OK " : L"FAIL ") + what + L"\r\n";
    ok = ok && pass;
}
}  // namespace

bool RegistrySelfTest(std::wstring& report) {
    report.clear();
    RegDeleteTreeW(HKEY_CURRENT_USER, kSandbox);

    const HKEY roots[] = {HKEY_LOCAL_MACHINE, HKEY_CURRENT_USER, HKEY_CLASSES_ROOT, HKEY_USERS};
    const wchar_t* const names[] = {L"HKLM", L"HKCU", L"HKCR", L"HKU"};
    HKEY sandboxes[4] = {};
    for (int i = 0; i < 4; ++i) {
        const std::wstring path = std::wstring(kSandbox) + L"\\" + names[i];
        if (RegCreateKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, nullptr, 0, KEY_ALL_ACCESS, nullptr, &sandboxes[i],
                            nullptr) != ERROR_SUCCESS) {
            report = L"FAIL could not create the sandbox key\r\n";
            for (HKEY key : sandboxes) if (key) RegCloseKey(key);
            return false;
        }
    }
    for (int i = 0; i < 4; ++i) RegOverridePredefKey(roots[i], sandboxes[i]);

    const std::wstring classes = L"Software\\Classes\\";
    const std::wstring thumb = std::wstring(L"\\shellex\\") + kThumbnailHandlerGuid;
    const std::wstring ours = GuidToString(CLSID_StlThumbnailProvider);
    const std::wstring stlLink = classes + L".stl" + thumb;
    const std::wstring progIdLink = classes + L"UserProg" + thumb;
    const std::wstring mfLink = classes + L".3mf" + thumb;

    // Start: a Creality-like foreign handler on .stl and on a ProgID known only from HKEY_USERS.
    Set(HKEY_LOCAL_MACHINE, classes + L"CLSID\\" + kForeign, L"foreign");
    Set(HKEY_LOCAL_MACHINE, classes + L"CLSID\\" + kOther, L"other");
    Set(HKEY_LOCAL_MACHINE, stlLink, kForeign);
    Set(HKEY_LOCAL_MACHINE, classes + L"UserProg", L"ProgID");
    Set(HKEY_LOCAL_MACHINE, progIdLink, kForeign);
    Set(HKEY_USERS, L"S-1-5-21-0-0-0-1000_Classes\\.stl", L"UserProg");

    bool ok = true;
    std::wstring log;
    Check(report, ok, RegisterMachineClasses(L"C:\\STLPreviewTest\\STLPreview.dll", log), L"register-machine");
    Check(report, ok, Get(HKEY_LOCAL_MACHINE, stlLink) == ours, L".stl link is ours");
    Check(report, ok, Get(HKEY_LOCAL_MACHINE, progIdLink) == ours, L"HKEY_USERS ProgID link is ours");
    Check(report, ok, Get(HKEY_LOCAL_MACHINE, L"Software\\STLPreview\\ShellexBackup", stlLink.c_str()) == kForeign,
          L"foreign .stl handler backed up");
    DWORD flag = 0;
    DWORD size = sizeof(flag);
    RegGetValueW(HKEY_LOCAL_MACHINE, (classes + L"CLSID\\" + GuidToString(CLSID_StlPreviewHandler)).c_str(),
                 L"DisableLowILProcessIsolation", RRF_RT_REG_DWORD, nullptr, &flag, &size);
    Check(report, ok, flag == 1, L"preview runs at normal integrity");

    // After setup another program took over .3mf: unregister must not touch it.
    Set(HKEY_LOCAL_MACHINE, mfLink, kOther);
    Check(report, ok, UnregisterMachineClasses(log), L"unregister-machine");
    Check(report, ok, Get(HKEY_LOCAL_MACHINE, stlLink) == kForeign, L".stl foreign handler restored");
    Check(report, ok, Get(HKEY_LOCAL_MACHINE, progIdLink) == kForeign, L"ProgID foreign handler restored");
    Check(report, ok, Get(HKEY_LOCAL_MACHINE, mfLink) == kOther, L".3mf taken over by another program left alone");
    Check(report, ok, !Exists(HKEY_LOCAL_MACHINE, classes + L"CLSID\\" + ours), L"our CLSID removed");
    Check(report, ok, !Exists(HKEY_LOCAL_MACHINE, L"Software\\STLPreview"), L"Software\\STLPreview gone");
    if (!ok) report += log;

    for (int i = 0; i < 4; ++i) {
        RegOverridePredefKey(roots[i], nullptr);
        RegCloseKey(sandboxes[i]);
    }
    RegDeleteTreeW(HKEY_CURRENT_USER, kSandbox);
    RegDeleteKeyW(HKEY_CURRENT_USER, kSandbox);
    return ok;
}
