# Qualcomm Iris V4L2/libVA backend

**English** | [中文](README.zh-CN.md)

A libva driver for the stateful V4L2 decoder that Qualcomm's Iris VPU exposes
on SM8550 and related SoCs. It gives Google Chrome, Electron and Firefox on
ARM64 Linux a hardware video decoder, and on the qualified target the frames
go from the firmware into Chrome's own buffers with no copy in between.

## Why this exists

Stock Google Chrome on arm64 Linux is built with `use_vaapi=true` and
`use_v4l2_codec=false`: the only hardware decode entry point compiled into the
binary is libva. The same is true of every prebuilt Electron application and
of Firefox's FFmpeg hwaccel. Without a VA-API driver those clients decode on
the CPU, and a 1080p HEVC stream on a tablet is a fan-and-battery problem.

Iris is a *stateful* V4L2 decoder: the firmware parses the bitstream itself
and the interface is an OUTPUT queue, a CAPTURE queue and a few controls.
Nobody had written the libva backend that turns VA-API decode calls into
Iris sessions. This repository is that backend. Chrome is the acceptance
client; FFmpeg, GStreamer, mpv and Firefox use the same driver and are
welcome, but they also have native V4L2 paths of their own.

## What works today

Every row is backed by a recorded hardware run in
[TEST-RESULTS.md](TEST-RESULTS.md) (boot ID, kernel, module hash and driver
hash for each). Qualified on a Snapdragon SM8550 tablet, Fedora 44, Google
Chrome 152.

| Codec | Chrome | Path in Chrome | Bit-exact check |
| --- | --- | --- | --- |
| H.264 (CB/Main/High) | hardware decoder | Import, zero-copy | frame MD5 equal to the native V4L2 decoder; Fluster JVT-AVC_V1 40/135, equal to the firmware's own pass set |
| HEVC Main | hardware decoder | Import, zero-copy | frame MD5 equal; Fluster JCT-VC-HEVC_V1 111/147 (firmware 112) |
| HEVC Main10 (P010) | hardware decoder | Import, zero-copy | frame MD5 equal at 10-bit |
| VP9 Profile 0 / 2 | hardware decoder | GPU blit (see limits) | frame MD5 equal, 8- and 10-bit |
| AV1 | not advertised | opt-in only | OBU rebuild bit-exact, but VA clients fail at the first picture (see limits) |

"Hardware decoder" means Chrome's media surface reports `VaapiVideoDecoder`
with the platform flag set and a decoded-frame count over a 30 s window with
no dropped frames; a mapped node or a running GPU process proves nothing.

The same display also answers `VAEntrypointEncSlice`, on the encoder node the
kernel exposes next to the decoder. Chrome does not use it; FFmpeg,
GStreamer and anything else that asks libva for an encode entrypoint do.

| Codec | Entrypoint | Input | Output | Verification |
| --- | --- | --- | --- | --- |
| H.264 Constrained Baseline / Main / High | `VAEntrypointEncSlice`, CBR, VBR or CQP | NV12 | Annex-B | 1080p `ffmpeg h264_vaapi`, 300-frame GOP-60 acceptance (TEST-RESULTS.md) |
| HEVC Main | `VAEntrypointEncSlice`, CBR, VBR or CQP | NV12 | Annex-B | 1080p `ffmpeg hevc_vaapi`, 300-frame GOP-60 acceptance (TEST-RESULTS.md) |

"Hardware encode" means the run moved the Iris IRQ counter, the bitstream
decodes without errors, and its SPS matches the requested profile, level and
geometry. A VA context that only returns bytes proves nothing.

## Requirements

- An SoC with the `qcom-iris` decoder (HFI gen2; SM8550 is the qualified
  one). aarch64.
- libva ≥ 1.1, libdrm ≥ 2.4.52; EGL/GLES/GBM for the GPU copy engine;
  `v4l-utils` at runtime (the launcher finds the decoder node by driver name
  because the number moves between boots).
- Any Linux distribution that can load a libva module. Fedora 44 is the
  qualified one; Fedora and Arch packaging is provided.
