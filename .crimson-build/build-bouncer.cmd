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
set "CRIMSON_EACH_FROM=src/irc/*.c"
set "CRIMSON_EACH_TO=build/irc/{id}.o"
set "CRIMSON_EACH_RUN=cc -std=%STD% $WARN $OPT $DEFINES $(pkg-config --cflags $BOUNCER_PKGS | sed s/-I/-isystem /g) -c {from} -o {to}"
set "CRIMSON_EACH_ALSO=src/irc/*.h"
call :crimson_each
if errorlevel 1 exit /b %errorlevel%
echo(==^> each src/bouncer/*.c
set "CRIMSON_EACH_FROM=src/bouncer/*.c"
set "CRIMSON_EACH_TO=build/bouncer/{id}.o"
set "CRIMSON_EACH_RUN=cc -std=%STD% $WARN $OPT $DEFINES $(pkg-config --cflags $BOUNCER_PKGS | sed s/-I/-isystem /g) -c {from} -o {to}"
set "CRIMSON_EACH_ALSO=src/irc/*.h src/bouncer/*.h"
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

$template = $env:CRIMSON_EACH_TO
$folder = Split-Path -Parent ($template.Substring(0, $template.IndexOf('{id}')) + 'x')
if (-not $folder) { $folder = '.' }
$md5 = [Security.Cryptography.MD5]::Create()
$hash = $md5.ComputeHash([Text.Encoding]::UTF8.GetBytes("$env:CRIMSON_EACH_FROM|$template"))
$list = Get-FullPath "$folder/.crimson-each-$([BitConverter]::ToString($hash, 0, 4).Replace('-', ''))"
[void][IO.Directory]::CreateDirectory((Get-FullPath $folder))
$previous = @()
if (Test-Path -LiteralPath $list) { $previous = @([IO.File]::ReadAllLines($list)) }

# A changed command or variable makes every earlier result stale
$settings = $env:CRIMSON_EACH_RUN
$oldSettings = $null
if (Test-Path -LiteralPath "$list.run") { $oldSettings = [IO.File]::ReadAllText("$list.run") }
if ($oldSettings -ne $settings) {
	foreach ($old in $previous) { Remove-Item -LiteralPath (Get-FullPath $old) -Force -ErrorAction SilentlyContinue }
	[IO.File]::WriteAllText("$list.run", $settings)
}

$newestExtra = [DateTime]::MinValue
foreach ($extra in Get-Matches $env:CRIMSON_EACH_ALSO) {
	$time = [IO.File]::GetLastWriteTimeUtc((Get-FullPath $extra))
	if ($time -gt $newestExtra) { $newestExtra = $time }
}

$outputs = @()
$work = @()
foreach ($source in @(Get-Matches $env:CRIMSON_EACH_FROM | Sort-Object -Unique)) {
	$base = $source
	if ($source.LastIndexOf('.') -ge 0) { $base = $source.Substring(0, $source.LastIndexOf('.')) }
	$to = $template.Replace('{id}', $base.Replace('/', '_').Replace('.', '_')).Replace('{name}', (Split-Path -Leaf $base))
	$outputs += $to
	$newest = [IO.File]::GetLastWriteTimeUtc((Get-FullPath $source))
	if ($newestExtra -gt $newest) { $newest = $newestExtra }
	$target = Get-FullPath $to
	if ((Test-Path -LiteralPath $target) -and [IO.File]::GetLastWriteTimeUtc($target) -ge $newest) { continue }
	$command = $settings.Replace('{from}', $source.Replace('/', '\')).Replace('{to}', $to.Replace('/', '\'))
	$work += [pscustomobject]@{ From = $source; To = $to; Command = $command }
}

# Results of files that are gone would still be picked up by the steps after this one
foreach ($old in $previous) {
	if ($outputs -notcontains $old) { Remove-Item -LiteralPath (Get-FullPath $old) -Force -ErrorAction SilentlyContinue }
}
[IO.File]::WriteAllLines($list, [string[]]$outputs)

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
