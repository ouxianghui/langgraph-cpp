#pragma once

#include <nlohmann/json.hpp>

namespace lc {

template <typename T>
static void putValue(json& output, const char* key,
    const std::optional<T>& value)
{
    if (value.has_value()) {
        output[key] = *value;
    }
}

template <typename T>
static void putValue(json& output, const char* key, const T& value)
{
    output[key] = value;
}

template <typename T>
static std::optional<T> getValue(const json& input, const char* key)
{
    if (!input.contains(key) || input.at(key).is_null()) {
        return std::nullopt;
    }
    return input.at(key).get<T>();
}

template <typename T>
static void assignValue(const json& input, const char* key,
    std::optional<T>& value)
{
    value = getValue<T>(input, key);
}

template <typename T>
static void assignValue(const json& input, const char* key, T& value)
{
    auto parsed = getValue<T>(input, key);
    if (parsed.has_value()) {
        value = *parsed;
    }
}

} // namespace lc

#define JM(field) #field, field

#define JSON_WRITE_FIELD(key, field) \
    ::lc::putValue(output, key, value.field);
#define JSON_READ_FIELD(key, field) \
    ::lc::assignValue(input, key, value.field);

#define JSON_PAIR_APPLY_0(M, ...)
#define JSON_PAIR_APPLY_2(M, k, v, ...) \
    M(k, v)                             \
    JSON_PAIR_APPLY_0(M, __VA_ARGS__)
#define JSON_PAIR_APPLY_4(M, k, v, ...) \
    M(k, v)                             \
    JSON_PAIR_APPLY_2(M, __VA_ARGS__)
#define JSON_PAIR_APPLY_6(M, k, v, ...) \
    M(k, v)                             \
    JSON_PAIR_APPLY_4(M, __VA_ARGS__)
#define JSON_PAIR_APPLY_8(M, k, v, ...) \
    M(k, v)                             \
    JSON_PAIR_APPLY_6(M, __VA_ARGS__)
#define JSON_PAIR_APPLY_10(M, k, v, ...) \
    M(k, v)                              \
    JSON_PAIR_APPLY_8(M, __VA_ARGS__)
#define JSON_PAIR_APPLY_12(M, k, v, ...) \
    M(k, v)                              \
    JSON_PAIR_APPLY_10(M, __VA_ARGS__)
#define JSON_PAIR_APPLY_14(M, k, v, ...) \
    M(k, v)                              \
    JSON_PAIR_APPLY_12(M, __VA_ARGS__)
#define JSON_PAIR_APPLY_16(M, k, v, ...) \
    M(k, v)                              \
    JSON_PAIR_APPLY_14(M, __VA_ARGS__)
#define JSON_PAIR_APPLY_18(M, k, v, ...) \
    M(k, v)                              \
    JSON_PAIR_APPLY_16(M, __VA_ARGS__)
#define JSON_PAIR_APPLY_20(M, k, v, ...) \
    M(k, v)                              \
    JSON_PAIR_APPLY_18(M, __VA_ARGS__)
#define JSON_PAIR_APPLY_22(M, k, v, ...) \
    M(k, v)                              \
    JSON_PAIR_APPLY_20(M, __VA_ARGS__)
#define JSON_PAIR_APPLY_24(M, k, v, ...) \
    M(k, v)                              \
    JSON_PAIR_APPLY_22(M, __VA_ARGS__)
#define JSON_PAIR_APPLY_26(M, k, v, ...) \
    M(k, v)                              \
    JSON_PAIR_APPLY_24(M, __VA_ARGS__)
#define JSON_PAIR_APPLY_28(M, k, v, ...) \
    M(k, v)                              \
    JSON_PAIR_APPLY_26(M, __VA_ARGS__)
#define JSON_PAIR_APPLY_30(M, k, v, ...) \
    M(k, v)                              \
    JSON_PAIR_APPLY_28(M, __VA_ARGS__)
