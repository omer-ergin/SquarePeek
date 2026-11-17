#pragma once

#include <string>
#include <vector>

// User step of the machine-wide install ("register --machine-classes"). NOTHING is written to HKCU; this user's old
// entries of ours (links, CLSIDs) are removed, their backups restored, and the HKLM registration is checked.
bool CleanUserForMachineInstall(std::wstring& log);
// Removes this user's old HKCU entries (only links that still point at us; backups are restored).
bool UnregisterShellExtension(std::wstring& log);

// Requires admin rights (HKLM). progIds: the installing user's .stl/.3mf ProgIDs (read in the normal user step and
// passed in, because the admin step may run under another account; see CurrentUserProgIds). The merged HKCR, HKLM
// and all loaded user hives (HKEY_USERS\<SID>_Classes) are read as well. If registration fails, the changes are
// rolled back.
bool RegisterMachineClasses(const std::wstring& dllPath, std::wstring& log,
                            const std::vector<std::wstring>& progIds = {});
// Only touches keys that still carry our CLSID. If a link cannot be removed, the CLSIDs and backups are NOT deleted
// and it returns false.
bool UnregisterMachineClasses(std::wstring& log, const std::vector<std::wstring>& progIds = {});
bool IsShellExtensionRegistered(std::wstring& log);

// ProgIDs of this user's .stl/.3mf extensions (merged HKCR, validated, no duplicates).
std::vector<std::wstring> CurrentUserProgIds();
// Only ProgIDs of [A-Za-z0-9._+-], at most 128 chars, not starting with '.', not AppX and not a reserved name.
bool IsValidProgId(const std::wstring& progId);