- Optionally, three Iris kernel changes (decode-order output, a 64-buffer
  CAPTURE pool and the encoder loop-filter controls) from
  [strongtz/libva-v4l2](https://github.com/strongtz/libva-v4l2), carried
  rebased in [`packaging/kernel/`](packaging/kernel/README.md) with a
  module-only build recipe. On a stock kernel the driver detects their
  absence and still works; Chrome then gets a GPU copy per frame instead of
  the zero-copy path, and an encoder deblocking request other than the
  firmware default is refused.

## Install and use

From the packages, on Fedora:

```sh
git archive --format=tar.gz --prefix=libva-v4l2-iris-0.1.0/ \
    -o ~/rpmbuild/SOURCES/libva-v4l2-iris-0.1.0.tar.gz HEAD
rpmbuild -ba packaging/fedora/libva-v4l2-iris.spec \
    --define "source_commit $(git rev-parse HEAD)" \
    --define "tracked_sha256 $(git ls-files -z | xargs -0 sha256sum | sha256sum | awk '{print $1}')"
sudo dnf install ~/rpmbuild/RPMS/aarch64/libva-v4l2-iris-*.rpm
```

Arch uses `packaging/arch/PKGBUILD`. Both install the driver into libva's
driver directory, an `msm_drv_video.so` symlink beside it, the
`iris-vaapi-browser` launcher and a "Google Chrome (Iris VA-API)" desktop
entry, plus an install manifest with the source commit and artifact hash.
The symlink is what libva resolves on its own: this SoC reports
`DRIVER=msm_dpu`, and without the alias a client that does not set
`LIBVA_DRIVER_NAME` (`mpv`, VLC, GStreamer) opens no driver at all. Then
launch Chrome from that desktop entry, or from a shell:

```sh
iris-vaapi-browser --browser=chrome
```

The launcher resolves the Iris node on every start, sets `LIBVA_DRIVER_NAME`
for that process only and passes no Chrome feature overrides; Chrome's
packaged defaults are what this project qualifies. Check the result in
`chrome://media-internals` while a video plays: the decoder must read
`VaapiVideoDecoder` with `kIsPlatformVideoDecoder: true`.

From a checkout instead of a package:

```sh
meson setup build && meson compile -C build
sudo ./scripts/install-system.sh build       # libva driverdir + launcher under /usr/local
LIBVA_DRIVERS_PATH=$PWD/build/src LIBVA_DRIVER_NAME=v4l2 vainfo   # or just try it
```

`scripts/uninstall-system.sh` reverses the helper install using the recorded
manifest. Details of the lab workflow are in
[`docs/build-and-deploy.md`](docs/build-and-deploy.md).

Other VA clients need no configuration once the package is installed, because
of the `msm_drv_video.so` alias. One exception: GStreamer's VA plugin keeps a
vendor allow-list and refuses an unknown driver, so the `va` elements only
register with `GST_VA_ALL_DRIVERS=1`. That is GStreamer's gate, not the
driver's; with it set, `vah264dec` and `vah265dec` decode through Iris.

## How it decodes

One ownership model with three publish policies, chosen per surface from the
client's behaviour. The driver owns the CAPTURE pool.

- **Direct**: a surface that is exported or mapped only after its decode
  (FFmpeg, GStreamer, mpv) takes the completed CAPTURE slot itself. True
  zero-copy.
- **Import**: a surface the client exported *before* its first decode (Chrome
  pre-exports its whole pool) has its buffer queued into CAPTURE for exactly
  that picture, so the firmware decodes straight into it. Needs decode-order
  output from the kernel patch; selected per codec from the measured
  evidence (H.264, HEVC, Main10).
- **Copy**: the same pre-exported surface is filled by a GPU blit (EGL on
  Adreno) from the driver's slot into a stable buffer whose file descriptor
  never changes. VP9 and stock kernels take this path.

Import holds because of two measured rules: exactly one client buffer is
with the firmware at a time, and no submitted picture is ever skipped. A
completion that lands in a neighbour's buffer fails both surfaces instead of
publishing. The contract, the state machine and the trace vocabulary are in
[`docs/architecture.md`](docs/architecture.md); the measurements are in
[TEST-RESULTS.md](TEST-RESULTS.md).

A client can also hand over the memory itself: `vaCreateSurfaces2` with
`VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2` (or the older `_DRM_PRIME`) creates
a surface over one linear NV12/P010 DMA-BUF instead of allocating one. The
buffer must already carry the CAPTURE layout the firmware writes — 128-byte
pitch (256 for P010), chroma after the 32-aligned luma rows — and the fd is
duplicated, so the client may close its own. The surface then takes whichever
policy above applies (Import on a decode-order kernel, Copy otherwise) and
is usable as an encode input as well.

## Known limits

- **VP9 stays on the GPU copy path.** A VP9 alt-ref access unit decodes as
  two pictures. With one buffer in flight the session stalls about 500 ms
  per alt-ref; with two, the firmware chooses the buffer itself and about a
  quarter of the completions land in the wrong one. Copy until the
  translator submits one access unit per picture.
- **AV1 is opt-in and does not decode through VA here.** The firmware
  returns no CAPTURE buffer for hidden (`show_frame=0`) pictures and VA has
  no `show_existing_frame` call; on top of that, the translator must hold
  each picture until the next one to recover `refresh_frame_flags`, so a
  client downloads a surface before its picture is submitted. Both failures
  hit the first picture of every stream. The OBU rebuild is bit-exact under
  `test/av1-obu-roundtrip.sh`; use native V4L2 (`v4l2av1dec`) instead.
  `V4L2_VA_EXPERIMENTAL_PROFILES=1` advertises the profile for qualification
  only.
- **Stock kernel means Copy for Chrome.** Without the decode-order control
  the session runs in display-order mode: bounded sync waits plus one
  STOP/LAST/START drain on timeout, and Import is refused.
- **Imported surfaces must match the CAPTURE layout.** A client DMA-BUF with
  a tiled modifier, more than one object, a pitch of the client's choosing or
  a chroma plane at a different offset is refused at `vaCreateSurfaces2` with
  `VA_STATUS_ERROR_INVALID_PARAMETER` rather than decoded into the wrong
  pixels. PRIME_1 has no modifier field; there the driver trusts
  `VA_SURFACE_EXTBUF_DESC_ENABLE_TILING` to say the buffer is linear.
- **Chrome 153 HEVC seek regression, upstream.** Chromium 153 turned on
  `ExtendedVideoBitstreamValidation`, and its shared `H265Decoder` treats
  the first SPS after every seek as a configuration change, so the
  compositor keeps showing pre-seek frames. Reported and bisected by
  [CFM880/iris-vaapi](https://github.com/CFM880/iris-vaapi) on SM8150, filed
  as [Chromium issue 563075803](https://issues.chromium.org/issues/563075803).
  Not reproduced here because the target runs Chrome 152; on 153, launch
  with `--disable-features=ExtendedVideoBitstreamValidation` or stay on 152.
- **One qualified device.** Everything above was measured on one SM8550
  tablet. Other Iris gen2 SoCs should work but carry no evidence yet; Arch
  packaging is provided but not validated.
- **On-demand IDR works by moving the GOP, not by a keyframe control.** The
  node exposes no `V4L2_CID_MPEG_VIDEO_FORCE_KEY_FRAME` and the firmware
  capability list has no RAP control, so a requested IDR is produced by
  setting the intra period to zero for that one picture and putting it back
  before the picture is queued. `-force_key_frames` with a stream copy or a
  segment muxer therefore cuts where it asks; the next automatic IDR is one
  GOP after the requested one, not after the original schedule. The
  intra-period change is a control write per requested picture, so a request
  on every frame costs more than a plain intra-only stream. A requested IDR
  is a cut point: it carries the last SPS/PPS (and SEI) the firmware wrote.
- **No B-frames.** The kernel allows at most one, and a delayed CAPTURE would
  break the one-in-one-out model. A context whose sequence parameters ask for
  `ip_period > 1` fails with `VA_STATUS_ERROR_INVALID_PARAMETER`.
- **No 10-bit, VP9 or AV1 encode.** The encoder OUTPUT queue enumerates NV12
  and QC08C only, so HEVC Main10 and VP9 Profile 2 cannot be fed.
- **No packed headers, so no extradata at open time.** The firmware writes
  SPS/PPS itself and the driver refuses to echo a second set. FFmpeg copes:
  it warns, carries the parameter sets in-band, and extracts `avcC`/`hvcC`
  from the first packet, so `.mp4` output works directly (43 bytes of
  extradata for H.264, 124 for HEVC). GStreamer's VA encoder is stricter and
  refuses a driver that advertises no packed headers at all, so `vah264enc`
  and `vah265enc` do not work; FFmpeg's `h264_vaapi`/`hevc_vaapi` do.
- **One picture per context at a time.** `vaEndPicture` runs the whole V4L2
  transaction, and a VA context has a fixed geometry: changing resolution
  needs a new context.
- **No multi-slice, tiles, ROI or `-max_frame_size`.** FFmpeg fails those
  requests loudly; they are not silently ignored. The driver has the same
  rule: more than one slice in the picture parameters, tiles, wavefront
  coding, weighted prediction, scaling lists, chroma QP offsets and several
  reference indexes are `VA_STATUS_ERROR_UNIMPLEMENTED`, because the
  firmware writes the sequence headers, owns the reference structure and
  picks the quantisation, and a stream that quietly used something else
  would be worse than a failure.
- **Slice deblocking is forwarded, or refused.** The VA slice parameters are
  the client's one chance to ask for filtering to be off or throttled, and
  the driver forwards them to
  `V4L2_CID_MPEG_VIDEO_H264_LOOP_FILTER_MODE`/`_ALPHA`/`_BETA` (and the HEVC
  equivalents) when the kernel has them. The stock SM8550 kernel does not,
  and there the firmware always filters with the default 0/0 offsets, so any
  other request fails with `VA_STATUS_ERROR_UNIMPLEMENTED` instead of coming
  back as a stream that ignores it. [`packaging/kernel/`](packaging/kernel/README.md)
  carries the kernel patch that adds the controls. Deblocking is per-context
  in V4L2 and per-slice in VA, so it is read from the first slice and changing
  it mid-context needs a new context.

## Development

```sh
meson setup build-local && meson compile -C build-local
sh test/run-static-checks.sh
meson test -C build-local --print-errorlogs
git diff --check
```

Hardware gates run on the target: `test/iris-browser-acceptance.sh --clip …`
(Chrome evidence), `test/iris-matrix.sh` (frame MD5 against the native
decoders), `test/iris-structure-matrix.sh`, `test/iris-dynamic-resolution.sh`,
`test/iris-concurrency-soak.sh`, `test/iris-zero-copy-suite.sh`. Experiments
that can reset the firmware go through `scripts/iris-experiment-guard.sh`.
The full list is in [`test/README.md`](test/README.md); the rules for
contributors and for coding agents are in [CONTRIBUTING.md](CONTRIBUTING.md)
and [AGENTS.md](AGENTS.md).

## License and credits

MIT with LGPL-2.1-covered parts, the same terms as upstream; see `COPYING`,
`COPYING.MIT` and `COPYING.LGPL`.

- This is a fork of [mxsrc/libva-v4l2](https://github.com/mxsrc/libva-v4l2),
  itself descended from Bootlin's libva-v4l2-request. The VA and V4L2
  scaffolding comes from there; the stateful Iris implementation in `src/`,
  the browser acceptance gates and the packaging are this fork's work.
- `include/linux/` vendors the kernel UAPI headers under their own SPDX tag
  (`GPL-2.0+ WITH Linux-syscall-note OR BSD-3-Clause`).
- `packaging/kernel/` carries three Iris kernel changes from
  [strongtz/libva-v4l2](https://github.com/strongtz/libva-v4l2) (Radxa, Xilin
  Wu). They touch GPL-2.0 kernel code and carry that license, not the
  driver's. The explicit-RPS HEVC slice rewrite follows the same project's
  approach.

## Environment variables

Every switch the driver reads is listed here and read in exactly one place,
`src/util/options.cc`; `test/iris-env-doc-check.sh` fails the build if this
table and that file disagree in either direction. All of them are diagnostic;
the launcher sets none of them.

| Variable | Default | Purpose |
| --- | --- | --- |
| `LIBVA_V4L2_VIDEO_PATH` | probe | Use this decoder node instead of scanning `/dev/video*` for the `iris_driver` decoder. |
| `LIBVA_V4L2_ENCODER_PATH` | probe | Use this encoder node instead of scanning `/dev/video*` for the `iris_driver` encoder. A path that is not an Iris encoder fails display init. |
| `V4L2_VA_TRACE` | off | Per-frame trace on stderr. Any value enables it. The record vocabulary is documented in `docs/architecture.md`. |
| `V4L2_VA_SYNC_TIMEOUT_MS` | `2000` | Bounded `vaSyncSurface()` wait, `50..60000`. Without an override the first frame of a cold session may wait up to `30000` ms for firmware bring-up. |
| `V4L2_VA_COPY` | `gpu` | Engine used to publish a frame into a surface the client exported before its first decode: `gpu` (EGL blit on Adreno) or `cpu`. |
| `V4L2_VA_PUBLISH` | `auto` | Force `copy`, `direct` or `import` publication for every surface. Diagnostics only; `auto` picks per surface from the client's export behaviour (Direct for post-decode exporters, Import for pre-exporters on a decode-order kernel, Copy otherwise). `import` degrades to `copy` where the session cannot honour it. |
| `V4L2_VA_DUMP` | off | Directory to write every submitted access unit into. |
| `V4L2_VA_EXPERIMENTAL_PROFILES` | off | Also advertise AV1. Qualification only; the launcher never sets it. |
