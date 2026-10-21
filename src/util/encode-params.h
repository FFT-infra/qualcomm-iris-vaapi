// SPDX-License-Identifier: MIT
//
// The encode parameters the codec layer produces and the encoder session
// consumes. Plain fields only: util/ is the one place both iris/ and codec/
// may include, and neither may include the other (docs/architecture.md §5).

#pragma once

#include <cstdint>

namespace iris {

enum class RateMode { Vbr, Cbr };

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
    int min_qp = -1; // < 0 leaves the firmware default
    int max_qp = -1;
    bool cabac = true; // H.264 only; HEVC is always CABAC
};

} // namespace iris
