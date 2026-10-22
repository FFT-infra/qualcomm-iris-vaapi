# Iris kernel module patches

Two patches, from [strongtz/libva-v4l2](https://github.com/strongtz/libva-v4l2)
(Radxa, Xilin Wu), rebased onto `ianchb/sm8550-mainline` branch `sheng-7.2.6`
(commit `42f3b40c702a`):

| Patch | Upstream | What it changes | Why the driver wants it |
| --- | --- | --- | --- |
| `0001-media-iris-decode-order-output-and-64-capture-buffers.patch` | `patches/0001` + `patches/0002` | Decode-order output through `V4L2_CID_MPEG_VIDEO_DEC_DISPLAY_DELAY{,_ENABLE}`; CAPTURE pool up to 64 | Chrome's zero-copy (Import) path, and the VP9 dynamic-resolution qualification |
| `0002-media-iris-deblocking-filter-controls.patch` | `patches/0003` | H.264 loop filter mode/alpha/beta and the HEVC equivalents, sent to firmware as `HFI_PROP_DEBLOCKING_MODE` | The driver forwards a client's slice deblocking request instead of refusing a non-default one |

Both patches modify GPL-2.0 kernel code (`drivers/media/platform/qcom/iris/`)
and are therefore GPL-2.0, unlike the driver itself. Upstream authors are
credited in each patch's trailer and in the driver's README. The driver works
without either: no display-delay control means the display-order fallback and
a GPU copy per Chrome frame; no loop-filter controls means the firmware's
enabled, 0/0 defaults and `VA_STATUS_ERROR_UNIMPLEMENTED` for any other
deblocking request.

Rebuild only the module, on the target, against the running kernel:

```sh
git clone --depth 1 -b sheng-7.2.6 https://github.com/ianchb/sm8550-mainline linux
cd linux && git am ../0001-*.patch ../0002-*.patch
```

`git am` applies both cleanly on `sheng-7.2.6`. On a differently shaped Iris
tree expect fuzz, and check two things by hand before building:

- The capability IDs belong in `enum platform_inst_fw_cap_type` in
  `iris_platform_common.h`, not in the neighbouring
  `enum platform_inst_fw_cap_flags`. A fuzzy 0001 apply has put
  `DISPLAY_DELAY_ENABLE`/`DISPLAY_DELAY` into the flags enum (seen on
  `sheng-7.2.2`).
- Upstream 0003 is written against a tree with two gen2 encoder tables
  (`milos_enc` and `sm8550_enc`). `sheng-7.2.6` has only
  `inst_fw_cap_sm8550_enc`, so both of the patch's table hunks land in the same
  array under fuzz and the six DEBLOCK entries appear twice. This rebased
  patch adds them once, after `ENTROPY_MODE` in `inst_fw_cap_sm8550_enc`.
  Verify with
  `grep -c 'cap_id = DEBLOCK_MODE_H264' drivers/media/platform/qcom/iris/iris_hfi_gen2.c`
  — it must print 1.

```sh
cp /boot/config-$(uname -r) .config
export LOCALVERSION=""                      # keep the "+" off the release string
make ARCH=arm64 LLVM=1 olddefconfig modules_prepare
# Module.symvers is not shipped; derive the dependency symbols from the
# running kernel so modpost records "depends:" correctly.
awk '$4 ~ /^\[/ && $2 ~ /[TDR]/ {gsub(/[\[\]]/,"",$4); print "0x00000000\t"$3"\t"$4"\tEXPORT_SYMBOL\t"}' /proc/kallsyms \
  | grep -E 'videobuf2|videodev|v4l2_mem2mem' > Module.symvers
make ARCH=arm64 LLVM=1 KBUILD_MODPOST_WARN=1 \
     KCFLAGS=-mbranch-protection=pac-ret+bti \
     M=drivers/media/platform/qcom/iris modules
llvm-readelf -n drivers/media/platform/qcom/iris/qcom-iris.ko | grep -q 'BTI, PAC'
```

`KCFLAGS=-mbranch-protection=pac-ret+bti` is required: the kernel is built
with `CONFIG_ARM64_BTI_KERNEL`, but Kconfig drops that option under
clang ≥ 21, and a module without BTI landing pads oopses on load.

Install by replacing the file and rebooting. Do not `modprobe -r qcom_iris`
on a booted system: unloading hangs in `iris_vpu_power_off` while the VPU
is powered.

```sh
zstd -19 -f qcom-iris.ko -o qcom-iris.ko.zst
sudo cp qcom-iris.ko.zst /lib/modules/$(uname -r)/kernel/drivers/media/platform/qcom/iris/
sudo depmod -a && sudo systemctl reboot
v4l2-ctl -d /dev/videoN --list-ctrls | grep display_delay   # present = decode-order
v4l2-ctl -d /dev/videoN --list-ctrls | grep loop_filter     # present = deblocking forwarded
```

Keep the stock `qcom-iris.ko.zst` beside it for rollback.
