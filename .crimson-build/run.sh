#!/bin/sh
# Generated from tasks.json by Crimson Build, edit that file instead
# Build and start Crimson Chat
set -eu
script_directory=$(cd "$(dirname "$0")" && pwd)
cd "$script_directory/.."

sh "$script_directory/build.sh"
export PROGRAM="crimson-chat"
export BOUNCER="crimson-chat-bouncer"
export APPLICATION_ID="io.github.CrimsonOrg.CrimsonChat"
export VERSION="0.1.0"
export PREFIX="${HOME}/.local"
export STD="c11"
export WARN="-Wall -Wextra -Wpedantic -Werror -Wno-unused-parameter"
export OPT="-O2 -g"
export PKGS="gtk4 libadwaita-1 libsoup-3.0"
export BOUNCER_PKGS="gio-2.0 gio-unix-2.0 sqlite3 gnutls"
export DEFINES="-D_GNU_SOURCE -DAPPLICATION_VERSION=\"${VERSION}\""
export INCLUDES="-Isrc -Isubprojects/crimson-core/src"
export EXTRA_PROGRAMS="build/${BOUNCER}"
printf '==> %s\n' './${PROGRAM}'
./${PROGRAM}
