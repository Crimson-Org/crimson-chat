#!/bin/sh
# Generated from tasks.json by Crimson Build, edit that file instead
# Build the Crimson Chat bouncer, which needs no graphical libraries
set -eu
script_directory=$(cd "$(dirname "$0")" && pwd)
cd "$script_directory/.."

crimson_copy() {
	if [ -d "$1" ]; then
		mkdir -p "$2" && cp -R "$1/." "$2"
	else
		mkdir -p "$(dirname "$2")" && cp "$1" "$2"
	fi
}

crimson_move() {
	mkdir -p "$(dirname "$2")" && mv "$1" "$2"
}

crimson_mkdir() {
	mkdir -p "$1"
}

crimson_needs() {
	command -v "$1" >/dev/null 2>&1 || { printf '%s\n' "$1 is needed, install it with your package manager" >&2; return 1; }
}

crimson_pkg() {
	pkg-config --exists "$1" || { printf '%s\n' "The development files for $1 are missing, install them with your package manager" >&2; return 1; }
}

crimson_each() {
	crimson_rules=$(mktemp) || return 1
	crimson_make=$(mktemp) || return 1
	crimson_extra=
	for crimson_glob in $4; do
		for crimson_file in $crimson_glob; do
			[ -e "$crimson_file" ] && crimson_extra="$crimson_extra $crimson_file"
		done
	done
	crimson_all=
	crimson_sep=$(printf '\037')
	for crimson_from in $1; do
		[ -e "$crimson_from" ] || continue
		crimson_id=$(printf '%s' "${crimson_from%.*}" | tr '/.' '__')
		crimson_name=$(basename "${crimson_from%.*}")
		crimson_to=$(printf '%s' "$2" | sed -e "s${crimson_sep}{id}${crimson_sep}$crimson_id${crimson_sep}g" -e "s${crimson_sep}{name}${crimson_sep}$crimson_name${crimson_sep}g")
		crimson_command=$(printf '%s' "$3" | sed -e 's/\$/$$/g' -e "s${crimson_sep}{from}${crimson_sep}$crimson_from${crimson_sep}g" -e "s${crimson_sep}{to}${crimson_sep}$crimson_to${crimson_sep}g")
		crimson_all="$crimson_all $crimson_to"
		printf '%s: %s%s\n\t@mkdir -p "$(dir $@)"\n\t@echo "==> %s"\n\t%s\n\n' "$crimson_to" "$crimson_from" "$crimson_extra" "$crimson_from" "$crimson_command" >> "$crimson_rules"
	done
	{ printf 'all:%s\n\n' "$crimson_all"; cat "$crimson_rules"; } > "$crimson_make"
	crimson_status=0
	make -s -f "$crimson_make" -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)" all || crimson_status=$?
	rm -f "$crimson_make" "$crimson_rules"
	return "$crimson_status"
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
printf '==> %s\n' 'needs cc'
crimson_needs "cc"
printf '==> %s\n' 'needs pkg-config'
crimson_needs "pkg-config"
printf '==> %s\n' 'pkg gio-2.0 >= 2.74'
crimson_pkg "gio-2.0 >= 2.74"
printf '==> %s\n' 'pkg sqlite3'
crimson_pkg "sqlite3"
printf '==> %s\n' 'pkg gnutls >= 3.6.13'
crimson_pkg "gnutls >= 3.6.13"
printf '==> %s\n' 'mkdir build/irc'
crimson_mkdir "build/irc"
printf '==> %s\n' 'mkdir build/bouncer'
crimson_mkdir "build/bouncer"
printf '==> %s\n' 'each src/irc/*.c'
crimson_each "src/irc/*.c" "build/irc/{id}.o" "cc -std=${STD} \$WARN \$OPT \$DEFINES \$(pkg-config --cflags \$BOUNCER_PKGS | sed \"s/-I/-isystem /g\") -c {from} -o {to}" "src/irc/*.h"
printf '==> %s\n' 'each src/bouncer/*.c'
crimson_each "src/bouncer/*.c" "build/bouncer/{id}.o" "cc -std=${STD} \$WARN \$OPT \$DEFINES \$(pkg-config --cflags \$BOUNCER_PKGS | sed \"s/-I/-isystem /g\") -c {from} -o {to}" "src/irc/*.h src/bouncer/*.h"
printf '==> %s\n' 'cc -o build/${BOUNCER} build/irc/*.o build/bouncer/*.o $(pkg-config --libs $BOUNCER_PKGS)'
cc -o build/${BOUNCER} build/irc/*.o build/bouncer/*.o $(pkg-config --libs $BOUNCER_PKGS)
printf '==> %s\n' 'copy build/${BOUNCER} to .${BOUNCER}.new'
crimson_copy "build/${BOUNCER}" ".${BOUNCER}.new"
printf '==> %s\n' 'move .${BOUNCER}.new to ${BOUNCER}'
crimson_move ".${BOUNCER}.new" "${BOUNCER}"
