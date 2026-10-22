# Build And Deploy

The target-independent build is reproducible on the development host:

```sh
CXX=/usr/bin/g++ CC=/usr/bin/gcc PKG_CONFIG=/usr/bin/pkg-config \
PKG_CONFIG_PATH=/usr/lib64/pkgconfig meson setup build-local
ninja -C build-local
ldd -r build-local/src/v4l2_drv_video.so
```

The last command must print no `undefined symbol` lines. A successful link is
not enough for a VA-API driver: an unresolved relocation otherwise appears to
clients as a silent software-decoder fallback.

For a tablet or other V4L2 target, set `IRIS_REMOTE_HOST` to an SSH config
host or address and run:

```sh
IRIS_REMOTE_HOST=your-tablet-host ./test/remote/deploy.sh
```

The remote scripts require this variable and do not contain a project-local
host default. This keeps a public checkout from accidentally targeting a
maintainer's private lab machine.

The deploy script synchronizes by content, excludes generated build and test
artifacts, refreshes source timestamps, performs a clean Ninja rebuild, and
gates the resulting shared object with `ldd -r`. It rejects absolute,
dot-relative, parent-relative, whitespace-containing and shell-active
`IRIS_REMOTE_ROOT` values before the `rsync --delete` command can contact the
target. The remote lab directory is durable and defaults to
`$HOME/iris-vaapi-lab` (`IRIS_LAB_ROOT` overrides it); downloaded Fluster
resources and Qualcomm test tools
stay there rather than under a disposable scratch directory.

Every successful deployment also writes
`build/iris-vaapi-provenance.json`. Because `.git` is deliberately not copied
to the target, `deploy.sh` resolves the full source commit and a SHA-256 over
the current contents of all tracked paths locally, then explicitly injects
both values into the clean remote build. The manifest additionally records
whether the source checkout was dirty, Meson's compiler inventory, the libva
and Meson versions, the target kernel, and the exact driver path, size and
SHA-256. Deployment immediately verifies the manifest against the generated
shared object; a copied, rebuilt or modified `.so` cannot retain a passing
record accidentally.

To re-check a deployed artifact later:

```sh
cd "$HOME/iris-vaapi-lab/src"
python3 test/remote/provenance.py verify \
  --manifest build/iris-vaapi-provenance.json \
  --artifact build/src/v4l2_drv_video.so
```

For a source export that has no local `.git`, deployment requires the source
identity to be explicit:

```sh
IRIS_SOURCE_COMMIT=<full-object-id> \
IRIS_TRACKED_SOURCE_SHA256=<sha256-of-tracked-paths> \
./test/remote/deploy.sh
```

The decoder node is resolved by the `iris_driver` name in
`test/remote/setup.sh`. Do not hardcode `/dev/video17`: V4L2 numbering changes
across boots and that node may belong to the camera subsystem. Override the
resolved node for a run with `LIBVA_V4L2_VIDEO_PATH`.

After deployment, run the short positive oracle before external corpora:

```sh
ssh "$IRIS_REMOTE_HOST" 'cd "$HOME/iris-vaapi-lab/src" && \
  . ./test/lib/iris-env.sh && device=$(iris_resolve_device) && \
  LIBVA_DRIVER_NAME=v4l2 \
  LIBVA_DRIVERS_PATH=$PWD/build/src \
  LIBVA_V4L2_VIDEO_PATH="$device" \
  ./test/iris-matrix.sh'
```

The matrix requires exact software/VA frame MD5 equality and strict stateful
EOS accounting. Fluster and native V4L2 suites are additional capability and
firmware diagnostics; always record which decoder path was used.

## Durable system and browser installation

The remote deployment above intentionally runs from the build tree. Once that
artifact has passed the codec matrix and soak gates, install it into libva's
configured system driver directory together with the process-scoped browser
launcher and desktop entries:

```sh
cd "$HOME/iris-vaapi-lab/src"
sudo ./scripts/install-system.sh build
```

