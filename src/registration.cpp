#include "registration.h"

#include "com_ids.h"
#include "com_util.h"

#include <windows.h>
#include <shlobj.h>
#include <shlwapi.h>

#include <cwchar>
#include <string>
#include <utility>
#include <vector>

// The extension's default value (ProgID), UserChoice and OpenWithProgids are NEVER touched: the program that opens on
// double-click does not change. Only the default value of the thumbnail and preview handler subkeys under shellex is
// written:
//   * Classes\<ext>\shellex\<guid> and Classes\SystemFileAssociations\<ext>\shellex\<guid>;
//   * if the extension's ProgID carries its own shellex\<guid> key (e.g. Creality's stl_cxsw), the shell picks that
//     first: only that ProgID's shellex\<guid> subkey. The ProgID's other values do not change.
//
// Machine-wide install (recommended): everything goes to HKLM, no link is written to HKCU (the Windows 11 26H2
// Explorer ignored HKCU overrides; HKCU entries can also only be cleaned up from one user).
// HKCU is touched only to remove entries left by old versions.
//
// In both scopes (HKLM / HKCU):
//   * Software\STLPreview\ShellexBackup: the foreign value that was overwritten (value name = key path). If the backup
//     cannot be written, the foreign value is NOT overwritten. The value is refreshed on every install (it may have
//     changed since).
//   * Software\STLPreview\MachineShellex (HKLM) / ProgIdShellex (HKCU, old name kept): every key written.
//   * Removal processes the union of recorded keys, all backup names and computed keys. If the value is still ours:
//     the backed-up CLSID is restored if it still exists, otherwise only our default value is deleted, along with keys
//     left empty because of us. If the value is no longer ours (e.g. Creality re-registered), it is left alone.
//   * If anything cannot be undone, backups and records are kept, the CLSIDs stay registered and the call fails.

namespace {
constexpr wchar_t kClassesRoot[] = L"Software\\Classes";
constexpr wchar_t kPreviewHandlersKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\PreviewHandlers";
constexpr wchar_t kAppKey[] = L"Software\\STLPreview";
constexpr wchar_t kBackupKey[] = L"Software\\STLPreview\\ShellexBackup";
constexpr wchar_t kUserRecordKey[] = L"Software\\STLPreview\\ProgIdShellex";
constexpr wchar_t kMachineRecordKey[] = L"Software\\STLPreview\\MachineShellex";
// The standard Windows preview process (prevhost.exe).
constexpr wchar_t kPrevhostAppId[] = L"{6d2b5079-2f0b-48dd-ab7f-97cec514d30b}";
// Windows' own 3MF previewer: restored if the .3mf preview backup has been lost.
constexpr wchar_t kMicrosoft3mfPreviewClsid[] = L"{E64164EB-1AE0-4C50-BAEF-A413C2B3A4BC}";

struct Scope {
    HKEY root;
    const wchar_t* name;
    const wchar_t* recordKey;
};
const Scope kMachine{HKEY_LOCAL_MACHINE, L"HKLM", kMachineRecordKey};
const Scope kUser{HKEY_CURRENT_USER, L"HKCU", kUserRecordKey};

std::wstring ClassesKey(const std::wstring& tail) { return std::wstring(kClassesRoot) + L"\\" + tail; }

std::wstring ThumbnailClsid() { return GuidToString(CLSID_StlThumbnailProvider); }
std::wstring PreviewClsid() { return GuidToString(CLSID_StlPreviewHandler); }

bool SameText(const std::wstring& a, const std::wstring& b) { return _wcsicmp(a.c_str(), b.c_str()) == 0; }

bool StartsWithText(const std::wstring& text, const std::wstring& prefix) {
    return text.size() >= prefix.size() && _wcsnicmp(text.c_str(), prefix.c_str(), prefix.size()) == 0;
}

void AddUnique(std::vector<std::wstring>& list, const std::wstring& item) {
    for (const std::wstring& existing : list) {
        if (SameText(existing, item)) return;
    }
    list.push_back(item);
}

// "a\b\shellex" -> "a\b"
std::wstring ParentKey(const std::wstring& key) {
    const size_t slash = key.find_last_of(L'\\');
    return slash == std::wstring::npos ? std::wstring() : key.substr(0, slash);
}

void Append(std::wstring& log, const std::wstring& line) {
    log += line;
    log += L"\r\n";
}

bool SetString(HKEY root, const std::wstring& subKey, const wchar_t* valueName, const std::wstring& value) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(root, subKey.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) !=
        ERROR_SUCCESS) {
        return false;
    }
    const LONG status = RegSetValueExW(key, valueName, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
                                       static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(key);
    return status == ERROR_SUCCESS;
}

