#include "langgraph/graph/state_graph.hpp"

#include "langgraph/graph/state_graph_common.hh"

#include "foundation/id/id_generator.hpp"
#include "langgraph/graph/graph_namespace.hh"
#include "langgraph/graph/stream_projection.hh"
#include "langgraph/graph/stream_state.hh"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <exception>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <utility>
#include <vector>

namespace lgc {
namespace {

constexpr char kCheckpointNamespaceSeparator = graph_detail::kCheckpointNamespaceSeparator;
constexpr char kCheckpointNamespaceTaskSeparator = graph_detail::kCheckpointNamespaceTaskSeparator;

[[nodiscard]] std::string joinCheckpointNamespace(std::string parent, std::string child)
{
    if (child.empty())
        child = "subgraph";
    if (parent.empty())
        return child;
    parent.push_back(kCheckpointNamespaceSeparator);
    parent.append(child);
    return parent;
}

[[nodiscard]] std::string checkpointNamespaceWithTask(std::string nameSpace, std::string_view taskId)
{
    if (taskId.empty())
        return nameSpace;
    nameSpace.push_back(kCheckpointNamespaceTaskSeparator);
    nameSpace.append(taskId);
    return nameSpace;
}

[[nodiscard]] Result<void> validateTargetNodeId(std::string_view id)
{
    if (id.empty())
        return Status::invalidArgument("target node id cannot be empty");
    if (id == START)
        return Status::invalidArgument("edge target cannot be START");
    return okResult();
}

[[nodiscard]] Result<StateUpdate> stateDiffUpdate(const State& before, const State& after)
{
    nlohmann::json diff = nlohmann::json::object();
    const auto& beforeJson = before.view();
    const auto& afterJson = after.view();
    for (auto it = afterJson.begin(); it != afterJson.end(); ++it) {
        const auto found = beforeJson.find(it.key());
        if (found == beforeJson.end() || *found != it.value())
            diff[it.key()] = it.value();
    }
    for (auto it = beforeJson.begin(); it != beforeJson.end(); ++it) {
        if (!afterJson.contains(it.key()))
            diff[it.key()] = nullptr;
    }
    return StateUpdate::fromJsonValue(diff);
}

[[nodiscard]] StatusCode statusCodeFromName(std::string_view name) noexcept
{
    for (const auto code : {
             StatusCode::Cancelled,
             StatusCode::Unknown,
             StatusCode::InvalidArgument,
             StatusCode::DeadlineExceeded,
             StatusCode::NotFound,
             StatusCode::AlreadyExists,
             StatusCode::PermissionDenied,
             StatusCode::ResourceExhausted,
             StatusCode::FailedPrecondition,
             StatusCode::Aborted,
             StatusCode::OutOfRange,
             StatusCode::Unimplemented,
             StatusCode::Internal,
             StatusCode::Unavailable,
             StatusCode::DataLoss,
             StatusCode::Unauthenticated,
         }) {
        if (statusCodeName(code) == name)
            return code;
    }
    return StatusCode::Unknown;
}

[[nodiscard]] Status statusFromFailedSubgraphRun(const RunResult& child)
{
    std::string message = "subgraph run failed";
    std::string codeName;
    const auto& view = child.state_.view();
    if (view.contains("__run_error__") && view.at("__run_error__").is_object()) {
        const auto& error = view.at("__run_error__");
        if (error.contains("message") && error.at("message").is_string())
            message = error.at("message").get<std::string>();
        if (error.contains("code") && error.at("code").is_string())
            codeName = error.at("code").get<std::string>();
    }

    if (child.status_ == RunStatus::Cancelled)
        return Status::cancelled(std::move(message));
    if (child.status_ == RunStatus::MaxStepsExceeded)
        return Status::resourceExhausted(message.empty() ? "subgraph exceeded max steps" : std::move(message));

    if (!codeName.empty())
        return Status(statusCodeFromName(codeName), std::move(message));
    return Status::failedPrecondition(std::move(message));
}

[[nodiscard]] std::set<NodeId> reachableFrom(
    const std::map<NodeId, std::vector<NodeId>>& adjacency,
    const NodeId& start)
{
    std::set<NodeId> visited;
    std::vector<NodeId> stack { start };
    while (!stack.empty()) {
        auto node = std::move(stack.back());
        stack.pop_back();
        if (!visited.insert(node).second)
            continue;

        const auto edges = adjacency.find(node);
        if (edges == adjacency.end())
            continue;
        for (const auto& target : edges->second) {
            if (!visited.contains(target))
                stack.push_back(target);
        }
    }
    return visited;
}

} // namespace

NodeOutput NodeOutput::update(StateUpdate update)
{
    return NodeOutput {
        .update_ = std::move(update),
    };
}

NodeOutput NodeOutput::command(Command command)
{
    return NodeOutput {
        .update_ = command.update_,
        .command_ = std::move(command),
    };
}

NodeOutput NodeOutput::interrupt(
    Interrupt interrupt,
    StateUpdate update)
{
    return NodeOutput {
        .update_ = std::move(update),
        .interrupt_ = std::move(interrupt),
    };
}

Send::Send(NodeId node, State arg)
    : node_(std::move(node))
    , arg_(std::move(arg))
{
}

bool StateGraph::hasNodeOrEnd(std::string_view id) const
{
    return id == END || nodes_.contains(std::string(id));
}

StateGraph& StateGraph::setNodeDefaults(NodeOptions options)
{
    nodeDefaults_ = std::move(options);
    return *this;
}

StateGraph& StateGraph::setSchemas(StateSchemaOptions schemas)
{
    schemas_ = std::move(schemas);
    return *this;
}

StateGraph& StateGraph::setInputSchema(JsonSchema schema)
{
    schemas_.inputSchema_ = std::move(schema);
    return *this;
}

StateGraph& StateGraph::setStateSchema(JsonSchema schema)
{
    schemas_.stateSchema_ = std::move(schema);
    return *this;
}

StateGraph& StateGraph::setOutputSchema(JsonSchema schema)
{
    schemas_.outputSchema_ = std::move(schema);
    return *this;
}

Result<void> StateGraph::addNode(NodeId id, NodeHandler handler)
{
    return addNode(std::move(id), std::move(handler), nodeDefaults_);
}

Result<void> StateGraph::addNode(NodeId id, NodeHandler handler, NodeOptions options)
{
    if (!handler)
        return Status::invalidArgument("node handler cannot be empty");

    return addNode(
        std::move(id),
        [handler = std::move(handler)](const State& state, Runtime& context) -> Result<NodeOutput> {
            auto update = handler(state, context);
            if (!update.isOk())
                return update.status();
            return NodeOutput::update(std::move(*update));
        },
        std::move(options));
}

Result<void> StateGraph::addNode(NodeId id, NodeOutputHandler handler)
{
    return addNode(std::move(id), std::move(handler), nodeDefaults_);
}

Result<void> StateGraph::addNode(NodeId id, NodeOutputHandler handler, NodeOptions options)
{
    if (auto result = graph_detail::validateUserNodeId(id); !result.isOk())
        return result.status();
    if (!handler)
        return Status::invalidArgument("node handler cannot be empty");
    if (nodes_.contains(id))
        return Status::alreadyExists("node already exists: " + id);

    nodes_.emplace(std::move(id), NodeSpec {
                                      .handler_ = std::move(handler),
                                      .options_ = std::move(options),
                                  });
    return okResult();
}

Result<void> StateGraph::addSubgraph(
    NodeId id,
    std::shared_ptr<CompiledStateGraph> graph,
    SubgraphOptions options)
{
    if (!graph)
        return Status::invalidArgument("subgraph cannot be null");

    return addNode(
        std::move(id),
        makeSubgraphNodeHandler(std::move(graph), std::move(options)));
}

NodeOutputHandler StateGraph::makeSubgraphNodeHandler(
    std::shared_ptr<CompiledStateGraph> graph,
    SubgraphOptions options)
{
    return [graph = std::move(graph), options = std::move(options)](
               const State& state,
               Runtime& context) -> Result<NodeOutput> {
        RunOptions childOptions = options.options_;

        const std::string parentNamespace(context.executionInfo().checkpointNamespace_);
        std::string baseNamespace = options.checkpointNamespace_.empty()
            ? std::string(context.executionInfo().nodeId_)
            : options.checkpointNamespace_;
        if (options.persistence_ == SubgraphPersistenceMode::PerInvocation) {
            std::string taskId = std::string(context.executionInfo().runId_);
            taskId.push_back('-');
            taskId.append(std::to_string(context.executionInfo().step_));
            baseNamespace = checkpointNamespaceWithTask(std::move(baseNamespace), taskId);
        }
        std::string childNamespace = joinCheckpointNamespace(parentNamespace, baseNamespace);
        childOptions.checkpointNamespace_ = childNamespace;

        if (childOptions.threadId_.empty() && options.inheritThreadId_) {
            childOptions.threadId_ = std::string(context.executionInfo().threadId_);
            childOptions.threadId_.append("/");
            childOptions.threadId_.append(std::string(context.executionInfo().nodeId_));
            if (!options.threadIdSuffix_.empty()) {
                childOptions.threadId_.append("/");
                childOptions.threadId_.append(options.threadIdSuffix_);
            }
            if (options.persistence_ == SubgraphPersistenceMode::PerInvocation) {
                childOptions.threadId_.append("/");
                childOptions.threadId_.append(std::string(context.executionInfo().runId_));
                childOptions.threadId_.append("-");
                childOptions.threadId_.append(std::to_string(context.executionInfo().step_));
            }
        }
        if (!childOptions.store_)
            childOptions.store_ = context.store();
        if (options.persistence_ == SubgraphPersistenceMode::Stateless) {
            childOptions.checkpointer_.reset();
        } else if (!childOptions.checkpointer_) {
            childOptions.checkpointer_ = context.checkpointer();
        }
        if (context.hasResumeValue()) {
            nlohmann::json childResumeValue = context.resumeValue();
            const std::string subgraphInterruptId =
                std::string("subgraph:") + std::string(context.executionInfo().nodeId_);
            if (childResumeValue.is_object() && childResumeValue.contains(subgraphInterruptId))
                childResumeValue = childResumeValue.at(subgraphInterruptId);
            childOptions.command_ = Command::resume(std::move(childResumeValue));
        }

        auto childCallback = std::move(childOptions.eventCallback_);
        childOptions.eventCallback_ =
            [&context, childCallback = std::move(childCallback), childNamespace](
                RuntimeEvent event) mutable -> Status {
            if (childCallback) {
                if (auto status = childCallback(event); !status.isOk())
                    return status;
            }
            if (event.payload_.is_object()) {
                if (!event.payload_.contains("ns"))
                    event.payload_["ns"] = childNamespace;
                if (!event.payload_.contains("checkpoint_ns"))
                    event.payload_["checkpoint_ns"] = childNamespace;
                if (!event.payload_.contains("parent_run_id"))
                    event.payload_["parent_run_id"] = std::string(context.executionInfo().runId_);
                if (!event.payload_.contains("parent_thread_id"))
                    event.payload_["parent_thread_id"] =
                        std::string(context.executionInfo().threadId_);
                if (!event.payload_.contains("parent_node"))
                    event.payload_["parent_node"] = std::string(context.executionInfo().nodeId_);

                nlohmann::json parentIds =
                    nlohmann::json::array({ std::string(context.executionInfo().runId_) });
                if (event.payload_.contains("parent_ids") && event.payload_.at("parent_ids").is_array()) {
                    for (const auto& parent : event.payload_.at("parent_ids")) {
                        if (parent.is_string()
                            && parent.get<std::string>() != std::string(context.executionInfo().runId_))
                            parentIds.push_back(parent);
                    }
                } else if (event.payload_.contains("parent_run_id")
                    && event.payload_.at("parent_run_id").is_string()
                    && event.payload_.at("parent_run_id").get<std::string>()
                        != std::string(context.executionInfo().runId_)) {
                    parentIds.push_back(event.payload_.at("parent_run_id"));
                }
                event.payload_["parent_ids"] = std::move(parentIds);

                std::vector<std::string> tracePath;
                auto appendTraceSegment = [&](std::string segment) {
                    if (segment.empty())
                        return;
                    if (std::ranges::find(tracePath, segment) == tracePath.end())
                        tracePath.push_back(std::move(segment));
                };
                appendTraceSegment(std::string(context.executionInfo().checkpointNamespace_));
                appendTraceSegment(childNamespace);
                if (event.payload_.contains("trace_path") && event.payload_.at("trace_path").is_array()) {
                    for (const auto& segment : event.payload_.at("trace_path")) {
                        if (segment.is_string())
                            appendTraceSegment(segment.get<std::string>());
                    }
                }
                nlohmann::json traceJson = nlohmann::json::array();
                for (const auto& segment : tracePath)
                    traceJson.push_back(segment);
                event.payload_["trace_path"] = traceJson;
                const std::string effectiveNamespace = event.payload_.contains("checkpoint_ns")
                        && event.payload_.at("checkpoint_ns").is_string()
                    ? event.payload_.at("checkpoint_ns").get<std::string>()
                    : childNamespace;
                event.payload_["namespace"] = detail::namespacePathFromString(effectiveNamespace);
            }
            return context.streamWriter().publish(std::move(event));
        };

        auto child = graph->invokeSubgraph(state, std::move(childOptions));
        if (!child.isOk())
            return child.status();

        if (child->status_ == RunStatus::Failed
            || child->status_ == RunStatus::Cancelled
            || child->status_ == RunStatus::MaxStepsExceeded) {
            return statusFromFailedSubgraphRun(*child);
        }

        auto diff = stateDiffUpdate(state, child->state_);
        if (!diff.isOk())
            return diff.status();

        if (child->status_ == RunStatus::Paused) {
            return NodeOutput::interrupt(
                Interrupt {
                    .id_ = std::string("subgraph:") + std::string(context.executionInfo().nodeId_),
                    .value_ = child->state_.view(),
                },
                std::move(*diff));
        }

        if (child->parentCommand_.has_value()) {
            return NodeOutput::command(Command::gotoNodes(
                child->parentCommand_->goto_,
                std::move(*diff)));
        }

        return NodeOutput::update(std::move(*diff));
    };
}

Result<void> StateGraph::setNodeOptions(NodeId id, NodeOptions options)
{
    if (auto result = graph_detail::validateUserNodeId(id); !result.isOk())
        return result.status();
    auto found = nodes_.find(id);
    if (found == nodes_.end())
        return Status::notFound("node not found: " + id);
    found->second.options_ = std::move(options);
    return okResult();
}

Result<void> StateGraph::addEdge(NodeId source, NodeId target)
{
    if (source.empty())
        return Status::invalidArgument("edge source cannot be empty");
    if (source == END)
        return Status::invalidArgument("edge source cannot be END");
    if (source != START) {
        if (auto result = graph_detail::validateUserNodeId(source); !result.isOk())
            return result.status();
    }
    if (auto result = validateTargetNodeId(target); !result.isOk())
        return result.status();

    auto& targets = edges_[std::move(source)];
    if (std::ranges::find(targets, target) != targets.end())
        return Status::alreadyExists("edge already exists");
    targets.push_back(std::move(target));
    return okResult();
}

Result<void> StateGraph::addEdge(std::vector<NodeId> sources, NodeId target)
{
    if (sources.empty())
        return Status::invalidArgument("edge sources cannot be empty");

    StateGraph candidate(*this);
    for (auto& source : sources) {
        auto added = candidate.addEdge(std::move(source), target);
        if (!added.isOk())
            return added.status();
    }
    *this = std::move(candidate);
    return okResult();
}

Result<void> StateGraph::addSequence(std::vector<std::pair<NodeId, NodeOutputHandler>> nodes)
{
    if (nodes.empty())
        return Status::invalidArgument("sequence requires at least one node");

    StateGraph candidate(*this);
    std::optional<NodeId> previous;
    for (auto& [id, handler] : nodes) {
        NodeId current = id;
        auto added = candidate.addNode(std::move(id), std::move(handler));
        if (!added.isOk())
            return added.status();
        if (previous.has_value()) {
            auto edge = candidate.addEdge(std::move(*previous), current);
            if (!edge.isOk())
                return edge.status();
        }
        previous = std::move(current);
    }
    *this = std::move(candidate);
    return okResult();
}

Result<void> StateGraph::addSequence(std::vector<std::pair<NodeId, NodeHandler>> nodes)
{
    if (nodes.empty())
        return Status::invalidArgument("sequence requires at least one node");

    std::vector<std::pair<NodeId, NodeOutputHandler>> outputNodes;
    outputNodes.reserve(nodes.size());
    for (auto& [id, handler] : nodes) {
        if (!handler)
            return Status::invalidArgument("node handler cannot be empty");
        outputNodes.emplace_back(
            std::move(id),
            [handler = std::move(handler)](const State& state, Runtime& context) mutable -> Result<NodeOutput> {
                auto update = handler(state, context);
                if (!update.isOk())
                    return update.status();
                return NodeOutput::update(std::move(*update));
            });
    }
    return addSequence(std::move(outputNodes));
}

Result<void> StateGraph::setEntryPoint(NodeId target)
{
    return addEdge(std::string(START), std::move(target));
}

Result<void> StateGraph::setFinishPoint(NodeId source)
{
    return addEdge(std::move(source), std::string(END));
}

Result<void> StateGraph::setConditionalEntryPoint(
    RouterHandler router,
    std::vector<NodeId> destinations)
{
    return addConditionalEdges(std::string(START), std::move(router), std::move(destinations));
}

Result<void> StateGraph::setConditionalEntryPoint(
    MultiRouterHandler router,
    std::vector<NodeId> destinations)
{
    return addConditionalEdges(std::string(START), std::move(router), std::move(destinations));
}

Result<void> StateGraph::setConditionalEntryPoint(
    SendRouterHandler router,
    std::vector<NodeId> destinations)
{
    return addConditionalEdges(std::string(START), std::move(router), std::move(destinations));
}

Result<void> StateGraph::addConditionalEdges(
    NodeId source,
    RouterHandler router,
    std::vector<NodeId> destinations)
{
    if (!router)
        return Status::invalidArgument("router handler cannot be empty");

    RouteHandler routeHandler = [router = std::move(router)](
                                    const State& state,
                                    Runtime& context) -> Result<std::vector<RouteTarget>> {
        auto routed = router(state, context);
        if (!routed.isOk())
            return routed.status();
        return std::vector<RouteTarget> {
            RouteTarget {
                .node_ = std::move(*routed),
            },
        };
    };
    return registerRoute(std::move(source), std::move(routeHandler), std::move(destinations));
}

Result<void> StateGraph::addConditionalEdges(
    NodeId source,
    MultiRouterHandler router,
    std::vector<NodeId> destinations)
{
    if (!router)
        return Status::invalidArgument("router handler cannot be empty");

    RouteHandler routeHandler = [router = std::move(router)](
                                    const State& state,
                                    Runtime& context) -> Result<std::vector<RouteTarget>> {
        auto routed = router(state, context);
        if (!routed.isOk())
            return routed.status();
        std::vector<RouteTarget> targets;
        targets.reserve(routed->size());
        for (auto& node : *routed) {
            targets.push_back(RouteTarget {
                .node_ = std::move(node),
            });
        }
        return targets;
    };
    return registerRoute(std::move(source), std::move(routeHandler), std::move(destinations));
}

Result<void> StateGraph::addConditionalEdges(
    NodeId source,
    SendRouterHandler router,
    std::vector<NodeId> destinations)
{
    if (!router)
        return Status::invalidArgument("router handler cannot be empty");

    RouteHandler routeHandler = [router = std::move(router)](
                                    const State& state,
                                    Runtime& context) -> Result<std::vector<RouteTarget>> {
        auto routed = router(state, context);
        if (!routed.isOk())
            return routed.status();
        std::vector<RouteTarget> targets;
        targets.reserve(routed->size());
        for (auto& send : *routed) {
            targets.push_back(RouteTarget {
                .node_ = std::move(send.node_),
                .arg_ = std::move(send.arg_),
            });
        }
        return targets;
    };
    return registerRoute(std::move(source), std::move(routeHandler), std::move(destinations));
}

Result<void> StateGraph::addCommandRoute(
    NodeId source,
    std::vector<NodeId> destinations)
{
    if (auto result = graph_detail::validateUserNodeId(source); !result.isOk())
        return result.status();
    if (commandDestinations_.contains(source))
        return Status::alreadyExists("command destinations already exist: " + source);

    std::set<NodeId> destinationSet;
    for (auto& destination : destinations) {
        if (auto result = validateTargetNodeId(destination); !result.isOk())
            return result.status();
        destinationSet.insert(std::move(destination));
    }

    commandDestinations_.emplace(std::move(source), std::move(destinationSet));
    return okResult();
}

Result<void> StateGraph::registerRoute(
    NodeId source,
    RouteHandler router,
    std::vector<NodeId> destinations)
{
    if (source.empty())
        return Status::invalidArgument("router source cannot be empty");
    if (source != START) {
        if (auto result = graph_detail::validateUserNodeId(source); !result.isOk())
            return result.status();
    }
    if (!router)
        return Status::invalidArgument("router handler cannot be empty");
    if (routers_.contains(source))
        return Status::alreadyExists("conditional edges already exist: " + source);

    std::set<NodeId> destinationSet;
    for (auto& destination : destinations) {
        if (auto result = validateTargetNodeId(destination); !result.isOk())
            return result.status();
        destinationSet.insert(std::move(destination));
    }

    routers_.emplace(source, std::move(router));
    routerDestinations_.emplace(std::move(source), std::move(destinationSet));
    return okResult();
}

Result<void> StateGraph::validate() const
{
    const auto start = edges_.find(std::string(START));
    if ((start == edges_.end() || start->second.empty())
        && !routers_.contains(std::string(START)))
        return Status::failedPrecondition("graph must have an edge from START");

    std::map<NodeId, std::vector<NodeId>> adjacency;
    adjacency.emplace(std::string(START), std::vector<NodeId> {});
    adjacency.emplace(std::string(END), std::vector<NodeId> {});
    for (const auto& [node, handler] : nodes_) {
        (void)handler;
        adjacency.emplace(node, std::vector<NodeId> {});
    }
    auto openRouteTargets = [&] {
        std::vector<NodeId> targets;
        targets.reserve(nodes_.size() + 1U);
        for (const auto& [node, handler] : nodes_) {
            (void)handler;
            targets.push_back(node);
        }
        targets.push_back(std::string(END));
        return targets;
    };

    for (const auto& [source, targets] : edges_) {
        if (source != START && !nodes_.contains(source))
            return Status::failedPrecondition("edge source is unknown: " + source);
        if (routers_.contains(source))
            return Status::failedPrecondition("node cannot have both normal and conditional edges: " + source);
        if (commandDestinations_.contains(source))
            return Status::failedPrecondition("node cannot have both normal and command routing edges: " + source);
        for (const auto& target : targets) {
            if (!hasNodeOrEnd(target))
                return Status::failedPrecondition("edge target is unknown: " + target);
        }
        adjacency[source] = targets;
    }

    for (const auto& [source, router] : routers_) {
        (void)router;
        if (source != START && !nodes_.contains(source))
            return Status::failedPrecondition("router source is unknown: " + source);
        if (commandDestinations_.contains(source))
            return Status::failedPrecondition("node cannot have both conditional and command routing edges: " + source);
        const auto destinations = routerDestinations_.find(source);
        if (destinations == routerDestinations_.end() || destinations->second.empty()) {
            adjacency[source] = openRouteTargets();
            continue;
        }
        for (const auto& target : destinations->second) {
            if (!hasNodeOrEnd(target))
                return Status::failedPrecondition("router target is unknown: " + target);
            adjacency[source].push_back(target);
        }
    }

    for (const auto& [source, destinations] : commandDestinations_) {
        if (!nodes_.contains(source))
            return Status::failedPrecondition("command source is unknown: " + source);
        if (destinations.empty()) {
            adjacency[source] = openRouteTargets();
            continue;
        }
        for (const auto& target : destinations) {
            if (!hasNodeOrEnd(target))
                return Status::failedPrecondition("command target is unknown: " + target);
            adjacency[source].push_back(target);
        }
    }

    for (const auto& [node, handler] : nodes_) {
        (void)handler;
        if (!edges_.contains(node) && !routers_.contains(node) && !commandDestinations_.contains(node))
            adjacency[node].push_back(std::string(END));
    }

    const auto reachable = reachableFrom(adjacency, std::string(START));
    if (!reachable.contains(std::string(END)))
        return Status::failedPrecondition("END is not reachable from START");
    for (const auto& [node, handler] : nodes_) {
        (void)handler;
        if (!reachable.contains(node))
            return Status::failedPrecondition("node is not reachable from START: " + node);
    }

    std::map<NodeId, std::vector<NodeId>> reverseAdjacency;
    for (const auto& [node, targets] : adjacency) {
        reverseAdjacency.emplace(node, std::vector<NodeId> {});
        for (const auto& target : targets)
            reverseAdjacency[target].push_back(node);
    }
    const auto canReachEnd = reachableFrom(reverseAdjacency, std::string(END));
    for (const auto& node : reachable) {
        if (node != END && !canReachEnd.contains(node))
            return Status::failedPrecondition("reachable node cannot reach END: " + node);
    }

    return okResult();
}

Result<CompiledStateGraph> StateGraph::compile() const
{
    auto valid = validate();
    if (!valid.isOk())
        return valid.status();
    return CompiledStateGraph(*this);
}


} // namespace lgc
