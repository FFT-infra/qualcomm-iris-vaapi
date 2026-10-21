// SPDX-License-Identifier: MIT
//
// One Iris stateful encode session per VA context. Unlike the decoder, this
// transaction is synchronous: one raw frame in, one access unit out, no slot
// pool and no publish policy. The firmware owns GOP placement and the
// sequence headers.

#pragma once

#include "alloc.h"
#include "device.h"

#include "../util/encode-params.h"
#include "../util/options.h"
#include "../util/trace.h"

#include <chrono>
#include <cstdint>
#include <deque>
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
    // First picture only: formats, crop, controls, buffer pools, STREAMON.
    // `width`/`height` are the input surface's, which may be smaller than
    // the context the client declared.
    void configure(const EncodeParams& params, unsigned width, unsigned height);
    // Queue `dmabuf_fd` as one raw frame, wait for its access unit, and
    // return the completion. Safe to call from one thread; the session
    // mutex serialises a stray second caller. The V4L2 timestamp is derived
    // from the frame rate: the firmware echoes it, and a bogus delta must
    // not reach anything that paces on timestamps.
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
    // One transaction at a time; also keeps finish() from tearing the queues
    // down under an encode that is still waiting for a completion.
    std::mutex mutex_;
};

} // namespace iris