// Absent: no value (or key). String: REG_SZ / REG_EXPAND_SZ was read. Other: another type or unreadable; not treated
// as absent (a foreign value must never be overwritten as if it did not exist).
enum class ValueKind { Absent, String, Other };

ValueKind ReadValue(HKEY root, const std::wstring& subKey, const wchar_t* valueName, std::wstring& value) {
    value.clear();
    HKEY key = nullptr;
    const LONG openStatus = RegOpenKeyExW(root, subKey.c_str(), 0, KEY_QUERY_VALUE, &key);
    if (openStatus == ERROR_FILE_NOT_FOUND) return ValueKind::Absent;
    if (openStatus != ERROR_SUCCESS) return ValueKind::Other;

    ValueKind kind = ValueKind::Other;
    DWORD type = 0;
    DWORD size = 0;
    LONG status = RegQueryValueExW(key, valueName, nullptr, &type, nullptr, &size);
    if (status == ERROR_FILE_NOT_FOUND) {
        kind = ValueKind::Absent;
    } else if (status == ERROR_SUCCESS && (type == REG_SZ || type == REG_EXPAND_SZ)) {
        // The size can grow between the query and the read: retry a few times.
        for (int attempt = 0; attempt < 4; ++attempt) {
            std::vector<wchar_t> buffer(size / sizeof(wchar_t) + 2, L'\0');
            DWORD bytes = static_cast<DWORD>((buffer.size() - 1) * sizeof(wchar_t));
            status = RegQueryValueExW(key, valueName, nullptr, &type, reinterpret_cast<BYTE*>(buffer.data()), &bytes);
            if (status == ERROR_MORE_DATA) {
                size = bytes;
                continue;
            }
            if (status == ERROR_SUCCESS && (type == REG_SZ || type == REG_EXPAND_SZ)) {
                value.assign(buffer.data());  // drops trailing NULs
                kind = ValueKind::String;
            } else if (status == ERROR_FILE_NOT_FOUND) {
                kind = ValueKind::Absent;
            }
            break;
        }
    }
    RegCloseKey(key);
    return kind;
}

bool ReadString(HKEY root, const std::wstring& subKey, const wchar_t* valueName, std::wstring& value) {
    return ReadValue(root, subKey, valueName, value) == ValueKind::String;
}

bool KeyExists(HKEY root, const std::wstring& subKey) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(root, subKey.c_str(), 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) return false;
    RegCloseKey(key);
    return true;
}

// Also succeeds if the value does not exist.
bool DeleteValue(HKEY root, const std::wstring& subKey, const wchar_t* valueName) {
    // "Already gone" counts as success even in a key we cannot write to.
    if (RegGetValueW(root, subKey.c_str(), valueName, RRF_RT_ANY, nullptr, nullptr, nullptr) == ERROR_FILE_NOT_FOUND) {
        return true;
    }
    HKEY key = nullptr;
    const LONG openStatus = RegOpenKeyExW(root, subKey.c_str(), 0, KEY_SET_VALUE, &key);
    if (openStatus == ERROR_FILE_NOT_FOUND) return true;
    if (openStatus != ERROR_SUCCESS) return false;
    const LONG status = RegDeleteValueW(key, valueName);
    RegCloseKey(key);
    return status == ERROR_SUCCESS || status == ERROR_FILE_NOT_FOUND;
}

// Also succeeds if the key does not exist.
bool DeleteTree(HKEY root, const std::wstring& subKey) {
    const LONG status = RegDeleteTreeW(root, subKey.c_str());
    if (status != ERROR_SUCCESS && status != ERROR_FILE_NOT_FOUND) return false;
    const LONG keyStatus = RegDeleteKeyW(root, subKey.c_str());
    return keyStatus == ERROR_SUCCESS || keyStatus == ERROR_FILE_NOT_FOUND;
}

// Deletes the key only if it is completely empty (no subkeys, no values); otherwise leaves it alone.
bool DeleteKeyIfEmpty(HKEY root, const std::wstring& subKey) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(root, subKey.c_str(), 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) return false;
    DWORD subKeys = 0;
    DWORD values = 0;
    const LONG status = RegQueryInfoKeyW(key, nullptr, nullptr, nullptr, &subKeys, nullptr, nullptr, &values,
                                         nullptr, nullptr, nullptr, nullptr);
    RegCloseKey(key);
    return status == ERROR_SUCCESS && subKeys == 0 && values == 0 && RegDeleteKeyW(root, subKey.c_str()) == ERROR_SUCCESS;
}

// After our value is deleted, deletes the keys left empty, walking upward (shellex\<guid>, shellex, e.g. an
// HKLM\Software\Classes\.stl or HKCU\...\SystemFileAssociations that did not exist before install). Software\Classes
// itself and keys that still contain anything are left alone.
void DeleteEmptyKeysUpward(HKEY root, std::wstring key) {
    const std::wstring stop = std::wstring(kClassesRoot) + L"\\";
    while (key.size() > stop.size() && StartsWithText(key, stop)) {
        if (!DeleteKeyIfEmpty(root, key)) return;
        key = ParentKey(key);
    }
}

