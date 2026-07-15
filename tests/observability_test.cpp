#include "foundation/observability/metrics.hpp"
#include "foundation/observability/tracing.hpp"
#include "foundation/redaction/redactor.hpp"
#include "foundation/status/status.hpp"
#include "foundation/time/clock.hpp"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <memory>
#include <span>
#include <thread>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace {

class FailingRandomSource final : public lgc::IRandomSource {
public:
    [[nodiscard]] lgc::Result<void> fill(std::span<std::byte>) override
    {
        return lgc::Status::unavailable("random source failed");
    }
};

[[nodiscard]] lgc::TraceContext requireRootContext()
{
    auto context = lgc::makeRootContext();
    assert(context.isOk());
    return std::move(*context);
}

} // namespace

int main()
{
    using namespace std::chrono_literals;

    lgc::InMemoryMetricRecorder metrics;
    assert(metrics.incrementCounter("graph.runs", 1, { { "status", "ok" } }).isOk());
    assert(metrics.incrementCounter("graph.runs", 2, { { "status", "ok" } }).isOk());
    assert(metrics.recordHistogram("node.output.bytes", 10).isOk());
    assert(metrics.recordHistogram("node.output.bytes", 30).isOk());
    assert(metrics.recordDuration("node.duration", 1500us).isOk());
    assert(metrics.recordDuration("node.duration", -1us).code() == lgc::StatusCode::InvalidArgument);

    lgc::ManualClock clock;
    {
        lgc::ScopedMetricTimer timer(metrics, "scoped.duration", {}, clock);
        clock.advance(7ms);
        assert(timer.stop().isOk());
        assert(timer.stopped());
    }

    auto snapshots = metrics.snapshots();
    assert(snapshots.size() == 4);
    bool sawCounter = false;
    bool sawHistogram = false;
    bool sawScopedTimer = false;
    for (const auto& snapshot : snapshots) {
        if (snapshot.name_ == "graph.runs") {
            sawCounter = true;
            assert(snapshot.type_ == lgc::MetricType::Counter);
            assert(snapshot.count_ == 2);
            assert(snapshot.sum_ == 3.0);
            assert(snapshot.tags_.front().key_ == "status");
        }
        if (snapshot.name_ == "node.output.bytes") {
            sawHistogram = true;
            assert(snapshot.type_ == lgc::MetricType::Histogram);
            assert(snapshot.count_ == 2);
            assert(snapshot.min_ == 10.0);
            assert(snapshot.max_ == 30.0);
        }
        if (snapshot.name_ == "scoped.duration") {
            sawScopedTimer = true;
            assert(snapshot.type_ == lgc::MetricType::Timer);
            assert(snapshot.count_ == 1);
            assert(snapshot.last_ == 7.0);
        }
    }
    assert(sawCounter);
    assert(sawHistogram);
    assert(sawScopedTimer);
    assert(metrics.incrementCounter("", 1).code() == lgc::StatusCode::InvalidArgument);
    assert(metrics.incrementCounter("graph.runs", -1).code() == lgc::StatusCode::InvalidArgument);
    assert(metrics.incrementCounter("graph.runs", 0).code() == lgc::StatusCode::InvalidArgument);
    assert(metrics.recordGauge("queue.depth", -2).isOk());
    assert(lgc::metricTypeName(lgc::MetricType::Timer) == "timer");
    metrics.clear();
    assert(metrics.snapshots().empty());

    {
        lgc::InMemoryMetricRecorder bounded(lgc::MetricOptions {
            .limits_ = lgc::MetricLimits {
                .maxMetrics_ = 1,
            },
            .overflowPolicy_ = lgc::MetricOverflowPolicy::DropOldest,
        });
        assert(bounded.incrementCounter("first").isOk());
        assert(bounded.incrementCounter("second").isOk());
        const auto boundedSnapshots = bounded.snapshots();
        assert(boundedSnapshots.size() == 1);
        assert(boundedSnapshots.front().name_ == "second");
        assert(lgc::metricOverflowPolicyName(lgc::MetricOverflowPolicy::DropNewest) == "drop_newest");
    }

    {
        auto redactor = std::make_shared<lgc::Redactor>();
        lgc::InMemoryMetricRecorder redacted(lgc::MetricOptions {
            .redactor_ = redactor,
        });
        assert(redacted.incrementCounter(
                           "llm.requests",
                           1,
                           {
                               { "api_key", "sk-1234567890abcdef" },
                               { "email", "user@example.com" },
                           })
                   .isOk());
        const auto redactedSnapshots = redacted.snapshots();
        assert(redactedSnapshots.size() == 1);
        const auto& tags = redactedSnapshots.front().tags_;
        const auto secret = std::find_if(tags.begin(), tags.end(), [](const auto& tag) {
            return tag.key_ == "api_key";
        });
        const auto email = std::find_if(tags.begin(), tags.end(), [](const auto& tag) {
            return tag.key_ == "email";
        });
        assert(secret != tags.end());
        assert(email != tags.end());
        assert(secret->value_ == "[REDACTED]");
        assert(email->value_.find("user@example.com") == std::string::npos);
    }

    {
        lgc::InMemoryMetricRecorder limited(lgc::MetricOptions {
            .limits_ = lgc::MetricLimits {
                .maxMetrics_ = 1,
                .maxNameLength_ = 8,
                .maxTags_ = 1,
                .maxTagKeyLength_ = 4,
                .maxTagValueLength_ = 4,
            },
        });
        assert(limited.incrementCounter("too.long.name").code() == lgc::StatusCode::InvalidArgument);
        assert(limited.incrementCounter("ok", 1, { { "abcd", "12345" } }).code() == lgc::StatusCode::InvalidArgument);
        assert(limited.incrementCounter("ok", 1, { { "a", "1" }, { "b", "2" } }).code() == lgc::StatusCode::InvalidArgument);
        assert(limited.incrementCounter("ok", 1, { { "run_id", "r1" } }).code() == lgc::StatusCode::InvalidArgument);
        assert(limited.incrementCounter("one").isOk());
        assert(limited.incrementCounter("two").code() == lgc::StatusCode::ResourceExhausted);
        assert(limited.close().isOk());
        assert(limited.isClosed());
        assert(limited.flush().code() == lgc::StatusCode::Unavailable);
        assert(limited.incrementCounter("one").code() == lgc::StatusCode::Unavailable);
    }

    {
        lgc::InMemoryMetricRecorder concurrent;
        std::vector<std::thread> workers;
        for (int i = 0; i < 4; ++i) {
            workers.emplace_back([&] {
                for (int j = 0; j < 250; ++j)
                    assert(concurrent.incrementCounter("concurrent").isOk());
            });
        }
        for (auto& worker : workers)
            worker.join();
        const auto concurrentSnapshots = concurrent.snapshots();
        assert(concurrentSnapshots.size() == 1);
        assert(concurrentSnapshots.front().count_ == 1000);
        assert(concurrentSnapshots.front().sum_ == 1000.0);
    }

    {
        lgc::InMemoryMetricRecorder bucketed(lgc::MetricOptions {
            .histogramBuckets_ = { 10.0, 20.0 },
        });
        assert(bucketed.recordHistogram("payload.bytes", 5).isOk());
        assert(bucketed.recordHistogram("payload.bytes", 15).isOk());
        assert(bucketed.recordHistogram("payload.bytes", 25).isOk());
        auto bucketedSnapshots = bucketed.snapshots();
        assert(bucketedSnapshots.size() == 1);
        assert(bucketedSnapshots.front().bucketBounds_.size() == 2);
        assert(bucketedSnapshots.front().bucketCounts_.size() == 3);
        assert(bucketedSnapshots.front().bucketCounts_[0] == 1);
        assert(bucketedSnapshots.front().bucketCounts_[1] == 2);
        assert(bucketedSnapshots.front().bucketCounts_[2] == 3);
    }

    {
        lgc::InMemoryMetricRecorder memoryLimited(lgc::MetricOptions {
            .limits_ = lgc::MetricLimits {
                .maxMetrics_ = 0,
                .maxApproxBytes_ = 260,
            },
        });
        assert(memoryLimited.incrementCounter("first").isOk());
        assert(memoryLimited.incrementCounter("second").code() == lgc::StatusCode::ResourceExhausted);
    }

    auto sink = std::make_shared<lgc::InMemoryTraceSink>();
    lgc::ManualClock traceClock;
    lgc::Tracer tracer(
        sink,
        lgc::TraceOptions {
            .clock_ = &traceClock,
        });

    auto root = tracer.startSpan("graph.run", std::nullopt, nlohmann::json { { "thread_id", "t1" } });
    assert(root.isOk());
    assert(root->isValid());
    const auto rootContext = root->context();
    assert(rootContext.isValid());
    assert(rootContext.sampled());
    auto traceparent = lgc::formatTraceParent(rootContext);
    assert(traceparent.isOk());
    auto parsed = lgc::parseTraceParent(*traceparent, "vendor=value");
    assert(parsed.isOk());
    assert(parsed->traceId_ == rootContext.traceId_);
    assert(parsed->spanId_ == rootContext.spanId_);
    assert(parsed->traceState_ == "vendor=value");
    assert(parsed->remoteParent_);
    auto parsedWithBaggage = lgc::parseTraceParent(*traceparent, "vendor=value", "tenant=alpha");
    assert(parsedWithBaggage.isOk());
    assert(parsedWithBaggage->baggage_ == "tenant=alpha");
    auto formattedBaggage = lgc::formatBaggage(*parsedWithBaggage);
    assert(formattedBaggage.isOk());
    assert(*formattedBaggage == "tenant=alpha");
    assert(root->setAttribute("run_id", "r1").isOk());
    traceClock.advance(1ms);
    assert(root->addEvent("run.started", nlohmann::json { { "step", 0 } }).isOk());

    {
        auto child = tracer.startSpan("node.execute", rootContext);
        assert(child.isOk());
        const auto childContext = child->context();
        assert(childContext.traceId_ == rootContext.traceId_);
        assert(childContext.parentSpanId_ == rootContext.spanId_);
        assert(child->addEvent("tool.call").isOk());
        assert(child->setStatus(lgc::SpanStatus::Ok).isOk());
    }

    assert(root->setStatus(lgc::SpanStatus::Error, "node failed").isOk());
    traceClock.advance(2ms);
    assert(root->end().isOk());
    assert(root->end().isOk());

    auto spans = sink->spans();
    assert(spans.size() == 2);
    bool sawRoot = false;
    bool sawChild = false;
    for (const auto& span : spans) {
        assert(span.context_.traceId_ == rootContext.traceId_);
        assert(span.endedAt_.has_value());
        if (span.name_ == "graph.run") {
            sawRoot = true;
            assert(span.status_ == lgc::SpanStatus::Error);
            assert(span.statusMessage_ == "node failed");
            assert(span.events_.size() == 1);
            assert(span.attributes_.at("run_id") == "r1");
        }
        if (span.name_ == "node.execute") {
            sawChild = true;
            assert(span.context_.parentSpanId_ == rootContext.spanId_);
            assert(span.status_ == lgc::SpanStatus::Ok);
            assert(span.events_.size() == 1);
        }
    }
    assert(sawRoot);
    assert(sawChild);
    assert(lgc::spanStatusName(lgc::SpanStatus::Cancelled) == "cancelled");
    assert(lgc::traceOverflowPolicyName(lgc::TraceOverflowPolicy::DropOldest) == "drop_oldest");

    lgc::InMemoryTraceSink bounded(lgc::TraceOptions {
        .limits_ = lgc::TraceLimits {
            .maxSpans_ = 1,
        },
    });
    auto spanData = lgc::SpanRecord {
        .context_ = requireRootContext(),
        .name_ = "one",
        .startedAt_ = traceClock.now(),
    };
    assert(bounded.recordSpan(spanData).isOk());
    spanData.context_ = requireRootContext();
    auto full = bounded.recordSpan(spanData);
    assert(!full.isOk());
    assert(full.code() == lgc::StatusCode::ResourceExhausted);

    {
        lgc::InMemoryTraceSink dropping(lgc::TraceOptions {
            .limits_ = lgc::TraceLimits {
                .maxSpans_ = 1,
            },
            .overflowPolicy_ = lgc::TraceOverflowPolicy::DropOldest,
        });
        assert(dropping.recordSpan(lgc::SpanRecord {
                   .context_ = requireRootContext(),
                   .name_ = "old",
               })
                   .isOk());
        assert(dropping.recordSpan(lgc::SpanRecord {
                   .context_ = requireRootContext(),
                   .name_ = "new",
               })
                   .isOk());
        auto dropped = dropping.spans();
        assert(dropped.size() == 1);
        assert(dropped.front().name_ == "new");
    }

    {
        auto redactor = std::make_shared<lgc::Redactor>();
        auto redactedSink = std::make_shared<lgc::InMemoryTraceSink>(lgc::TraceOptions {
            .redactor_ = redactor,
        });
        lgc::Tracer redactedTracer(
            redactedSink,
            lgc::TraceOptions {
                .redactor_ = redactor,
            });
        auto span = redactedTracer.startSpan("redacted", std::nullopt, nlohmann::json {
            { "authorization", "Bearer abcdefghijklmnop" },
            { "email", "user@example.com" },
        });
        assert(span.isOk());
        assert(span->setStatus(lgc::SpanStatus::Error, "token=sk-1234567890abcdef").isOk());
        assert(span->end().isOk());
        auto redactedSpans = redactedSink->spans();
        assert(redactedSpans.size() == 1);
        assert(redactedSpans.front().attributes_["authorization"] == "[REDACTED]");
        assert(redactedSpans.front().attributes_["email"].get<std::string>().find("user@example.com") == std::string::npos);
        assert(redactedSpans.front().statusMessage_.find("sk-1234567890abcdef") == std::string::npos);
    }

    {
        lgc::InMemoryTraceSink limited(lgc::TraceOptions {
            .limits_ = lgc::TraceLimits {
                .maxNameLength_ = 4,
                .maxAttributes_ = 1,
                .maxAttributeKeyLength_ = 4,
                .maxAttributeStringLength_ = 4,
                .maxEvents_ = 1,
            },
        });
        auto badName = limited.recordSpan(lgc::SpanRecord {
            .context_ = requireRootContext(),
            .name_ = "toolong",
        });
        assert(badName.code() == lgc::StatusCode::InvalidArgument);
        auto tooLongAttribute = limited.recordSpan(lgc::SpanRecord {
            .context_ = requireRootContext(),
            .name_ = "ok",
            .attributes_ = nlohmann::json { { "key", "value-too-long" } },
        });
        assert(tooLongAttribute.code() == lgc::StatusCode::ResourceExhausted);
    }

    {
        lgc::Tracer failingTracer(
            std::make_shared<lgc::InMemoryTraceSink>(),
            lgc::TraceOptions {
                .randomSource_ = std::make_shared<FailingRandomSource>(),
            });
        auto failed = failingTracer.startSpan("span");
        assert(!failed.isOk());
        assert(failed.status().code() == lgc::StatusCode::Unavailable);
    }

    {
        auto concurrentSink = std::make_shared<lgc::InMemoryTraceSink>();
        lgc::Tracer concurrentTracer(concurrentSink);
        std::vector<std::thread> workers;
        for (int i = 0; i < 4; ++i) {
            workers.emplace_back([&] {
                for (int j = 0; j < 50; ++j) {
                    auto span = concurrentTracer.startSpan("worker");
                    assert(span.isOk());
                    assert(span->end().isOk());
                }
            });
        }
        for (auto& worker : workers)
            worker.join();
        assert(concurrentSink->spans().size() == 200);
    }

    assert(sink->close().isOk());
    assert(sink->isClosed());
    assert(sink->flush().code() == lgc::StatusCode::Unavailable);
    auto closedRecord = sink->recordSpan(lgc::SpanRecord {
        .context_ = requireRootContext(),
        .name_ = "closed",
    });
    assert(closedRecord.code() == lgc::StatusCode::Unavailable);

    return 0;
}
