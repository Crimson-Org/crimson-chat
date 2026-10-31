#!/bin/sh
# Generated from tasks.json by Crimson Build, edit that file instead
# Install Crimson Chat and its bouncer for the current user
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
export BOUNCER_PKGS="gio-2.0 gio-unix-2.0 sqlite3 gnutls gdk-pixbuf-2.0"
export DEFINES="-D_GNU_SOURCE -DAPPLICATION_VERSION=\"${VERSION}\""
export INCLUDES="-Isrc -Isubprojects/crimson-core/src"
export EXTRA_PROGRAMS="build/${BOUNCER}"
printf '==> %s\n' 'install -Dm755 build/${PROGRAM} ${PREFIX}/bin/${PROGRAM}'
install -Dm755 build/${PROGRAM} ${PREFIX}/bin/${PROGRAM}
printf '==> %s\n' 'install -Dm755 build/${BOUNCER} ${PREFIX}/bin/${BOUNCER}'
install -Dm755 build/${BOUNCER} ${PREFIX}/bin/${BOUNCER}
printf '==> %s\n' 'mkdir -p ${PREFIX}/share/applications && for desktop in assets/${APPLICATION_ID}*.desktop.in; do sed "s|@EXEC@|${PREFIX}/bin/${PROGRAM}|" "$desktop" > "${PREFIX}/share/applications/$(basename "$desktop" .in)"; done'
mkdir -p ${PREFIX}/share/applications && for desktop in assets/${APPLICATION_ID}*.desktop.in; do sed "s|@EXEC@|${PREFIX}/bin/${PROGRAM}|" "$desktop" > "${PREFIX}/share/applications/$(basename "$desktop" .in)"; done
printf '==> %s\n' 'install -Dm644 assets/icons/scalable/apps/${APPLICATION_ID}.svg ${PREFIX}/share/icons/hicolor/scalable/apps/${APPLICATION_ID}.svg'
install -Dm644 assets/icons/scalable/apps/${APPLICATION_ID}.svg ${PREFIX}/share/icons/hicolor/scalable/apps/${APPLICATION_ID}.svg
printf '==> %s\n' 'install -Dm644 assets/icons/symbolic/apps/${APPLICATION_ID}-symbolic.svg ${PREFIX}/share/icons/hicolor/symbolic/apps/${APPLICATION_ID}-symbolic.svg'
install -Dm644 assets/icons/symbolic/apps/${APPLICATION_ID}-symbolic.svg ${PREFIX}/share/icons/hicolor/symbolic/apps/${APPLICATION_ID}-symbolic.svg
printf '==> %s\n' 'install -Dm644 assets/${APPLICATION_ID}.metainfo.xml ${PREFIX}/share/metainfo/${APPLICATION_ID}.metainfo.xml'
install -Dm644 assets/${APPLICATION_ID}.metainfo.xml ${PREFIX}/share/metainfo/${APPLICATION_ID}.metainfo.xml
printf '==> %s\n' 'update-desktop-database ${PREFIX}/share/applications >/dev/null 2>&1 || true'
update-desktop-database ${PREFIX}/share/applications >/dev/null 2>&1 || true
printf '==> %s\n' 'gtk-update-icon-cache --quiet --ignore-theme-index ${PREFIX}/share/icons/hicolor >/dev/null 2>&1 || true'
gtk-update-icon-cache --quiet --ignore-theme-index ${PREFIX}/share/icons/hicolor >/dev/null 2>&1 || true
