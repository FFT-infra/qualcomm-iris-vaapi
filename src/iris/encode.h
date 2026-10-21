// SPDX-License-Identifier: MIT
//
// One Iris stateful encode session per VA context. Unlike the decoder, this
// transaction is synchronous: one raw frame in, one access unit out, no slot
// pool and no publish policy. The firmware owns GOP placement and the
// sequence headers.

#pragma once

#include "alloc.h"
#include "device.h"

#include "../codec/annexb.h"
#include "../util/encode-params.h"
#include "../util/options.h"
#include "../util/trace.h"

#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

namespace iris {

struct EncodeConfig {
    uint32_t codec = 0; // V4L2_PIX_FMT_H264 or V4L2_PIX_FMT_HEVC
    // The context's declared size, used only as an upper bound. FFmpeg
    // aligns a context up (1920x1088) while its frames stay 1920x1080, so
    // the real raw geometry comes from the first surface (configure()).
    unsigned width = 0, height = 0;
};

struct EncodeResult {
    std::vector<uint8_t> bytes;
};

class EncoderSession {
public:
    EncoderSession(std::unique_ptr<Device> device, const EncodeConfig& config, const Options& options,
        const Trace& trace);
    ~EncoderSession();
    EncoderSession(const EncoderSession&) = delete;
    EncoderSession& operator=(const EncoderSession&) = delete;

    bool configured() const { return configured_; }
    const EncodeConfig& config() const { return config_; }
    // Codec hook for a requested IDR: the firmware prefixes SPS/PPS to its
    // own IDRs only, so a levered one would not be decodable from a cut.
    using PrepareAccessUnit
        = std::function<std::vector<uint8_t>(const std::vector<uint8_t>&, std::vector<codec::NalUnit>&)>;
    // First picture only: formats, crop, controls, buffer pools, STREAMON.
    // `width`/`height` are the input surface's, which may be smaller than
    // the context the client declared.
    void configure(const EncodeParams& params, unsigned width, unsigned height, PrepareAccessUnit prepare = {});
    // Queue `dmabuf_fd` as one raw frame, wait for its access unit, and
    // return the completion. Safe to call from one thread; the session
    // mutex serialises a stray second caller. The V4L2 timestamp advances by
    // one frame duration of the session rate on every picture: the firmware
    // echoes it, and it stops producing access units if the sequence ever
    // goes backwards.
    EncodeResult encode(int dmabuf_fd, unsigned dmabuf_length, const EncodeParams& params);
    // STOP, drain the tail, STREAMOFF, REQBUFS(0). Idempotent.
    void finish();

private:
    struct CaptureSlot {
        unsigned index = 0;
        unsigned length = 0;
        uint8_t* mapping = nullptr;
    };

    using Clock = std::chrono::steady_clock;

    void apply_dynamic(const EncodeParams& params);
    void set_control(uint32_t id, int32_t value, const char* name);
    void set_frame_qp(int qp);
    void configure_deblocking(const EncodeParams& params);
    void reclaim_output(Clock::time_point deadline);
    std::optional<Dequeued> wait_capture(Clock::time_point deadline);
    int sync_timeout_ms() const;
    Clock::time_point deadline() const;

    std::unique_ptr<Device> device_;
    EncodeConfig config_;
    Options options_;
    Trace trace_;

    bool configured_ = false;
    bool finished_ = false;
    bool cold_ = true;
    EncodeParams applied_;

    std::vector<CaptureSlot> capture_;
    std::deque<unsigned> free_output_;
    unsigned frames_ = 0;
    // Session frame rate, fixed in configure(), and the timestamp of the
    // last submitted picture.
    unsigned rate_num_ = 30;
    unsigned rate_den_ = 1;
    uint64_t timestamp_us_ = 0;
    // Parameter sets carried by the last access unit, for a requested IDR the
    // firmware wrote without them.
    std::vector<codec::NalUnit> parameter_sets_;
    PrepareAccessUnit prepare_;
    // One transaction at a time; also keeps finish() from tearing the queues
    // down under an encode that is still waiting for a completion.
    std::mutex mutex_;
};

} // namespace iris
