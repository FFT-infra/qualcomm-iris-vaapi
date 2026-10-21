# Qualcomm Iris V4L2/libVA 后端

[English](README.md) | **中文**

给高通 Iris 视频解码器写的 libva 驱动。Iris 是 SM8550 这一代 SoC 上的
stateful V4L2 解码器；有了这个驱动，ARM64 Linux 上的 Google Chrome、Electron
和 Firefox 就能用上硬件解码。在合格的目标机上，固件把画面直接解到 Chrome
自己的缓冲区里，中间没有任何拷贝。

## 为什么要有这个项目

ARM64 Linux 上的官方 Google Chrome 编译时开了 `use_vaapi=true`、关了
`use_v4l2_codec`：二进制里唯一的硬解入口是 libva。预编译的 Electron 应用和
Firefox 的 FFmpeg hwaccel 同样如此。没有 VA-API 驱动，这些客户端只能靠 CPU
解码，平板上放一段 1080p HEVC 就是发热和掉电的问题。

Iris 是 stateful 解码器：固件自己解析码流，接口只有 OUTPUT 队列、CAPTURE
队列和几个控制项。之前没有人写过把 VA-API 解码调用翻译成 Iris 会话的 libva
后端，这个仓库补的就是这一层。Chrome 是验收客户端；FFmpeg、GStreamer、mpv、
Firefox 用的是同一个驱动，也都欢迎，只是它们本来就有各自的原生 V4L2 路径。

## 现在能用到什么程度

下表每一行都对应 [TEST-RESULTS.md](TEST-RESULTS.md) 里一次记录在案的硬件
运行，每次都带 boot ID、内核版本、模块哈希和驱动哈希。合格环境：骁龙 SM8550
平板、Fedora 44、Google Chrome 152。

| 编码 | Chrome | Chrome 里走的路径 | 逐比特验证 |
| --- | --- | --- | --- |
| H.264（CB/Main/High） | 硬件解码 | Import，零拷贝 | 逐帧 MD5 与原生 V4L2 解码器一致；Fluster JVT-AVC_V1 40/135，与固件自身的通过集相同 |
| HEVC Main | 硬件解码 | Import，零拷贝 | 逐帧 MD5 一致；Fluster JCT-VC-HEVC_V1 111/147（固件 112） |
| HEVC Main10（P010） | 硬件解码 | Import，零拷贝 | 10-bit 逐帧 MD5 一致 |
| VP9 Profile 0 / 2 | 硬件解码 | GPU blit（见限制） | 逐帧 MD5 一致，8-bit 和 10-bit |
| AV1 | 不对外声明 | 仅手动开启 | OBU 重组逐比特一致，但 VA 客户端第一个画面就失败（见限制） |

"硬件解码"的标准：Chrome 的 media 面板报告 `VaapiVideoDecoder`、平台解码器
标志为真、30 秒窗口内有解码帧数且零丢帧。节点被打开、GPU 进程在跑，这些都
不算证据。

同一个 display 也提供 `VAEntrypointEncSlice`，走内核在解码节点旁边暴露的编码
节点。Chrome 不用它，用它的是 FFmpeg、GStreamer，以及任何向 libva 要编码
entrypoint 的程序。

| 编码 | Entrypoint | 输入 | 输出 | 验证 |
| --- | --- | --- | --- | --- |
| H.264 Constrained Baseline / Main / High | `VAEntrypointEncSlice`，CBR、VBR 或 CQP | NV12 | Annex-B | 1080p `ffmpeg h264_vaapi`，300 帧 GOP60 验收（TEST-RESULTS.md） |
| HEVC Main | `VAEntrypointEncSlice`，CBR、VBR 或 CQP | NV12 | Annex-B | 1080p `ffmpeg hevc_vaapi`，300 帧 GOP60 验收（TEST-RESULTS.md） |

"硬件编码"的标准：这轮跑完 Iris 的 IRQ 计数有增量、码流解码无错误、SPS 里
的 profile、level 和分辨率与请求一致。一个 VA context 只返回了字节，不算证据。

## 需要什么

- 带 `qcom-iris` 解码器的 SoC（HFI gen2；SM8550 是合格过的那一个），aarch64。
- libva ≥ 1.1、libdrm ≥ 2.4.52；GPU 拷贝引擎需要 EGL/GLES/GBM；运行时需要
  `v4l-utils`（启动器按驱动名找解码节点，因为节点编号每次开机都会变）。
- 任何能加载 libva 模块的 Linux 发行版。Fedora 44 是合格过的；提供 Fedora
  和 Arch 的打包。