// All value names of a key (of any type).
std::vector<std::wstring> ValueNames(HKEY root, const std::wstring& subKey) {
    std::vector<std::wstring> names;
    HKEY key = nullptr;
    if (RegOpenKeyExW(root, subKey.c_str(), 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) return names;
    DWORD maxNameLength = 0;
    if (RegQueryInfoKeyW(key, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, &maxNameLength, nullptr,
                         nullptr, nullptr) == ERROR_SUCCESS) {
        std::vector<wchar_t> name(maxNameLength + 2, L'\0');
        for (DWORD index = 0;; ++index) {
            DWORD nameLength = static_cast<DWORD>(name.size());
            const LONG status =
                RegEnumValueW(key, index, name.data(), &nameLength, nullptr, nullptr, nullptr, nullptr);
            if (status == ERROR_NO_MORE_ITEMS) break;
            if (status == ERROR_SUCCESS) names.emplace_back(name.data(), nameLength);
        }
    }
    RegCloseKey(key);
    return names;
}

// Is a CLSID still registered (HKLM or merged HKCR)?
bool ClsidExists(const std::wstring& clsid) {
    if (clsid.empty() || clsid.find(L'\\') != std::wstring::npos) return false;
    return KeyExists(HKEY_LOCAL_MACHINE, ClassesKey(L"CLSID\\" + clsid)) || KeyExists(HKEY_CLASSES_ROOT, L"CLSID\\" + clsid);
}

// The CLSID the shell will use for this extension and handler type (ProgID, extension, then
// SystemFileAssociations). Empty if none is found.
std::wstring EffectiveShellExtension(const wchar_t* extension, const wchar_t* handlerGuid) {
    wchar_t buffer[128] = {};
    DWORD length = ARRAYSIZE(buffer);
    if (FAILED(AssocQueryStringW(ASSOCF_NONE, ASSOCSTR_SHELLEXTENSION, extension, handlerGuid, buffer, &length))) {
        return std::wstring();
    }
    return std::wstring(buffer);
}

std::wstring HandlerKey(const std::wstring& shellexKey, const wchar_t* handlerGuid) {
    return shellexKey + L"\\" + handlerGuid;
}

std::wstring PrimaryShellexKey(const wchar_t* extension) { return ClassesKey(std::wstring(extension) + L"\\shellex"); }

std::wstring SystemShellexKey(const wchar_t* extension) {
    return ClassesKey(L"SystemFileAssociations\\" + std::wstring(extension) + L"\\shellex");
}

std::wstring ProgIdShellexKey(const std::wstring& progId) { return ClassesKey(progId + L"\\shellex"); }

// Our CLSID (thumbnail or preview) for a key of the form Software\Classes\...\shellex\<guid>; empty for any other
// form. Record and backup value names are never processed without passing this filter.
std::wstring OurClsidForHandlerKey(const std::wstring& key) {
    if (!StartsWithText(key, std::wstring(kClassesRoot) + L"\\")) return std::wstring();
    const std::wstring shellex = ParentKey(key);
    if (shellex.size() < 8 || !SameText(shellex.substr(shellex.size() - 8), L"\\shellex")) return std::wstring();
    const std::wstring guid = key.substr(shellex.size() + 1);
    if (SameText(guid, kThumbnailHandlerGuid)) return ThumbnailClsid();
    if (SameText(guid, kPreviewHandlerGuid)) return PreviewClsid();
    return std::wstring();
}

// Computed (fixed) link keys: extension and SystemFileAssociations for each extension, shellex for each ProgID.
void AddLinkKeys(std::vector<std::wstring>& keys, const std::wstring& shellexKey) {
    AddUnique(keys, HandlerKey(shellexKey, kThumbnailHandlerGuid));
    AddUnique(keys, HandlerKey(shellexKey, kPreviewHandlerGuid));
}

void AddExtensionLinkKeys(std::vector<std::wstring>& keys) {
    for (const wchar_t* extension : kModelExtensions) {
        AddLinkKeys(keys, PrimaryShellexKey(extension));
        AddLinkKeys(keys, SystemShellexKey(extension));
    }
}

// The extensions' default ProgIDs under one root (validated).
void AddExtensionProgIds(std::vector<std::wstring>& progIds, HKEY root, bool classesSubKey) {
    for (const wchar_t* extension : kModelExtensions) {
        std::wstring progId;
        const std::wstring key = classesSubKey ? ClassesKey(extension) : std::wstring(extension);
        if (ReadString(root, key, nullptr, progId) && IsValidProgId(progId)) AddUnique(progIds, progId);
    }
}

// Extension ProgIDs from all loaded user hives (HKEY_USERS\<SID>_Classes): finds the signed-in user's ProgID (e.g. a
// .stl -> stl_cxsw that exists only in HKCU) even when another admin account was chosen at the UAC prompt.
void AddLoadedUserProgIds(std::vector<std::wstring>& progIds) {
    wchar_t name[256];
    for (DWORD index = 0;; ++index) {
        DWORD length = ARRAYSIZE(name);
        const LONG status = RegEnumKeyExW(HKEY_USERS, index, name, &length, nullptr, nullptr, nullptr, nullptr);
        if (status == ERROR_NO_MORE_ITEMS) break;
        if (status != ERROR_SUCCESS) continue;
        const std::wstring hive(name, length);
        if (hive.size() < 8 || !SameText(hive.substr(hive.size() - 8), L"_Classes")) continue;
        for (const wchar_t* extension : kModelExtensions) {
            std::wstring progId;
            if (ReadString(HKEY_USERS, hive + L"\\" + extension, nullptr, progId) && IsValidProgId(progId)) {
                AddUnique(progIds, progId);
            }
        }
    }
}

// Sets the default value of shellex\<guid> to our CLSID.
//   * already ours: only added to the record;
//   * absent: the old (now invalid) backup is deleted, the key is recorded and written;
//   * a foreign string: the backup is refreshed; if the backup cannot be written, it is NOT overwritten (false);
//   * not a string / unreadable: left alone, with a warning.
bool WriteShellex(const Scope& scope, const std::wstring& key, const std::wstring& clsid, std::wstring& log) {
    std::wstring existing;
    const ValueKind kind = ReadValue(scope.root, key, nullptr, existing);
    if (kind == ValueKind::Other) {
        Append(log, L"  WARNING: " + std::wstring(scope.name) + L"\\" + key +
                        L" holds a non-string / unreadable value; left alone");
        return true;
    }
    const bool ours = kind == ValueKind::String && SameText(existing, clsid);
    if (!SetString(scope.root, scope.recordKey, key.c_str(), L"1")) {
        Append(log, L"  ERROR: could not write to the record list: " + std::wstring(scope.name) + L"\\" +
                        scope.recordKey);
        return false;
    }
    if (ours) return true;
    if (kind == ValueKind::String) {
        if (!SetString(scope.root, kBackupKey, key.c_str(), existing)) {
            Append(log, L"  ERROR: could not write backup; " + std::wstring(scope.name) + L"\\" + key + L" = " +
                            existing + L" left unchanged");
            return false;
        }
        Append(log, L"  note: " + std::wstring(scope.name) + L"\\" + key + L" = " + existing +
                        L" backed up and replaced");
    } else if (!DeleteValue(scope.root, kBackupKey, key.c_str())) {
        Append(log, L"  ERROR: could not delete old backup: " + key);
        return false;
    }
    if (!SetString(scope.root, key, nullptr, clsid)) {
        Append(log, L"  ERROR: could not write: " + std::wstring(scope.name) + L"\\" + key);
        return false;
    }
    return true;
}

// If the value is still ours, restores / deletes it; otherwise leaves it alone. On success the backup and record
// values are dropped.
bool RemoveShellex(const Scope& scope, const std::wstring& key, const std::wstring& clsid, std::wstring& log) {
    std::wstring current;
    std::wstring backup;
    const ValueKind currentKind = ReadValue(scope.root, key, nullptr, current);
    const ValueKind backupKind = ReadValue(scope.root, kBackupKey, key.c_str(), backup);
    bool ok = true;
    if (currentKind == ValueKind::String && SameText(current, clsid)) {
        bool restore = backupKind == ValueKind::String && (backup.empty() || ClsidExists(backup));
        if (backupKind == ValueKind::String && !restore) {
            Append(log, L"  note: backed-up handler is no longer registered (" + backup + L"), not restored: " +
                            std::wstring(scope.name) + L"\\" + key);
        }
        // If the .3mf preview backup is lost, Windows' own 3MF previewer is restored (if registered).
        if (backupKind == ValueKind::Absent && scope.root == HKEY_LOCAL_MACHINE &&
            SameText(key, HandlerKey(PrimaryShellexKey(L".3mf"), kPreviewHandlerGuid)) &&
            ClsidExists(kMicrosoft3mfPreviewClsid)) {
            backup = kMicrosoft3mfPreviewClsid;
            restore = true;
            Append(log, L"  note: no .3mf preview backup; restored the Windows 3MF previewer");
        }
        if (restore) {
            ok = SetString(scope.root, key, nullptr, backup);
        } else {
            ok = DeleteValue(scope.root, key, nullptr);
            if (ok) DeleteEmptyKeysUpward(scope.root, key);
        }
        if (!ok) Append(log, L"  ERROR: could not undo: " + std::wstring(scope.name) + L"\\" + key);
    }
    if (ok) {
        ok = DeleteValue(scope.root, kBackupKey, key.c_str()) && DeleteValue(scope.root, scope.recordKey, key.c_str());
        if (!ok) Append(log, L"  ERROR: could not delete backup/record value: " + key);
    }
    return ok;
}

// Removal candidates: recorded keys + all backup names + computed keys (only our form).
std::vector<std::wstring> RemovalKeys(const Scope& scope, const std::vector<std::wstring>& progIds) {
    std::vector<std::wstring> candidates;
    AddExtensionLinkKeys(candidates);
    for (const std::wstring& progId : progIds) AddLinkKeys(candidates, ProgIdShellexKey(progId));
    for (const std::wstring& name : ValueNames(scope.root, scope.recordKey)) AddUnique(candidates, name);
    for (const std::wstring& name : ValueNames(scope.root, kBackupKey)) AddUnique(candidates, name);
    std::vector<std::wstring> keys;
    for (const std::wstring& key : candidates) {
        // A sibling of a recorded / backed-up key is a candidate too (an old version backed up only foreign values;
        // the other handler entry it wrote to the same ProgID can only be found this way).
        if (!OurClsidForHandlerKey(key).empty()) AddLinkKeys(keys, ParentKey(key));
    }
    return keys;
}

bool RemoveAllLinks(const Scope& scope, const std::vector<std::wstring>& progIds, std::wstring& log) {
    bool ok = true;
    for (const std::wstring& key : RemovalKeys(scope, progIds)) {
        if (!RemoveShellex(scope, key, OurClsidForHandlerKey(key), log)) ok = false;
    }
    // Keys are deleted only if empty: they stay if a backup could not be restored.
    DeleteKeyIfEmpty(scope.root, scope.recordKey);
    DeleteKeyIfEmpty(scope.root, kBackupKey);
    DeleteKeyIfEmpty(scope.root, kAppKey);
    return ok;
}

// Links in this scope whose value is our CLSID (for the status report).
std::vector<std::wstring> LinksPointingAtUs(const Scope& scope, const std::vector<std::wstring>& progIds) {
    std::vector<std::wstring> found;
    for (const std::wstring& key : RemovalKeys(scope, progIds)) {
        std::wstring value;
        if (ReadString(scope.root, key, nullptr, value) && SameText(value, OurClsidForHandlerKey(key))) {
            found.push_back(key);
        }
    }
    return found;
}

void NotifyShell() { SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr); }