#define JSON_PAIR_APPLY_32(M, k, v, ...) \
    M(k, v)                              \
    JSON_PAIR_APPLY_30(M, __VA_ARGS__)
#define JSON_PAIR_APPLY_34(M, k, v, ...) \
    M(k, v)                              \
    JSON_PAIR_APPLY_32(M, __VA_ARGS__)
#define JSON_PAIR_APPLY_36(M, k, v, ...) \
    M(k, v)                              \
    JSON_PAIR_APPLY_34(M, __VA_ARGS__)
#define JSON_PAIR_APPLY_38(M, k, v, ...) \
    M(k, v)                              \
    JSON_PAIR_APPLY_36(M, __VA_ARGS__)
#define JSON_PAIR_APPLY_40(M, k, v, ...) \
    M(k, v)                              \
    JSON_PAIR_APPLY_38(M, __VA_ARGS__)
#define JSON_PAIR_APPLY_42(M, k, v, ...) \
    M(k, v)                              \
    JSON_PAIR_APPLY_40(M, __VA_ARGS__)
#define JSON_PAIR_APPLY_44(M, k, v, ...) \
    M(k, v)                              \
    JSON_PAIR_APPLY_42(M, __VA_ARGS__)
#define JSON_PAIR_APPLY_46(M, k, v, ...) \
    M(k, v)                              \
    JSON_PAIR_APPLY_44(M, __VA_ARGS__)
#define JSON_PAIR_APPLY_48(M, k, v, ...) \
    M(k, v)                              \
    JSON_PAIR_APPLY_46(M, __VA_ARGS__)
#define JSON_PAIR_APPLY_50(M, k, v, ...) \
    M(k, v)                              \
    JSON_PAIR_APPLY_48(M, __VA_ARGS__)
#define JSON_PAIR_APPLY_52(M, k, v, ...) \
    M(k, v)                              \
    JSON_PAIR_APPLY_50(M, __VA_ARGS__)
#define JSON_PAIR_APPLY_54(M, k, v, ...) \
    M(k, v)                              \
    JSON_PAIR_APPLY_52(M, __VA_ARGS__)
#define JSON_PAIR_APPLY_56(M, k, v, ...) \
    M(k, v)                              \
    JSON_PAIR_APPLY_54(M, __VA_ARGS__)
#define JSON_PAIR_APPLY_58(M, k, v, ...) \
    M(k, v)                              \
    JSON_PAIR_APPLY_56(M, __VA_ARGS__)
#define JSON_PAIR_APPLY_60(M, k, v, ...) \
    M(k, v)                              \
    JSON_PAIR_APPLY_58(M, __VA_ARGS__)
#define JSON_PAIR_APPLY_62(M, k, v, ...) \
    M(k, v)                              \
    JSON_PAIR_APPLY_60(M, __VA_ARGS__)
#define JSON_PAIR_APPLY_64(M, k, v, ...) \
    M(k, v)                              \
    JSON_PAIR_APPLY_62(M, __VA_ARGS__)
#define JSON_PAIR_APPLY_66(M, k, v, ...) \
    M(k, v)                              \
    JSON_PAIR_APPLY_64(M, __VA_ARGS__)
#define JSON_PAIR_APPLY_68(M, k, v, ...) \
    M(k, v)                              \
    JSON_PAIR_APPLY_66(M, __VA_ARGS__)
#define JSON_PAIR_APPLY_70(M, k, v, ...) \
    M(k, v)                              \
    JSON_PAIR_APPLY_68(M, __VA_ARGS__)
#define JSON_PAIR_APPLY_72(M, k, v, ...) \
    M(k, v)                              \
    JSON_PAIR_APPLY_70(M, __VA_ARGS__)
#define JSON_PAIR_APPLY_74(M, k, v, ...) \
    M(k, v)                              \
    JSON_PAIR_APPLY_72(M, __VA_ARGS__)
#define JSON_PAIR_APPLY_76(M, k, v, ...) \
    M(k, v)                              \
    JSON_PAIR_APPLY_74(M, __VA_ARGS__)
