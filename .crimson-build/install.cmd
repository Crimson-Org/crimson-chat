@echo off
rem Generated from tasks.json by Crimson Build, edit that file instead
rem Install Crimson Chat and its bouncer for the current user
setlocal
cd /d "%~dp0.."

call "%~dp0build.cmd"
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
set "BOUNCER_PKGS=gio-2.0 gio-unix-2.0 sqlite3 gnutls"
set "DEFINES=-D_GNU_SOURCE -DAPPLICATION_VERSION=%VERSION%"
set "INCLUDES=-Isrc -Isubprojects/crimson-core/src"
set "EXTRA_PROGRAMS=build/%BOUNCER%"
echo(==^> install -Dm755 build/%PROGRAM% %PREFIX%/bin/%PROGRAM%
install -Dm755 build/%PROGRAM% %PREFIX%/bin/%PROGRAM%
if errorlevel 1 exit /b %errorlevel%
echo(==^> install -Dm755 build/%BOUNCER% %PREFIX%/bin/%BOUNCER%
install -Dm755 build/%BOUNCER% %PREFIX%/bin/%BOUNCER%
if errorlevel 1 exit /b %errorlevel%
echo(==^> mkdir -p %PREFIX%/share/applications ^&^& for desktop in data/%APPLICATION_ID%*.desktop.in; do sed "s^|@EXEC@^|%PREFIX%/bin/%PROGRAM%^|" "$desktop" ^> "%PREFIX%/share/applications/$^(basename "$desktop" .in^)"; done
mkdir -p %PREFIX%/share/applications && for desktop in data/%APPLICATION_ID%*.desktop.in; do sed s|@EXEC@|%PREFIX%/bin/%PROGRAM%| $desktop > %PREFIX%/share/applications/$(basename $desktop .in); done
if errorlevel 1 exit /b %errorlevel%
echo(==^> install -Dm644 data/icons/scalable/apps/%APPLICATION_ID%.svg %PREFIX%/share/icons/hicolor/scalable/apps/%APPLICATION_ID%.svg
install -Dm644 data/icons/scalable/apps/%APPLICATION_ID%.svg %PREFIX%/share/icons/hicolor/scalable/apps/%APPLICATION_ID%.svg
if errorlevel 1 exit /b %errorlevel%
echo(==^> install -Dm644 data/icons/symbolic/apps/%APPLICATION_ID%-symbolic.svg %PREFIX%/share/icons/hicolor/symbolic/apps/%APPLICATION_ID%-symbolic.svg
install -Dm644 data/icons/symbolic/apps/%APPLICATION_ID%-symbolic.svg %PREFIX%/share/icons/hicolor/symbolic/apps/%APPLICATION_ID%-symbolic.svg
if errorlevel 1 exit /b %errorlevel%
echo(==^> install -Dm644 data/%APPLICATION_ID%.metainfo.xml %PREFIX%/share/metainfo/%APPLICATION_ID%.metainfo.xml
install -Dm644 data/%APPLICATION_ID%.metainfo.xml %PREFIX%/share/metainfo/%APPLICATION_ID%.metainfo.xml
if errorlevel 1 exit /b %errorlevel%
echo(==^> update-desktop-database %PREFIX%/share/applications ^>/dev/null 2^>^&1 ^|^| true
update-desktop-database %PREFIX%/share/applications >/dev/null 2>&1 || true
if errorlevel 1 exit /b %errorlevel%
echo(==^> gtk-update-icon-cache --quiet --ignore-theme-index %PREFIX%/share/icons/hicolor ^>/dev/null 2^>^&1 ^|^| true
gtk-update-icon-cache --quiet --ignore-theme-index %PREFIX%/share/icons/hicolor >/dev/null 2>&1 || true
if errorlevel 1 exit /b %errorlevel%
exit /b 0
