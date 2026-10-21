// Exercise the VA -> V4L2 parameter translation, not the order of tokens in
// the source. No hardware: the translation is a pure function.
#include "../src/codec/encode.h"

#include "../src/util/error.h"

#include <cstring>
#include <iostream>
#include <stdexcept>
#include <vector>

using iris::RateMode;
using iris::codec::EncodePicture;
using iris::codec::Encoder;

namespace {

int failures = 0;

void check(bool condition, const char* what)
{
    if (!condition) {
        std::cerr << "FAIL: " << what << '\n';
        ++failures;
    }
}

template <typename T> std::vector<uint32_t> misc(VAEncMiscParameterType type, const T& payload)
{
    const size_t bytes = sizeof(VAEncMiscParameterBuffer) + sizeof(T);
    std::vector<uint32_t> storage((bytes + 3) / 4, 0);
    auto* header = reinterpret_cast<VAEncMiscParameterBuffer*>(storage.data());
    header->type = type;
    std::memcpy(header->data, &payload, sizeof(T));
    return storage;
}

template <typename Function> bool throws(Function&& body)
{
    try {
        body();
    } catch (const iris::Error&) {
        return true;
    }
    return false;
}

// The same, reporting the VA status: a rejection asserted only as "it threw"
// would pass on the wrong error.
template <typename Function> VAStatus status_of(Function&& body)
{
    try {
        body();
    } catch (const iris::Error& error) {
        return error.status;
    }
    return static_cast<VAStatus>(-1);
}

VAEncSequenceParameterBufferH264 h264_sequence()
{
    VAEncSequenceParameterBufferH264 sequence = {};
    sequence.level_idc = 41;
    sequence.intra_period = 60;
    sequence.ip_period = 1;
    sequence.bits_per_second = 4000000;
    sequence.seq_fields.bits.chroma_format_idc = 1;
    return sequence;
}

VAEncPictureParameterBufferH264 h264_picture()
{
    VAEncPictureParameterBufferH264 picture = {};
    picture.pic_init_qp = 26;
    picture.pic_fields.bits.entropy_coding_mode_flag = 1;
    picture.coded_buf = 0x1234;
    return picture;
}

EncodePicture h264_input(const VAEncSequenceParameterBufferH264& sequence,
    const VAEncPictureParameterBufferH264& picture, std::vector<const void*> params = {})
{
    EncodePicture input;
    input.sequence = &sequence;
    input.picture = &picture;
    input.misc = std::move(params);
    input.surface_width = 1920;
    input.surface_height = 1080;
    return input;
}

void test_h264_translation()
{
    auto encoder = iris::codec::make_h264_encoder(VAProfileH264High, RateMode::Vbr);
    check(encoder->v4l2_pixelformat() == V4L2_PIX_FMT_H264, "H.264 fourcc");

    const VAEncSequenceParameterBufferH264 sequence = h264_sequence();
    const VAEncPictureParameterBufferH264 picture = h264_picture();
    VAEncMiscParameterRateControl rate = {};
    rate.bits_per_second = 5000000;
    rate.min_qp = 10;
    rate.max_qp = 40;
    VAEncMiscParameterFrameRate frame = {};
    frame.framerate = 1u << 16 | 30u; // denominator 1, numerator 30
    std::vector<uint32_t> rate_storage = misc(VAEncMiscParameterTypeRateControl, rate);
    std::vector<uint32_t> frame_storage = misc(VAEncMiscParameterTypeFrameRate, frame);

    const EncodePicture input = h264_input(sequence, picture,
        { rate_storage.data(), frame_storage.data() });
    check(encoder->coded_buf(input) == picture.coded_buf, "coded buffer comes from the picture parameters");

    const iris::EncodeParams params = encoder->translate(input);
    check(params.profile == V4L2_MPEG_VIDEO_H264_PROFILE_HIGH, "H.264 High profile");
    check(params.level == V4L2_MPEG_VIDEO_H264_LEVEL_4_1, "H.264 level 4.1");
    check(params.rate_mode == RateMode::Vbr, "VBR rate mode");
    check(params.bitrate == 5000000, "rate-control bitrate wins over the sequence bitrate");
    check(params.peak_bitrate == 0, "VBR sends no peak bitrate");
    check(params.gop_size == 60, "GOP from intra_period");
    check(params.cabac, "CABAC from the picture flags");
    check(params.min_qp == 10 && params.max_qp == 40, "QP bounds from the rate-control misc");
    check(params.frame_rate_num == 30 && params.frame_rate_den == 1, "frame rate from the misc buffer");

    // CBR names the same rate as its peak, which the kernel only sends in CBR.
    auto cbr = iris::codec::make_h264_encoder(VAProfileH264Main, RateMode::Cbr);
    const iris::EncodeParams cbr_params = cbr->translate(input);
    check(cbr_params.profile == V4L2_MPEG_VIDEO_H264_PROFILE_MAIN, "H.264 Main profile");
    check(cbr_params.rate_mode == RateMode::Cbr, "CBR rate mode");
    check(cbr_params.peak_bitrate == cbr_params.bitrate, "CBR peak equals the target");

    // CQP has no bitrate: the QP comes from the picture plus its slice
    // delta, which is exactly what the bitstream carries.
    auto cqp = iris::codec::make_h264_encoder(VAProfileH264High, RateMode::Cqp);
    VAEncSliceParameterBufferH264 slice = {};
    slice.slice_qp_delta = 2;
    EncodePicture cqp_input = h264_input(sequence, picture);
    cqp_input.slices = { &slice };
    const iris::EncodeParams cqp_params = cqp->translate(cqp_input);
    check(cqp_params.rate_mode == RateMode::Cqp, "CQP rate mode");
    check(cqp_params.qp == 28, "CQP QP combines the picture and the slice delta");
    check(cqp_params.peak_bitrate == 0, "CQP sends no peak bitrate");
}

void test_h264_rounding_and_rejections()
{
    auto encoder = iris::codec::make_h264_encoder(VAProfileH264ConstrainedBaseline, RateMode::Vbr);
    VAEncSequenceParameterBufferH264 sequence = h264_sequence();
    const VAEncPictureParameterBufferH264 picture = h264_picture();

    // 43 is not a level; the encoder must not claim 4.3 and must not round up.
    sequence.level_idc = 43;
    check(encoder->translate(h264_input(sequence, picture)).level == V4L2_MPEG_VIDEO_H264_LEVEL_4_2,
        "unknown level rounds down");

    sequence = h264_sequence();
    sequence.ip_period = 2;
    check(throws([&] { encoder->translate(h264_input(sequence, picture)); }), "B-frames are rejected");

    sequence = h264_sequence();
    sequence.bit_depth_luma_minus8 = 2;
    check(throws([&] { encoder->translate(h264_input(sequence, picture)); }), "10-bit is rejected");

    sequence = h264_sequence();
    sequence.seq_fields.bits.chroma_format_idc = 2;
    check(throws([&] { encoder->translate(h264_input(sequence, picture)); }), "4:2:2 is rejected");

    EncodePicture missing;
    check(throws([&] { encoder->translate(missing); }), "a picture without parameters is rejected");

    auto cqp = iris::codec::make_h264_encoder(VAProfileH264High, RateMode::Cqp);
    VAEncPictureParameterBufferH264 no_qp = h264_picture();
    no_qp.pic_init_qp = 0;
    check(throws([&] { cqp->translate(h264_input(h264_sequence(), no_qp)); }),
        "CQP without a picture QP is rejected");
}

VAEncSequenceParameterBufferHEVC hevc_sequence()
{
    VAEncSequenceParameterBufferHEVC sequence = {};
    sequence.general_profile_idc = 1;
    sequence.general_level_idc = 120; // 4.0
    sequence.intra_period = 60;
    sequence.ip_period = 1;
    sequence.bits_per_second = 4000000;
    sequence.pic_width_in_luma_samples = 1920;
    sequence.pic_height_in_luma_samples = 1080;
    sequence.seq_fields.bits.chroma_format_idc = 1;
    return sequence;
}

void test_hevc_translation()
{
    auto encoder = iris::codec::make_hevc_encoder(VAProfileHEVCMain, RateMode::Vbr);
    check(encoder->v4l2_pixelformat() == V4L2_PIX_FMT_HEVC, "HEVC fourcc");

    VAEncSequenceParameterBufferHEVC sequence = hevc_sequence();
    VAEncPictureParameterBufferHEVC picture = {};
    picture.coded_buf = 0x5678;
    EncodePicture input;
    input.sequence = &sequence;
    input.picture = &picture;
    input.surface_width = 1920;
    input.surface_height = 1080;

    const iris::EncodeParams params = encoder->translate(input);
    check(params.profile == V4L2_MPEG_VIDEO_HEVC_PROFILE_MAIN, "HEVC Main profile");
    check(params.level == V4L2_MPEG_VIDEO_HEVC_LEVEL_4, "HEVC level 4.0");
    check(params.bitrate == 4000000, "HEVC bitrate from the sequence");
    check(params.gop_size == 60, "HEVC GOP");
    check(encoder->coded_buf(input) == picture.coded_buf, "HEVC coded buffer");

    auto cqp = iris::codec::make_hevc_encoder(VAProfileHEVCMain, RateMode::Cqp);
    VAEncPictureParameterBufferHEVC qp_picture = {};
    qp_picture.pic_init_qp = 30;
    EncodePicture cqp_input;
    cqp_input.sequence = &sequence;
    cqp_input.picture = &qp_picture;
    cqp_input.surface_width = 1920;
    cqp_input.surface_height = 1080;
    const iris::EncodeParams cqp_params = cqp->translate(cqp_input);
    check(cqp_params.rate_mode == RateMode::Cqp, "HEVC CQP rate mode");
    check(cqp_params.qp == 30, "HEVC CQP QP");

    // 93 is 3.1; 100 is between 3.1 and 4 and must round down.
    sequence.general_level_idc = 93;
    check(encoder->translate(input).level == V4L2_MPEG_VIDEO_HEVC_LEVEL_3_1, "HEVC level 3.1");
    sequence.general_level_idc = 100;
    check(encoder->translate(input).level == V4L2_MPEG_VIDEO_HEVC_LEVEL_3_1, "HEVC level rounds down");

    sequence = hevc_sequence();
    sequence.general_profile_idc = 2;
    check(throws([&] { encoder->translate(input); }), "HEVC Main 10 is rejected");

    sequence = hevc_sequence();
    sequence.pic_width_in_luma_samples = 3840;
    check(throws([&] { encoder->translate(input); }), "oversized coded width is rejected");

    sequence = hevc_sequence();
    sequence.ip_period = 2;
    check(throws([&] { encoder->translate(input); }), "HEVC B-frames are rejected");

    sequence = hevc_sequence();
    sequence.seq_fields.bits.bit_depth_luma_minus8 = 2;
    check(throws([&] { encoder->translate(input); }), "HEVC 10-bit is rejected");

    // An encoder built for a non-Main profile must refuse to pretend.
    auto main10 = iris::codec::make_hevc_encoder(VAProfileHEVCMain10, RateMode::Vbr);
    sequence = hevc_sequence();
    check(throws([&] { main10->translate(input); }), "Main10 encode profile is rejected");
}

void test_deblocking()
{
    auto h264 = iris::codec::make_h264_encoder(VAProfileH264High, RateMode::Vbr);
    VAEncSliceParameterBufferH264 slice = {};
    EncodePicture input = h264_input(h264_sequence(), h264_picture());
    input.slices = { &slice };

    // A picture with no slice parameters keeps the default: filtering on.
    EncodePicture bare = h264_input(h264_sequence(), h264_picture());
    const iris::EncodeParams plain = h264->translate(bare);
    check(plain.deblock_mode == 0 && plain.deblock_alpha == 0 && plain.deblock_beta == 0,
        "no slice means the default deblocking");

    slice.disable_deblocking_filter_idc = 0;
    slice.slice_alpha_c0_offset_div2 = -3;
    slice.slice_beta_offset_div2 = 4;
    const iris::EncodeParams offsets = h264->translate(input);
    check(offsets.deblock_mode == 0 && offsets.deblock_alpha == -3 && offsets.deblock_beta == 4,
        "H.264 deblocking offsets are forwarded as asked");

    // The slice header carries no offsets when filtering is off, so a stale
    // value in the client's buffer must not reach the kernel.
    slice.disable_deblocking_filter_idc = 1;
    const iris::EncodeParams off = h264->translate(input);
    check(off.deblock_mode == 1 && off.deblock_alpha == 0 && off.deblock_beta == 0,
        "disabled deblocking drops the offsets");

    slice.disable_deblocking_filter_idc = 3;
    check(throws([&] { h264->translate(input); }), "an invalid H.264 deblocking idc is rejected");
    slice.disable_deblocking_filter_idc = 0;
    slice.slice_beta_offset_div2 = 7;
    check(throws([&] { h264->translate(input); }), "an out-of-range H.264 beta offset is rejected");

    auto hevc = iris::codec::make_hevc_encoder(VAProfileHEVCMain, RateMode::Vbr);
    VAEncSequenceParameterBufferHEVC sequence = hevc_sequence();
    VAEncPictureParameterBufferHEVC picture = {};
    VAEncSliceParameterBufferHEVC hslice = {};
    EncodePicture hinput;
    hinput.sequence = &sequence;
    hinput.picture = &picture;
    hinput.surface_width = 1920;
    hinput.surface_height = 1080;
    hinput.slices = { &hslice };

    // Filtering across slice boundaries off is mode 2 in H.264's numbering,
    // which is what the session maps to the HEVC menu.
    const iris::EncodeParams boundaries = hevc->translate(hinput);
    check(boundaries.deblock_mode == 2, "HEVC without across-slice filtering is mode 2");

    hslice.slice_fields.bits.slice_loop_filter_across_slices_enabled_flag = 1;
    hslice.slice_tc_offset_div2 = 2;
    hslice.slice_beta_offset_div2 = -2;
    const iris::EncodeParams henabled = hevc->translate(hinput);
    check(henabled.deblock_mode == 0 && henabled.deblock_alpha == 2 && henabled.deblock_beta == -2,
        "HEVC tc and beta offsets are forwarded");

    hslice.slice_fields.bits.slice_deblocking_filter_disabled_flag = 1;
    const iris::EncodeParams hoff = hevc->translate(hinput);
    check(hoff.deblock_mode == 1 && hoff.deblock_alpha == 0 && hoff.deblock_beta == 0,
        "disabled HEVC deblocking drops the offsets");

    hslice.slice_fields.bits.slice_deblocking_filter_disabled_flag = 2;
    check(throws([&] { hevc->translate(hinput); }), "an invalid HEVC deblocking flag is rejected");
    hslice.slice_fields.bits.slice_deblocking_filter_disabled_flag = 0;
    hslice.slice_tc_offset_div2 = 7;
    check(throws([&] { hevc->translate(hinput); }), "an out-of-range HEVC tc offset is rejected");
}


// A client request the encoder cannot honour fails; it is never answered with
// a stream that quietly used something else.
void test_unimplemented_features()
{
    auto h264 = iris::codec::make_h264_encoder(VAProfileH264High, RateMode::Vbr);
    VAEncPictureParameterBufferH264 picture = h264_picture();
    const VAEncSequenceParameterBufferH264 sequence = h264_sequence();
    check(status_of([&] { h264->translate(h264_input(sequence, picture)); }) == -1,
        "the default picture parameters translate");

    picture.pic_fields.bits.weighted_pred_flag = 1;
    check(status_of([&] { h264->translate(h264_input(sequence, picture)); }) == VA_STATUS_ERROR_UNIMPLEMENTED,
        "weighted prediction is refused");
    picture = h264_picture();
    picture.chroma_qp_index_offset = 1;
    check(status_of([&] { h264->translate(h264_input(sequence, picture)); }) == VA_STATUS_ERROR_UNIMPLEMENTED,
        "a chroma QP offset is refused");
    picture = h264_picture();
    picture.num_ref_idx_l0_active_minus1 = 1;
    check(status_of([&] { h264->translate(h264_input(sequence, picture)); }) == VA_STATUS_ERROR_UNIMPLEMENTED,
        "several reference indexes are refused");

    VAEncSliceParameterBufferH264 slice = {};
    EncodePicture one = h264_input(sequence, h264_picture());
    one.slices = { &slice };
    slice.cabac_init_idc = 1;
    check(status_of([&] { h264->translate(one); }) == VA_STATUS_ERROR_UNIMPLEMENTED,
        "a CABAC initialisation index is refused");

    VAEncSliceParameterBufferH264 second = {};
    EncodePicture many = h264_input(sequence, h264_picture());
    many.slices = { &slice, &second };
    check(status_of([&] { h264->translate(many); }) == VA_STATUS_ERROR_UNIMPLEMENTED,
        "two slices are refused");

    auto hevc = iris::codec::make_hevc_encoder(VAProfileHEVCMain, RateMode::Vbr);
    VAEncSequenceParameterBufferHEVC hsequence = hevc_sequence();
    VAEncPictureParameterBufferHEVC hpicture = {};
    EncodePicture hinput;
    hinput.sequence = &hsequence;
    hinput.picture = &hpicture;
    hinput.surface_width = 1920;
    hinput.surface_height = 1080;
    check(status_of([&] { hevc->translate(hinput); }) == -1, "the default HEVC parameters translate");

    hpicture.pic_fields.bits.tiles_enabled_flag = 1;
    check(status_of([&] { hevc->translate(hinput); }) == VA_STATUS_ERROR_UNIMPLEMENTED,
        "HEVC tiles are refused");
    hpicture = {};
    hpicture.pic_fields.bits.weighted_pred_flag = 1;
    check(status_of([&] { hevc->translate(hinput); }) == VA_STATUS_ERROR_UNIMPLEMENTED,
        "HEVC weighted prediction is refused");
    hpicture = {};
    hpicture.pps_cb_qp_offset = 1;
    check(status_of([&] { hevc->translate(hinput); }) == VA_STATUS_ERROR_UNIMPLEMENTED,
        "a HEVC PPS chroma QP offset is refused");
    hpicture = {};
    hpicture.num_ref_idx_l0_default_active_minus1 = 1;
    check(status_of([&] { hevc->translate(hinput); }) == VA_STATUS_ERROR_UNIMPLEMENTED,
        "HEVC reference indexes are refused");

    VAEncSliceParameterBufferHEVC hslice = {};
    hinput.slices = { &hslice };
    hslice.slice_cb_qp_offset = 1;
    check(status_of([&] { hevc->translate(hinput); }) == VA_STATUS_ERROR_UNIMPLEMENTED,
        "a HEVC slice chroma QP offset is refused");
    hslice = {};
    hslice.slice_fields.bits.dependent_slice_segment_flag = 1;
    check(status_of([&] { hevc->translate(hinput); }) == VA_STATUS_ERROR_UNIMPLEMENTED,
        "dependent slice segments are refused");

    VAEncSliceParameterBufferHEVC hsecond = {};
    hslice = {};
    hinput.slices = { &hslice, &hsecond };
    check(status_of([&] { hevc->translate(hinput); }) == VA_STATUS_ERROR_UNIMPLEMENTED,
        "two HEVC slices are refused");
}
} // namespace

int main()
{
    test_h264_translation();
    test_h264_rounding_and_rejections();
    test_hevc_translation();
    test_deblocking();
    test_unimplemented_features();
    if (failures) {
        std::cerr << failures << " encode parameter check(s) failed\n";
        return 1;
    }
    std::cout << "PASS encode parameter translation (H.264 and HEVC)\n";
    return 0;
}
