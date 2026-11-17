@echo off
setlocal EnableExtensions DisableDelayedExpansion
rem Removes the machine-wide install:
rem  1) this user's HKCU leftovers (from old installs; backups are restored),
rem  2) as admin (this script calls itself with "/admin"): the HKLM registration; backed-up Creality/Windows
rem     handlers are restored; ONLY if that succeeds are the "%ProgramW6432%\STLPreview" files deleted
rem     (locked ones on reboot). If the registration cannot be removed, no file is deleted.
if /i "%~1"=="/admin" goto :admin

if not defined ProgramW6432 (
  echo 64-bit Windows is required.
  exit /b 1
)
set "DST=%ProgramW6432%\STLPreview"
set "SETUP=%DST%\stlprev-setup.exe"
if not exist "%SETUP%" set "SETUP=%~dp0build\bin\Release\stlprev-setup.exe"
if not exist "%SETUP%" (
  echo stlprev-setup.exe not found.
  exit /b 1
)

set "STLP_PROGIDS="
for /f "usebackq delims=" %%A in (`""%SETUP%" user-progids"`) do set "STLP_PROGIDS=%%A"

"%SETUP%" unregister
if errorlevel 1 echo WARNING: some of this user's HKCU entries could not be removed; details above.

set "STLP_ADMIN=%~f0"
powershell -NoProfile -ExecutionPolicy Bypass -Command "try { $a = '/c \"\"' + $env:STLP_ADMIN + '\" /admin ' + $env:STLP_PROGIDS + '\"'; $p = Start-Process -FilePath $env:ComSpec -ArgumentList $a -Verb RunAs -PassThru -ErrorAction Stop; $null = $p.Handle; $p.WaitForExit(); exit $p.ExitCode } catch { exit 1223 }"
set "RC=%ERRORLEVEL%"
if "%RC%"=="1223" (
  echo Admin approval was not given; the HKLM registration and files are UNCHANGED.
  exit /b 1
)
if "%RC%"=="0" (
  echo Uninstalled.
  exit /b 0
)
if "%RC%"=="2" (
  echo Uninstalled. Some files in use will be deleted after a reboot.
  exit /b 0
)
echo "Uninstall FAILED (code %RC%). Registration and files were left in place; Explorer keeps working."
exit /b 1

:admin
rem --- admin ---
if not defined ProgramW6432 exit /b 10
set "DST=%ProgramW6432%\STLPreview"
set "SETUP=%DST%\stlprev-setup.exe"
if not exist "%SETUP%" set "SETUP=%~dp0build\bin\Release\stlprev-setup.exe"
if not exist "%SETUP%" exit /b 11
"%SETUP%" unregister-machine --progid "%~2" --progid "%~3" --progid "%~4" --progid "%~5"
if errorlevel 1 (
  echo Could not remove the HKLM registration; files were not deleted.
  pause
  exit /b 12
)
if not exist "%DST%\" exit /b 0

rem Registration fully removed: delete the files. Locked ones (the DLL loaded by Explorer) on reboot.
set "REBOOT="
for %%F in ("%DST%\STLPreview.dll" "%DST%\STLPreview.dll.old-*" "%DST%\STLPreviewHost.exe" "%DST%\STLPreviewHost.exe.old-*") do (
  if exist "%%~fF" (
    del /f /q "%%~fF" >nul 2>nul
    if exist "%%~fF" (
      "%SETUP%" delete-on-reboot "%%~fF"
      set "REBOOT=1"
    )
  )
)
if defined REBOOT (
  rem stlprev-setup and the folder are marked for deletion AFTER the locked files (order is kept).
  "%SETUP%" delete-on-reboot "%DST%\stlprev-setup.exe" "%DST%"
  exit /b 2
)
del /f /q "%DST%\stlprev-setup.exe" >nul 2>nul
rem The folder is deleted only if empty (it stays if other files are there).
rd "%DST%" >nul 2>nul
exit /b 0
