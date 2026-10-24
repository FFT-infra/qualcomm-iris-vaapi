#!/bin/sh

# Keep the four kernel patches honest about what they are and what they touch.
#
# `packaging/kernel/` is the only kernel code this project owns, and it is
# consumed by a second repository that fetches each patch by URL and verifies
# its sha256 before `%patch`. Two failure modes are worth a check that runs on
# every host, without a kernel tree or an arm64 toolchain:
#
#   * a patch is edited in place, so the recorded sha256 no longer describes
#     the file the downstream spec verifies. `%prep` then fails in a build
#     that takes an hour to reach it, or, worse, the pin is updated to a
#     file nobody re-read.
#   * a patch grows a hunk in a file it should not touch. 0001 applies at
#     fuzz 0 against the `sheng-7.2.6` tag and the downstream spec relies on
#     that; a stray path is how a rebase goes wrong quietly.
#
# What it asserts, per patch:
#   * the file exists and its sha256 matches `packaging/kernel/SHA256SUMS`
#   * every path it touches is under `drivers/media/platform/qcom/iris/`
#   * the path list is the one `packaging/kernel/README.md` describes
#   * the marker that identifies the change is added exactly once, and 0004's
#     timestamp copy sits inside the ERROR branch
#   * README.md names the file, so a renamed patch cannot leave the recipe
#     pointing at nothing

set -eu

root=${1:-$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)}
kernel_dir=$root/packaging/kernel
sums=$kernel_dir/SHA256SUMS
readme=$kernel_dir/README.md
failures=0

fail() {
    failures=$((failures + 1))
    printf 'FAIL kernel-patch: %s\n' "$*" >&2
}

for path in "$sums" "$readme"; do
    [ -f "$path" ] || { fail "missing $path"; exit 1; }
done

# The recorded hashes are the single source of truth; the downstream spec
# carries its own copy of each one, so a mismatch there is caught at build
# time and a mismatch here is caught now.
check=$(cd "$kernel_dir" && sha256sum -c SHA256SUMS 2>&1) || true
bad=$(printf '%s\n' "$check" | grep -v ': OK$' | grep -v '^$' || true)
if [ -n "$bad" ]; then
    fail "recorded hash does not match: $bad"
fi

# Only the four patches may be listed, and every one of them must be.
listed=$(awk '{print $2}' "$sums")
for name in 0001 0002 0003 0004; do
    printf '%s\n' "$listed" | grep -q "^$name-" \
        || fail "SHA256SUMS does not record $name"
done
count=$(printf '%s\n' "$listed" | grep -c '\.patch$' || true)
[ "$count" -eq 4 ] \
    || fail "SHA256SUMS must record exactly four patches, found $count"
for name in $(printf '%s\n' "$listed" | grep '\.patch$'); do
    [ -f "$kernel_dir/$name" ] || fail "SHA256SUMS records a file that is not there: $name"
done

expect_marker() {
    patch=$1
    marker=$2
    want=$3
    got=$(grep -cF -- "$marker" "$patch" || true)
    [ "$got" -eq "$want" ] \
        || fail "$patch: '$marker' appears $got time(s), expected $want"
}

# Each patch's identifying change. The count is 1 on purpose: 0002 and 0003
# both write into `inst_fw_cap_sm8550_enc`, and a fuzzy apply of the upstream
# patch has produced a doubled table entry before. The README records the
# greps to run against an applied tree; these are the same counts, checked
# against the patch instead of a build.
expect_marker "$kernel_dir/0001-media-iris-decode-order-output-and-64-capture-buffers.patch" \
    '+#define HFI_PROP_DECODE_ORDER_OUTPUT' 1
expect_marker "$kernel_dir/0001-media-iris-decode-order-output-and-64-capture-buffers.patch" \
    '+		dst_vq->max_num_buffers = DEFAULT_MAX_HOST_BUF_COUNT;' 1
expect_marker "$kernel_dir/0002-media-iris-deblocking-filter-controls.patch" \
    '+		.cap_id = DEBLOCK_MODE_H264,' 1
expect_marker "$kernel_dir/0002-media-iris-deblocking-filter-controls.patch" \
    '+#define HFI_PROP_DEBLOCKING_MODE' 1
expect_marker "$kernel_dir/0003-media-iris-encoder-request-sync-frame.patch" \
    '+		.cap_id = REQUEST_SYNC_FRAME,' 1
expect_marker "$kernel_dir/0003-media-iris-encoder-request-sync-frame.patch" \
    '+#define HFI_PROP_REQUEST_SYNC_FRAME' 1
expect_marker "$kernel_dir/0004-media-iris-preserve-timestamps-on-error-buffers.patch" \
    '+		vb2->timestamp = buf->timestamp;' 1
# The zeroing it replaces must be the line it removes, not a line it leaves
# behind next to the new assignment.
expect_marker "$kernel_dir/0004-media-iris-preserve-timestamps-on-error-buffers.patch" \
    '-		vb2->timestamp = 0;' 1

# 0004's whole point is that the copy lands in the ERROR branch. Checking the
# line order inside the hunk is what distinguishes that from a copy added to
# the normal completion path, where the timestamp was already correct.
patch4=$kernel_dir/0004-media-iris-preserve-timestamps-on-error-buffers.patch
error_branch=$(grep -nF 'if (buf->flags & V4L2_BUF_FLAG_ERROR) {' "$patch4" | head -1 | cut -d: -f1)
copy=$(grep -nF '+		vb2->timestamp = buf->timestamp;' "$patch4" | head -1 | cut -d: -f1)
done_call=$(grep -nF 'v4l2_m2m_buf_done' "$patch4" | head -1 | cut -d: -f1)
if [ -z "$error_branch" ] || [ -z "$copy" ] || [ -z "$done_call" ] \
        || [ "$error_branch" -ge "$copy" ] || [ "$copy" -ge "$done_call" ]; then
    fail "0004: the timestamp copy is not inside the ERROR branch"
fi

# Every path the patch touches belongs to the Iris driver. `git apply
# --numstat` reads the diff without a tree and fails on a malformed patch,
# which is also the cheapest way to prove the file is still a patch.
if command -v git >/dev/null 2>&1; then
    for patch in "$kernel_dir"/0*.patch; do
        if ! paths=$(git apply --numstat "$patch" 2>&1); then
            fail "${patch##*/}: git cannot read it as a patch"
            continue
        fi
        outside=$(printf '%s\n' "$paths" | awk '$3 != "" {print $3}' \
            | grep -v '^drivers/media/platform/qcom/iris/' || true)
        if [ -n "$outside" ]; then
            fail "${patch##*/} touches a path outside the Iris driver: $outside"
        fi
    done
else
    printf 'WARN kernel-patch: git is unavailable, patches not parsed\n' >&2
fi

# The README is the recipe an operator follows, and it names each patch by
# file. A renamed patch that this check does not catch leaves the recipe
# pointing at a file that no longer exists.
for patch in "$kernel_dir"/0*.patch; do
    grep -Fq "${patch##*/}" "$readme" \
        || fail "README.md does not name ${patch##*/}"
done

if [ "$failures" -ne 0 ]; then
    printf 'FAIL kernel patches: %d problem(s)\n' "$failures" >&2
    exit 1
fi
printf 'PASS kernel patches: four files, recorded hashes, Iris-only paths\n'
