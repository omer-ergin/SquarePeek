Unicode true

; SquarePeek installer (NSIS 3) - MACHINE-WIDE. Requires admin rights; this signed installer must be the only admin
; entry point on the machine (install-machine*.cmd is for developers only).
; Files: $PROGRAMFILES64\STLPreview (STLPreview.dll + stlprev-setup.exe). Registration and removal are done by
; stlprev-setup.exe (src/registration.cpp): two CLSIDs and the .stl/.3mf shellex links are written to HKLM, overwritten
; handlers are backed up under HKLM\Software\STLPreview\ShellexBackup and restored on uninstall. No link is written to
; HKCU. The program that opens files on double-click (ProgID default, UserChoice, OpenWithProgids) is NOT CHANGED.

!define APP_NAME "SquarePeek"
!ifndef APP_VERSION
  !define APP_VERSION "1.1.0"
!endif
!define APP_PUBLISHER "STLPreview"
!define APP_KEY "Software\STLPreview"
!define BACKUP_KEY "Software\STLPreview\ShellexBackup"
!define UNINSTALL_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\STLPreview"

!ifndef SOURCE_DIR
  !define SOURCE_DIR "..\build\bin\Release"
!endif
!ifndef OUT_DIR
  !define OUT_DIR "..\dist"
!endif

Name "${APP_NAME}"
OutFile "${OUT_DIR}\SquarePeek-Setup-${APP_VERSION}.exe"
InstallDir "$PROGRAMFILES64\STLPreview"
RequestExecutionLevel admin
SetCompressor /SOLID lzma
BrandingText "${APP_NAME} ${APP_VERSION}"

!include "MUI2.nsh"
!include "LogicLib.nsh"
!include "FileFunc.nsh"
!include "x64.nsh"

; Icon: assets/app.ico (the same icon is embedded in STLPreview.dll; DisplayIcon points there).
!define MUI_ICON "${__FILEDIR__}\..\assets\app.ico"
!define MUI_UNICON "${__FILEDIR__}\..\assets\app.ico"
!define MUI_ABORTWARNING
!define MUI_WELCOMEPAGE_TITLE "$(WelcomeTitle)"
!define MUI_WELCOMEPAGE_TEXT "$(WelcomeText)"
!define MUI_FINISHPAGE_TITLE "$(FinishTitle)"
!define MUI_FINISHPAGE_TEXT "$(FinishText)"
!define MUI_FINISHPAGE_NOAUTOCLOSE

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES

!insertmacro MUI_LANGUAGE "English"
!insertmacro MUI_LANGUAGE "Turkish"

VIProductVersion "${APP_VERSION}.0"
VIAddVersionKey /LANG=${LANG_ENGLISH} "ProductName" "${APP_NAME}"
VIAddVersionKey /LANG=${LANG_ENGLISH} "FileDescription" "STL/3MF thumbnail and preview setup for Windows Explorer"
VIAddVersionKey /LANG=${LANG_ENGLISH} "FileVersion" "${APP_VERSION}"
VIAddVersionKey /LANG=${LANG_ENGLISH} "ProductVersion" "${APP_VERSION}"
VIAddVersionKey /LANG=${LANG_ENGLISH} "CompanyName" "${APP_PUBLISHER}"
VIAddVersionKey /LANG=${LANG_ENGLISH} "LegalCopyright" "MIT; based on DMarkovIT/thumbnail-stl"

