// SPDX-License-Identifier: MIT
//
// Access-unit reassembly for the encoder: a client that asks for an IDR in
// the middle of a GOP expects a stream it can cut at that picture, and the
// firmware only prefixes SPS/PPS to the IDRs it makes itself. Carrying the
// last parameter sets forward is the same thing FFmpeg does when it prepends
// extradata to a keyframe.

#pragma once

#include <cstdint>
#include <vector>

namespace iris::codec {

struct NalUnit {
    uint8_t type = 0;
    std::vector<uint8_t> bytes; // start code included
};

// Split an Annex-B buffer on three- and four-byte start codes. Returns
// nullptr-free units; a buffer with no start code yields nothing.
std::vector<NalUnit> split_annexb(const uint8_t* data, size_t size);

// `access_unit` with the parameter-set units from `last` in front of it when
// it does not already carry them. Both sides must be Annex-B. `last` is
// updated with the parameter sets the access unit itself carried, so the
// next call has them even when this one needed none.
std::vector<uint8_t> prepend_parameter_sets(const std::vector<uint8_t>& access_unit, std::vector<NalUnit>& last);

} // namespace iris::codec
