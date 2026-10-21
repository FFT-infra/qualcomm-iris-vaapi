// SPDX-License-Identifier: MIT
//
// The encode parameters the codec layer produces and the encoder session
// consumes. Plain fields only: util/ is the one place both iris/ and codec/
// may include, and neither may include the other (docs/architecture.md §5).

#pragma once

#include <cstdint>

namespace iris {

// Cqp is constant-quality: the firmware's rate control is off and the
// per-frame QP controls decide the size. FFmpeg picks it when the client
// gives no bitrate, so a driver that omits it cannot open the encoder.
enum class RateMode { Vbr, Cbr, Cqp };

struct EncodeParams {
    // V4L2 menu values (V4L2_MPEG_VIDEO_*_PROFILE and *_LEVEL). A negative
    // level means "leave the driver default".
    int profile = 0;
    int level = -1;
    RateMode rate_mode = RateMode::Vbr;
    uint32_t bitrate = 0; // bits per second
    uint32_t peak_bitrate = 0; // CBR only; 0 means the target bitrate
    uint32_t gop_size = 0; // 0 asks the firmware for intra only
    unsigned frame_rate_num = 30;
    unsigned frame_rate_den = 1;
    // CQP only: the picture's QP, already combined with its slice delta.
    // < 0 when the client runs rate control instead.
    int qp = -1;
    int min_qp = -1; // < 0 leaves the firmware default
    int max_qp = -1;
    bool cabac = true; // H.264 only; HEVC is always CABAC
    // Deblocking, in the numbering both codecs share: 0 filtering enabled,
    // 1 disabled, 2 disabled across slice boundaries. VA carries these in
    // the slice header and V4L2 in a per-context control, so only the first
    // slice can reach the kernel. `alpha` is H.264's alpha_c0 offset and
    // HEVC's tc offset; `beta` is beta in both. All three are the spec's
    // div2 values, and the offsets are 0 whenever filtering is off, because
    // the slice header does not carry them then.
    int deblock_mode = 0;
    int deblock_alpha = 0;
    int deblock_beta = 0;
    // This picture must be an IDR (VA's idr_pic_flag). The firmware has no
    // force-keyframe control, so the session uses the intra period for one
    // picture; see EncoderSession::encode().
    bool keyframe = false;
};

} // namespace iris
