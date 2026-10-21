// SPDX-License-Identifier: MIT

#include "annexb.h"

namespace iris::codec {

namespace {

bool is_parameter_set(uint8_t type)
{
    // SPS, PPS, SEI, sequence end and fill all travel with a keyframe:
    // decoders need the parameter sets and treat the rest as harmless.
    return type == 6 || type == 7 || type == 8 || type == 9 || type == 10 || type == 11 || type == 12;
}

bool carries_parameter_set(const std::vector<NalUnit>& units)
{
    for (const NalUnit& unit : units)
        if (unit.type == 7 || unit.type == 8)
            return true;
    return false;
}

} // namespace

std::vector<NalUnit> split_annexb(const uint8_t* data, size_t size)
{
    // Find each 00 00 01 from where the previous unit ended, so the three
    // bytes that close a four-byte code are never considered again: 00 00 00
    // 01 is one code whose extra zero belongs to its own unit.
    auto find_code = [data, size](size_t from) -> size_t {
        for (size_t i = from; i + 3 < size; ++i)
            if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1)
                return i;
        return size;
    };

    std::vector<NalUnit> units;
    size_t search = 0;
    size_t code = find_code(search);
    while (code != size) {
        const size_t header = code + 3;
        // A four-byte code starts at `code`, which `find_code` matched on
        // its last three bytes; the leading zero is part of this unit and is
        // never the last byte of the previous one. `search` is where the
        // previous unit ended, which is the same check without an off-by-one
        // at the start of the buffer.
        const size_t begin = code > search && data[code - 1] == 0 ? code - 1 : code;
        const size_t next = find_code(header + 1);
        NalUnit unit;
        unit.type = data[header] & 0x1f;
        unit.bytes.assign(data + begin, data + next);
        units.push_back(std::move(unit));
        search = next;
        code = next;
    }
    return units;
}

std::vector<uint8_t> prepend_parameter_sets(const std::vector<uint8_t>& access_unit, std::vector<NalUnit>& last)
{
    const std::vector<NalUnit> units = split_annexb(access_unit.data(), access_unit.size());
    if (units.empty())
        return access_unit;

    std::vector<uint8_t> out;
    if (!carries_parameter_set(units)) {
        for (const NalUnit& unit : last)
            out.insert(out.end(), unit.bytes.begin(), unit.bytes.end());
    }
    out.insert(out.end(), access_unit.begin(), access_unit.end());

    // Remember what this access unit carried, in order, so a later one can
    // reuse it.
    std::vector<NalUnit> carried;
    for (const NalUnit& unit : units)
        if (is_parameter_set(unit.type))
            carried.push_back(unit);
    if (!carried.empty())
        last = std::move(carried);
    return out;
}

} // namespace iris::codec
