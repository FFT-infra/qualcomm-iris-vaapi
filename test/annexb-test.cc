// SPDX-License-Identifier: MIT
//
// Access-unit reassembly for a requested IDR. The firmware prefixes SPS/PPS
// to its own sync frames only, so the driver carries the last ones forward;
// these are the rules that decide what is a parameter set and where a NAL
// unit ends.

#include "../src/codec/annexb.h"

#include <cstdio>
#include <string>
#include <vector>

namespace {

int failures = 0;

void check(bool condition, const char* what)
{
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

// 00 00 01 <nal header> <payload>: five bytes, six with the extra zero of a
// four-byte start code.
std::vector<uint8_t> unit(uint8_t type, std::initializer_list<uint8_t> payload = { 0xaa })
{
    std::vector<uint8_t> out { 0, 0, 1, static_cast<uint8_t>(0x60 | type) };
    out.insert(out.end(), payload);
    return out;
}

std::vector<uint8_t> join(std::initializer_list<std::vector<uint8_t>> parts)
{
    std::vector<uint8_t> out;
    for (const std::vector<uint8_t>& part : parts)
        out.insert(out.end(), part.begin(), part.end());
    return out;
}

std::vector<uint8_t> with_four_byte_code(std::vector<uint8_t> one)
{
    one.insert(one.begin(), 0);
    return one;
}

void test_split()
{
    // SPS, PPS, IDR slice: three units, four-byte code on the first.
    const std::vector<uint8_t> access = join({ with_four_byte_code(unit(7)), unit(8), unit(5) });
    const std::vector<iris::codec::NalUnit> units = iris::codec::split_annexb(access.data(), access.size());
    check(units.size() == 3, "three units");
    check(units.size() == 3 && units[0].type == 7 && units[1].type == 8 && units[2].type == 5, "unit types");
    check(units.size() == 3 && units[0].bytes.size() == 6, "a four-byte start code belongs to its unit");
    check(units.size() == 3 && units[1].bytes.size() == 5, "a three-byte start code keeps its unit to the next one");

    check(iris::codec::split_annexb(nullptr, 0).empty(), "an empty buffer has no units");
}

void test_prepend()
{
    std::vector<iris::codec::NalUnit> last;
    const std::vector<uint8_t> sets = join({ with_four_byte_code(unit(7)), unit(8), unit(6) });
    const std::vector<uint8_t> idr = join({ sets, unit(5) });
    const std::vector<uint8_t> p = unit(1);

    // The first access unit carries the sets and only updates the cache.
    check(iris::codec::prepend_parameter_sets(idr, last) == idr, "a unit that has its sets is unchanged");
    check(last.size() == 3, "SPS, PPS and SEI are remembered");

    // A bare picture gets them in front, in the order they arrived.
    const std::vector<uint8_t> prefixed = iris::codec::prepend_parameter_sets(p, last);
    check(prefixed.size() == sets.size() + p.size(), "prefix plus picture");
    check(prefixed == join({ sets, p }), "prefix order");

    // A keyframe without sets also gets them, and the cache survives a
    // picture that carried nothing.
    const std::vector<uint8_t> bare_idr = unit(5);
    check(iris::codec::prepend_parameter_sets(bare_idr, last) == join({ sets, bare_idr }),
        "a bare keyframe gets the sets");
    check(last.size() == 3, "a picture without sets leaves the cache alone");

    // Nothing known yet: nothing to add, and no crash.
    std::vector<iris::codec::NalUnit> empty;
    check(iris::codec::prepend_parameter_sets(p, empty) == p, "no cache yet means no prefix");
}

} // namespace

int main()
{
    test_split();
    test_prepend();
    if (failures) {
        std::fprintf(stderr, "%d annexb check(s) failed\n", failures);
        return 1;
    }
    std::printf("PASS annexb parameter sets\n");
    return 0;
}
