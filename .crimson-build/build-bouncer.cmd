@echo off
rem Generated from tasks.json by Crimson Build, edit that file instead
rem Build the Crimson Chat bouncer, which needs no graphical libraries
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
set "BOUNCER_PKGS=gio-2.0 gio-unix-2.0 sqlite3 gnutls"
set "DEFINES=-D_GNU_SOURCE -DAPPLICATION_VERSION=%VERSION%"
set "INCLUDES=-Isrc -Isubprojects/crimson-core/src"
set "EXTRA_PROGRAMS=build/%BOUNCER%"
echo(==^> needs cc
call :crimson_needs "cc"
if errorlevel 1 exit /b %errorlevel%
echo(==^> needs pkg-config
call :crimson_needs "pkg-config"
if errorlevel 1 exit /b %errorlevel%
echo(==^> pkg gio-2.0 ^>= 2.74
call :crimson_pkg "gio-2.0 >= 2.74"
if errorlevel 1 exit /b %errorlevel%
echo(==^> pkg sqlite3
call :crimson_pkg "sqlite3"
if errorlevel 1 exit /b %errorlevel%
echo(==^> pkg gnutls ^>= 3.6.13
call :crimson_pkg "gnutls >= 3.6.13"
if errorlevel 1 exit /b %errorlevel%
echo(==^> mkdir build/irc
call :crimson_mkdir "build\irc"
if errorlevel 1 exit /b %errorlevel%
echo(==^> mkdir build/bouncer
call :crimson_mkdir "build\bouncer"
if errorlevel 1 exit /b %errorlevel%
echo(==^> each src/irc/*.c
call :crimson_each
if errorlevel 1 exit /b %errorlevel%
echo(==^> each src/bouncer/*.c
call :crimson_each
if errorlevel 1 exit /b %errorlevel%
echo(==^> cc -o build/%BOUNCER% build/irc/*.o build/bouncer/*.o $^(pkg-config --libs $BOUNCER_PKGS^)
cc -o build/%BOUNCER% build/irc/*.o build/bouncer/*.o $(pkg-config --libs $BOUNCER_PKGS)
if errorlevel 1 exit /b %errorlevel%
echo(==^> copy build/%BOUNCER% to .%BOUNCER%.new
call :crimson_copy "build\%BOUNCER%" ".%BOUNCER%.new"
if errorlevel 1 exit /b %errorlevel%
echo(==^> move .%BOUNCER%.new to %BOUNCER%
call :crimson_move ".%BOUNCER%.new" "%BOUNCER%"
if errorlevel 1 exit /b %errorlevel%
exit /b 0

:crimson_copy
if exist "%~1\*" (
	xcopy "%~1" "%~2\" /e /i /y /q >nul
	exit /b
)
if not exist "%~dp2" mkdir "%~dp2"
copy /y "%~1" "%~2" >nul
exit /b

:crimson_move
if not exist "%~dp2" mkdir "%~dp2"
move /y "%~1" "%~2" >nul
exit /b

:crimson_mkdir
if not exist "%~1" mkdir "%~1"
exit /b

:crimson_needs
where "%~1" >nul 2>&1 || (echo %~1 is needed, install it first & exit /b 1)
exit /b 0

:crimson_pkg
pkg-config --exists "%~1" || (echo The development files for %~1 are missing & exit /b 1)
exit /b 0

:crimson_each
echo Building each file needs a Unix shell with make
exit /b 1
