// SPDX-License-Identifier: MIT
//
// The synchronous encoder transaction (docs/architecture.md §4). The
// decoder keeps a slot pool so completions can be published zero-copy;
// encoding has nothing to publish, so one picture in means one access unit
// out and vaEndPicture can return the bytes.

#include "encode.h"

#include "alloc.h"

#include "../util/error.h"

#include <algorithm>
#include <cstring>
#include <string>

extern "C" {
#include <poll.h>
}

namespace iris {

namespace {

constexpr unsigned kOutputSlots = 4;
constexpr unsigned kCaptureSlots = 4;
constexpr int kPollMs = 20;

int bitrate_mode(RateMode mode)
{
    return mode == RateMode::Cbr ? V4L2_MPEG_VIDEO_BITRATE_MODE_CBR : V4L2_MPEG_VIDEO_BITRATE_MODE_VBR;
}

// Iris counts the I frame inside GOP_SIZE: a value of 60 puts the next IDR
// at frame 61, while VA's intra_period is the distance between I frames.
// Program one less; 0 asks the firmware for intra-only.
int32_t gop_control(uint32_t intra_period)
{
    return static_cast<int32_t>(intra_period ? intra_period - 1 : 0);
}

const char* rate_mode_name(RateMode mode)
{
    switch (mode) {
    case RateMode::Cbr:
        return "cbr";
    case RateMode::Cqp:
        return "cqp";
    default:
        return "vbr";
    }
}

} // namespace

EncoderSession::EncoderSession(
    std::unique_ptr<Device> device, const EncodeConfig& config, const Options& options, const Trace& trace)
    : device_(std::move(device))
    , config_(config)
    , options_(options)
    , trace_(trace)
{
}

EncoderSession::~EncoderSession()
{
    try {
        finish();
    } catch (...) {
        // A destructor cannot report; finish() already traced what failed.
    }
}

int EncoderSession::sync_timeout_ms() const
{
    if (cold_ && !options_.sync_timeout_overridden)
        return options_.cold_start_timeout_ms;
    return options_.sync_timeout_ms;
}

EncoderSession::Clock::time_point EncoderSession::deadline() const
{
    return Clock::now() + std::chrono::milliseconds(sync_timeout_ms());
}

void EncoderSession::set_control(uint32_t id, int32_t value, const char* name)
{
    if (!device_->set_control(id, value)) {
        trace_("encode control missing name=%s id=%#x", name, id);
        throw Error(VA_STATUS_ERROR_OPERATION_FAILED, std::string("encoder control not available: ") + name);
    }
}

void EncoderSession::configure(const EncodeParams& params, unsigned width, unsigned height)
{
    std::lock_guard<std::mutex> lock(mutex_);
    require(!configured_, "encoder configured twice", VA_STATUS_ERROR_INVALID_PARAMETER);

    const bool h264 = config_.codec == V4L2_PIX_FMT_H264;
    const bool hevc = config_.codec == V4L2_PIX_FMT_HEVC;
    require(h264 || hevc, "encoder codec", VA_STATUS_ERROR_UNSUPPORTED_PROFILE);
    require(width && height, "encoder geometry", VA_STATUS_ERROR_INVALID_PARAMETER);
    // The context is the client's upper bound. A larger surface would need a
    // reconfigure, and the encoder has no SOURCE_CHANGE.
    require(width <= config_.width && height <= config_.height, "encode surface exceeds the context",
        VA_STATUS_ERROR_INVALID_SURFACE);
    config_.width = width;
    config_.height = height;

    // CAPTURE first: this is what selects the codec. G_FMT is not a
    // formality — iris_venc silently keeps the previous fourcc when it does
    // not recognise the request.
    const FormatInfo capture = device_->set_format(
        Queue::Capture, FormatInfo { config_.codec, config_.width, config_.height, 1, 0, 0 });
    require(capture.pixelformat == config_.codec, "encoder did not accept the codec",
        VA_STATUS_ERROR_UNSUPPORTED_PROFILE);

    const FormatInfo output = device_->set_format(
        Queue::Output, FormatInfo { V4L2_PIX_FMT_NV12, config_.width, config_.height, 1, 0, 0 });
    require(output.pixelformat == V4L2_PIX_FMT_NV12, "encoder did not accept NV12",
        VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT);
    const Layout input_layout = stable_layout(VA_FOURCC_NV12, config_.width, config_.height);
    require(input_layout.size >= output.sizeimage, "surface buffer is smaller than the encoder input",
        VA_STATUS_ERROR_INVALID_SURFACE);

    // The raw port is aligned to 128x32; the crop is what makes the coded
    // size the visible size instead of the aligned one.
    device_->set_crop(0, 0, config_.width, config_.height);

    if (h264) {
        set_control(V4L2_CID_MPEG_VIDEO_H264_PROFILE, params.profile, "h264 profile");
        if (params.level >= 0)
            set_control(V4L2_CID_MPEG_VIDEO_H264_LEVEL, params.level, "h264 level");
        set_control(V4L2_CID_MPEG_VIDEO_H264_ENTROPY_MODE,
            params.cabac ? V4L2_MPEG_VIDEO_H264_ENTROPY_MODE_CABAC
                         : V4L2_MPEG_VIDEO_H264_ENTROPY_MODE_CAVLC,
            "h264 entropy mode");
    } else {
        set_control(V4L2_CID_MPEG_VIDEO_HEVC_PROFILE, params.profile, "hevc profile");
        if (params.level >= 0)
            set_control(V4L2_CID_MPEG_VIDEO_HEVC_LEVEL, params.level, "hevc level");
    }

    const bool cqp = params.rate_mode == RateMode::Cqp;
    require(!cqp || params.qp >= 1, "CQP needs a picture QP", VA_STATUS_ERROR_INVALID_PARAMETER);
    set_control(V4L2_CID_MPEG_VIDEO_FRAME_RC_ENABLE, cqp ? 0 : 1, "frame rc enable");
    if (cqp) {
        // Rate control off: the I/P QP controls decide every picture.
        set_frame_qp(params.qp);
    } else {
        set_control(V4L2_CID_MPEG_VIDEO_BITRATE_MODE, bitrate_mode(params.rate_mode), "bitrate mode");
        if (params.bitrate)
            set_control(V4L2_CID_MPEG_VIDEO_BITRATE, static_cast<int32_t>(params.bitrate), "bitrate");
        if (params.rate_mode == RateMode::Cbr) {
            const uint32_t peak = params.peak_bitrate ? params.peak_bitrate : params.bitrate;
            if (peak)
                set_control(V4L2_CID_MPEG_VIDEO_BITRATE_PEAK, static_cast<int32_t>(peak), "peak bitrate");
        }
    }
    set_control(V4L2_CID_MPEG_VIDEO_B_FRAMES, 0, "b frames");
    set_control(V4L2_CID_MPEG_VIDEO_GOP_SIZE, gop_control(params.gop_size), "gop size");
    if (params.min_qp >= 0)
        set_control(h264 ? V4L2_CID_MPEG_VIDEO_H264_MIN_QP : V4L2_CID_MPEG_VIDEO_HEVC_MIN_QP, params.min_qp,
            "min qp");
    if (params.max_qp >= 0)
        set_control(h264 ? V4L2_CID_MPEG_VIDEO_H264_MAX_QP : V4L2_CID_MPEG_VIDEO_HEVC_MAX_QP, params.max_qp,
            "max qp");
    // Packed headers are not advertised: the firmware writes SPS/PPS itself.
    // Prefixing them to every sync frame is what makes the Annex-B stream
    // decodable from any IDR.
    if (!device_->set_control(V4L2_CID_MPEG_VIDEO_PREPEND_SPSPPS_TO_IDR, 1))
        trace_("encode control missing name=prepend-spspps-to-idr");

    // CAPTURE carries the frame rate the firmware rates against; OUTPUT
    // carries the operating rate, which only affects clocking.
    device_->set_frame_rate(Queue::Capture, params.frame_rate_num, params.frame_rate_den);
    device_->set_frame_rate(Queue::Output, params.frame_rate_num, params.frame_rate_den);

    // Iris sizes coded buffers from the current rate-control controls. A
    // format set before them keeps the buffered-RC size, and queueing then
    // fails EINVAL; re-derive it now that the controls are final and before
    // any buffer exists.
    const FormatInfo coded = device_->set_format(
        Queue::Capture, FormatInfo { config_.codec, config_.width, config_.height, 1, 0, 0 });
    require(coded.pixelformat == config_.codec, "encoder lost the codec after configuration",
        VA_STATUS_ERROR_UNSUPPORTED_PROFILE);

    require(device_->request_buffers(Queue::Output, kOutputSlots, V4L2_MEMORY_DMABUF) >= kOutputSlots,
        "encoder input buffers", VA_STATUS_ERROR_OPERATION_FAILED);
    const unsigned capture_count = device_->request_buffers(Queue::Capture, kCaptureSlots);
    require(capture_count > 0, "encoder output buffers", VA_STATUS_ERROR_OPERATION_FAILED);
    for (unsigned index = 0; index < capture_count; ++index) {
        const BufferInfo info = device_->query_buffer(Queue::Capture, index);
        capture_.push_back(CaptureSlot { index, info.length, static_cast<uint8_t*>(device_->map_buffer(
                                                                     Queue::Capture, info)) });
    }
    free_output_.clear();
    for (unsigned index = 0; index < kOutputSlots; ++index)
        free_output_.push_back(index);

    device_->stream(Queue::Output, true);
    device_->stream(Queue::Capture, true);
    for (const CaptureSlot& slot : capture_)
        device_->queue_buffer(Queue::Capture, slot.index, slot.length, 0, 0);

    applied_ = params;
    configured_ = true;
    trace_("encode open node=%s codec=%.4s raw=%ux%u crop=%ux%u mode=%s gop=%u rate=%u/%u",
        device_->path().c_str(), reinterpret_cast<const char*>(&config_.codec), output.width, output.height,
        config_.width, config_.height, rate_mode_name(params.rate_mode), params.gop_size,
        params.frame_rate_num, params.frame_rate_den);
}

void EncoderSession::set_frame_qp(int qp)
{
    const bool h264 = config_.codec == V4L2_PIX_FMT_H264;
    set_control(h264 ? V4L2_CID_MPEG_VIDEO_H264_I_FRAME_QP : V4L2_CID_MPEG_VIDEO_HEVC_I_FRAME_QP, qp,
        "i-frame qp");
    set_control(h264 ? V4L2_CID_MPEG_VIDEO_H264_P_FRAME_QP : V4L2_CID_MPEG_VIDEO_HEVC_P_FRAME_QP, qp,
        "p-frame qp");
}

void EncoderSession::apply_dynamic(const EncodeParams& params)
{
    // Only the controls the kernel marks CAP_FLAG_DYNAMIC_ALLOWED may be
    // written while the raw queue streams; the rest were set in configure().
    if (params.rate_mode == RateMode::Cqp && params.qp != applied_.qp) {
        set_frame_qp(params.qp);
        applied_.qp = params.qp;
    }
    if (params.bitrate && params.bitrate != applied_.bitrate) {
        set_control(V4L2_CID_MPEG_VIDEO_BITRATE, static_cast<int32_t>(params.bitrate), "bitrate");
        applied_.bitrate = params.bitrate;
    }
    if (params.rate_mode == RateMode::Cbr) {
        const uint32_t peak = params.peak_bitrate ? params.peak_bitrate : params.bitrate;
        if (peak && peak != applied_.peak_bitrate) {
            set_control(V4L2_CID_MPEG_VIDEO_BITRATE_PEAK, static_cast<int32_t>(peak), "peak bitrate");
            applied_.peak_bitrate = peak;
        }
    }
    if (params.gop_size != applied_.gop_size) {
        set_control(V4L2_CID_MPEG_VIDEO_GOP_SIZE, gop_control(params.gop_size), "gop size");
        applied_.gop_size = params.gop_size;
    }
}

void EncoderSession::reclaim_output(Clock::time_point limit)
{
    for (;;) {
        if (auto done = device_->dequeue_buffer(Queue::Output)) {
            free_output_.push_back(done->index);
            return;
        }
        if (Clock::now() >= limit)
            throw Error(VA_STATUS_ERROR_TIMEDOUT, "encoder input buffer was not returned");
        device_->poll(POLLIN | POLLPRI, kPollMs);
    }
}

std::optional<Dequeued> EncoderSession::wait_capture(Clock::time_point limit)
{
    for (;;) {
        if (auto done = device_->dequeue_buffer(Queue::Capture))
            return done;
        if (Clock::now() >= limit)
            return std::nullopt;
        device_->poll(POLLIN | POLLPRI, kPollMs);
    }
}

EncodeResult EncoderSession::encode(int dmabuf_fd, unsigned dmabuf_length, const EncodeParams& params)
{
    std::lock_guard<std::mutex> lock(mutex_);
    require(configured_ && !finished_, "encoder is not streaming", VA_STATUS_ERROR_OPERATION_FAILED);
    apply_dynamic(params);

    // The device layer takes microseconds and the encoder echoes the value
    // into the access unit. Derive it from the frame rate rather than a
    // counter: frame N starts N frame durations in, never at zero.
    const unsigned rate_num = params.frame_rate_num ? params.frame_rate_num : 30;
    const unsigned rate_den = params.frame_rate_den ? params.frame_rate_den : 1;
    const uint64_t timestamp_us = (frames_ + 1) * 1000000ull * rate_den / rate_num;

    const Clock::time_point limit = deadline();
    if (free_output_.empty())
        reclaim_output(limit);
    const unsigned index = free_output_.front();
    free_output_.pop_front();
    // bytesused must be non-zero: vb2 rejects a zero-length raw buffer and
    // poisons the instance.
    device_->queue_buffer_dmabuf(Queue::Output, index, dmabuf_fd, dmabuf_length, dmabuf_length, timestamp_us, 0);

    const std::optional<Dequeued> completion = wait_capture(limit);
    require(completion.has_value(), "encoder produced no access unit", VA_STATUS_ERROR_TIMEDOUT);
    require(!completion->error(), "encoder returned an error buffer", VA_STATUS_ERROR_ENCODING_ERROR);
    require(completion->index < capture_.size(), "encoder returned an unknown buffer",
        VA_STATUS_ERROR_ENCODING_ERROR);
    const CaptureSlot& slot = capture_[completion->index];
    require(completion->data_offset <= completion->bytesused && completion->bytesused <= slot.length,
        "encoder payload exceeds its buffer", VA_STATUS_ERROR_ENCODING_ERROR);

    EncodeResult result;
    result.bytes.assign(slot.mapping + completion->data_offset, slot.mapping + completion->bytesused);
    // No picture type here: the SM8550 encoder firmware does not send frame
    // info, so V4L2_BUF_FLAG_KEYFRAME is always clear. The bitstream is the
    // evidence (an IDR NAL at every GOP boundary).
    trace_("encode frame index=%u bytes=%zu", frames_++, result.bytes.size());

    if (!completion->last())
        device_->queue_buffer(Queue::Capture, slot.index, slot.length, 0, 0);
    reclaim_output(limit);
    cold_ = false;
    return result;
}

void EncoderSession::finish()
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!configured_ || finished_)
        return;
    finished_ = true;

    // STOP puts the drain marker on the raw port; the firmware returns the
    // pictures it still holds and marks the last one. The VA client has
    // already received every access unit, so the tail is discarded.
    try {
        device_->encoder_command(V4L2_ENC_CMD_STOP);
    } catch (const std::exception& error) {
        trace_("encode drain stop failed: %s", error.what());
    }
    try {
        const Clock::time_point limit = deadline();
        for (;;) {
            const std::optional<Dequeued> done = wait_capture(limit);
            if (!done) {
                trace_("encode drain result=timeout");
                break;
            }
            if (done->last()) {
                trace_("encode drain result=last");
                break;
            }
            if (done->index < capture_.size())
                device_->queue_buffer(Queue::Capture, done->index, capture_[done->index].length, 0, 0);
        }
    } catch (const std::exception& error) {
        trace_("encode drain failed: %s", error.what());
    }
    for (const CaptureSlot& slot : capture_)
        device_->unmap_buffer(slot.mapping, slot.length);
    capture_.clear();
    free_output_.clear();
    for (Queue queue : { Queue::Capture, Queue::Output }) {
        try {
            device_->stream(queue, false);
        } catch (const std::exception& error) {
            trace_("encode streamoff failed: %s", error.what());
        }
    }
    try {
        device_->request_buffers(Queue::Capture, 0);
    } catch (const std::exception& error) {
        trace_("encode release failed: %s", error.what());
    }
    try {
        device_->request_buffers(Queue::Output, 0, V4L2_MEMORY_DMABUF);
    } catch (const std::exception& error) {
        trace_("encode release failed: %s", error.what());
    }
}

} // namespace iris
