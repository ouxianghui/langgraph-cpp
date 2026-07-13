#include "foundation/status/result.hpp"
#include "foundation/status/status.hpp"

#include <cassert>
#include <memory>
#include <string>

int main()
{
    const lc::Status ok = lc::Status::ok();
    assert(ok.isOk());
    assert(ok.code() == lc::StatusCode::Ok);
    assert(ok.toString() == "ok");

    const lc::Status okWithMessage(lc::StatusCode::Ok, "ignored");
    assert(okWithMessage.isOk());
    assert(okWithMessage.message().empty());
    assert(okWithMessage.toString() == "ok");

    const lc::Status unknownCode(static_cast<lc::StatusCode>(255), "bad code");
    assert(!unknownCode.isOk());
    assert(unknownCode.code() == lc::StatusCode::Unknown);
    assert(unknownCode.toString() == "unknown: bad code");

    const lc::Status missing = lc::Status::notFound("checkpoint");
    assert(!missing.isOk());
    assert(missing.code() == lc::StatusCode::NotFound);
    assert(missing.message() == "checkpoint");
    assert(missing.toString() == "not_found: checkpoint");

    lc::Result<int> value = 42;
    assert(value.isOk());
    assert(value.status().isOk());
    assert(value.value() == 42);
    assert(*value == 42);

    lc::Result<int> error = lc::Status::invalidArgument("bad node id");
    assert(!error.isOk());
    assert(error.status().code() == lc::StatusCode::InvalidArgument);

    lc::Result<int> invalidError = lc::Status::ok();
    assert(!invalidError.isOk());
    assert(invalidError.status().code() == lc::StatusCode::Internal);

    lc::Result<std::unique_ptr<std::string>> owned(
        std::make_unique<std::string>("state"));
    assert(owned.isOk());
    assert(*owned.value() == "state");

    lc::Result<void> voidOk;
    assert(voidOk.isOk());
    voidOk.value();

    lc::Result<void> voidError = lc::Status::unavailable("storage");
    assert(!voidError.isOk());
    assert(voidError.status().code() == lc::StatusCode::Unavailable);

    lc::Result<void> invalidVoidError = lc::Status::ok();
    assert(!invalidVoidError.isOk());
    assert(invalidVoidError.status().code() == lc::StatusCode::Internal);

    return 0;
}
