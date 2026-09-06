#!/usr/bin/env bash
#
# fix-alsa-plugin-dir.sh - point a source-built alsa-lib at the distro ALSA plugins.
#
# THE PROBLEM
#   A source build of alsa-lib (e.g. 1.2.14, installed for MIDI 2.0 / UMP support)
#   configured with no --prefix defaults to /usr/local and bakes in
#       ALSA_PLUGIN_DIR = "/usr/local/lib/alsa-lib"
#   If alsa-plugins was not also built, that directory never gets created. Meanwhile
#   the distro config at /etc/alsa/conf.d/99-pulse.conf - which the source alsa.conf
#   still loads - asks for "libasound_module_conf_pulse.so" by bare name. alsa-lib
#   looks only in its own (empty) plugin dir, fails, and aborts config parsing before
#   the sequencer is ever reachable. Every ALSA client then dies with:
#
#       ALSA lib conf.c:####:(snd_config_hooks_call) Cannot open shared library
#           libasound_module_conf_pulse.so (/usr/local/lib/alsa-lib/...: No such file)
#       ALSA lib seq.c:####:(snd_seq_open_noupdate) Unknown SEQ default
#
#   ...including aconnect(1), amidi(1) and anything using RtMidi (SendSysEx included).
#
# THE FIX
#   Symlink the source build's plugin dir at the distro plugin dir, so the existing
#   libasound2-plugins package satisfies the lookup. This keeps the new alsa-lib (and
#   its MIDI 2.0 / UMP tools: aplaymidi2, arecordmidi2, aseqsend) fully in place.
#
#   Compatibility was verified before choosing this approach: both libraries carry
#   SONAME libasound.so.2, the source build exports the ALSA_0.9 version node the
#   plugins are linked against, and conf_pulse imports exactly one libasound symbol
#   (snd_config_hook_load), which is stable across these versions.
#
#   Alternatives considered and rejected:
#     - ALSA_PLUGIN_DIR env var: honoured by alsa-lib, but only helps processes that
#       set it - same shortcoming as an LD_LIBRARY_PATH workaround.
#     - Rebuilding alsa-plugins from source: pulls in PulseAudio/JACK/speex/samplerate
#       dev dependencies to duplicate packages that already work. Do this only if a
#       plugin-specific bug shows up later.
#     - Deleting the pulse config: silences the error but breaks PulseAudio routing
#       for every other app on the machine.
#
# USAGE
#   ./scripts/fix-alsa-plugin-dir.sh            # apply the fix (prompts for sudo)
#   ./scripts/fix-alsa-plugin-dir.sh --check    # report status, change nothing
#   ./scripts/fix-alsa-plugin-dir.sh --undo     # remove the symlink this script made
#
# Linux only; a no-op anywhere else.

set -euo pipefail

LOCAL_PLUGIN_DIR="/usr/local/lib/alsa-lib"
DISTRO_PLUGIN_DIR="/usr/lib/x86_64-linux-gnu/alsa-lib"
PROBE_PLUGIN="libasound_module_conf_pulse.so"

MODE="apply"
case "${1:-}" in
    --check) MODE="check" ;;
    --undo)  MODE="undo" ;;
    -h|--help)
        sed -n '2,50p' "$0" | sed 's/^# \{0,1\}//'
        exit 0
        ;;
    "") ;;
    *)  echo "error: unknown argument '$1' (try --help)" >&2; exit 2 ;;
esac

if [[ "$(uname -s)" != "Linux" ]]; then
    echo "Not Linux - nothing to do."
    exit 0
fi

# --- report current state -----------------------------------------------------

echo "=== ALSA plugin-dir status ==="

if [[ -e /usr/local/lib/libasound.so.2 ]]; then
    echo "  source alsa-lib : /usr/local/lib/libasound.so.2 (present)"
else
    echo "  source alsa-lib : none in /usr/local - this fix is probably unnecessary."
fi

if [[ -d "$DISTRO_PLUGIN_DIR" ]]; then
    echo "  distro plugins  : $DISTRO_PLUGIN_DIR (present)"
else
    echo "  distro plugins  : MISSING - install them first:"
    echo "                    sudo apt install libasound2-plugins"
    exit 1
fi

if [[ -L "$LOCAL_PLUGIN_DIR" ]]; then
    echo "  local plugin dir: symlink -> $(readlink -f "$LOCAL_PLUGIN_DIR")"
elif [[ -d "$LOCAL_PLUGIN_DIR" ]]; then
    echo "  local plugin dir: real directory (not created by this script)"
else
    echo "  local plugin dir: absent  <-- this is what breaks ALSA"
fi

# --- undo ---------------------------------------------------------------------

if [[ "$MODE" == "undo" ]]; then
    if [[ -L "$LOCAL_PLUGIN_DIR" ]]; then
        echo
        echo "Removing symlink $LOCAL_PLUGIN_DIR ..."
        sudo rm -f "$LOCAL_PLUGIN_DIR"
        echo "Done. (ALSA will break again until the plugin dir is provided another way.)"
    else
        echo
        echo "Nothing to undo: $LOCAL_PLUGIN_DIR is not a symlink."
    fi
    exit 0
fi

# --- check --------------------------------------------------------------------

verify() {
    echo
    echo "=== Verifying ==="
    local ok=0

    if aconnect -l >/dev/null 2>&1; then
        echo "  aconnect -l : OK (sequencer opens cleanly)"
    else
        echo "  aconnect -l : FAILED - ALSA still cannot open the sequencer:"
        aconnect -l 2>&1 | sed 's/^/                /' | head -4
        ok=1
    fi

    if [[ -e "$LOCAL_PLUGIN_DIR/$PROBE_PLUGIN" ]]; then
        echo "  $PROBE_PLUGIN : resolvable"
    else
        echo "  $PROBE_PLUGIN : NOT resolvable via $LOCAL_PLUGIN_DIR"
        ok=1
    fi

    return $ok
}

if [[ "$MODE" == "check" ]]; then
    verify || true
    exit 0
fi

# --- apply --------------------------------------------------------------------

if [[ -d "$LOCAL_PLUGIN_DIR" && ! -L "$LOCAL_PLUGIN_DIR" ]]; then
    echo
    echo "ERROR: $LOCAL_PLUGIN_DIR is a real directory, not a symlink."
    echo "       Something installed plugins there. Refusing to replace it - inspect it"
    echo "       by hand and remove it first if it is an incomplete build artifact."
    exit 1
fi

echo
echo "Linking $LOCAL_PLUGIN_DIR -> $DISTRO_PLUGIN_DIR (requires sudo) ..."
sudo mkdir -p "$(dirname "$LOCAL_PLUGIN_DIR")"
sudo ln -sfn "$DISTRO_PLUGIN_DIR" "$LOCAL_PLUGIN_DIR"

if verify; then
    echo
    echo "ALSA is working. SendSysEx no longer needs the LD_LIBRARY_PATH workaround."
    exit 0
fi

echo
echo "The symlink is in place but ALSA still fails - something else is also wrong."
echo "Re-run with --undo to revert this change."
exit 1
