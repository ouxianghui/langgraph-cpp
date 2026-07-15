#include "foundation/cache/cache.hpp"

#include "foundation/cache/cache_common.hh"

namespace lgc {

Status validateCacheKey(const CacheKey& key)
{
    if (key.key_.empty())
        return Status::invalidArgument("cache key cannot be empty");
    if (cache_detail::containsNull(key.namespace_))
        return Status::invalidArgument("cache namespace contains a null byte");
    if (cache_detail::containsNull(key.key_))
        return Status::invalidArgument("cache key contains a null byte");
    return Status::ok();
}

} // namespace lgc
