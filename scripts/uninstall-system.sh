#!/bin/sh

# Remove an installation created by install-system.sh and restore the driver
# that was present before it. Refuses to remove a driver whose hash changed.

set -eu

prefix=${IRIS_INSTALL_PREFIX:-/usr/local}
destdir=${DESTDIR:-}
manifest=${IRIS_INSTALL_MANIFEST:-$destdir$prefix/share/iris-vaapi/install-manifest.txt}
fail() { echo "uninstall-system.sh: $*" >&2; exit 1; }
[ -f "$manifest" ] || fail "install manifest not found: $manifest"
get() { awk -F= -v key="$1" '$1 == key { sub(/^[^=]*=/, ""); print; exit }' "$manifest"; }
schema=$(get schema)
[ "$schema" = qualcomm-iris-vaapi/system-install-v1 ] || fail "unsupported manifest schema: $schema"
# The manifest records target paths without DESTDIR so a packaged build does
# not carry its build root; a staged uninstall re-applies its own.
driver=$destdir$(get driver_path)
alias_path=$destdir$(get driver_alias_path)
launcher=$destdir$(get launcher_path)
desktop_dir=$destdir$(get desktop_dir)
expected=$(get artifact_sha256)
backup=$(get previous_driver_backup)
[ -n "$backup" ] && backup=$destdir$backup
[ -n "$driver" ] || fail 'manifest has no driver path'
[ -n "$launcher" ] || fail 'manifest has no launcher path'
[ -n "$desktop_dir" ] || fail 'manifest has no desktop directory'
case "$driver" in
    *'/../'*|*'/..'|*'/./'*|*'/.'|*'//') fail "unsafe driver path: $driver" ;;
esac
case "$driver" in /*/v4l2_drv_video.so) ;; *) fail "unexpected driver path: $driver" ;; esac
case "$alias_path" in
    ''|/*/msm_drv_video.so) ;;
    *) fail "unexpected driver alias path: $alias_path" ;;
esac
case "$launcher" in
    *'/../'*|*'/..'|*'/./'*|*'/.'|*'//') fail "unsafe launcher path: $launcher" ;;
esac
case "$launcher" in /*/iris-vaapi-browser) ;; *) fail "unexpected launcher path: $launcher" ;; esac
case "$desktop_dir" in
    *'/../'*|*'/..'|*'/./'*|*'/.'|*'//') fail "unsafe desktop directory: $desktop_dir" ;;
esac
case "$desktop_dir" in /*/applications) ;; *) fail "unexpected desktop directory: $desktop_dir" ;; esac
case "$backup" in
    *'/../'*|*'/..'|*'/./'*|*'/.'|*'//') fail "unsafe backup path: $backup" ;;
esac
case "$backup" in ''|/*/backup/v4l2_drv_video.so) ;; *) fail "unexpected backup path: $backup" ;; esac
[ -f "$driver" ] || fail "installed driver is missing: $driver"
[ -L "$driver" ] && fail "refusing to remove symlinked driver: $driver"

# The same layout now also ships as an RPM and an Arch package, and those
# install the manifest too. Deleting the files behind the package manager
# leaves it convinced they are still there, so the next upgrade or verify
# reports a corrupt package and reinstalling does not obviously fix it.
# Whoever owns the file gets to remove it.
if command -v rpm >/dev/null 2>&1 && rpm -qf "$driver" >/dev/null 2>&1; then
    fail "$driver belongs to $(rpm -qf "$driver"); remove it with dnf or rpm"
fi
if command -v pacman >/dev/null 2>&1 && pacman -Qo "$driver" >/dev/null 2>&1; then
    fail "$driver belongs to $(pacman -Qoq "$driver"); remove it with pacman"
fi

actual=$(sha256sum "$driver" | awk '{print $1}')
[ "$actual" = "$expected" ] || fail "refusing to remove modified driver (expected $expected, got $actual)"
# The alias is ours only while it points at our driver.
if [ -n "$alias_path" ] && [ -L "$alias_path" ] \
    && [ "$(readlink "$alias_path")" = v4l2_drv_video.so ]; then
    rm -f "$alias_path"
fi
rm -f "$driver" "$launcher"
for desktop in "$desktop_dir"/*.desktop; do
    [ -f "$desktop" ] || continue
    case "$desktop" in
        *chromium-iris-v4l2.desktop|*google-chrome-iris-v4l2.desktop|*chromium-iris-vulkan-webgpu.desktop|*google-chrome-iris-vulkan-webgpu.desktop) rm -f "$desktop" ;;
    esac
done
if [ -n "$backup" ] && [ -f "$backup" ]; then
    install -D -m 0755 "$backup" "$driver"
    rm -f "$backup"
fi
rm -f "$manifest"
rmdir "$desktop_dir" 2>/dev/null || true
rmdir "$(dirname "$manifest")/backup" "$(dirname "$manifest")" 2>/dev/null || true
printf 'PASS system installation removed; previous driver restored when recorded\n'
