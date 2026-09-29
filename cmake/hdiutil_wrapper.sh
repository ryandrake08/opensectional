#!/bin/bash
# Stand-in for hdiutil, used by CPack's DragNDrop generator via
# CPACK_COMMAND_HDIUTIL. Every subcommand passes through to the real hdiutil
# except `create -srcfolder`.
#
# `hdiutil create -srcfolder` copies the staged tree preserving file
# ownership. When the build tree is on a network share whose files belong to
# another uid, that needs root, and without a GUI session (e.g. over SSH) it
# fails with "user interaction required for authorization". Instead: create
# an empty read-write image, attach it, copy the tree in as the current user,
# and detach. The image is left in UDRW format whatever -format asks for;
# CPack always runs `hdiutil convert` to the final format afterwards.
set -euo pipefail

HDIUTIL=/usr/bin/hdiutil

case " $* " in
    " create "*" -srcfolder "*) ;;
    *) exec "${HDIUTIL}" "$@" ;;
esac

srcfolder="" volname="" fs="HFS+" image=""
shift
while [ $# -gt 0 ]; do
    case "$1" in
        -ov) ;;
        -srcfolder) srcfolder="$2"; shift ;;
        -volname) volname="$2"; shift ;;
        -fs) fs="$2"; shift ;;
        -format) shift ;;
        -*) echo "hdiutil_wrapper: unsupported create option: $1" >&2; exit 1 ;;
        *) image="$1" ;;
    esac
    shift
done
if [ -z "${srcfolder}" ] || [ -z "${image}" ] || [ -z "${volname}" ]; then
    echo "hdiutil_wrapper: create needs -srcfolder, -volname, and an image path" >&2
    exit 1
fi

# Size: the tree's allocated size plus one 4 KiB block per entry for
# small-file slack, plus 25% and 64 MiB for filesystem overhead. Free space
# costs nothing in the final image; convert compresses it away.
used_kb=$(du -sk "${srcfolder}" | awk '{print $1}')
entries=$(find "${srcfolder}" | wc -l)
size_kb=$(( (used_kb + entries * 4) * 5 / 4 + 65536 ))

"${HDIUTIL}" create -ov -size "${size_kb}k" -fs "${fs}" -volname "${volname}" \
    -type UDIF "${image}"

mountpoint=$("${HDIUTIL}" attach -nobrowse -noautoopen -noverify "${image}" \
    | sed -n 's|^.*\(/Volumes/.*[^[:space:]]\)[[:space:]]*$|\1|p' | tail -n 1)
if [ -z "${mountpoint}" ]; then
    echo "hdiutil_wrapper: could not determine mount point of ${image}" >&2
    exit 1
fi

detach()
{
    for _ in 1 2 3 4 5; do
        "${HDIUTIL}" detach "${mountpoint}" >/dev/null && return 0
        sleep 2
    done
    "${HDIUTIL}" detach -force "${mountpoint}"
}
trap detach EXIT

cp -R "${srcfolder}/." "${mountpoint}/"
