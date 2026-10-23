@echo off
rem Generated from tasks.json by Crimson Build, edit that file instead
rem Build and start the bouncer, which asks for an admin account on its first launch
setlocal
cd /d "%~dp0.."

call "%~dp0build-bouncer.cmd"
if errorlevel 1 exit /b %errorlevel%
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
echo(==^> ./%BOUNCER%
./%BOUNCER%
if errorlevel 1 exit /b %errorlevel%
exit /b 0
