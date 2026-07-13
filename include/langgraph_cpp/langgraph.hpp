#pragma once

/// Public aggregate header for the pre-1.0 source API.
///
/// This header intentionally exposes the embeddable runtime surface rather than
/// implementation internals. The source API is governed by docs/API_CONTRACT.md;
/// ABI and private layout are not frozen before 1.0.

#include "foundation/cancellation/cancellation_token.hpp"
#include "foundation/executor/concurrent_executor.hpp"
#include "foundation/executor/inline_executor.hpp"
#include "foundation/event/runtime_event.hpp"
#include "foundation/json/json_schema.hpp"
#include "foundation/resource/resource_limits.hpp"
#include "foundation/serialization/state_codec.hpp"
#include "foundation/status/result.hpp"
#include "foundation/status/status.hpp"
#include "foundation/storage/i_storage.hpp"
#include "foundation/storage/memory_storage.hpp"
#include "foundation/versioning/versioning.hpp"
#include "langgraph/checkpoint/checkpointer.hpp"
#include "langgraph/core/ids.hpp"
#include "langgraph/edge/hardware.hpp"
#include "langgraph/graph/state_graph.hpp"
#include "langgraph/message/message.hpp"
#include "langgraph/model/chat_model.hpp"
#include "langgraph/model/provider_chat_model.hpp"
#if LANGGRAPH_CPP_WITH_LLAMA_CPP
#include "langgraph/model/llamacpp_chat_model.hpp"
#endif
#include "langgraph/runtime/runtime.hpp"
#include "langgraph/state/reducer.hpp"
#include "langgraph/state/state_update.hpp"
#include "langgraph/store/store.hpp"
#include "langgraph/tool/tool_call_grammar.hpp"
#include "langgraph/tool/tool.hpp"