LangString WelcomeTitle ${LANG_ENGLISH} "${APP_NAME} Setup"
LangString WelcomeTitle ${LANG_TURKISH} "${APP_NAME} Kurulumu"
LangString WelcomeText ${LANG_ENGLISH} "Thumbnails and a 3D preview with a measuring bed for STL and 3MF files in Windows Explorer.$\r$\n$\r$\nThe installation is machine-wide (all users) and requires admin rights. Files are copied to $PROGRAMFILES64\STLPreview.$\r$\n$\r$\nThe program that opens files on double-click does NOT change. Replaced thumbnail/preview handlers (e.g. Creality Print) are backed up and restored on uninstall."
LangString WelcomeText ${LANG_TURKISH} "Windows Gezgini'nde STL ve 3MF dosyaları için küçük resimler ve ölçüm tablalı 3B önizleme.$\r$\n$\r$\nKurulum bilgisayar genelidir (bütün kullanıcılar) ve yönetici hakkı gerektirir. Dosyalar $PROGRAMFILES64\STLPreview altına kopyalanır.$\r$\n$\r$\nDosyaların çift tıklayınca açıldığı program DEĞİŞMEZ. Değiştirilen küçük resim/önizleme işleyicileri (ör. Creality Print) yedeklenir ve kaldırmada geri yüklenir."
LangString FinishTitle ${LANG_ENGLISH} "Completed"
LangString FinishTitle ${LANG_TURKISH} "Tamamlandı"
LangString FinishText ${LANG_ENGLISH} "Thumbnails and the preview pane for .stl and .3mf files are now enabled for all users on this computer. The program that opens on double-click did not change.$\r$\n$\r$\nSelect a file in Explorer and press Alt+P to open the preview pane; click the pane and press H to show the shortcuts.$\r$\n$\r$\nExplorer caches thumbnails: if an old file keeps its previous icon, try a new file."
LangString FinishText ${LANG_TURKISH} ".stl ve .3mf dosyaları için küçük resimler ve önizleme bölmesi bu bilgisayardaki bütün kullanıcılar için etkinleştirildi. Çift tıklamayla açılan program değişmedi.$\r$\n$\r$\nGezgin'de bir dosya seçip Alt+P ile önizleme bölmesini açın; bölmeye tıklayıp H tuşuna basınca kısayollar görünür.$\r$\n$\r$\nGezgin küçük resimleri önbelleğe alır: eski bir dosyada önceki simge kalırsa yeni bir dosyayla deneyin."
LangString MsgNeedAdmin ${LANG_ENGLISH} "Admin rights are required."
LangString MsgNeedAdmin ${LANG_TURKISH} "Yönetici hakkı gerekir."
LangString MsgNeed64 ${LANG_ENGLISH} "64-bit Windows is required."
LangString MsgNeed64 ${LANG_TURKISH} "64 bit Windows gerekir."
LangString DetCopying ${LANG_ENGLISH} "Copying files"
LangString DetCopying ${LANG_TURKISH} "Dosyalar kopyalanıyor"
LangString DetRegistering ${LANG_ENGLISH} "Registering components (HKLM)"
LangString DetRegistering ${LANG_TURKISH} "Bileşenler kaydediliyor (HKLM)"
LangString DetRegFailed ${LANG_ENGLISH} "Registration failed (stlprev-setup: $0); rolling back"
LangString DetRegFailed ${LANG_TURKISH} "Kayıt başarısız (stlprev-setup: $0); geri alınıyor"
LangString MsgRegFailed ${LANG_ENGLISH} "Could not register the shell extension (stlprev-setup: $0). Changes were rolled back."
LangString MsgRegFailed ${LANG_TURKISH} "Kabuk uzantısı kaydedilemedi (stlprev-setup: $0). Yapılan değişiklikler geri alındı."
LangString DetCleanUser ${LANG_ENGLISH} "Removing this user's old (HKCU) entries"
LangString DetCleanUser ${LANG_TURKISH} "Bu kullanıcının eski (HKCU) kayıtları kaldırılıyor"
LangString DetCleanUserWarn ${LANG_ENGLISH} "Warning: user entries could not be fully removed (stlprev-setup: $0)"
LangString DetCleanUserWarn ${LANG_TURKISH} "Uyarı: kullanıcı kayıtları tam kaldırılamadı (stlprev-setup: $0)"
LangString DetOldUserDir ${LANG_ENGLISH} "Removing old per-user install: $OLD_USER_DIR"
LangString DetOldUserDir ${LANG_TURKISH} "Eski kullanıcı kurulumu kaldırılıyor: $OLD_USER_DIR"
LangString DisplayName ${LANG_ENGLISH} "SquarePeek (STL/3MF preview for Explorer)"
LangString DisplayName ${LANG_TURKISH} "SquarePeek (Gezgin için STL/3MF önizleme)"
LangString MsgNoSetup ${LANG_ENGLISH} "stlprev-setup.exe not found: $INSTDIR. Nothing was deleted."
LangString MsgNoSetup ${LANG_TURKISH} "stlprev-setup.exe bulunamadı: $INSTDIR. Hiçbir şey silinmedi."
LangString DetUnregUserWarn ${LANG_ENGLISH} "Warning: this account's HKCU entries could not be fully removed (stlprev-setup: $0)"
LangString DetUnregUserWarn ${LANG_TURKISH} "Uyarı: bu hesabın HKCU kayıtları tam kaldırılamadı (stlprev-setup: $0)"
LangString MsgUnregFailed ${LANG_ENGLISH} "Could not remove the registration (stlprev-setup: $0). Files and backups were not deleted; try again."
LangString MsgUnregFailed ${LANG_TURKISH} "Kayıt kaldırılamadı (stlprev-setup: $0). Dosyalar ve yedekler silinmedi; yeniden deneyin."