#define JSON_PAIR_APPLY_78(M, k, v, ...) \
    M(k, v)                              \
    JSON_PAIR_APPLY_76(M, __VA_ARGS__)
#define JSON_PAIR_APPLY_80(M, k, v, ...) \
    M(k, v)                              \
    JSON_PAIR_APPLY_78(M, __VA_ARGS__)
#define JSON_PAIR_APPLY_82(M, k, v, ...) \
    M(k, v)                              \
    JSON_PAIR_APPLY_80(M, __VA_ARGS__)
#define JSON_PAIR_APPLY_84(M, k, v, ...) \
    M(k, v)                              \
    JSON_PAIR_APPLY_82(M, __VA_ARGS__)
#define JSON_PAIR_APPLY_86(M, k, v, ...) \
    M(k, v)                              \
    JSON_PAIR_APPLY_84(M, __VA_ARGS__)
#define JSON_PAIR_APPLY_88(M, k, v, ...) \
    M(k, v)                              \
    JSON_PAIR_APPLY_86(M, __VA_ARGS__)
#define JSON_PAIR_APPLY_90(M, k, v, ...) \
    M(k, v)                              \
    JSON_PAIR_APPLY_88(M, __VA_ARGS__)
#define JSON_PAIR_APPLY_92(M, k, v, ...) \
    M(k, v)                              \
    JSON_PAIR_APPLY_90(M, __VA_ARGS__)
#define JSON_PAIR_APPLY_94(M, k, v, ...) \
    M(k, v)                              \
    JSON_PAIR_APPLY_92(M, __VA_ARGS__)
#define JSON_PAIR_APPLY_96(M, k, v, ...) \
    M(k, v)                              \
    JSON_PAIR_APPLY_94(M, __VA_ARGS__)
#define JSON_PAIR_APPLY_98(M, k, v, ...) \
    M(k, v)                              \
    JSON_PAIR_APPLY_96(M, __VA_ARGS__)
#define JSON_PAIR_APPLY_100(M, k, v, ...) \
    M(k, v)                               \
    JSON_PAIR_APPLY_98(M, __VA_ARGS__)
#define JSON_PAIR_APPLY_102(M, k, v, ...) \
    M(k, v)                               \
    JSON_PAIR_APPLY_100(M, __VA_ARGS__)
#define JSON_PAIR_APPLY_104(M, k, v, ...) \
    M(k, v)                               \
    JSON_PAIR_APPLY_102(M, __VA_ARGS__)
#define JSON_PAIR_APPLY_106(M, k, v, ...) \
    M(k, v)                               \
    JSON_PAIR_APPLY_104(M, __VA_ARGS__)
#define JSON_PAIR_APPLY_108(M, k, v, ...) \
    M(k, v)                               \
    JSON_PAIR_APPLY_106(M, __VA_ARGS__)
#define JSON_PAIR_APPLY_110(M, k, v, ...) \
    M(k, v)                               \
    JSON_PAIR_APPLY_108(M, __VA_ARGS__)
#define JSON_PAIR_APPLY_112(M, k, v, ...) \
    M(k, v)                               \
    JSON_PAIR_APPLY_110(M, __VA_ARGS__)
#define JSON_PAIR_APPLY_114(M, k, v, ...) \
    M(k, v)                               \
    JSON_PAIR_APPLY_112(M, __VA_ARGS__)
#define JSON_PAIR_APPLY_116(M, k, v, ...) \
    M(k, v)                               \
    JSON_PAIR_APPLY_114(M, __VA_ARGS__)
#define JSON_PAIR_APPLY_118(M, k, v, ...) \
    M(k, v)                               \
    JSON_PAIR_APPLY_116(M, __VA_ARGS__)
#define JSON_PAIR_APPLY_120(M, k, v, ...) \
    M(k, v)                               \
    JSON_PAIR_APPLY_118(M, __VA_ARGS__)
#define JSON_PAIR_APPLY_122(M, k, v, ...) \
    M(k, v)                               \
    JSON_PAIR_APPLY_120(M, __VA_ARGS__)