// Removes entries left by earlier experiments (launcher class and its AppID, the preview host's LocalServer32).
void DeleteObsoleteKeys(HKEY root) {
    DeleteTree(root, ClassesKey(L"CLSID\\" + GuidToString(CLSID_StlLaunchBroker)));
    DeleteTree(root, ClassesKey(L"AppID\\" + GuidToString(APPID_StlLaunchBroker)));
    DeleteTree(root, ClassesKey(L"CLSID\\" + PreviewClsid() + L"\\LocalServer32"));
}

// Two CLSIDs: InprocServer32; the preview runs in the Windows preview process (prevhost).
bool WriteClasses(HKEY root, const std::wstring& dllPath) {
    const std::wstring thumbnailKey = ClassesKey(L"CLSID\\" + ThumbnailClsid());
    const std::wstring previewKey = ClassesKey(L"CLSID\\" + PreviewClsid());
    bool ok = true;
    ok = ok && SetString(root, thumbnailKey, nullptr, kThumbnailProviderFriendlyName);
    ok = ok && SetString(root, thumbnailKey + L"\\InprocServer32", nullptr, dllPath);
    ok = ok && SetString(root, thumbnailKey + L"\\InprocServer32", L"ThreadingModel", L"Apartment");
    ok = ok && SetString(root, previewKey, nullptr, kPreviewHandlerFriendlyName);
    ok = ok && SetString(root, previewKey + L"\\InprocServer32", nullptr, dllPath);
    ok = ok && SetString(root, previewKey + L"\\InprocServer32", L"ThreadingModel", L"Apartment");
    ok = ok && SetString(root, previewKey, L"AppID", kPrevhostAppId);
    {
        // DisableLowILProcessIsolation = 1: prevhost starts at NORMAL integrity level. Otherwise a program opened from
        // the preview (e.g. Creality Print) also started at low level and crashed; settings and logs could not be
        // written. Tried and did not work (2026-11-16, Windows 11 26H2):
        //   - our own LocalServer32 host + DisableLowILProcessIsolation: Explorer never started the handler,
        //   - ShellExecute via the Explorer site: the site does not provide SID_STopLevelBrowser (0x80004001),
        //   - RunAs = Interactive User launcher: called from low level, the launcher also started at low level,
        //   - explorer.exe <file> from low level: nothing opened.
        const DWORD one = 1;
        ok = ok && RegSetKeyValueW(root, previewKey.c_str(), L"DisableLowILProcessIsolation", REG_DWORD, &one,
                                   sizeof(one)) == ERROR_SUCCESS;
    }
    DeleteObsoleteKeys(root);
    return ok;
}