Var OLD_USER_DIR

; A DLL in use cannot be overwritten or deleted, but it can be renamed: try to delete first; if that fails, move it
; aside under a unique name and delete it on reboot.
!macro MoveAsideAndDelete DIR FILE
  ${If} ${FileExists} "${DIR}\${FILE}"
    Delete "${DIR}\${FILE}"
    ${If} ${FileExists} "${DIR}\${FILE}"
      System::Call 'kernel32::GetTickCount()i.r1'
      Rename "${DIR}\${FILE}" "${DIR}\${FILE}.old-$1"
      Delete /REBOOTOK "${DIR}\${FILE}.old-$1"
    ${EndIf}
  ${EndIf}
  ClearErrors
!macroend

!macro RequireAdmin
  UserInfo::GetAccountType
  Pop $0
  ${If} $0 != "admin"
    MessageBox MB_ICONSTOP|MB_OK "$(MsgNeedAdmin)" /SD IDOK
    SetErrorLevel 740
    Quit
  ${EndIf}
!macroend

Function .onInit
  ${IfNot} ${RunningX64}
    MessageBox MB_ICONSTOP|MB_OK "$(MsgNeed64)" /SD IDOK
    Abort
  ${EndIf}
  SetRegView 64
  !insertmacro RequireAdmin
  StrCpy $INSTDIR "$PROGRAMFILES64\STLPreview"
  ; Old per-user install (before 1.1.0: %LOCALAPPDATA%\Programs\STLPreview\<version>, HKCU registration).
  ReadRegStr $OLD_USER_DIR HKCU "${APP_KEY}" "InstallDir"
  ; The value comes from a user-writable key: only the old install's folder is accepted.
  StrCpy $0 "$LOCALAPPDATA\Programs\STLPreview\"
  StrLen $1 $0
  StrCpy $2 $OLD_USER_DIR $1
  ${If} $2 != $0
    StrCpy $OLD_USER_DIR ""
  ${EndIf}
FunctionEnd

Function un.onInit
  SetRegView 64
  !insertmacro RequireAdmin
FunctionEnd

