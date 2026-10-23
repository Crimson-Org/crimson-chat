@echo off
rem Generated from tasks.json by Crimson Build, edit that file instead
rem Remove Crimson Chat and its bouncer from the current user
setlocal
cd /d "%~dp0.."

set "PROGRAM=crimson-chat"
set "BOUNCER=crimson-chat-bouncer"
set "APPLICATION_ID=io.github.CrimsonOrg.CrimsonChat"
set "VERSION=0.1.0"
set "PREFIX=%HOME%/.local"
set "STD=c11"
set "WARN=-Wall -Wextra -Wpedantic -Werror -Wno-unused-parameter"
set "OPT=-O2 -g"
set "PKGS=gtk4 libadwaita-1 libsoup-3.0"
set "BOUNCER_PKGS=gio-2.0 gio-unix-2.0 sqlite3 gnutls gdk-pixbuf-2.0"
set "DEFINES=-D_GNU_SOURCE -DAPPLICATION_VERSION=%VERSION%"
set "INCLUDES=-Isrc -Isubprojects/crimson-core/src"
set "EXTRA_PROGRAMS=build/%BOUNCER%"
echo(==^> remove %PREFIX%/bin/%PROGRAM%
call :crimson_remove "%PREFIX%\bin\%PROGRAM%"
if errorlevel 1 exit /b %errorlevel%
echo(==^> remove %PREFIX%/bin/%BOUNCER%
call :crimson_remove "%PREFIX%\bin\%BOUNCER%"
if errorlevel 1 exit /b %errorlevel%
echo(==^> remove %PREFIX%/share/applications/%APPLICATION_ID%.desktop
call :crimson_remove "%PREFIX%\share\applications\%APPLICATION_ID%.desktop"
if errorlevel 1 exit /b %errorlevel%
echo(==^> remove %PREFIX%/share/icons/hicolor/scalable/apps/%APPLICATION_ID%.svg
call :crimson_remove "%PREFIX%\share\icons\hicolor\scalable\apps\%APPLICATION_ID%.svg"
if errorlevel 1 exit /b %errorlevel%
echo(==^> remove %PREFIX%/share/icons/hicolor/symbolic/apps/%APPLICATION_ID%-symbolic.svg
call :crimson_remove "%PREFIX%\share\icons\hicolor\symbolic\apps\%APPLICATION_ID%-symbolic.svg"
if errorlevel 1 exit /b %errorlevel%
echo(==^> remove %PREFIX%/share/metainfo/%APPLICATION_ID%.metainfo.xml
call :crimson_remove "%PREFIX%\share\metainfo\%APPLICATION_ID%.metainfo.xml"
if errorlevel 1 exit /b %errorlevel%
exit /b 0

:crimson_remove
if exist "%~1\*" (
	rmdir /s /q "%~1"
	exit /b
)
if exist "%~1" del /f /q "%~1"
exit /b 0