- 可选：来自 [strongtz/libva-v4l2](https://github.com/strongtz/libva-v4l2)
  的两个 Iris 内核补丁（按解码顺序出帧、CAPTURE 池上限 64），已 rebase 后放在
  [`packaging/kernel/`](packaging/kernel/README.md)，附只重编模块的步骤。
  原版内核上驱动会探测到补丁不在，照样能用，只是 Chrome 每帧多一次 GPU
  拷贝，拿不到零拷贝。

## 安装和使用

Fedora 上从包安装：

```sh
git archive --format=tar.gz --prefix=libva-v4l2-iris-0.1.0/ \
    -o ~/rpmbuild/SOURCES/libva-v4l2-iris-0.1.0.tar.gz HEAD
rpmbuild -ba packaging/fedora/libva-v4l2-iris.spec \
    --define "source_commit $(git rev-parse HEAD)" \
    --define "tracked_sha256 $(git ls-files -z | xargs -0 sha256sum | sha256sum | awk '{print $1}')"
sudo dnf install ~/rpmbuild/RPMS/aarch64/libva-v4l2-iris-*.rpm
```

Arch 用 `packaging/arch/PKGBUILD`。两种包都会把驱动装进 libva 的驱动目录，
旁边再放一个 `msm_drv_video.so` 符号链接，同时装上 `iris-vaapi-browser`
启动器、一个 "Google Chrome (Iris VA-API)" 桌面项，以及记录源码提交和产物
哈希的安装清单。这个链接是给 libva 自己找驱动用的：这颗 SoC 报的是
`DRIVER=msm_dpu`，没有它，不设 `LIBVA_DRIVER_NAME` 的客户端（mpv、VLC、
GStreamer）根本打不开驱动。之后从那个桌面项启动 Chrome，或者在终端里：

```sh
iris-vaapi-browser --browser=chrome
```

启动器每次启动都重新解析 Iris 节点，只给这个进程设置 `LIBVA_DRIVER_NAME`，
不加任何 Chrome feature 开关；本项目验收的就是 Chrome 打包时的默认配置。
播放视频时打开 `chrome://media-internals` 看结果：解码器一栏必须是
`VaapiVideoDecoder`，且 `kIsPlatformVideoDecoder: true`。

不打包、直接从源码树装：

```sh
meson setup build && meson compile -C build
sudo ./scripts/install-system.sh build       # 驱动进 libva driverdir，启动器进 /usr/local
LIBVA_DRIVERS_PATH=$PWD/build/src LIBVA_DRIVER_NAME=v4l2 vainfo   # 或者先这样试一下
```

`scripts/uninstall-system.sh` 按安装清单把这种安装撤掉。实验室里的完整流程见
[`docs/build-and-deploy.md`](docs/build-and-deploy.md)。

包装好之后其他 VA 客户端不用配，靠的就是那个 `msm_drv_video.so` 链接。唯一的
例外是 GStreamer：它的 VA 插件维护一份厂商白名单，遇到不认识的驱动直接拒绝，
所以 `va` 系列元素只有在 `GST_VA_ALL_DRIVERS=1` 时才会注册。这是 GStreamer 的
门槛，不是驱动的；设上之后 `vah264dec` / `vah265dec` 走的都是 Iris。

## 它怎么解码

一个所有权模型，三种发布策略，按每个 surface 的客户端行为自动选。CAPTURE
池归驱动所有。

- **Direct**：解码之后才导出或映射的 surface（FFmpeg、GStreamer、mpv）直接
  拿走解码完成的那个 CAPTURE 槽位。真正的零拷贝。
- **Import**：客户端在首次解码*之前*就导出的 surface（Chrome 会预先导出整个
  池），驱动把它的缓冲区排进 CAPTURE 队列、只给那一张图，固件直接解进去。
  需要内核补丁提供的解码顺序出帧；按编码从实测证据里选（H.264、HEVC、
  Main10）。
- **Copy**：同样是预先导出的 surface，由 GPU blit（Adreno 上的 EGL）从驱动
  的槽位拷进一块文件描述符永不变化的稳定缓冲区。VP9 和原版内核走这条。

Import 靠两条实测出来的规则成立：同一时刻固件手里只有一个客户端缓冲区；
已提交的图一张都不能跳过。完成结果落到邻居缓冲区里的话，两个 surface 一起
判失败，不发布。契约、状态机和 trace 词表在
[`docs/architecture.md`](docs/architecture.md)，测量数据在
[TEST-RESULTS.md](TEST-RESULTS.md)。

## 已知限制

- **VP9 还在 GPU 拷贝路径上。** 一个 VP9 alt-ref 访问单元会解出两张图。只挂
  一个缓冲区时，每遇到一次 alt-ref 会话就卡约 500 ms；挂两个时固件自己挑
  缓冲区，大约四分之一的完成结果落错位置。要等翻译器做到一张图一个访问
  单元之后才能换 Import。
- **AV1 只能手动开，而且在 VA 上解不出来。** 隐藏帧（`show_frame=0`）固件不返回
  CAPTURE 缓冲区，VA 又没有 `show_existing_frame` 这种调用；另外翻译器要拿到下一帧
  的参考表才能补出 `refresh_frame_flags`，所以每张图都要压后一帧提交，客户端下载时
  这张表面还没有像素。两个问题都在每路流的第一个画面就出现。OBU 重组在
  `test/av1-obu-roundtrip.sh` 下逐比特一致；实际使用走原生 V4L2（`v4l2av1dec`）。
  `V4L2_VA_EXPERIMENTAL_PROFILES=1` 只在验证时声明该 profile。
- **原版内核上 Chrome 走 Copy。** 没有解码顺序控制项时会话运行在显示顺序
  模式：同步等待有上限，超时后做一次 STOP/LAST/START 排空，Import 被拒绝。
- **Chrome 153 的 HEVC seek 回归，上游问题。** Chromium 153 默认打开了
  `ExtendedVideoBitstreamValidation`，共用的 `H265Decoder` 把每次 seek 后的
  第一个 SPS 当成配置变更，合成器会一直显示 seek 之前的画面。
  [CFM880/iris-vaapi](https://github.com/CFM880/iris-vaapi) 在 SM8150 上定位
  并提交了 [Chromium issue 563075803](https://issues.chromium.org/issues/563075803)。
  本项目目标机跑的是 Chrome 152，没有复现；用 153 的话加
  `--disable-features=ExtendedVideoBitstreamValidation` 启动，或者留在 152。
- **只在一台设备上合格过。** 上面所有数据都来自一台 SM8550 平板。其他 Iris
  gen2 SoC 应该能用，但目前没有证据；Arch 打包提供了，没有验证过。
- **编码不能按需出 IDR。** 节点没有 `V4L2_CID_MPEG_VIDEO_FORCE_KEY_FRAME`，
  固件能力表里也没有 RAP 控制项，关键帧只能靠 GOP 长度。从 GOP 中间切开，
  流要等到下一个 IDR 才能播。
- **不支持 B 帧。** 内核最多允许 1 个，而延迟返回的 CAPTURE 会破坏一进一出
  的模型。序列参数里 `ip_period > 1` 的 context 直接返回
  `VA_STATUS_ERROR_INVALID_PARAMETER`。
- **不支持 10-bit、VP9、AV1 编码。** 编码节点的 OUTPUT 队列只列 NV12 和
  QC08C，HEVC Main10 和 VP9 Profile 2 喂不进去。
- **不声明 packed headers，所以打开编码器时没有 extradata。** SPS/PPS 由固件
  自己写，驱动不会再把客户端那一份塞进去。FFmpeg 能处理：它报个警告，把参数
  集留在码流里，再从第一个包提取 `avcC` / `hvcC`，所以直接输出 `.mp4` 是可用的
  （H.264 的 extradata 43 字节，HEVC 124 字节）。GStreamer 的 VA 编码器要求
  驱动至少声明一种 packed header，否则直接拒绝，因此 `vah264enc` /
  `vah265enc` 用不了；FFmpeg 的 `h264_vaapi` / `hevc_vaapi` 可以。
- **一个 context 同时只编一张图。** `vaEndPicture` 同步跑完整个 V4L2 事务，
  VA context 的分辨率又是固定的：换分辨率要新建 context。
- **不支持 multi-slice、tiles、ROI 和 `-max_frame_size`。** FFmpeg 请求这些
  会直接报错，不会被静默忽略。驱动自己也守同样的规矩：picture 参数里
  slice 多于一个、tiles、wavefront 编码、加权预测、scaling list、chroma QP
  偏移、多个参考帧索引，一律返回 `VA_STATUS_ERROR_UNIMPLEMENTED`。序列头由
  固件写、参考结构和量化由固件定，交出一段没照做的流比直接失败更糟。
- **slice 的去块滤波参数要么照做，要么报错。** VA 的 slice 参数是客户端唯一
  能提出关掉滤波或调偏移量的地方；内核有这个控制项时，驱动把
  `disable_deblocking_filter_idc`、alpha/beta 转给
  `V4L2_CID_MPEG_VIDEO_H264_LOOP_FILTER_MODE`/`_ALPHA`/`_BETA`（HEVC 有对应
  的 tc/beta）。SM8550 的内核没有这些控制项，固件永远按默认的 0/0 滤波，
  所以这时任何非默认请求都返回 `VA_STATUS_ERROR_UNIMPLEMENTED`，而不是交出
  一段没照做的流。V4L2 里去块是每个 context 一个值，VA 里是每个 slice 一个
  值，取值只看第一个 slice，同一个 context 里改动要新建 context。

## 开发

```sh
meson setup build-local && meson compile -C build-local
sh test/run-static-checks.sh
meson test -C build-local --print-errorlogs
git diff --check
```

硬件门禁在目标机上跑：`test/iris-browser-acceptance.sh --clip …`（Chrome
证据）、`test/iris-matrix.sh`（与原生解码器比逐帧 MD5）、
`test/iris-structure-matrix.sh`、`test/iris-dynamic-resolution.sh`、
`test/iris-concurrency-soak.sh`、`test/iris-zero-copy-suite.sh`。可能导致
固件复位的实验走 `scripts/iris-experiment-guard.sh`。完整清单见
[`test/README.md`](test/README.md)；贡献者和编码代理的规则在
[CONTRIBUTING.md](CONTRIBUTING.md) 和 [AGENTS.md](AGENTS.md)。

## 许可证与致谢

MIT，含 LGPL-2.1 覆盖的部分，与上游相同；见 `COPYING`、`COPYING.MIT`、
`COPYING.LGPL`。

- 本项目 fork 自 [mxsrc/libva-v4l2](https://github.com/mxsrc/libva-v4l2)，
  后者又源于 Bootlin 的 libva-v4l2-request。VA 和 V4L2 的脚手架来自那里；
  `src/` 里的 stateful Iris 实现、浏览器验收门禁和打包是本 fork 的工作。
- `include/linux/` 内置的内核 UAPI 头文件带各自的 SPDX 标记
  （`GPL-2.0+ WITH Linux-syscall-note OR BSD-3-Clause`）。
- `packaging/kernel/` 携带 [strongtz/libva-v4l2](https://github.com/strongtz/libva-v4l2)
  （Radxa，Xilin Wu）的两个 Iris 内核补丁。它们改的是 GPL-2.0 的内核代码，
  许可证也是 GPL-2.0，与驱动本身不同。HEVC 显式 RPS 的 slice 重写沿用了
  同一项目的做法。

## 环境变量

驱动读取的每个开关都列在这里，且只在 `src/util/options.cc` 一处读取；
`test/iris-env-doc-check.sh` 会在这张表、英文 README 的表和该文件三者不一致
时让构建失败。这些全部是诊断用途，启动器一个都不设。

| 变量 | 默认值 | 用途 |
| --- | --- | --- |
| `LIBVA_V4L2_VIDEO_PATH` | 自动探测 | 指定解码节点，不再扫描 `/dev/video*` 找 `iris_driver` 解码器。 |
| `LIBVA_V4L2_ENCODER_PATH` | 自动探测 | 指定编码节点，不再扫描 `/dev/video*` 找 `iris_driver` 编码器。指到非 Iris 编码节点会让 display 初始化失败。 |
| `V4L2_VA_TRACE` | 关 | 在 stderr 上输出逐帧 trace，任何值都表示开启。记录格式见 `docs/architecture.md`。 |
| `V4L2_VA_SYNC_TIMEOUT_MS` | `2000` | `vaSyncSurface()` 的等待上限，`50..60000`。不覆盖时，冷会话的第一帧最多等 `30000` ms 让固件起来。 |
| `V4L2_VA_COPY` | `gpu` | 往客户端预先导出的 surface 里发布画面时用的引擎：`gpu`（Adreno 上的 EGL blit）或 `cpu`。 |
| `V4L2_VA_PUBLISH` | `auto` | 对所有 surface 强制 `copy`、`direct` 或 `import` 发布。仅诊断用；`auto` 按客户端导出行为逐 surface 选择（解码后导出走 Direct，解码顺序内核上的预导出走 Import，其余走 Copy）。会话无法满足 `import` 时退回 `copy`。 |
| `V4L2_VA_DUMP` | 关 | 把每个提交的访问单元写进这个目录。 |
| `V4L2_VA_EXPERIMENTAL_PROFILES` | 关 | 额外声明 AV1。仅用于验证，启动器从不设置。 |
