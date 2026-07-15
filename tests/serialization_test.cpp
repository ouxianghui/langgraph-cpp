#include "foundation/serialization/state_codec.hpp"

#include <cassert>
#include <chrono>
#include <string>

int main()
{
    const lgc::JsonStateCodec stateCodec;
    auto state = lgc::State::fromJson(R"({"messages":[],"step":1})");
    assert(state.isOk());
    assert(state->json() == R"({"messages":[],"step":1})");

    auto encodedState = stateCodec.encode(*state);
    assert(encodedState.isOk());
    assert(encodedState->contentType_ == "application/json");

    auto decodedState = stateCodec.decode(*encodedState);
    assert(decodedState.isOk());
    assert(*decodedState == *state);

    auto invalidState = lgc::State::fromJson(R"(["not","object"])");
    assert(!invalidState.isOk());
    assert(invalidState.status().code() == lgc::StatusCode::InvalidArgument);

    const auto createdAt = std::chrono::system_clock::time_point(std::chrono::milliseconds(123456));
    lgc::Checkpoint checkpoint {
        .threadId_ = "thread-1",
        .checkpointId_ = "checkpoint-1",
        .parentCheckpointId_ = std::string("checkpoint-0"),
        .step_ = 7,
        .state_ = *state,
        .nextNodes_ = { "model", "tools" },
        .nextTasks_ = {
            lgc::CheckpointTask {
                .nodeId_ = "model",
                .order_ = 0,
            },
            lgc::CheckpointTask {
                .nodeId_ = "tools",
                .state_ = *lgc::State::fromJson(R"({"tool":"calculator"})"),
                .order_ = 1,
            },
        },
        .writes_ = {
            lgc::CheckpointWrite {
                .taskPath_ = "root/model",
                .nodeId_ = "model",
                .update_ = *lgc::State::fromJson(R"({"messages":[{"role":"assistant"}]})"),
                .order_ = 0,
                .hasNextTasks_ = true,
                .nextTasks_ = {
                    lgc::CheckpointTask {
                        .nodeId_ = "tools",
                        .order_ = 1,
                    },
                },
            },
        },
        .pendingWrites_ = {
            lgc::CheckpointWrite {
                .taskPath_ = "root/sensor",
                .nodeId_ = "sensor",
                .update_ = *lgc::State::fromJson(R"({"readings":["ok"]})"),
                .order_ = 2,
            },
        },
        .channelVersions_ = {
            { "messages", 2 },
            { "step", 1 },
        },
        .versionsSeen_ = {
            { "model", { { "messages", 1 } } },
        },
        .updatedChannels_ = { "messages" },
        .createdAt_ = createdAt,
    };

    const lgc::JsonCheckpointCodec checkpointCodec;
    auto encodedCheckpoint = checkpointCodec.encode(checkpoint);
    assert(encodedCheckpoint.isOk());
    assert(encodedCheckpoint->contentType_ == "application/vnd.langgraph-cpp.checkpoint+json");

    auto decodedCheckpoint = checkpointCodec.decode(*encodedCheckpoint);
    assert(decodedCheckpoint.isOk());
    assert(decodedCheckpoint->threadId_ == checkpoint.threadId_);
    assert(decodedCheckpoint->checkpointId_ == checkpoint.checkpointId_);
    assert(decodedCheckpoint->parentCheckpointId_ == checkpoint.parentCheckpointId_);
    assert(decodedCheckpoint->step_ == checkpoint.step_);
    assert(decodedCheckpoint->state_ == checkpoint.state_);
    assert(decodedCheckpoint->nextNodes_ == checkpoint.nextNodes_);
    assert(decodedCheckpoint->nextTasks_ == checkpoint.nextTasks_);
    assert(decodedCheckpoint->writes_ == checkpoint.writes_);
    assert(decodedCheckpoint->pendingWrites_ == checkpoint.pendingWrites_);
    assert(decodedCheckpoint->channelVersions_ == checkpoint.channelVersions_);
    assert(decodedCheckpoint->versionsSeen_ == checkpoint.versionsSeen_);
    assert(decodedCheckpoint->updatedChannels_ == checkpoint.updatedChannels_);
    assert(decodedCheckpoint->createdAt_ == createdAt);

    auto encodedWrite = checkpointCodec.encodeWrite(checkpoint.writes_.front());
    assert(encodedWrite.isOk());
    assert(encodedWrite->contentType_ == "application/vnd.langgraph-cpp.checkpoint-write+json");
    auto decodedWrite = checkpointCodec.decodeWrite(*encodedWrite);
    assert(decodedWrite.isOk());
    assert(*decodedWrite == checkpoint.writes_.front());

    auto invalidCheckpoint = checkpointCodec.decode(lgc::Payload {
        .contentType_ = "application/vnd.langgraph-cpp.checkpoint+json",
        .data_ = R"({"thread_id":"","checkpoint_id":"c1","step":1,"state":{}})",
    });
    assert(!invalidCheckpoint.isOk());
    assert(invalidCheckpoint.status().code() == lgc::StatusCode::InvalidArgument);

    auto wrongContentType = stateCodec.decode(lgc::Payload {
        .contentType_ = "text/plain",
        .data_ = "{}",
    });
    assert(!wrongContentType.isOk());
    assert(wrongContentType.status().code() == lgc::StatusCode::InvalidArgument);

    lgc::JsonDecodeLimits tinyLimits;
    tinyLimits.maxBytes_ = 8;
    const lgc::JsonStateCodec limitedStateCodec(tinyLimits);
    auto tooLargeState = limitedStateCodec.decode(lgc::Payload {
        .contentType_ = "application/json",
        .data_ = R"({"too":"large"})",
    });
    assert(!tooLargeState.isOk());
    assert(tooLargeState.status().code() == lgc::StatusCode::ResourceExhausted);

    auto unknownCheckpointField = checkpointCodec.decode(lgc::Payload {
        .contentType_ = "application/vnd.langgraph-cpp.checkpoint+json",
        .data_ = R"({"thread_id":"t","checkpoint_id":"c","step":1,"state":{},"extra":true})",
    });
    assert(!unknownCheckpointField.isOk());
    assert(unknownCheckpointField.status().code() == lgc::StatusCode::InvalidArgument);

    return 0;
}
