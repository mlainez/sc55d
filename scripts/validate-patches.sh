#!/usr/bin/env bash
#
# Decide whether the patches in patches/ are safe to enable, using your ROMs.
#
#   ./scripts/validate-patches.sh --roms <dir> [--seconds 30] [--models "mk2 st ..."]
#
# Builds the core twice, patched and unpatched, renders the same benchmark
# sequence with each, and compares the audio digest. The digests must match for
# every romset. This is the gate for -DSC55D_PATCH_CORE=ON, and the reason that
# option ships off by default.

set -u

ROMS=""
SECONDS_ARG=30
MODELS=""

while [ $# -gt 0 ]; do
    case "$1" in
        --roms)    ROMS="${2:-}"; shift 2 ;;
        --seconds) SECONDS_ARG="${2:-30}"; shift 2 ;;
        --models)  MODELS="${2:-}"; shift 2 ;;
        -h|--help) sed -n '2,12p' "$0"; exit 0 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done

if [ -z "$ROMS" ] || [ ! -d "$ROMS" ]; then
    echo "error: --roms <dir> is required and must exist" >&2
    echo "       The comparison is meaningless without real ROMs: placeholder" >&2
    echo "       files render silence, and silence always compares equal." >&2
    exit 2
fi

cd "$(dirname "$0")/.." || exit 1

OFF_DIR=build-patch-off
ON_DIR=build-patch-on

echo "== Building unpatched and patched"
for spec in "$OFF_DIR:OFF" "$ON_DIR:ON"; do
    dir=${spec%%:*}
    val=${spec##*:}
    nuked_arg=""
    [ -n "${NUKED_DIR:-}" ] && nuked_arg="-DNUKED_DIR=$NUKED_DIR"
    if ! (cmake -S . -B "$dir" -DCMAKE_BUILD_TYPE=Release -DSC55D_PATCH_CORE=$val $nuked_arg > "$dir.log" 2>&1 &&
          cmake --build "$dir" -j"$(nproc)" >> "$dir.log" 2>&1); then
        echo "  build failed for SC55D_PATCH_CORE=$val; see $dir.log" >&2
        exit 1
    fi
    echo "  built $dir (SC55D_PATCH_CORE=$val)"
done

# Default to whichever romsets are actually complete in the ROM directory.
if [ -z "$MODELS" ]; then
    MODELS="mk2 st mk1 cm300 jv880 scb55 rlp3237 sc155 sc155mk2"
fi

echo
echo "== Comparing audio digests over ${SECONDS_ARG}s per romset"
echo

mismatches=0
compared=0
skipped=0

for model in $MODELS; do
    off=$("$OFF_DIR/sc55d" --roms "$ROMS" --model "$model" --bench \
              --bench-seconds "$SECONDS_ARG" --no-realtime --quiet-core 2>/dev/null \
          | awk '/audio digest/ {print $3, $4}')
    if [ -z "$off" ]; then
        printf '  %-10s skipped (romset not available)\n' "$model"
        skipped=$((skipped + 1))
        continue
    fi

    on=$("$ON_DIR/sc55d" --roms "$ROMS" --model "$model" --bench \
             --bench-seconds "$SECONDS_ARG" --no-realtime --quiet-core 2>/dev/null \
         | awk '/audio digest/ {print $3, $4}')

    case "$off" in
        *SILENT*)
            printf '  %-10s USELESS  rendered silence, so the digests prove nothing\n' "$model"
            skipped=$((skipped + 1))
            continue
            ;;
    esac

    compared=$((compared + 1))
    if [ "$off" = "$on" ]; then
        printf '  %-10s match    %s\n' "$model" "$off"
    else
        printf '  %-10s DIFFERS  unpatched %s / patched %s\n' "$model" "$off" "$on"
        mismatches=$((mismatches + 1))
    fi
done

echo
if [ "$compared" -eq 0 ]; then
    echo "Nothing was actually compared. Check the ROM directory."
    exit 1
fi
if [ "$mismatches" -gt 0 ]; then
    echo "FAIL: $mismatches of $compared romsets differ. Do not enable the patches."
    echo "      Please report which romset differs — that is a genuine bug."
    exit 1
fi
echo "PASS: $compared romsets identical, $skipped skipped."
echo "      Safe to build with -DSC55D_PATCH_CORE=ON."
exit 0
