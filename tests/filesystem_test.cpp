#include "foundation/filesystem/filesystem.hpp"
#include "foundation/status/status.hpp"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#if !defined(_WIN32)
#include <unistd.h>
#endif

int main()
{
    namespace fs = std::filesystem;

    const auto root = fs::temp_directory_path() / "langgraph_cpp_filesystem_test";
    fs::remove_all(root);
    assert(lgc::ensureDir(root).isOk());

    auto normalized = lgc::normalize("a/../b.txt", root);
    assert(normalized.isOk());
    assert(normalized->is_absolute());
    assert(normalized->filename() == "b.txt");

    auto safeChild = lgc::resolveChild(root, "checkpoints/thread-1/state.json");
    assert(safeChild.isOk());
    assert(lgc::requireInside(root, *safeChild).isOk());

    auto unsafe = lgc::resolveChild(root, "../outside.txt");
    assert(!unsafe.isOk());
    assert(unsafe.status().code() == lgc::StatusCode::PermissionDenied);

    assert(lgc::requireSafeRelativePath("models/local.bin").isOk());
    assert(lgc::requireSafeRelativePath("/absolute/path").code() == lgc::StatusCode::InvalidArgument);
    assert(lgc::requireSafeRelativePath("a/../../b").code() == lgc::StatusCode::PermissionDenied);
    assert(lgc::requireSafeRelativePath("models/local:bin").code() == lgc::StatusCode::InvalidArgument);
    assert(lgc::requireSafeRelativePath("CON.txt").code() == lgc::StatusCode::InvalidArgument);
    lgc::PathPolicy shortPolicy;
    shortPolicy.maxComponentLength_ = 4;
    assert(lgc::requireSafeRelativePath("model.bin", shortPolicy).code() == lgc::StatusCode::InvalidArgument);
    assert(lgc::normalize("").status().code() == lgc::StatusCode::InvalidArgument);

    const auto target = root / "checkpoint.json";
    assert(lgc::writeFileAtomic(target, R"({"step":1})").isOk());
    auto text = lgc::readFile(target);
    assert(text.isOk());
    assert(*text == R"({"step":1})");

    lgc::AtomicWriteOptions noReplace;
    noReplace.replaceExisting_ = false;
    auto writeAgain = lgc::writeFileAtomic(target, "new", noReplace);
    assert(!writeAgain.isOk());
    assert(writeAgain.status().code() == lgc::StatusCode::AlreadyExists);
    assert(lgc::readFile(target).value() == R"({"step":1})");

    lgc::AtomicWriteOptions durable;
    durable.durable_ = true;
    assert(lgc::writeFileAtomic(root / "durable.txt", "durable", durable).isOk());
    assert(lgc::readFile(root / "durable.txt").value() == "durable");

    lgc::AtomicWriteOptions badAtomicPrefix;
    badAtomicPrefix.tempPrefix_ = "bad/prefix";
    auto badAtomic = lgc::writeFileAtomic(root / "bad-prefix.txt", "x", badAtomicPrefix);
    assert(!badAtomic.isOk());
    assert(badAtomic.status().code() == lgc::StatusCode::InvalidArgument);

    const auto parentFile = root / "not-a-directory";
    {
        std::ofstream file(parentFile);
        file << "file";
    }
    assert(lgc::ensureDir(parentFile).status().code() == lgc::StatusCode::FailedPrecondition);
    auto writeUnderFile = lgc::writeFileAtomic(parentFile / "child.txt", "x");
    assert(!writeUnderFile.isOk());
    assert(writeUnderFile.status().code() == lgc::StatusCode::FailedPrecondition);

    auto tempUnderFile = lgc::TempFile::create(lgc::TempFileOptions {
        .directory_ = parentFile,
        .createDirectory_ = false,
    });
    assert(!tempUnderFile.isOk());

    auto badTempPrefix = lgc::TempFile::create(lgc::TempFileOptions {
        .directory_ = root,
        .prefix_ = "bad/prefix",
    });
    assert(!badTempPrefix.isOk());
    assert(badTempPrefix.status().code() == lgc::StatusCode::InvalidArgument);

    const std::vector<std::byte> bytes {
        std::byte { 0x01 },
        std::byte { 0x02 },
        std::byte { 0x03 },
    };
    const auto binaryTarget = root / "model.bin";
    assert(lgc::writeFileAtomic(binaryTarget, bytes).isOk());
    assert(fs::file_size(binaryTarget) == bytes.size());

    {
        auto temp = lgc::TempFile::create(lgc::TempFileOptions { .directory_ = root });
        assert(temp.isOk());
        const auto tempPath = temp->path();
        assert(fs::exists(tempPath));
        assert(temp->write("scratch").isOk());
        assert(temp->flush().isOk());
        assert(lgc::readFile(tempPath).value() == "scratch");
        assert(temp->close().isOk());
        assert(!temp->valid());
        assert(lgc::readFile(tempPath).value() == "scratch");
    }

    auto temp = lgc::TempFile::create(lgc::TempFileOptions { .directory_ = root });
    assert(temp.isOk());
    const auto releasedPath = temp->release();
    assert(!releasedPath.empty());
    assert(fs::exists(releasedPath));
    temp = lgc::TempFile {};
    assert(fs::exists(releasedPath));
    fs::remove(releasedPath);

    const auto missing = lgc::realPath(root / "missing.txt");
    assert(!missing.isOk());
    assert(missing.status().code() == lgc::StatusCode::NotFound);

    auto directoryRead = lgc::readFile(root);
    assert(!directoryRead.isOk());
    assert(directoryRead.status().code() == lgc::StatusCode::FailedPrecondition);

    const auto largePath = root / "large.txt";
    const std::string largeText(1024, 'x');
    assert(lgc::writeFileAtomic(largePath, largeText).isOk());
    auto tooLarge = lgc::readFile(largePath, lgc::ReadFileOptions { .maxBytes_ = 128 });
    assert(!tooLarge.isOk());
    assert(tooLarge.status().code() == lgc::StatusCode::ResourceExhausted);
    auto unlimited = lgc::readFile(largePath, lgc::ReadFileOptions { .maxBytes_ = 0 });
    assert(unlimited.isOk());
    assert(unlimited->size() == largeText.size());

    const auto outside = fs::temp_directory_path() / "langgraph_cpp_filesystem_outside.txt";
    assert(lgc::writeFileAtomic(outside, "outside").isOk());
    auto inside = lgc::isInside(root, outside);
    assert(inside.isOk());
    assert(!*inside);
    fs::remove(outside);

    const auto symlinkOutside = fs::temp_directory_path() / "langgraph_cpp_filesystem_symlink_outside";
    fs::remove_all(symlinkOutside);
    assert(lgc::ensureDir(symlinkOutside).isOk());
    std::error_code symlinkEc;
    fs::create_directory_symlink(symlinkOutside, root / "link-out", symlinkEc);
    if (!symlinkEc) {
        auto escaped = lgc::resolveChild(root, "link-out/escaped.txt");
        assert(!escaped.isOk());
        assert(escaped.status().code() == lgc::StatusCode::PermissionDenied);
    }
    fs::remove_all(symlinkOutside);

    const auto concurrentTarget = root / "concurrent.txt";
    std::vector<std::string> values;
    for (int i = 0; i < 12; ++i)
        values.push_back("value-" + std::to_string(i));

    std::atomic<int> failures { 0 };
    std::vector<std::thread> writers;
    for (const auto& value : values) {
        writers.emplace_back([&, value] {
            if (!lgc::writeFileAtomic(concurrentTarget, value).isOk())
                failures.fetch_add(1, std::memory_order_relaxed);
        });
    }
    for (auto& writer : writers)
        writer.join();

    assert(failures.load(std::memory_order_relaxed) == 0);
    auto concurrentText = lgc::readFile(concurrentTarget);
    assert(concurrentText.isOk());
    assert(std::find(values.begin(), values.end(), *concurrentText) != values.end());

#if !defined(_WIN32)
    const auto readOnlyDirectory = root / "readonly";
    assert(lgc::ensureDir(readOnlyDirectory).isOk());
    std::error_code permissionEc;
    fs::permissions(
        readOnlyDirectory,
        fs::perms::owner_read | fs::perms::owner_exec,
        fs::perm_options::replace,
        permissionEc);
    if (!permissionEc && ::geteuid() != 0) {
        auto denied = lgc::writeFileAtomic(readOnlyDirectory / "blocked.txt", "x");
        assert(!denied.isOk());
    }
    fs::permissions(readOnlyDirectory, fs::perms::owner_all, fs::perm_options::replace, permissionEc);
#endif

    fs::remove_all(root);
    return 0;
}
