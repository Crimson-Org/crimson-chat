#!/bin/sh
# Generated from tasks.json by Crimson Build, edit that file instead
# Remove Crimson Chat and its bouncer from the current user
set -eu
script_directory=$(cd "$(dirname "$0")" && pwd)
cd "$script_directory/.."

crimson_remove() {
	rm -rf "$1"
}

export PROGRAM="crimson-chat"
export BOUNCER="crimson-chat-bouncer"
export APPLICATION_ID="io.github.CrimsonOrg.CrimsonChat"
export VERSION="0.1.0"
export PREFIX="${HOME}/.local"
export STD="c11"
export WARN="-Wall -Wextra -Wpedantic -Werror -Wno-unused-parameter"
export OPT="-O2 -g"
export PKGS="gtk4 libadwaita-1 libsoup-3.0"
export BOUNCER_PKGS="gio-2.0 gio-unix-2.0 sqlite3 gnutls gdk-pixbuf-2.0"
export DEFINES="-D_GNU_SOURCE -DAPPLICATION_VERSION=\"${VERSION}\""
export INCLUDES="-Isrc -Isubprojects/crimson-core/src"
export EXTRA_PROGRAMS="build/${BOUNCER}"
printf '==> %s\n' 'remove ${PREFIX}/bin/${PROGRAM}'
crimson_remove "${PREFIX}/bin/${PROGRAM}"
printf '==> %s\n' 'remove ${PREFIX}/bin/${BOUNCER}'
crimson_remove "${PREFIX}/bin/${BOUNCER}"
printf '==> %s\n' 'remove ${PREFIX}/share/applications/${APPLICATION_ID}.desktop'
crimson_remove "${PREFIX}/share/applications/${APPLICATION_ID}.desktop"
printf '==> %s\n' 'remove ${PREFIX}/share/icons/hicolor/scalable/apps/${APPLICATION_ID}.svg'
crimson_remove "${PREFIX}/share/icons/hicolor/scalable/apps/${APPLICATION_ID}.svg"
printf '==> %s\n' 'remove ${PREFIX}/share/icons/hicolor/symbolic/apps/${APPLICATION_ID}-symbolic.svg'
crimson_remove "${PREFIX}/share/icons/hicolor/symbolic/apps/${APPLICATION_ID}-symbolic.svg"
printf '==> %s\n' 'remove ${PREFIX}/share/metainfo/${APPLICATION_ID}.metainfo.xml'
crimson_remove "${PREFIX}/share/metainfo/${APPLICATION_ID}.metainfo.xml"
