#!/bin/bash
# Starts MX Simulator with MXS Remastered (Linux).
#
# Put this file and libmxs-remastered.so in your MX Simulator folder, next to mx and mxbin, and
# run ./mxs-remastered.sh instead of ./mx. ./mx still starts the game without the mod.
#
# The mod is loaded into mxbin with LD_PRELOAD, which splits its value at spaces and colons with no
# way to escape them: from a folder with either in its path, the library is linked into
# ~/.local/share/mxs-remastered/ (a path without them) and loaded from there.
#
# Optional: MX_HOME=/path/to/game (the folder holding mxbin), MXS_REMASTERED_LIB=/path/to/the.so

here="$(cd "$(dirname "$(readlink -f "$0")")" && pwd)"
export MX_HOME="${MX_HOME:-$here}"
lib="${MXS_REMASTERED_LIB:-$here/libmxs-remastered.so}"

if [ ! -x "$MX_HOME/mxbin" ]; then
    echo "MXS Remastered: no mxbin in $MX_HOME." >&2
    echo "Put this script in your MX Simulator folder, or run: MX_HOME=/path/to/your/game $0" >&2
    exit 1
fi
if [ ! -f "$lib" ]; then
    echo "MXS Remastered: $lib not found. Put libmxs-remastered.so next to this script." >&2
    exit 1
fi
lib="$(readlink -f "$lib")"

case "$lib" in
*[[:space:]:]*)
    data="${XDG_DATA_HOME:-}"
    case "$data" in /*) ;; *) data="$HOME/.local/share" ;; esac  # (the XDG spec ignores a relative path)
    data="$data/mxs-remastered"
    case "$data" in
    *[[:space:]:]*) data="/tmp/mxs-remastered-$(id -u)" ;;
    esac
    mkdir -p "$data" && ln -sf "$lib" "$data/libmxs-remastered.so" || exit 1
    lib="$data/libmxs-remastered.so"
    ;;
esac

exec env LD_PRELOAD="$lib${LD_PRELOAD:+:$LD_PRELOAD}" "$MX_HOME/mxbin" "$@"