#define JSON_PAIR_APPLY_124(M, k, v, ...) \
    M(k, v)                               \
    JSON_PAIR_APPLY_122(M, __VA_ARGS__)
#define JSON_PAIR_APPLY_126(M, k, v, ...) \
    M(k, v)                               \
    JSON_PAIR_APPLY_124(M, __VA_ARGS__)
#define JSON_PAIR_APPLY_128(M, k, v, ...) \
    M(k, v)                               \
    JSON_PAIR_APPLY_126(M, __VA_ARGS__)
#define JSON_PAIR_APPLY_130(M, k, v, ...) \
    M(k, v)                               \
    JSON_PAIR_APPLY_128(M, __VA_ARGS__)
#define JSON_PAIR_APPLY_132(M, k, v, ...) \
    M(k, v)                               \
    JSON_PAIR_APPLY_130(M, __VA_ARGS__)
#define JSON_PAIR_APPLY_134(M, k, v, ...) \
    M(k, v)                               \
    JSON_PAIR_APPLY_132(M, __VA_ARGS__)
#define JSON_PAIR_APPLY_136(M, k, v, ...) \
    M(k, v)                               \
    JSON_PAIR_APPLY_134(M, __VA_ARGS__)
#define JSON_PAIR_APPLY_138(M, k, v, ...) \
    M(k, v)                               \
    JSON_PAIR_APPLY_136(M, __VA_ARGS__)
#define JSON_PAIR_APPLY_140(M, k, v, ...) \
    M(k, v)                               \
    JSON_PAIR_APPLY_138(M, __VA_ARGS__)
#define JSON_PAIR_APPLY_142(M, k, v, ...) \
    M(k, v)                               \
    JSON_PAIR_APPLY_140(M, __VA_ARGS__)
#define JSON_PAIR_APPLY_144(M, k, v, ...) \
    M(k, v)                               \
    JSON_PAIR_APPLY_142(M, __VA_ARGS__)
#define JSON_PAIR_APPLY_146(M, k, v, ...) \
    M(k, v)                               \
    JSON_PAIR_APPLY_144(M, __VA_ARGS__)
#define JSON_PAIR_APPLY_148(M, k, v, ...) \
    M(k, v)                               \
    JSON_PAIR_APPLY_146(M, __VA_ARGS__)
#define JSON_PAIR_APPLY_150(M, k, v, ...) \
    M(k, v)                               \
    JSON_PAIR_APPLY_148(M, __VA_ARGS__)
#define JSON_PAIR_APPLY_152(M, k, v, ...) \
    M(k, v)                               \
    JSON_PAIR_APPLY_150(M, __VA_ARGS__)
#define JSON_PAIR_APPLY_154(M, k, v, ...) \
    M(k, v)                               \
    JSON_PAIR_APPLY_152(M, __VA_ARGS__)
#define JSON_PAIR_APPLY_156(M, k, v, ...) \
    M(k, v)                               \
    JSON_PAIR_APPLY_154(M, __VA_ARGS__)
#define JSON_PAIR_APPLY_158(M, k, v, ...) \
    M(k, v)                               \
    JSON_PAIR_APPLY_156(M, __VA_ARGS__)
#define JSON_PAIR_APPLY_160(M, k, v, ...) \
    M(k, v)                               \
    JSON_PAIR_APPLY_158(M, __VA_ARGS__)
#define JSON_PAIR_APPLY_162(M, k, v, ...) \
    M(k, v)                               \
    JSON_PAIR_APPLY_160(M, __VA_ARGS__)
#define JSON_PAIR_APPLY_164(M, k, v, ...) \
    M(k, v)                               \
    JSON_PAIR_APPLY_162(M, __VA_ARGS__)
#define JSON_PAIR_APPLY_166(M, k, v, ...) \
    M(k, v)                               \
    JSON_PAIR_APPLY_164(M, __VA_ARGS__)