bool DeleteClasses(HKEY root) {
    DeleteObsoleteKeys(root);
    DeleteTree(root, ClassesKey(L"AppID\\" + GuidToString(APPID_StlPreviewHandler)));
    const std::wstring thumbnailKey = ClassesKey(L"CLSID\\" + ThumbnailClsid());
    const std::wstring previewKey = ClassesKey(L"CLSID\\" + PreviewClsid());
    DeleteTree(root, thumbnailKey);
    DeleteTree(root, previewKey);
    DeleteKeyIfEmpty(root, ClassesKey(L"CLSID"));  // may not have existed in HKCU before install
    DeleteKeyIfEmpty(root, ClassesKey(L"AppID"));
    return !KeyExists(root, thumbnailKey) && !KeyExists(root, previewKey);
}

}  // namespace

// User step of the machine-wide install: removes this user's old entries and checks the HKLM registration.
bool CleanUserForMachineInstall(std::wstring& log) {
    std::wstring cleanup;
    const bool cleaned = UnregisterShellExtension(cleanup);
    const bool machine = KeyExists(HKEY_LOCAL_MACHINE, ClassesKey(L"CLSID\\" + ThumbnailClsid() + L"\\InprocServer32")) &&
                         KeyExists(HKEY_LOCAL_MACHINE, ClassesKey(L"CLSID\\" + PreviewClsid() + L"\\InprocServer32"));
    Append(log, cleaned ? L"Cleaned this user's HKCU entries (the machine-wide install does not write to HKCU)."
                        : L"Some of this user's HKCU entries could not be removed:");
    if (!cleaned) log += cleanup;
    if (!machine) Append(log, L"  WARNING: no component registration in HKLM; run 'register-machine' as admin first");
    for (const wchar_t* extension : kModelExtensions) {
        const std::pair<const wchar_t*, std::wstring> handlers[] = {{kThumbnailHandlerGuid, ThumbnailClsid()},
                                                                    {kPreviewHandlerGuid, PreviewClsid()}};
        for (const auto& handler : handlers) {
            const std::wstring winner = EffectiveShellExtension(extension, handler.first);
            if (!winner.empty() && !SameText(winner, handler.second)) {
                Append(log, L"  WARNING: the shell uses another handler for " + std::wstring(extension) + L": " +
                                winner);
            }
        }
    }
    return cleaned && machine;
}