`install-system.sh` obtains `driverdir` from `pkg-config libva` (falling back
to libva's `libdir/dri` only when the package omits `driverdir`) and runs
`ldd -r` before copying the shared object. It installs the launcher under
`/usr/local/bin` and desktop files under `/usr/local/share/applications` by
default. `IRIS_INSTALL_PREFIX` changes only the launcher/data prefix; it does
not redirect the VA driver away from libva's canonical driver directory.
`DESTDIR` provides a non-root packaging/staging mode.

The helper records an install manifest at
`/usr/local/share/iris-vaapi/install-manifest.txt` (or the selected prefix).
It includes the source commit, tracked-source digest, artifact hash, resolved
driver path, and any pre-existing driver backup. To remove only that recorded
installation and restore the previous driver, run
`sudo ./scripts/uninstall-system.sh`; removal refuses to proceed if the
installed artifact was modified after installation. A `DESTDIR` staging run
is packaging evidence only and must not be described as a live system install.

Package builders must refresh the manifest after any ELF post-processing. The
Fedora spec runs the distribution brp strip hooks before
`scripts/refresh-install-manifest.sh`; the Arch PKGBUILD disables makepkg's
later implicit strip, strips explicitly, and runs the same refresh. This keeps
the recorded artifact hash and size equal to the installed package payload.

The launcher resolves `/dev/video*` on every start. It requires an exact
`iris_driver` match and prefers the node whose card or sysfs name identifies it
as the decoder, so the sibling Iris encoder cannot be selected merely because
its number happens to sort first. An explicit `LIBVA_V4L2_VIDEO_PATH` remains
available for a checked diagnostic override. `v4l2-ctl` from v4l-utils is
therefore a runtime dependency of the desktop launcher.

Check the installed driver without a build-tree path override:

```sh
node=$(iris-vaapi-browser --print-node)
env -u LIBVA_DRIVERS_PATH \
  LIBVA_DRIVER_NAME=v4l2 \
  LIBVA_V4L2_VIDEO_PATH="$node" \
  vainfo
```

The desktop entry, `Google Chrome (Iris VA-API)`, uses Wayland and passes no video-decoder feature
override at all:

```text
--ozone-platform=wayland
```

The entry uses a profile below `${XDG_CONFIG_HOME:-$HOME/.config}/iris-vaapi-browser`; this
isolation ensures a pre-existing ordinary browser process cannot absorb a new
URL while silently ignoring the requested GPU flags. Set
`IRIS_BROWSER_PROFILE_ROOT` only when a different durable profile location is
needed.

### Browser acceptance gate

Successful playback is not proof of hardware decoding. Use a high-resolution
H.264 or HEVC sample, open `chrome://media-internals`, select the player by its
exact URL, and require both:

```text
kVideoDecoderName = VaapiVideoDecoder
kIsPlatformVideoDecoder = true
```

The qualified Chrome 151 run on August 30, 2026 reached this gate with the
project driver mapped in the GPU process and a dynamically resolved Iris node,
`VaapiVideoDecoder`, and `kIsPlatformVideoDecoder=true`; its VA trace then
showed repeated `vaBeginPicture`/`vaEndPicture` and `stateful capture complete`
events. The Fedora Chromium 151 run is a separate native-V4L2 baseline and
reported `V4L2VideoDecoder`; that result does not prove the project's VA driver.

Also record decoded/dropped frame counts, confirm the Iris V4L2 context and
OUTPUT/CAPTURE activity in a trace-enabled diagnostic run, and compare the
device boot ID before and after. Small videos may fall below Chromium's
hardware-decoder performance threshold, so a low-resolution
`FFmpegVideoDecoder` result is not a release verdict. H.264, HEVC, VP9 and
AV1 are the qualified VA profiles (each with its own caveat in the README).
Validate Google
Chrome; Fedora's Chromium decodes through its own native V4L2 stack and its
result says nothing about this driver.

## Distribution packaging

`scripts/install-system.sh` is enough for a lab target, but it needs a build
tree, a checkout and root. For an image that should work on first boot, build
a package instead. Both manifests live under `packaging/` and both install
through that same script, so the packaged layout cannot drift from the one the
lab uses:

| Target | Manifest | libva driver directory |
| --- | --- | --- |
| Fedora aarch64 | `packaging/fedora/libva-v4l2-iris.spec` | `%{_libdir}/dri` |
| Arch aarch64 | `packaging/arch/PKGBUILD` | `/usr/lib/dri` |

```sh
# Fedora
git archive --format=tar.gz --prefix=libva-v4l2-iris-0.1.0/ \
    -o ~/rpmbuild/SOURCES/libva-v4l2-iris-0.1.0.tar.gz HEAD
rpmbuild -ba packaging/fedora/libva-v4l2-iris.spec \
    --define "source_commit $(git rev-parse HEAD)" \
    --define "tracked_sha256 $(git ls-files -z | xargs -0 sha256sum | sha256sum | awk '{print $1}')"
```

Both declare `v4l-utils` as a hard runtime dependency. The launcher resolves
the decoder node by driver name because the number moves between boots on this
device, and without `v4l2-ctl` it exits instead of falling back to a fixed
`/dev/videoN` that may be a camera.

Google Chrome is not a dependency. The desktop entry names the browser in
`TryExec`, so on a system without Chrome the entry hides itself instead of
appearing in the menu and failing on click.

Which browser benefits differs by distribution. On Fedora 44, Chromium
151.0.7922.169 already decodes in hardware through Chromium's own native V4L2
stack and reports `V4L2VideoDecoder` without loading this driver at all.
Google Chrome has no such path and is the client this driver exists for; it is
the one that moves from `FFmpegVideoDecoder` to `VaapiVideoDecoder`.

`test/iris-packaging-check.sh` runs in CI and fails when a shipped desktop
entry is missing from the spec's `%files`, when a manifest stops delegating to
`install-system.sh`, when `TryExec` disagrees with the browser the entry
launches, or when `v4l-utils` is dropped.

For the Arch image projects, drop `packaging/arch/PKGBUILD` into the image
repository's `pkgs/libva-v4l2-iris/` directory and add it to the package list.
The generated rootfs then ships the driver in libva's standard directory and
no user has to set `LIBVA_DRIVERS_PATH`. That path is not validated here: the
qualified target in this repository is Fedora, and an Arch image needs its own
`vainfo` and `media-internals` acceptance before its result may be claimed.

## Dynamic resolution qualification

The production/fallback/rejected codec boundary is machine-readable in
`data/iris-codec-capabilities.json`; keep it in sync with source capability
advertisement and test expectations. It is intentionally conservative: a
codec with an empty `va_profiles` list is not a VA production capability even
when the native V4L2 node can decode it.

For experiments that may reset firmware, wrap the bounded command with
`scripts/iris-experiment-guard.sh -- ...`. It records the initial boot ID,
thermal samples and command output, enforces a timeout, and refuses to reuse a
running guard file until the previous run has been inspected. A reset or
thermal breach is a failed experiment, not evidence of codec support.

Run the dedicated H.264 reconfiguration gate on the target:

```sh
. ./test/lib/iris-env.sh
device=$(iris_resolve_device)
LIBVA_DRIVER_NAME=v4l2 \
LIBVA_DRIVERS_PATH=$PWD/build/src \
LIBVA_V4L2_VIDEO_PATH="$device" \
./test/iris-dynamic-resolution.sh
```

The production default assembles deterministic 640x360 and 1280x720 keyframe
segments into streams with exactly 100 geometry changes. H.264 passes only if
the native GStreamer V4L2 decoder produces byte-identical I420 output, one
FFmpeg VA process survives every change with exact software frame MD5 and EOS
accounting, and 101 fresh VA processes decode the alternating geometries with
the same checks. FFmpeg is allowed to recreate VA contexts on a coded-size
change; the single-process lane therefore qualifies context retirement and
session cleanup rather than asserting a client behavior FFmpeg does not have.

VP9 runs the same lanes with `IRIS_DYNAMIC_CODECS=vp9`; on the decode-order
kernel module it passed all 100 switches in both lanes (TEST-RESULTS.md,
Phase 5). Run it through `scripts/iris-experiment-guard.sh` on any other
kernel, because the old display-order module rebooted on VP9 source changes.
A missing device format, required element or VA capability is a failure,
not a skip.
Development runs may shorten the matrix explicitly with
`IRIS_DYNAMIC_SWITCHES` and `IRIS_DYNAMIC_FRAMES_PER_SEGMENT`; qualification
results must retain the defaults.

## Concurrent soak qualification

The concurrent runner has separate dual-H.264 and mixed H.264+HEVC scenarios:

```sh
./test/iris-concurrency-soak.sh dual-h264
./test/iris-concurrency-soak.sh mixed
# Run both sequentially:
./test/iris-concurrency-soak.sh all
```

Each selected scenario defaults to 7200 seconds of wall-clock playback, using
real-time throttling rather than decoding two hours of timestamps as quickly
as possible. Before the soak starts, every selected codec must pass an exact
software/VA frame-MD5 oracle. During the run the script records progress,
driver traces, boot ID, uptime and every readable thermal zone. It terminates
workers and fails on a boot-ID change, an unreadable thermal inventory, a
temperature above 85000 millidegrees Celsius, a driver timeout/timestamp
error, incomplete stateful teardown, too few decoded frames, or any worker
failure.

`IRIS_SOAK_SECONDS` may shorten a development run, but the report is labelled
`SHORTENED` unless it is exactly 7200 seconds. `IRIS_SOAK_MAX_TEMP_MILLIC` and
`IRIS_SOAK_TEMPERATURE_INTERVAL_SECONDS` tune the thermal guard. A durable
guard file records a running test; a later invocation refuses to hide an
interrupted or rebooted soak until the operator inspects it and deliberately
sets `IRIS_SOAK_ALLOW_STALE_GUARD=1`.

## Host-side release gate

The CI and local release sequence is:

```sh
meson setup build
ninja -C build
sh test/run-static-checks.sh
meson test -C build --list
meson test -C build --no-rebuild --print-errorlogs
ldd -r build/src/v4l2_drv_video.so
```

CI asserts that the Meson inventory is non-empty and names every required
hardware-independent test, including `stateful-session` and the provenance
tamper test, before executing it. This prevents a renamed, unregistered or
unbuilt behavioural test from yielding an empty green result.