#define JSON_PAIR_APPLY_168(M, k, v, ...) \
    M(k, v)                               \
    JSON_PAIR_APPLY_166(M, __VA_ARGS__)
#define JSON_PAIR_APPLY_170(M, k, v, ...) \
    M(k, v)                               \
    JSON_PAIR_APPLY_168(M, __VA_ARGS__)
#define JSON_PAIR_APPLY_172(M, k, v, ...) \
    M(k, v)                               \
    JSON_PAIR_APPLY_170(M, __VA_ARGS__)
#define JSON_PAIR_APPLY_174(M, k, v, ...) \
    M(k, v)                               \
    JSON_PAIR_APPLY_172(M, __VA_ARGS__)
#define JSON_PAIR_APPLY_176(M, k, v, ...) \
    M(k, v)                               \
    JSON_PAIR_APPLY_174(M, __VA_ARGS__)
#define JSON_PAIR_APPLY_178(M, k, v, ...) \
    M(k, v)                               \
    JSON_PAIR_APPLY_176(M, __VA_ARGS__)
#define JSON_PAIR_APPLY_180(M, k, v, ...) \
    M(k, v)                               \
    JSON_PAIR_APPLY_178(M, __VA_ARGS__)
#define JSON_PAIR_APPLY_182(M, k, v, ...) \
    M(k, v)                               \
    JSON_PAIR_APPLY_180(M, __VA_ARGS__)
#define JSON_PAIR_APPLY_184(M, k, v, ...) \
    M(k, v)                               \
    JSON_PAIR_APPLY_182(M, __VA_ARGS__)
#define JSON_PAIR_APPLY_186(M, k, v, ...) \
    M(k, v)                               \
    JSON_PAIR_APPLY_184(M, __VA_ARGS__)
#define JSON_PAIR_APPLY_188(M, k, v, ...) \
    M(k, v)                               \
    JSON_PAIR_APPLY_186(M, __VA_ARGS__)
#define JSON_PAIR_APPLY_190(M, k, v, ...) \
    M(k, v)                               \
    JSON_PAIR_APPLY_188(M, __VA_ARGS__)
#define JSON_PAIR_APPLY_192(M, k, v, ...) \
    M(k, v)                               \
    JSON_PAIR_APPLY_190(M, __VA_ARGS__)
#define JSON_PAIR_APPLY_194(M, k, v, ...) \
    M(k, v)                               \
    JSON_PAIR_APPLY_192(M, __VA_ARGS__)
#define JSON_PAIR_APPLY_196(M, k, v, ...) \
    M(k, v)                               \
    JSON_PAIR_APPLY_194(M, __VA_ARGS__)
#define JSON_PAIR_APPLY_198(M, k, v, ...) \
    M(k, v)                               \
    JSON_PAIR_APPLY_196(M, __VA_ARGS__)
#define JSON_PAIR_APPLY_200(M, k, v, ...) \
    M(k, v)                               \
    JSON_PAIR_APPLY_198(M, __VA_ARGS__)

#define JSON_COUNT_ARGS_IMPL(                                                  \
    _1, _2, _3, _4, _5, _6, _7, _8, _9, _10, _11, _12, _13, _14, _15, _16,     \
    _17, _18, _19, _20, _21, _22, _23, _24, _25, _26, _27, _28, _29, _30, _31, \
    _32, _33, _34, _35, _36, _37, _38, _39, _40, _41, _42, _43, _44, _45, _46, \
    _47, _48, _49, _50, _51, _52, _53, _54, _55, _56, _57, _58, _59, _60, _61, \
    _62, _63, _64, _65, _66, _67, _68, _69, _70, _71, _72, _73, _74, _75, _76, \
    _77, _78, _79, _80, _81, _82, _83, _84, _85, _86, _87, _88, _89, _90, _91, \
    _92, _93, _94, _95, _96, _97, _98, _99, _100, _101, _102, _103, _104,      \
    _105, _106, _107, _108, _109, _110, _111, _112, _113, _114, _115, _116,    \
    _117, _118, _119, _120, _121, _122, _123, _124, _125, _126, _127, _128,    \
    _129, _130, _131, _132, _133, _134, _135, _136, _137, _138, _139, _140,    \
    _141, _142, _143, _144, _145, _146, _147, _148, _149, _150, _151, _152,    \
    _153, _154, _155, _156, _157, _158, _159, _160, _161, _162, _163, _164,    \
    _165, _166, _167, _168, _169, _170, _171, _172, _173, _174, _175, _176,    \
    _177, _178, _179, _180, _181, _182, _183, _184, _185, _186, _187, _188,    \
    _189, _190, _191, _192, _193, _194, _195, _196, _197, _198, _199, _200,    \
    _201, N, ...)                                                              \
    _201