bool IsValidProgId(const std::wstring& progId) {
    if (progId.empty() || progId.size() > 128 || progId[0] == L'.') return false;
    for (const wchar_t c : progId) {
        const bool allowed = (c >= L'A' && c <= L'Z') || (c >= L'a' && c <= L'z') || (c >= L'0' && c <= L'9') ||
                             c == L'.' || c == L'_' || c == L'-' || c == L'+';
        if (!allowed) return false;
    }
    if (StartsWithText(progId, L"AppX")) return false;
    static const wchar_t* const kReserved[] = {L"CLSID", L"AppID", L"Interface", L"TypeLib", L"SystemFileAssociations",
                                               L"Wow6432Node", L"DirectShow", L"MIME", L"Media Type"};
    for (const wchar_t* reserved : kReserved) {
        if (SameText(progId, reserved)) return false;
    }
    return true;
}

std::vector<std::wstring> CurrentUserProgIds() {
    std::vector<std::wstring> progIds;
    AddExtensionProgIds(progIds, HKEY_CLASSES_ROOT, false);
    return progIds;
}

bool RegisterMachineClasses(const std::wstring& dllPath, std::wstring& log, const std::vector<std::wstring>& progIds) {
    log.clear();
    if (dllPath.empty() || PathIsRelativeW(dllPath.c_str())) {
        Append(log, L"The STLPreview.dll path must be absolute: " + dllPath);
        return false;
    }
    for (const std::wstring& progId : progIds) {
        if (!IsValidProgId(progId)) {
            Append(log, L"Invalid ProgID: " + progId);
            return false;
        }
    }

    // ProgIDs to link: the given ones (or the merged HKCR if none) + the extension ProgIDs in HKLM. Only ProgIDs that
    // already exist in HKLM are written to (no new ProgID is created).
    std::vector<std::wstring> candidates = progIds;
    if (candidates.empty()) AddExtensionProgIds(candidates, HKEY_CLASSES_ROOT, false);
    AddExtensionProgIds(candidates, HKEY_LOCAL_MACHINE, true);
    AddLoadedUserProgIds(candidates);
    std::vector<std::wstring> targets;
    AddExtensionLinkKeys(targets);
    std::wstring notes;
    for (const std::wstring& progId : candidates) {
        if (KeyExists(HKEY_LOCAL_MACHINE, ClassesKey(progId))) {
            AddLinkKeys(targets, ProgIdShellexKey(progId));
        } else {
            Append(notes, L"  note: ProgID not in HKLM, skipped: " + progId);
        }
    }

    bool ok = WriteClasses(HKEY_LOCAL_MACHINE, dllPath);
    if (!ok) Append(notes, L"  ERROR: could not write under HKLM\\Software\\Classes\\CLSID");
    for (const std::wstring& key : targets) {
        if (!ok) break;
        ok = WriteShellex(kMachine, key, OurClsidForHandlerKey(key), notes);
    }
    if (ok) {
        // Preview handler list (HKLM): some Explorer versions only preview the handlers listed here.
        ok = SetString(HKEY_LOCAL_MACHINE, kPreviewHandlersKey, PreviewClsid().c_str(), kPreviewHandlerFriendlyName);
        if (!ok) Append(notes, L"  ERROR: could not write to the PreviewHandlers list");
    }

    if (!ok) {
        // Roll back the partial registration: only keys that carry our CLSID.
        std::wstring rollback;
        const bool rolledBack = UnregisterMachineClasses(rollback, candidates);
        Append(log, L"Machine-wide registration FAILED (requires admin rights).");
        log += notes;
        Append(log, rolledBack ? L"Changes were rolled back." : L"WARNING: changes could not be fully rolled back:");
        if (!rolledBack) log += rollback;
        return false;
    }

    NotifyShell();
    Append(log, L"Registered machine-wide (HKLM):");
    Append(log, L"  DLL:   " + dllPath);
    Append(log, L"  host:  Windows preview process (prevhost), normal integrity level");
    std::wstring progIdText;
    for (const std::wstring& progId : candidates) progIdText += (progIdText.empty() ? L"" : L", ") + progId;
    Append(log, L"  .stl/.3mf links: extension, SystemFileAssociations" +
                    (progIdText.empty() ? std::wstring() : L", ProgID: " + progIdText));
    log += notes;
    return true;
}

