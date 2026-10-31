@echo off
rem Generated from tasks.json by Crimson Build, edit that file instead
rem Build Crimson Chat and its bouncer into the project folder
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
echo(==^> needs cc
call :crimson_needs "cc"
if errorlevel 1 exit /b %errorlevel%
echo(==^> needs pkg-config
call :crimson_needs "pkg-config"
if errorlevel 1 exit /b %errorlevel%
echo(==^> needs glib-compile-resources
call :crimson_needs "glib-compile-resources"
if errorlevel 1 exit /b %errorlevel%
echo(==^> pkg gtk4 ^>= 4.14
call :crimson_pkg "gtk4 >= 4.14"
if errorlevel 1 exit /b %errorlevel%
echo(==^> pkg libadwaita-1 ^>= 1.6
call :crimson_pkg "libadwaita-1 >= 1.6"
if errorlevel 1 exit /b %errorlevel%
echo(==^> pkg libsoup-3.0
call :crimson_pkg "libsoup-3.0"
if errorlevel 1 exit /b %errorlevel%
echo(==^> mkdir build/obj
call :crimson_mkdir "build\obj"
if errorlevel 1 exit /b %errorlevel%
echo(==^> glib-compile-resources --sourcedir assets --target build/resources.c --generate-source assets/crimson-chat.gresource.xml
glib-compile-resources --sourcedir assets --target build/resources.c --generate-source assets/crimson-chat.gresource.xml
if errorlevel 1 exit /b %errorlevel%
echo(==^> each src/*.c src/app/*.c src/conversation/*.c src/rooms/*.c src/session/*.c src/window/*.c
set "CRIMSON_EACH_FROM=src/*.c src/app/*.c src/conversation/*.c src/rooms/*.c src/session/*.c src/window/*.c"
set "CRIMSON_EACH_TO=build/obj/{id}.o"
set "CRIMSON_EACH_RUN=cc -std=%STD% $WARN $OPT $DEFINES $INCLUDES $(pkg-config --cflags $PKGS | sed s/-I/-isystem /g) -c {from} -o {to}"
set "CRIMSON_EACH_ALSO=src/*/*.h subprojects/crimson-core/src/common/*.h"
call :crimson_each
if errorlevel 1 exit /b %errorlevel%
echo(==^> each subprojects/crimson-core/src/common/*.c
set "CRIMSON_EACH_FROM=subprojects/crimson-core/src/common/*.c"
set "CRIMSON_EACH_TO=build/obj/{id}.o"
set "CRIMSON_EACH_RUN=cc -std=%STD% $WARN $OPT $DEFINES $INCLUDES $(pkg-config --cflags $PKGS | sed s/-I/-isystem /g) -c {from} -o {to}"
set "CRIMSON_EACH_ALSO=subprojects/crimson-core/src/common/*.h"
call :crimson_each
if errorlevel 1 exit /b %errorlevel%
echo(==^> cc -std=gnu11 -O2 -g $^(pkg-config --cflags gio-2.0^) -c build/resources.c -o build/obj/resources.o
cc -std=gnu11 -O2 -g $(pkg-config --cflags gio-2.0) -c build/resources.c -o build/obj/resources.o
if errorlevel 1 exit /b %errorlevel%
echo(==^> glib-compile-resources --sourcedir subprojects/crimson-core/assets --target build/core-resources.c --generate-source subprojects/crimson-core/assets/crimson-core.gresource.xml
glib-compile-resources --sourcedir subprojects/crimson-core/assets --target build/core-resources.c --generate-source subprojects/crimson-core/assets/crimson-core.gresource.xml
if errorlevel 1 exit /b %errorlevel%
echo(==^> cc -std=gnu11 -O2 -g $^(pkg-config --cflags gio-2.0^) -c build/core-resources.c -o build/obj/core-resources.o
cc -std=gnu11 -O2 -g $(pkg-config --cflags gio-2.0) -c build/core-resources.c -o build/obj/core-resources.o
if errorlevel 1 exit /b %errorlevel%
echo(==^> cc -o build/%PROGRAM% build/obj/*.o build/irc/*.o $^(pkg-config --libs $PKGS^) -lm
cc -o build/%PROGRAM% build/obj/*.o build/irc/*.o $(pkg-config --libs $PKGS) -lm
if errorlevel 1 exit /b %errorlevel%
echo(==^> copy build/%PROGRAM% to .%PROGRAM%.new
call :crimson_copy "build\%PROGRAM%" ".%PROGRAM%.new"
if errorlevel 1 exit /b %errorlevel%
echo(==^> move .%PROGRAM%.new to %PROGRAM%
call :crimson_move ".%PROGRAM%.new" "%PROGRAM%"
if errorlevel 1 exit /b %errorlevel%
echo(==^> mkdir logs
call :crimson_mkdir "logs"
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
powershell -NoProfile -ExecutionPolicy Bypass -Command "$text = [IO.File]::ReadAllText('%~f0'); Invoke-Expression $text.Substring($text.LastIndexOf('<# crimson_each #>'))"
exit /b

<# crimson_each #>
# Runs one each step for crimson_each above, which cmd cannot do by itself
$ErrorActionPreference = 'Stop'
$root = (Get-Location).Path

function Get-Matches([string] $patterns) {
	foreach ($pattern in $patterns.Split(' ', [StringSplitOptions]::RemoveEmptyEntries)) {
		foreach ($file in Get-ChildItem -Path $pattern -File -ErrorAction SilentlyContinue) {
			(Resolve-Path -LiteralPath $file.FullName -Relative) -replace '^\.[\\/]', '' -replace '\\', '/'
		}
	}
}

function Get-FullPath([string] $path) {
	return [IO.Path]::Combine($root, $path)
}

function Get-Key([string] $text) {
	$hash = [Security.Cryptography.MD5]::Create().ComputeHash([Text.Encoding]::UTF8.GetBytes($text))
	return [BitConverter]::ToString($hash, 0, 4).Replace('-', '')
}

$template = $env:CRIMSON_EACH_TO
$settings = $env:CRIMSON_EACH_RUN
$folder = Split-Path -Parent ($template.Substring(0, $template.IndexOf('{id}')) + 'x')
if (-not $folder) { $folder = '.' }
$list = Get-FullPath "$folder/.crimson-each-$(Get-Key $template)"
$stamp = Get-FullPath "$folder/.crimson-each-$(Get-Key "$env:CRIMSON_EACH_FROM|$template").run"
[void][IO.Directory]::CreateDirectory((Get-FullPath $folder))

$newestExtra = [DateTime]::MinValue
foreach ($extra in Get-Matches $env:CRIMSON_EACH_ALSO) {
	$time = [IO.File]::GetLastWriteTimeUtc((Get-FullPath $extra))
	if ($time -gt $newestExtra) { $newestExtra = $time }
}

$mine = [ordered]@{}
foreach ($source in @(Get-Matches $env:CRIMSON_EACH_FROM | Sort-Object -Unique)) {
	$base = $source
	if ($source.LastIndexOf('.') -ge 0) { $base = $source.Substring(0, $source.LastIndexOf('.')) }
	$mine[$source] = $template.Replace('{id}', $base.Replace('/', '_').Replace('.', '_')).Replace('{name}', (Split-Path -Leaf $base))
}

# A changed command or variable makes every result of this step stale
$oldSettings = $null
if (Test-Path -LiteralPath $stamp) { $oldSettings = [IO.File]::ReadAllText($stamp) }
if ($oldSettings -ne $settings) {
	foreach ($to in $mine.Values) { Remove-Item -LiteralPath (Get-FullPath $to) -Force -ErrorAction SilentlyContinue }
	[IO.File]::WriteAllText($stamp, $settings)
}

# Steps writing to the same place share one list, and the result of a source that is gone would still be picked up
$entries = @()
if (Test-Path -LiteralPath $list) {
	foreach ($line in [IO.File]::ReadAllLines($list)) {
		$source, $to = $line.Split("`t")
		if (-not (Test-Path -LiteralPath (Get-FullPath $source))) {
			Remove-Item -LiteralPath (Get-FullPath $to) -Force -ErrorAction SilentlyContinue
		} elseif (-not $mine.Contains($source)) {
			$entries += $line
		}
	}
}
foreach ($source in $mine.Keys) { $entries += "$source`t$($mine[$source])" }
[IO.File]::WriteAllLines($list, [string[]]($entries | Sort-Object))

$work = @()
foreach ($source in $mine.Keys) {
	$to = $mine[$source]
	$newest = [IO.File]::GetLastWriteTimeUtc((Get-FullPath $source))
	if ($newestExtra -gt $newest) { $newest = $newestExtra }
	$target = Get-FullPath $to
	if ((Test-Path -LiteralPath $target) -and [IO.File]::GetLastWriteTimeUtc($target) -ge $newest) { continue }
	$command = $settings.Replace('{from}', $source.Replace('/', '\')).Replace('{to}', $to.Replace('/', '\'))
	$work += [pscustomobject]@{ From = $source; To = $to; Command = $command }
}

# Runs as many commands at once as there are processors, and starts no new one after a failure
$running = New-Object 'Collections.Generic.List[Diagnostics.Process]'
$hasFailed = $false
foreach ($item in $work) {
	while ($running.Count -ge [Environment]::ProcessorCount) {
		$running[0].WaitForExit()
		if ($running[0].ExitCode -ne 0) { $hasFailed = $true }
		$running.RemoveAt(0)
	}
	if ($hasFailed) { break }
	[void][IO.Directory]::CreateDirectory((Split-Path -Parent (Get-FullPath $item.To)))
	Write-Host "==> $($item.From)"
	$start = New-Object Diagnostics.ProcessStartInfo $env:ComSpec, "/d /s /c `"$($item.Command)`""
	$start.UseShellExecute = $false
	$start.WorkingDirectory = $root
	$running.Add([Diagnostics.Process]::Start($start))
}
foreach ($process in $running) {
	$process.WaitForExit()
	if ($process.ExitCode -ne 0) { $hasFailed = $true }
}
if ($hasFailed) { exit 1 }
exit 0
