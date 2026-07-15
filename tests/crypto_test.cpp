#include "foundation/crypto/crypto.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <string>

int main()
{
    auto sha256 = lgc::digestHex(lgc::HashAlgorithm::Sha256, "abc");
    assert(sha256.isOk());
    assert(*sha256 == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");

    auto sha512 = lgc::digestHex(lgc::HashAlgorithm::Sha512, "");
    assert(sha512.isOk());
    assert(*sha512
        == "cf83e1357eefb8bdf1542850d66d8007d620e4050b5715dc83f4a921d36ce9ce"
           "47d0d13c5d85f2b0ff8318d2877eec2f63b931bd47417a81a538327af927da3e");

    auto sign = lgc::signHex(
        lgc::HashAlgorithm::Sha256,
        "key",
        "The quick brown fox jumps over the lazy dog");
    assert(sign.isOk());
    assert(*sign == "f7bc83f430538424b13298e6aa6fb143ef4d59a14946175997479dbc2d1a3cd8");

    auto random = lgc::random(32);
    assert(random.isOk());
    assert(random->size() == 32);

    auto randomHex = lgc::randomHex(16);
    assert(randomHex.isOk());
    assert(randomHex->size() == 32);

    const std::array<std::uint8_t, 3> bytes { 0x0f, 0xa0, 0x5c };
    const auto hex = lgc::toHex(bytes);
    assert(hex == "0fa05c");

    auto parsed = lgc::fromHex("0FA05c");
    assert(parsed.isOk());
    assert(std::equal(parsed->begin(), parsed->end(), bytes.begin(), bytes.end()));

    auto invalidHex = lgc::fromHex("abc");
    assert(!invalidHex.isOk());
    assert(invalidHex.status().code() == lgc::StatusCode::InvalidArgument);

    assert(lgc::maskSecret("sk-1234567890abcdef") == "sk-1********cdef");
    assert(lgc::maskSecret("short") == "********");
    assert(lgc::maskSecret("") == "");
    assert(lgc::maskSecret("abcdef", lgc::MaskOptions {
        .visiblePrefix_ = 2,
        .visibleSuffix_ = 2,
        .minMaskedLength_ = 3,
        .maskChar_ = '#',
    }) == "###");

    const std::array<std::uint8_t, 3> lhs { 1, 2, 3 };
    const std::array<std::uint8_t, 3> rhs { 1, 2, 3 };
    const std::array<std::uint8_t, 3> other { 1, 2, 4 };
    assert(lgc::secureEquals(lhs, rhs));
    assert(!lgc::secureEquals(lhs, other));

    return 0;
}
