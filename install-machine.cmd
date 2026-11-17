@echo off
setlocal EnableExtensions DisableDelayedExpansion
rem Machine-wide install (developer path; for distribution the signed NSIS installer must be the only admin entry point).
rem  1) Reads this user's .stl/.3mf ProgIDs (the admin step may run under another account).
rem  2) As admin (install-machine-admin.cmd): files are copied to "%ProgramW6432%\STLPreview",
rem     verified and registered in HKLM. If registration fails, the changes are rolled back.
rem  3) This user: our old HKCU entries are removed (backups restored). NO links are written to HKCU.
rem The program that opens on double-click does not change.
set "SRC=%~dp0build\bin\Release"
if not defined ProgramW6432 (
  echo 64-bit Windows is required.
  exit /b 1
)
set "DST=%ProgramW6432%\STLPreview"
if not exist "%SRC%\STLPreview.dll" (
  echo Build first: build.cmd
  exit /b 1
)
if not exist "%SRC%\stlprev-setup.exe" (
  echo Build first: build.cmd
  exit /b 1
)

rem --- step 1: this user's ProgIDs (only [A-Za-z0-9._+-] names are printed) ---
set "STLP_PROGIDS="
for /f "usebackq delims=" %%A in (`""%SRC%\stlprev-setup.exe" user-progids"`) do set "STLP_PROGIDS=%%A"

rem --- step 2: admin (UAC). The result is reported only via the exit code; UAC cancel = 1223 ---
set "STLP_ADMIN=%~dp0install-machine-admin.cmd"
powershell -NoProfile -ExecutionPolicy Bypass -Command "try { $a = '/c \"\"' + $env:STLP_ADMIN + '\" ' + $env:STLP_PROGIDS + '\"'; $p = Start-Process -FilePath $env:ComSpec -ArgumentList $a -Verb RunAs -PassThru -ErrorAction Stop; $null = $p.Handle; $p.WaitForExit(); exit $p.ExitCode } catch { exit 1223 }"
set "RC=%ERRORLEVEL%"
if "%RC%"=="1223" (
  echo Admin approval was not given; nothing changed.
  exit /b 1
)
if not "%RC%"=="0" (
  echo "Admin step FAILED (code %RC%: 10 not 64-bit, 11 folder, 12 copy, 13 verification, 14 registration - rolled back)."
  exit /b 1
)

rem --- step 3: this user ---
"%DST%\stlprev-setup.exe" register --machine-classes
if errorlevel 1 (
  echo WARNING: this user's old HKCU entries could not be fully cleaned; details above.
  exit /b 1
)
echo.
"%DST%\stlprev-setup.exe" status
echo.
echo "Installed (%DST%). Select an STL/3MF in Explorer and press Alt+P to open the preview."
exit /b 0
