@echo off
setlocal EnableExtensions DisableDelayedExpansion
rem Called AS ADMIN by install-machine.cmd. Arguments: the user's ProgIDs (at most 4; stlprev-setup validates them).
rem The target folder is computed here (the caller is not trusted); only the two expected files are copied and
rem verified before registering. The result is reported only via the exit code:
rem   0 ok, 10 not 64-bit, 11 folder, 12 copy, 13 verification, 14 registration (rolled back)
set "SRC=%~dp0build\bin\Release"
if not defined ProgramW6432 exit /b 10
set "DST=%ProgramW6432%\STLPreview"
if not exist "%DST%\" mkdir "%DST%" || exit /b 11

rem A DLL in use cannot be overwritten but can be renamed. Old copies are deleted or (if locked)
rem marked for deletion on reboot.
del /f /q "%DST%\*.old-*" >nul 2>nul
set "OLDDLL="
if exist "%DST%\STLPreview.dll" (
  set "OLDDLL=STLPreview.dll.old-%RANDOM%%RANDOM%"
)
if defined OLDDLL ren "%DST%\STLPreview.dll" "%OLDDLL%" || exit /b 12
rem The preview host of old versions is no longer used.
if exist "%DST%\STLPreviewHost.exe" ren "%DST%\STLPreviewHost.exe" "STLPreviewHost.exe.old-%RANDOM%%RANDOM%"

copy /y "%SRC%\STLPreview.dll" "%DST%\STLPreview.dll" >nul || goto :copy_failed
copy /y "%SRC%\stlprev-setup.exe" "%DST%\stlprev-setup.exe" >nul || goto :copy_failed

rem Verification (the copies are in an admin-only folder): PE file and our version info.
set "STLP_DST=%DST%"
powershell -NoProfile -ExecutionPolicy Bypass -Command "$ok = $true; foreach ($f in 'STLPreview.dll','stlprev-setup.exe') { try { $p = Join-Path $env:STLP_DST $f; $v = [Diagnostics.FileVersionInfo]::GetVersionInfo($p); $b = [IO.File]::ReadAllBytes($p); $o = [BitConverter]::ToInt32($b, 60); if ($v.ProductName -ne 'STL Preview' -or $v.CompanyName -ne 'STLPreview' -or $b[0] -ne 77 -or $b[1] -ne 90 -or $o -lt 64 -or $o -gt ($b.Length - 4) -or $b[$o] -ne 80 -or $b[$o+1] -ne 69 -or $b[$o+2] -ne 0 -or $b[$o+3] -ne 0) { $ok = $false } } catch { $ok = $false } }; if ($ok) { exit 0 } else { exit 1 }"
if errorlevel 1 goto :verify_failed

"%DST%\stlprev-setup.exe" register-machine "%DST%\STLPreview.dll" --progid "%~1" --progid "%~2" --progid "%~3" --progid "%~4"
if errorlevel 1 (
  echo Registration failed; changes were rolled back.
  pause
  exit /b 14
)
for %%F in ("%DST%\*.old-*") do "%DST%\stlprev-setup.exe" delete-on-reboot "%%~fF" >nul
exit /b 0

:copy_failed
if defined OLDDLL if not exist "%DST%\STLPreview.dll" ren "%DST%\%OLDDLL%" "STLPreview.dll"
pause
exit /b 12

:verify_failed
echo The copied files could not be verified; not registered.
del /f /q "%DST%\STLPreview.dll" >nul 2>nul
if defined OLDDLL if not exist "%DST%\STLPreview.dll" ren "%DST%\%OLDDLL%" "STLPreview.dll"
pause
exit /b 13
