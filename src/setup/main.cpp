#include "registration.h"

#include <windows.h>

#include <cstdio>
#include <fcntl.h>
#include <io.h>
#include <string>
#include <vector>

namespace {
std::wstring ExecutableDirectory() {
    wchar_t path[MAX_PATH * 2] = {};
    const DWORD length = GetModuleFileNameW(nullptr, path, ARRAYSIZE(path));
    if (length == 0 || length >= ARRAYSIZE(path)) return std::wstring();
    std::wstring full(path, length);
    const size_t slash = full.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return std::wstring();
    return full.substr(0, slash + 1);
}

void PrintUsage() {
    wprintf(L"Usage:\r\n");
    wprintf(L"  stlprev-setup register-machine [path to STLPreview.dll] [--progid <ProgID>]...   (admin)\r\n");
    wprintf(L"      Registers in HKLM. --progid: the installing user's .stl/.3mf ProgIDs (see user-progids);\r\n");
    wprintf(L"      HKCR, HKLM and loaded user hives are read too. On failure, the changes are rolled back.\r\n");
    wprintf(L"  stlprev-setup unregister-machine [--progid <ProgID>]...                           (admin)\r\n");
    wprintf(L"  stlprev-setup register --machine-classes\r\n");
    wprintf(L"      user step of the machine-wide install: removes this user's old HKCU entries\r\n");
    wprintf(L"  stlprev-setup unregister                         this user's HKCU entries\r\n");
    wprintf(L"  stlprev-setup user-progids                       this user's ProgIDs (one line)\r\n");
    wprintf(L"  stlprev-setup delete-on-reboot <path>...         deletes; if locked, on reboot (admin)\r\n");
    wprintf(L"  stlprev-setup status\r\n");
    wprintf(L"Exit codes: 0 ok, 1 usage, 2 file missing / reboot required, 3 failed, 4 not registered\r\n");
}

// If a relative path (".\STLPreview.dll") were written to the registry as is, COM would resolve it against
// Explorer's / dllhost's folder: convert it to a full path.
std::wstring FullPath(const std::wstring& path) {
    const DWORD needed = GetFullPathNameW(path.c_str(), 0, nullptr, nullptr);
    if (needed == 0) return path;
    std::wstring full(needed, L'\0');
    const DWORD written = GetFullPathNameW(path.c_str(), needed, &full[0], nullptr);
    if (written == 0 || written >= needed) return path;
    full.resize(written);
    return full;
}

// Deletes; if that fails (e.g. Explorer keeps the DLL loaded), marks it for deletion on reboot. Folders are deleted
// only if empty (on reboot too). 0: deleted / absent, 2: marked, 3: failed.
int DeleteOnReboot(const std::wstring& rawPath) {
    const std::wstring path = FullPath(rawPath);
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) return 0;
    const bool directory = (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    if (directory ? RemoveDirectoryW(path.c_str()) : DeleteFileW(path.c_str())) return 0;
    if (MoveFileExW(path.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT)) {
        wprintf(L"will be deleted on reboot: %s\r\n", path.c_str());
        return 2;
    }
    wprintf(L"could not delete: %s (error %lu)\r\n", path.c_str(), GetLastError());
    return 3;
}
}  // namespace

int wmain(int argc, wchar_t** argv) {
    // UTF-16 to the console, UTF-8 to a pipe/file (non-ASCII characters show correctly in both cases).
    const HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
    const DWORD outputType = output && output != INVALID_HANDLE_VALUE ? GetFileType(output) : FILE_TYPE_UNKNOWN;
    _setmode(_fileno(stdout), outputType == FILE_TYPE_CHAR ? _O_U16TEXT : _O_U8TEXT);

    if (argc < 2) {
        PrintUsage();
        return 1;
    }

    const std::wstring command = argv[1];
    std::wstring log;

    // Common options: --machine-classes, --progid <name> (an empty name is ignored; scripts may pass empty
    // placeholders).
    std::wstring path;
    bool machineClasses = false;
    std::vector<std::wstring> progIds;
    std::vector<std::wstring> paths;
    for (int i = 2; i < argc; ++i) {
        const std::wstring arg = argv[i];
        if (arg == L"--machine-classes") {
            machineClasses = true;
        } else if (arg == L"--progid") {
            if (i + 1 >= argc) {
                PrintUsage();
                return 1;
            }
            const std::wstring progId = argv[++i];
            if (progId.empty()) continue;
            if (!IsValidProgId(progId)) {
                wprintf(L"Invalid ProgID: %s\r\n", progId.c_str());
                return 1;
            }
            progIds.push_back(progId);
        } else {
            paths.push_back(arg);
        }
    }
    if (!paths.empty()) path = paths.front();

    if (command == L"register" || command == L"register-machine") {
        // User-only (HKCU) registration was removed: Windows 11 26H2 ignores it, and on other versions the preview
        // ran at low integrity and lost the "Open with" bar. "register" works only with --machine-classes.
        if (paths.size() > 1 || (command == L"register" && (!progIds.empty() || !paths.empty() || !machineClasses)) ||
            (command == L"register-machine" && machineClasses)) {
            PrintUsage();
            return 1;
        }
        if (command == L"register") {
            // User step of the machine-wide install: no links written to HKCU, no DLL path needed.
            const bool ok = CleanUserForMachineInstall(log);
            wprintf(L"%s", log.c_str());
            return ok ? 0 : 3;
        }
        if (path.empty()) path = ExecutableDirectory() + L"STLPreview.dll";
        path = FullPath(path);
        if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
            wprintf(L"DLL file not found: %s\r\n", path.c_str());
            return 2;
        }
        const bool ok = RegisterMachineClasses(path, log, progIds);
        wprintf(L"%s", log.c_str());
        return ok ? 0 : 3;
    }

    if (command == L"unregister" || command == L"unregister-machine") {
        if (!paths.empty() || machineClasses || (command == L"unregister" && !progIds.empty())) {
            PrintUsage();
            return 1;
        }
        const bool ok = command == L"unregister" ? UnregisterShellExtension(log) : UnregisterMachineClasses(log, progIds);
        wprintf(L"%s", log.c_str());
        return ok ? 0 : 3;
    }

    if (command == L"user-progids") {
        std::wstring line;
        for (const std::wstring& progId : CurrentUserProgIds()) line += (line.empty() ? L"" : L" ") + progId;
        wprintf(L"%s\r\n", line.c_str());
        return 0;
    }

    if (command == L"delete-on-reboot") {
        if (paths.empty() || machineClasses || !progIds.empty()) {
            PrintUsage();
            return 1;
        }
        int result = 0;
        for (const std::wstring& item : paths) {
            const bool drive = item.size() >= 3 && item[1] == L':' && (item[2] == L'\\' || item[2] == L'/');
            const bool unc = item.rfind(L"\\\\", 0) == 0;
            if (!drive && !unc) {
                wprintf(L"Path must be absolute: %s\r\n", item.c_str());
                return 1;
            }
        }
        for (const std::wstring& item : paths) {
            const int one = DeleteOnReboot(item);
            if (one > result) result = one;
        }
        return result;
    }

    if (command == L"status") {
        const bool ok = IsShellExtensionRegistered(log);
        wprintf(L"%s", log.c_str());
        return ok ? 0 : 4;
    }

    PrintUsage();
    return 1;
}