#define JSON_COUNT_ARGS(...)                                                     \
    JSON_COUNT_ARGS_IMPL(                                                        \
        __VA_ARGS__, 200, 199, 198, 197, 196, 195, 194, 193, 192, 191, 190, 189, \
        188, 187, 186, 185, 184, 183, 182, 181, 180, 179, 178, 177, 176, 175,    \
        174, 173, 172, 171, 170, 169, 168, 167, 166, 165, 164, 163, 162, 161,    \
        160, 159, 158, 157, 156, 155, 154, 153, 152, 151, 150, 149, 148, 147,    \
        146, 145, 144, 143, 142, 141, 140, 139, 138, 137, 136, 135, 134, 133,    \
        132, 131, 130, 129, 128, 127, 126, 125, 124, 123, 122, 121, 120, 119,    \
        118, 117, 116, 115, 114, 113, 112, 111, 110, 109, 108, 107, 106, 105,    \
        104, 103, 102, 101, 100, 99, 98, 97, 96, 95, 94, 93, 92, 91, 90, 89, 88, \
        87, 86, 85, 84, 83, 82, 81, 80, 79, 78, 77, 76, 75, 74, 73, 72, 71, 70,  \
        69, 68, 67, 66, 65, 64, 63, 62, 61, 60, 59, 58, 57, 56, 55, 54, 53, 52,  \
        51, 50, 49, 48, 47, 46, 45, 44, 43, 42, 41, 40, 39, 38, 37, 36, 35, 34,  \
        33, 32, 31, 30, 29, 28, 27, 26, 25, 24, 23, 22, 21, 20, 19, 18, 17, 16,  \
        15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0)
#define JSON_CAT_IMPL(a, b) a##b
#define JSON_CAT(a, b) JSON_CAT_IMPL(a, b)
#define JSON_FOR_EACH_PAIR(M, ...) \
    JSON_CAT(JSON_PAIR_APPLY_, JSON_COUNT_ARGS(__VA_ARGS__))(M, __VA_ARGS__)

#define DEFINE_JSON_FROM_TO(TYPE, ...)                    \
    void toJson(json& output, const TYPE& value)          \
    {                                                     \
        output = json::object();                          \
        JSON_FOR_EACH_PAIR(JSON_WRITE_FIELD, __VA_ARGS__) \
    }                                                     \
    void fromJson(const json& input, TYPE& value)         \
    {                                                     \
        JSON_FOR_EACH_PAIR(JSON_READ_FIELD, __VA_ARGS__)  \
    }

/// Declares `fromJson` / `toJson` (implementations live in a `.cpp` via
/// `DEFINE_JSON_FROM_TO`) and nlohmann ADL `from_json` / `to_json` so
/// `json(value)`, `j.get<T>()`, and `j[key] = nestedStruct` work. Expand inside
/// `namespace lc`.
#define DECLARE_JSON_ADL(TYPE)                                        \
    void fromJson(const lc::json& input, TYPE& value);                \
    void toJson(lc::json& output, const TYPE& value);                 \
    inline void to_json(lc::json& j, const TYPE& v) { toJson(j, v); } \
    inline void from_json(const lc::json& j, TYPE& v) { fromJson(j, v); }