bool UnregisterMachineClasses(std::wstring& log, const std::vector<std::wstring>& progIds) {
    log.clear();
    std::vector<std::wstring> allProgIds;
    for (const std::wstring& progId : progIds) {
        if (IsValidProgId(progId)) AddUnique(allProgIds, progId);
    }
    AddExtensionProgIds(allProgIds, HKEY_CLASSES_ROOT, false);
    AddExtensionProgIds(allProgIds, HKEY_LOCAL_MACHINE, true);
    AddLoadedUserProgIds(allProgIds);

    std::wstring notes;
    const bool linksRemoved = RemoveAllLinks(kMachine, allProgIds, notes);
    if (!linksRemoved) {
        // A link may still point at us: deleting the CLSID would break that type's thumbnails for everyone.
        Append(log, L"Some HKLM links could not be removed (requires admin rights); component registration, backups "
                    L"and files are kept.");
        log += notes;
        return false;
    }
    DeleteValue(HKEY_LOCAL_MACHINE, kPreviewHandlersKey, PreviewClsid().c_str());
    NotifyShell();
    const bool gone = DeleteClasses(HKEY_LOCAL_MACHINE);
    Append(log, gone ? L"Machine-wide registration removed (HKLM)."
                     : L"Could not remove the HKLM component registration (requires admin rights)");
    log += notes;
    return gone;
}

bool UnregisterShellExtension(std::wstring& log) {
    log.clear();
    // A CLSID registration in HKCU shadows the one in HKLM: it is deleted for the machine-wide install too.
    bool ok = DeleteClasses(HKEY_CURRENT_USER);
    DeleteValue(HKEY_CURRENT_USER, kPreviewHandlersKey, PreviewClsid().c_str());

    std::wstring notes;
    if (!RemoveAllLinks(kUser, CurrentUserProgIds(), notes)) ok = false;
    NotifyShell();
    Append(log, ok ? L"User (HKCU) registration removed."
                   : L"Part of the user (HKCU) registration could not be removed.");
    log += notes;
    return ok;
}

