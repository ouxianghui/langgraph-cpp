#include "foundation/utils/http_utils.hpp"

#include <cctype>
#include <cstdio>
#include <random>
#include <string>

namespace lc {

std::string makeReqUuid8()
{
    thread_local std::mt19937_64 rng { std::random_device {}() };
    std::uniform_int_distribution<std::uint64_t> dist;
    const auto v = dist(rng) & 0xffffffffULL;
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%08llx",
        static_cast<unsigned long long>(v));
    return std::string(buf);
}

std::string appendReqUuid(std::string path)
{
    const std::string id = makeReqUuid8();
    if (path.find('?') != std::string::npos) {
        path += "&requuid=" + id;
    } else {
        path += "?requuid=" + id;
    }
    return path;
}

std::string urlEncode(std::string_view s)
{
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    out.reserve(s.size() * 3);
    for (unsigned char c : s) {
        if (std::isalnum(c) != 0 || c == '-' || c == '_' || c == '.' || c == '~') {
            out += static_cast<char>(c);
        } else {
            out.push_back('%');
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 0xF]);
        }
    }
    return out;
}

std::string base64Encode(std::string_view in)
{
    static const char kTable[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((in.size() + 2) / 3) * 4);
    const auto* bytes = reinterpret_cast<const unsigned char*>(in.data());
    const std::size_t len = in.size();
    for (std::size_t i = 0; i < len; i += 3) {
        const unsigned octet_a = bytes[i];
        const unsigned octet_b = i + 1 < len ? bytes[i + 1] : 0;
        const unsigned octet_c = i + 2 < len ? bytes[i + 2] : 0;
        const unsigned triple = (octet_a << 16) | (octet_b << 8) | octet_c;
        out.push_back(kTable[(triple >> 18) & 63]);
        out.push_back(kTable[(triple >> 12) & 63]);
        out.push_back(i + 1 < len ? kTable[(triple >> 6) & 63] : '=');
        out.push_back(i + 2 < len ? kTable[triple & 63] : '=');
    }
    return out;
}

std::string basicAuthHeaderValue(std::string_view clientId, std::string_view clientSecret)
{
    const std::string credentials = std::string(clientId) + ":" + std::string(clientSecret);
    return std::string("Basic ") + base64Encode(credentials);
}

} // namespace lc
