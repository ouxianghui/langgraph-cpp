#pragma once

#include <string>
#include <string_view>

namespace lc {

/// 8 hex chars (32-bit value), for `requuid`-style query parameters.
[[nodiscard]] std::string makeReqUuid8();

/// Appends `?requuid=` or `&requuid=` using `makeReqUuid8()`.
[[nodiscard]] std::string appendReqUuid(std::string path);

/// RFC 3986 unreserved characters kept literal; others as `%XX`. Suitable for query segments and
/// `application/x-www-form-urlencoded` field values.
[[nodiscard]] std::string urlEncode(std::string_view s);

/// Standard base64 (no line breaks).
[[nodiscard]] std::string base64Encode(std::string_view in);

/// Value for an `Authorization` header (`Basic …`).
[[nodiscard]] std::string basicAuthHeaderValue(std::string_view clientId,
    std::string_view clientSecret);

} // namespace lc
