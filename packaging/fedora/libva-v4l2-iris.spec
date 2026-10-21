# Fedora package for the Qualcomm Iris VA-API backend.
#
# The spec installs through scripts/install-system.sh rather than plain
# `meson install`. Both paths exist in this tree and they disagree: meson puts
# the launcher and desktop entries under the configured prefix, while the
# helper additionally resolves libva's canonical driverdir. On the qualified
# target that divergence left desktop entries in two directories at once, one
# of them stale. One install path is the point.
#
# Building from a git checkout:
#
#   git archive --format=tar.gz --prefix=libva-v4l2-iris-0.1.0/ \
#       -o ~/rpmbuild/SOURCES/libva-v4l2-iris-0.1.0.tar.gz HEAD
#   rpmbuild -ba packaging/fedora/libva-v4l2-iris.spec \
#       --define "source_commit $(git rev-parse HEAD)" \
#       --define "tracked_sha256 $(git ls-files -z | xargs -0 sha256sum | sha256sum | awk '{print $1}')"
#
# Omitting the defines is allowed and records the install as dirty, which is
# the honest answer when the source identity was not supplied.

%global forgename libva-v4l2-iris
# The shipped artifact is a small runtime-only VA module. Keep debug symbols in
# the build/debug workflow rather than generating an empty automatic debugsource
# subpackage after the staged runtime ELF has been deliberately post-processed.
%global debug_package %{nil}
%global _source_commit %{?source_commit}%{!?source_commit:0000000000000000000000000000000000000000}
%global _tracked_sha256 %{?tracked_sha256}%{!?tracked_sha256:0000000000000000000000000000000000000000000000000000000000000000}
%global _source_dirty %{?source_commit:%{?tracked_sha256:0}}%{!?source_commit:1}

Name:           libva-v4l2-iris
Version:        0.1.0
Release:        1%{?dist}
Summary:        VA-API backend for the Qualcomm Iris V4L2 stateful decoder and encoder

# The backend carries both licences; see COPYING, COPYING.LGPL and COPYING.MIT.
License:        MIT AND LGPL-2.1-or-later
URL:            https://github.com/phxinyang/qualcomm-iris-vaapi
Source0:        %{forgename}-%{version}.tar.gz

ExclusiveArch:  aarch64

BuildRequires:  meson >= 0.56.0
BuildRequires:  ninja-build
BuildRequires:  gcc-c++
BuildRequires:  pkgconfig
BuildRequires:  python3
BuildRequires:  pkgconfig(libva) >= 1.1.0
BuildRequires:  pkgconfig(libdrm) >= 2.4.52
BuildRequires:  pkgconfig(egl)
BuildRequires:  pkgconfig(glesv2)
BuildRequires:  pkgconfig(gbm)

# The launcher resolves the decoder node by driver name with v4l2-ctl, because
# the node number is not stable across boots on this device. Without v4l-utils
# it refuses to start rather than guessing /dev/video0, which may be a camera.
Requires:       v4l-utils

# Neither browser is required: the driver is useful to any VA-API client, and
# the desktop entries hide themselves through TryExec when the browser they
# name is absent.
Suggests:       chromium

%description
A libVA backend that drives the Qualcomm Iris V4L2 stateful decoder and
encoder, so VA-API clients such as Chromium, Google Chrome and FFmpeg can
decode and encode H.264 and HEVC in hardware instead of falling back to a
CPU codec.

The package installs the driver into libva's driver directory, a browser
launcher that resolves the Iris decoder node at runtime, and desktop entries
for the VA-only and the separate Vulkan/WebGPU experiment profiles.

VA VP9 is deliberately withdrawn on the qualified target and VA AV1 is not
advertised; see data/iris-codec-capabilities.json for the machine-readable
support boundary.

%prep
%autosetup -n %{forgename}-%{version}

%build
%meson
%meson_build

%check
# The repository's own gate. IRIS_TEST_SCRATCH_ROOT keeps the scratch
# directories inside the build tree instead of the builder's home.
IRIS_TEST_SCRATCH_ROOT=%{_vpath_builddir}/scratch %meson_test

%install
# install-system.sh stages into DESTDIR, verifies the artifact has no
# unresolved symbols, and writes the provenance manifest the runtime checks
# read. IRIS_VA_DRIVER_DIR is passed explicitly so the staged tree does not
# depend on the builder's libva.pc resolving to the same libdir.
DESTDIR=%{buildroot} \
IRIS_INSTALL_PREFIX=%{_prefix} \
IRIS_VA_DRIVER_DIR=%{_libdir}/dri \
IRIS_SOURCE_COMMIT=%{_source_commit} \
IRIS_TRACKED_SOURCE_SHA256=%{_tracked_sha256} \
IRIS_SOURCE_DIRTY=%{_source_dirty} \
    sh scripts/install-system.sh %{_vpath_builddir}

# Fedora's brp hooks run after %install and strip the shared object. Run the
# same hooks before refreshing the staged manifest so artifact_sha256 and
# artifact_size describe the payload that enters the RPM rather than the
# unstripped build-tree file. The hooks are idempotent when rpmbuild runs them
# again during normal build-root post-processing.
RPM_BUILD_ROOT=%{buildroot} %{__brp_strip} %{__strip}
RPM_BUILD_ROOT=%{buildroot} %{__brp_strip_comment_note} %{__strip} %{__objdump}
RPM_BUILD_ROOT=%{buildroot} %{__brp_strip_lto} %{__strip}
sh scripts/refresh-install-manifest.sh \
    %{buildroot}%{_datadir}/iris-vaapi/install-manifest.txt \
    %{buildroot}%{_libdir}/dri/v4l2_drv_video.so

%files
%license COPYING COPYING.LGPL COPYING.MIT
%doc README.md README.zh-CN.md AUTHORS CREDITS
%{_libdir}/dri/v4l2_drv_video.so
# libva falls back to the DRM driver name when LIBVA_DRIVER_NAME is unset, and
# this SoC reports DRIVER=msm_dpu. The alias keeps clients that do not set the
# variable (mpv, VLC, GStreamer) on this driver instead of software.
%{_libdir}/dri/msm_drv_video.so
%{_bindir}/iris-vaapi-browser
%{_datadir}/applications/google-chrome-iris-v4l2.desktop
%dir %{_datadir}/iris-vaapi
%{_datadir}/iris-vaapi/install-manifest.txt

%changelog
* Wed Sep 02 2026 Iris VA-API maintainers - 0.1.0-1
- First packaged snapshot: driver, launcher, desktop entries and the install
  provenance manifest in one artifact.