bool IsShellExtensionRegistered(std::wstring& log) {
    log.clear();
    bool ok = true;

    const std::wstring thumbnailClsid = ThumbnailClsid();
    const std::wstring previewClsid = PreviewClsid();
    const bool machineClasses = KeyExists(HKEY_LOCAL_MACHINE, ClassesKey(L"CLSID\\" + thumbnailClsid)) ||
                                KeyExists(HKEY_LOCAL_MACHINE, ClassesKey(L"CLSID\\" + previewClsid));
    const bool userClasses = KeyExists(HKEY_CURRENT_USER, ClassesKey(L"CLSID\\" + thumbnailClsid)) ||
                             KeyExists(HKEY_CURRENT_USER, ClassesKey(L"CLSID\\" + previewClsid));
    if (machineClasses && userClasses) {
        Append(log, L"Component registration: HKLM and HKCU (WARNING: the HKCU entry shadows the machine-wide one)");
    } else if (machineClasses) {
        Append(log, L"Component registration: machine-wide (HKLM)");
    } else if (userClasses) {
        Append(log, L"Component registration: user (HKCU)");
    } else {
        Append(log, L"Component registration: NONE");
        ok = false;
    }

    // What counts is the handler the shell will actually pick (ProgID, extension, SystemFileAssociations; HKCU
    // shadows HKLM).
    for (const wchar_t* extension : kModelExtensions) {
        const struct {
            const wchar_t* guid;
            const std::wstring& clsid;
            const wchar_t* label;
        } handlers[] = {{kThumbnailHandlerGuid, thumbnailClsid, L" thumbnail: "},
                        {kPreviewHandlerGuid, previewClsid, L" preview:   "}};
        for (const auto& handler : handlers) {
            const std::wstring winner = EffectiveShellExtension(extension, handler.guid);
            if (SameText(winner, handler.clsid)) {
                Append(log, extension + std::wstring(handler.label) + L"active");
            } else {
                Append(log, extension + std::wstring(handler.label) + L"NOT ACTIVE" +
                                (winner.empty() ? std::wstring(L" (no handler)") : L" (active: " + winner + L")"));
                ok = false;
            }
        }
    }

    std::wstring dllPath;
    if (ReadString(HKEY_CLASSES_ROOT, L"CLSID\\" + thumbnailClsid + L"\\InprocServer32", nullptr, dllPath)) {
        Append(log, L"DLL:  " + dllPath);
        if (PathIsRelativeW(dllPath.c_str())) {
            Append(log, L"WARNING: DLL path is not absolute");
            ok = false;
        } else if (GetFileAttributesW(dllPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
            Append(log, L"WARNING: no DLL file at this path");
            ok = false;
        }
    } else {
        Append(log, L"DLL:  no path set");
        ok = false;
    }

    std::wstring value;
    if (ReadString(HKEY_CLASSES_ROOT, L"CLSID\\" + previewClsid + L"\\LocalServer32", nullptr, value)) {
        Append(log, L"WARNING: old preview host registration (LocalServer32) still present: " + value);
        ok = false;
    } else if (ReadString(HKEY_CLASSES_ROOT, L"CLSID\\" + previewClsid, L"AppID", value) &&
               SameText(value, kPrevhostAppId)) {
        DWORD flag = 0;
        DWORD size = sizeof(flag);
        const bool normal = RegGetValueW(HKEY_CLASSES_ROOT, (L"CLSID\\" + previewClsid).c_str(),
                                         L"DisableLowILProcessIsolation", RRF_RT_REG_DWORD, nullptr, &flag,
                                         &size) == ERROR_SUCCESS &&
                            flag == 1;
        Append(log, normal ? L"Host: Windows preview process (prevhost), normal integrity level"
                           : L"Host: Windows preview process (prevhost), low integrity level");
    } else {
        Append(log, L"Host: not registered");
        ok = false;
    }

    std::vector<std::wstring> progIds = CurrentUserProgIds();
    AddExtensionProgIds(progIds, HKEY_LOCAL_MACHINE, true);
    const std::vector<std::wstring> machineLinks = LinksPointingAtUs(kMachine, progIds);
    const std::vector<std::wstring> userLinks = LinksPointingAtUs(kUser, progIds);
    Append(log, L"HKLM links: " + std::to_wstring(machineLinks.size()));
    for (const std::wstring& key : machineLinks) Append(log, L"  HKLM\\" + key);
    Append(log, L"HKCU links: " + std::to_wstring(userLinks.size()) +
                    (machineClasses && !userLinks.empty()
                         ? L" (WARNING: should not exist with a machine-wide install; 'register --machine-classes' "
                           L"cleans them)"
                         : L""));
    for (const std::wstring& key : userLinks) Append(log, L"  HKCU\\" + key);
    const size_t backups = ValueNames(HKEY_LOCAL_MACHINE, kBackupKey).size() + ValueNames(HKEY_CURRENT_USER, kBackupKey).size();
    Append(log, L"Backed-up foreign handlers: " + std::to_wstring(backups));
    return ok;
}
