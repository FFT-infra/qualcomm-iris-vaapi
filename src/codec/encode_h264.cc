// SPDX-License-Identifier: MIT
//
// H.264 encode parameters. The firmware builds SPS/PPS from the V4L2
// controls, so this translation only has to pin profile, level, rate
// control, GOP and entropy coding; nothing here emits a bitstream.

#include "encode.h"

#include "../util/error.h"

namespace iris::codec {

namespace {

struct Level {
    uint8_t idc;
    int menu;
};

// level_idc values from H.264 Annex A. 1b is 9 and sits below 1.0.
constexpr Level kLevels[] = {
    { 9, V4L2_MPEG_VIDEO_H264_LEVEL_1B },
    { 10, V4L2_MPEG_VIDEO_H264_LEVEL_1_0 },
    { 11, V4L2_MPEG_VIDEO_H264_LEVEL_1_1 },
    { 12, V4L2_MPEG_VIDEO_H264_LEVEL_1_2 },
    { 13, V4L2_MPEG_VIDEO_H264_LEVEL_1_3 },
    { 20, V4L2_MPEG_VIDEO_H264_LEVEL_2_0 },
    { 21, V4L2_MPEG_VIDEO_H264_LEVEL_2_1 },
    { 22, V4L2_MPEG_VIDEO_H264_LEVEL_2_2 },
    { 30, V4L2_MPEG_VIDEO_H264_LEVEL_3_0 },
    { 31, V4L2_MPEG_VIDEO_H264_LEVEL_3_1 },
    { 32, V4L2_MPEG_VIDEO_H264_LEVEL_3_2 },
    { 40, V4L2_MPEG_VIDEO_H264_LEVEL_4_0 },
    { 41, V4L2_MPEG_VIDEO_H264_LEVEL_4_1 },
    { 42, V4L2_MPEG_VIDEO_H264_LEVEL_4_2 },
    { 50, V4L2_MPEG_VIDEO_H264_LEVEL_5_0 },
    { 51, V4L2_MPEG_VIDEO_H264_LEVEL_5_1 },
    { 52, V4L2_MPEG_VIDEO_H264_LEVEL_5_2 },
    { 60, V4L2_MPEG_VIDEO_H264_LEVEL_6_0 },
    { 61, V4L2_MPEG_VIDEO_H264_LEVEL_6_1 },
    { 62, V4L2_MPEG_VIDEO_H264_LEVEL_6_2 },
};

// The highest level the firmware has at or below `idc`; -1 asks the driver
// to keep its default. Rounding down never promises a level the stream may
// exceed.
int level_menu(uint8_t idc)
{
    if (idc == 0)
        return -1;
    int menu = -1;
    for (const Level& level : kLevels)
        if (level.idc <= idc)
            menu = level.menu;
    return menu;
}

int profile_menu(VAProfile profile)
{
    switch (profile) {
    case VAProfileH264ConstrainedBaseline:
        return V4L2_MPEG_VIDEO_H264_PROFILE_CONSTRAINED_BASELINE;
    case VAProfileH264Main:
        return V4L2_MPEG_VIDEO_H264_PROFILE_MAIN;
    case VAProfileH264High:
        return V4L2_MPEG_VIDEO_H264_PROFILE_HIGH;
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
    case VAProfileH264Baseline:
#pragma GCC diagnostic pop
        return V4L2_MPEG_VIDEO_H264_PROFILE_BASELINE;
    default:
        throw Error(VA_STATUS_ERROR_UNSUPPORTED_PROFILE, "no H.264 profile menu value");
    }
}

class H264Encoder final : public Encoder {
public:
    H264Encoder(VAProfile profile, RateMode rate_mode)
        : profile_(profile)
        , rate_mode_(rate_mode)
    {
    }

    uint32_t v4l2_pixelformat() const override { return V4L2_PIX_FMT_H264; }

    VABufferID coded_buf(const EncodePicture& picture) const override
    {
        const auto* params = static_cast<const VAEncPictureParameterBufferH264*>(picture.picture);
        return params ? params->coded_buf : VA_INVALID_ID;
    }

    EncodeParams translate(const EncodePicture& picture) override
    {
        const auto* sequence = static_cast<const VAEncSequenceParameterBufferH264*>(picture.sequence);
        const auto* params = static_cast<const VAEncPictureParameterBufferH264*>(picture.picture);
        require(sequence != nullptr, "H.264 encode picture has no sequence parameters",
            VA_STATUS_ERROR_INVALID_PARAMETER);
        require(params != nullptr, "H.264 encode picture has no picture parameters",
            VA_STATUS_ERROR_INVALID_PARAMETER);

        // The encoder's raw port is NV12 only (iris_venc.c formats_out).
        require(sequence->bit_depth_luma_minus8 == 0 && sequence->bit_depth_chroma_minus8 == 0,
            "10-bit encode is not supported", VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT);
        require(sequence->seq_fields.bits.chroma_format_idc == 1, "4:2:0 encode is required",
            VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT);
        // One picture in, one picture out: a delayed B picture would break
        // the synchronous transaction and misorder presentation.
        require(sequence->ip_period <= 1, "B-frames are not supported", VA_STATUS_ERROR_INVALID_PARAMETER);

        EncodeParams out;
        out.profile = profile_menu(profile_);
        out.level = level_menu(sequence->level_idc);
        out.rate_mode = rate_mode_;
        out.bitrate = sequence->bits_per_second;
        out.gop_size = sequence->intra_period;
        out.cabac = params->pic_fields.bits.entropy_coding_mode_flag != 0;

        if (const auto* rate = static_cast<const VAEncMiscParameterRateControl*>(
                find_misc(picture.misc, VAEncMiscParameterTypeRateControl))) {
            if (rate->bits_per_second)
                out.bitrate = rate->bits_per_second;
            if (rate->min_qp)
                out.min_qp = rate->min_qp;
            if (rate->max_qp)
                out.max_qp = rate->max_qp;
        }
        if (const auto* frame = static_cast<const VAEncMiscParameterFrameRate*>(
                find_misc(picture.misc, VAEncMiscParameterTypeFrameRate))) {
            const unsigned num = frame->framerate & 0xffff;
            const unsigned den = frame->framerate >> 16;
            if (num && den) {
                out.frame_rate_num = num;
                out.frame_rate_den = den;
            }
        }
        out.peak_bitrate = out.rate_mode == RateMode::Cbr ? out.bitrate : 0;
        require(out.bitrate > 0 || out.rate_mode == RateMode::Vbr, "CBR needs a bitrate",
            VA_STATUS_ERROR_INVALID_PARAMETER);
        return out;
    }

private:
    VAProfile profile_;
    RateMode rate_mode_;
};

} // namespace

std::unique_ptr<Encoder> make_h264_encoder(VAProfile profile, RateMode rate_mode)
{
    return std::make_unique<H264Encoder>(profile, rate_mode);
}

} // namespace iris::codec