Section "Install" SecInstall
  SectionIn RO
  SetRegView 64
  SetShellVarContext all
  SetOutPath "$INSTDIR"

  DetailPrint "$(DetCopying)"
  Delete /REBOOTOK "$INSTDIR\*.old-*"
  !insertmacro MoveAsideAndDelete "$INSTDIR" "STLPreview.dll"
  ; The preview host from before 1.1.0 is no longer used (the process is not killed; if locked, deleted on reboot).
  !insertmacro MoveAsideAndDelete "$INSTDIR" "STLPreviewHost.exe"
  File "${SOURCE_DIR}\STLPreview.dll"
  File "${SOURCE_DIR}\stlprev-setup.exe"

  ; HKLM registration. On failure stlprev-setup already rolls back its own changes; the files are deleted too.
  DetailPrint "$(DetRegistering)"
  nsExec::ExecToLog '"$INSTDIR\stlprev-setup.exe" register-machine "$INSTDIR\STLPreview.dll"'
  Pop $0
  ${If} $0 != "0"
    DetailPrint "$(DetRegFailed)"
    nsExec::ExecToLog '"$INSTDIR\stlprev-setup.exe" unregister-machine'
    Pop $1
    ${If} $1 == "0"
      !insertmacro MoveAsideAndDelete "$INSTDIR" "STLPreview.dll"
      Delete "$INSTDIR\stlprev-setup.exe"
      Delete "$INSTDIR\Uninstall.exe"
      RMDir "$INSTDIR"
      DeleteRegKey HKLM "${UNINSTALL_KEY}"
    ${EndIf}
    MessageBox MB_ICONSTOP|MB_OK "$(MsgRegFailed)" /SD IDOK
    SetErrorLevel 3
    Abort
  ${EndIf}

  ; This account's old HKCU entries (old per-user install, install.cmd, old machine-wide flow) are removed and their
  ; backups restored: HKCU never points at us. The installed stlprev-setup (in the admin folder) is used; the old
  ; user-writable copy is NEVER run as admin. Note: if another admin account was chosen at the UAC prompt, this step
  ; cleans that account's HKCU; the actual user should run "stlprev-setup register --machine-classes"
  ; ("stlprev-setup status" shows HKCU links).
  DetailPrint "$(DetCleanUser)"
  nsExec::ExecToLog '"$INSTDIR\stlprev-setup.exe" register --machine-classes'
  Pop $0
  ${If} $0 != "0"
    DetailPrint "$(DetCleanUserWarn)"
  ${EndIf}

  ; Files of the old per-user install: only this program's files; the folder only if empty.
  ${If} $OLD_USER_DIR != ""
    DetailPrint "$(DetOldUserDir)"
    Delete /REBOOTOK "$OLD_USER_DIR\STLPreview.dll.old-*"
    !insertmacro MoveAsideAndDelete "$OLD_USER_DIR" "STLPreview.dll"
    !insertmacro MoveAsideAndDelete "$OLD_USER_DIR" "STLPreviewHost.exe"
    Delete "$OLD_USER_DIR\stlprev-setup.exe"
    Delete "$OLD_USER_DIR\Uninstall.exe"
    RMDir /REBOOTOK "$OLD_USER_DIR"
    DeleteRegKey HKCU "${UNINSTALL_KEY}"
    DeleteRegValue HKCU "${APP_KEY}" "InstallDir"
    DeleteRegValue HKCU "${APP_KEY}" "Version"
    DeleteRegKey /ifempty HKCU "${APP_KEY}"
  ${EndIf}

  WriteUninstaller "$INSTDIR\Uninstall.exe"
  WriteRegStr HKLM "${UNINSTALL_KEY}" "DisplayName" "$(DisplayName)"
  WriteRegStr HKLM "${UNINSTALL_KEY}" "DisplayVersion" "${APP_VERSION}"
  WriteRegStr HKLM "${UNINSTALL_KEY}" "Publisher" "${APP_PUBLISHER}"
  WriteRegStr HKLM "${UNINSTALL_KEY}" "DisplayIcon" "$INSTDIR\STLPreview.dll"
  WriteRegStr HKLM "${UNINSTALL_KEY}" "InstallLocation" "$INSTDIR"
  WriteRegStr HKLM "${UNINSTALL_KEY}" "UninstallString" '"$INSTDIR\Uninstall.exe"'
  WriteRegStr HKLM "${UNINSTALL_KEY}" "QuietUninstallString" '"$INSTDIR\Uninstall.exe" /S'
  WriteRegDWORD HKLM "${UNINSTALL_KEY}" "NoModify" 1
  WriteRegDWORD HKLM "${UNINSTALL_KEY}" "NoRepair" 1
  ${GetSize} "$INSTDIR" "/S=0K" $0 $1 $2
  WriteRegDWORD HKLM "${UNINSTALL_KEY}" "EstimatedSize" $0
SectionEnd

Section "Uninstall"
  SetRegView 64
  SetShellVarContext all

  ${IfNot} ${FileExists} "$INSTDIR\stlprev-setup.exe"
    MessageBox MB_ICONSTOP|MB_OK "$(MsgNoSetup)" /SD IDOK
    SetErrorLevel 2
    Abort
  ${EndIf}

  ; 1) Registration. Only keys whose value is still our CLSID; backups are restored. On failure NO file or key is
  ;    deleted (Explorer links keep pointing at us and keep working).
  nsExec::ExecToLog '"$INSTDIR\stlprev-setup.exe" unregister'
  Pop $0
  ${If} $0 != "0"
    DetailPrint "$(DetUnregUserWarn)"
  ${EndIf}
  nsExec::ExecToLog '"$INSTDIR\stlprev-setup.exe" unregister-machine'
  Pop $0
  ${If} $0 != "0"
    MessageBox MB_ICONSTOP|MB_OK "$(MsgUnregFailed)" /SD IDOK
    SetErrorLevel 3
    Abort
  ${EndIf}

  ; 2) Files (if locked, moved aside and deleted on reboot).
  !insertmacro MoveAsideAndDelete "$INSTDIR" "STLPreview.dll"
  !insertmacro MoveAsideAndDelete "$INSTDIR" "STLPreviewHost.exe"
  Delete /REBOOTOK "$INSTDIR\*.old-*"
  Delete /REBOOTOK "$INSTDIR\stlprev-setup.exe"
  Delete /REBOOTOK "$INSTDIR\Uninstall.exe"
  RMDir /REBOOTOK "$INSTDIR"

  ; 3) Keys. Software\STLPreview is deleted only if no backup is left and it is empty. User settings
  ;    (HKCU\Software\STLPreview\Settings) are not deleted.
  DeleteRegKey HKLM "${UNINSTALL_KEY}"
  ClearErrors
  EnumRegValue $1 HKLM "${BACKUP_KEY}" 0
  ${If} $1 == ""
    DeleteRegKey /ifempty HKLM "${APP_KEY}"
  ${EndIf}
SectionEnd
