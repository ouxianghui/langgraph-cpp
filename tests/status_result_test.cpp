#include "foundation/status/result.hpp"
#include "foundation/status/status.hpp"

#include <cassert>
#include <memory>
#include <string>

int main()
{
    const lgc::Status ok = lgc::Status::ok();
    assert(ok.isOk());
    assert(ok.code() == lgc::StatusCode::Ok);
    assert(ok.toString() == "ok");

    const lgc::Status okWithMessage(lgc::StatusCode::Ok, "ignored");
    assert(okWithMessage.isOk());
    assert(okWithMessage.message().empty());
    assert(okWithMessage.toString() == "ok");

    const lgc::Status unknownCode(static_cast<lgc::StatusCode>(255), "bad code");
    assert(!unknownCode.isOk());
    assert(unknownCode.code() == lgc::StatusCode::Unknown);
    assert(unknownCode.toString() == "unknown: bad code");

    const lgc::Status missing = lgc::Status::notFound("checkpoint");
    assert(!missing.isOk());
    assert(missing.code() == lgc::StatusCode::NotFound);
    assert(missing.message() == "checkpoint");
    assert(missing.toString() == "not_found: checkpoint");

    lgc::Result<int> value = 42;
    assert(value.isOk());
    assert(value.status().isOk());
    assert(value.value() == 42);
    assert(*value == 42);

    lgc::Result<int> error = lgc::Status::invalidArgument("bad node id");
    assert(!error.isOk());
    assert(error.status().code() == lgc::StatusCode::InvalidArgument);

    lgc::Result<int> invalidError = lgc::Status::ok();
    assert(!invalidError.isOk());
    assert(invalidError.status().code() == lgc::StatusCode::Internal);

    lgc::Result<std::unique_ptr<std::string>> owned(
        std::make_unique<std::string>("state"));
    assert(owned.isOk());
    assert(*owned.value() == "state");

    lgc::Result<void> voidOk;
    assert(voidOk.isOk());
    voidOk.value();

    lgc::Result<void> voidError = lgc::Status::unavailable("storage");
    assert(!voidError.isOk());
    assert(voidError.status().code() == lgc::StatusCode::Unavailable);

    lgc::Result<void> invalidVoidError = lgc::Status::ok();
    assert(!invalidVoidError.isOk());
    assert(invalidVoidError.status().code() == lgc::StatusCode::Internal);

    return 0;
}
