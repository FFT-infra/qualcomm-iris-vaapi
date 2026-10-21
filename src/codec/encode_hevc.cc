// SPDX-License-Identifier: MIT
//
// HEVC encode parameters. Main profile only: the encoder's raw port accepts
// NV12 and QC08C, never P010, so there is no 10-bit path to advertise.

#include "encode.h"

#include "../util/error.h"

namespace iris::codec {

namespace {

struct Level {
    unsigned idc; // general_level_idc = 30 * level
    int menu;
};

constexpr Level kLevels[] = {
    { 30, V4L2_MPEG_VIDEO_HEVC_LEVEL_1 },
    { 60, V4L2_MPEG_VIDEO_HEVC_LEVEL_2 },
    { 63, V4L2_MPEG_VIDEO_HEVC_LEVEL_2_1 },
    { 90, V4L2_MPEG_VIDEO_HEVC_LEVEL_3 },
    { 93, V4L2_MPEG_VIDEO_HEVC_LEVEL_3_1 },
    { 120, V4L2_MPEG_VIDEO_HEVC_LEVEL_4 },
    { 123, V4L2_MPEG_VIDEO_HEVC_LEVEL_4_1 },
    { 150, V4L2_MPEG_VIDEO_HEVC_LEVEL_5 },
    { 153, V4L2_MPEG_VIDEO_HEVC_LEVEL_5_1 },
    { 156, V4L2_MPEG_VIDEO_HEVC_LEVEL_5_2 },
    { 180, V4L2_MPEG_VIDEO_HEVC_LEVEL_6 },
    { 183, V4L2_MPEG_VIDEO_HEVC_LEVEL_6_1 },
    { 186, V4L2_MPEG_VIDEO_HEVC_LEVEL_6_2 },
};

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

class HevcEncoder final : public Encoder {
public:
    HevcEncoder(VAProfile profile, RateMode rate_mode)
        : profile_(profile)
        , rate_mode_(rate_mode)
    {
    }

    uint32_t v4l2_pixelformat() const override { return V4L2_PIX_FMT_HEVC; }

    VABufferID coded_buf(const EncodePicture& picture) const override
    {
        const auto* params = static_cast<const VAEncPictureParameterBufferHEVC*>(picture.picture);
        return params ? params->coded_buf : VA_INVALID_ID;
    }

    EncodeParams translate(const EncodePicture& picture) override
    {
        const auto* sequence = static_cast<const VAEncSequenceParameterBufferHEVC*>(picture.sequence);
        const auto* params = static_cast<const VAEncPictureParameterBufferHEVC*>(picture.picture);
        require(sequence != nullptr, "HEVC encode picture has no sequence parameters",
            VA_STATUS_ERROR_INVALID_PARAMETER);
        require(params != nullptr, "HEVC encode picture has no picture parameters",
            VA_STATUS_ERROR_INVALID_PARAMETER);
        require(profile_ == VAProfileHEVCMain, "only HEVC Main encode is supported",
            VA_STATUS_ERROR_UNSUPPORTED_PROFILE);
        // 0 is an unset profile in some clients; anything else must be Main.
        require(sequence->general_profile_idc == 0 || sequence->general_profile_idc == 1,
            "only HEVC Main encode is supported", VA_STATUS_ERROR_UNSUPPORTED_PROFILE);
        require(sequence->seq_fields.bits.bit_depth_luma_minus8 == 0
                && sequence->seq_fields.bits.bit_depth_chroma_minus8 == 0,
            "10-bit encode is not supported", VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT);
        require(sequence->seq_fields.bits.chroma_format_idc == 1, "4:2:0 encode is required",
            VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT);
        require(sequence->ip_period <= 1, "B-frames are not supported", VA_STATUS_ERROR_INVALID_PARAMETER);
        require(sequence->pic_width_in_luma_samples > 0 && sequence->pic_height_in_luma_samples > 0,
            "HEVC coded size is not set", VA_STATUS_ERROR_INVALID_PARAMETER);
        // The firmware codes at the surface size rounded to 32. A client may
        // declare that aligned size (FFmpeg passes its 1920x1088 context for
        // a 1920x1080 frame), but not a larger picture than we can feed.
        const unsigned coded_width = (picture.surface_width + 31) & ~31u;
        const unsigned coded_height = (picture.surface_height + 31) & ~31u;
        require(sequence->pic_width_in_luma_samples <= coded_width
                && sequence->pic_height_in_luma_samples <= coded_height,
            "HEVC coded size exceeds the input surface", VA_STATUS_ERROR_INVALID_PARAMETER);

        EncodeParams out;
        out.profile = V4L2_MPEG_VIDEO_HEVC_PROFILE_MAIN;
        out.level = level_menu(sequence->general_level_idc);
        out.rate_mode = rate_mode_;
        out.bitrate = sequence->bits_per_second;
        out.gop_size = sequence->intra_period;
        out.cabac = true;

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
        return out;
    }

private:
    VAProfile profile_;
    RateMode rate_mode_;
};

} // namespace

std::unique_ptr<Encoder> make_hevc_encoder(VAProfile profile, RateMode rate_mode)
{
    return std::make_unique<HevcEncoder>(profile, rate_mode);
}

} // namespace iris::codec
