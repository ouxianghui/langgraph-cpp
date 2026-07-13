#pragma once

#include "langgraph/graph/compiled_state_graph.hpp"
#include "langgraph/graph/state_graph_types.hpp"

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string_view>
#include <utility>
#include <vector>

namespace lc {

/// Mutable graph builder.
///
/// StateGraph owns graph declaration only: nodes, edges, routers, schemas, and
/// subgraph/command route metadata. It is not an execution engine and is not
/// designed for concurrent mutation. Call compile() to validate the declaration
/// and produce an immutable CompiledStateGraph that can be invoked repeatedly.
class StateGraph final {
public:
    /// LangGraph-style builder API using C++ camelCase names.
    StateGraph& setNodeDefaults(NodeOptions options);
    StateGraph& setSchemas(StateSchemaOptions schemas);
    StateGraph& setInputSchema(JsonSchema schema);
    StateGraph& setStateSchema(JsonSchema schema);
    StateGraph& setOutputSchema(JsonSchema schema);

    [[nodiscard]] Result<void> addNode(NodeId id, NodeHandler handler);
    [[nodiscard]] Result<void> addNode(NodeId id, NodeHandler handler, NodeOptions options);
    [[nodiscard]] Result<void> addNode(NodeId id, NodeOutputHandler handler);
    [[nodiscard]] Result<void> addNode(NodeId id, NodeOutputHandler handler, NodeOptions options);
    [[nodiscard]] Result<void> addSubgraph(
        NodeId id,
        std::shared_ptr<CompiledStateGraph> graph,
        SubgraphOptions options = {});
    [[nodiscard]] Result<void> setNodeOptions(NodeId id, NodeOptions options);
    [[nodiscard]] Result<void> addEdge(NodeId source, NodeId target);
    [[nodiscard]] Result<void> addEdge(std::vector<NodeId> sources, NodeId target);
    [[nodiscard]] Result<void> addSequence(
        std::vector<std::pair<NodeId, NodeOutputHandler>> nodes);
    [[nodiscard]] Result<void> addSequence(
        std::vector<std::pair<NodeId, NodeHandler>> nodes);
    [[nodiscard]] Result<void> setEntryPoint(NodeId target);
    [[nodiscard]] Result<void> setFinishPoint(NodeId source);
    [[nodiscard]] Result<void> setConditionalEntryPoint(
        RouterHandler router,
        std::vector<NodeId> destinations = {});
    [[nodiscard]] Result<void> setConditionalEntryPoint(
        MultiRouterHandler router,
        std::vector<NodeId> destinations = {});
    [[nodiscard]] Result<void> setConditionalEntryPoint(
        SendRouterHandler router,
        std::vector<NodeId> destinations = {});
    [[nodiscard]] Result<void> addConditionalEdges(
        NodeId source,
        RouterHandler router,
        std::vector<NodeId> destinations = {});
    [[nodiscard]] Result<void> addConditionalEdges(
        NodeId source,
        MultiRouterHandler router,
        std::vector<NodeId> destinations = {});
    [[nodiscard]] Result<void> addConditionalEdges(
        NodeId source,
        SendRouterHandler router,
        std::vector<NodeId> destinations = {});
    [[nodiscard]] Result<void> addCommandRoute(
        NodeId source,
        std::vector<NodeId> destinations = {});

    [[nodiscard]] Result<void> validate() const;
    /// Validate the graph and return an immutable executable graph.
    [[nodiscard]] Result<CompiledStateGraph> compile() const;

private:
    friend class CompiledStateGraph;

    struct RouteTarget {
        NodeId node_;
        std::optional<State> arg_;
    };
    using RouteHandler = std::function<Result<std::vector<RouteTarget>>(const State&, Runtime&)>;

    struct NodeSpec {
        NodeOutputHandler handler_;
        NodeOptions options_;
    };

    [[nodiscard]] Result<void> registerRoute(
        NodeId source,
        RouteHandler router,
        std::vector<NodeId> destinations);
    [[nodiscard]] bool hasNodeOrEnd(std::string_view id) const;

    std::map<NodeId, NodeSpec> nodes_;
    std::map<NodeId, std::vector<NodeId>> edges_;
    std::map<NodeId, RouteHandler> routers_;
    std::map<NodeId, std::set<NodeId>> routerDestinations_;
    std::map<NodeId, std::set<NodeId>> commandDestinations_;
    NodeOptions nodeDefaults_;
    StateSchemaOptions schemas_;
};

} // namespace lc
