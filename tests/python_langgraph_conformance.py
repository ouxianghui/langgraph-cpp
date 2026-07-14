#!/usr/bin/env python3
"""Optional behavior checks against the upstream Python LangGraph package.

The default C++ build does not require Python. CMake only wires this script into
CTest when LANGGRAPH_CPP_ENABLE_PYTHON_LANGGRAPH_CONFORMANCE=ON.
"""

from __future__ import annotations

import argparse
import dataclasses
import json
import operator
import subprocess
import sys
from typing import Annotated, Any, TypedDict

EXPECTED_SCENARIOS = {
    "history_snapshot",
    "runnable_config",
    "command_goto",
    "interrupt_replay",
    "multi_interrupt",
    "sequential_interrupt",
    "send_map_reduce",
    "subgraph_boundary",
    "stream_envelope",
    "stream_projection",
    "stream_projection_v2",
    "stream_interrupt_error",
    "tool_returned_command",
    "contract",
}


def die(message: str) -> None:
    print(f"FAIL: {message}", file=sys.stderr)
    raise SystemExit(1)


def run_cpp_probe(path: str) -> dict[str, Any]:
    completed = subprocess.run(
        [path],
        check=False,
        capture_output=True,
        text=True,
    )
    if completed.returncode != 0:
        die(
            "C++ conformance probe failed\n"
            f"stdout:\n{completed.stdout}\n"
            f"stderr:\n{completed.stderr}"
        )
    try:
        return json.loads(completed.stdout)
    except json.JSONDecodeError as error:
        die(f"C++ conformance probe did not emit JSON: {error}")


def import_langgraph() -> tuple[Any, Any, Any, Any, Any, Any]:
    try:
        from langgraph.graph import END, START, StateGraph
        from langgraph.types import Command, Send, interrupt
    except Exception as error:  # pragma: no cover - depends on local Python env
        die(
            "Python LangGraph is not importable or does not expose the expected "
            f"core types: {error}"
        )

    try:
        from langgraph.checkpoint.memory import MemorySaver
    except Exception:
        try:
            from langgraph.checkpoint.memory import InMemorySaver as MemorySaver
        except Exception as error:  # pragma: no cover - depends on local package version
            die(f"Python LangGraph memory checkpointer is not importable: {error}")

    return StateGraph, START, END, Command, Send, interrupt, MemorySaver


def to_jsonable(value: Any) -> Any:
    if value is None or isinstance(value, (str, int, float, bool)):
        return value
    if isinstance(value, dict):
        return {str(k): to_jsonable(v) for k, v in value.items()}
    if isinstance(value, (list, tuple)):
        return [to_jsonable(v) for v in value]
    if dataclasses.is_dataclass(value):
        return to_jsonable(dataclasses.asdict(value))
    if hasattr(value, "_asdict"):
        return to_jsonable(value._asdict())
    return str(value)


def snapshot_to_json(snapshot: Any) -> dict[str, Any]:
    tasks = []
    for task in getattr(snapshot, "tasks", []) or []:
        tasks.append(
            {
                "id": to_jsonable(getattr(task, "id", "")),
                "name": to_jsonable(getattr(task, "name", "")),
                "checkpoint_ns": to_jsonable(getattr(task, "checkpoint_ns", "")),
                "error": to_jsonable(getattr(task, "error", None)),
                "interrupts": to_jsonable(getattr(task, "interrupts", [])),
            }
        )
    return {
        "values": to_jsonable(getattr(snapshot, "values", {})),
        "next": to_jsonable(getattr(snapshot, "next", [])),
        "tasks": tasks,
        "writes": to_jsonable(getattr(snapshot, "writes", [])),
        "pending_writes": to_jsonable(getattr(snapshot, "pending_writes", [])),
        "config": to_jsonable(getattr(snapshot, "config", {})),
        "parent_config": to_jsonable(getattr(snapshot, "parent_config", None)),
        "metadata": to_jsonable(getattr(snapshot, "metadata", {})),
        "created_at": to_jsonable(getattr(snapshot, "created_at", None)),
    }


def require_scenarios(cpp: dict[str, Any]) -> None:
    actual = set(cpp.keys())
    missing = sorted(EXPECTED_SCENARIOS - actual)
    unexpected = sorted(actual - EXPECTED_SCENARIOS)
    if missing or unexpected:
        die(f"Scenario set mismatch: missing={missing} unexpected={unexpected}")
    print("PASS: scenario_set.strict")


def history_count_sequence(history: list[dict[str, Any]], field: str) -> list[Any]:
    values_in_order = []
    for snapshot in history:
        values = snapshot.get("values") or {}
        if field not in values:
            continue
        values_in_order.append(values[field])
    return values_in_order


def run_python_history_snapshot(StateGraph: Any, START: Any, END: Any, MemorySaver: Any) -> dict[str, Any]:
    class State(TypedDict, total=False):
        count: int

    def tick(state: State) -> dict[str, int]:
        return {"count": state.get("count", 0) + 1}

    builder = StateGraph(State)
    builder.add_node("tick", tick)
    builder.add_edge(START, "tick")
    builder.add_edge("tick", END)

    graph = builder.compile(checkpointer=MemorySaver())
    config = {"configurable": {"thread_id": "conformance-history"}}
    output = graph.invoke({"count": 0}, config)
    history = [snapshot_to_json(snapshot) for snapshot in graph.get_state_history(config)]
    return {"output": to_jsonable(output), "history": history}


def run_python_command_goto(StateGraph: Any, START: Any, END: Any, Command: Any) -> dict[str, Any]:
    class State(TypedDict, total=False):
        routed: bool
        finished: bool

    def decide(_: State) -> Any:
        return Command(update={"routed": True}, goto="finish")

    def finish(state: State) -> dict[str, bool]:
        if not state.get("routed"):
            raise AssertionError("command update was not visible to destination node")
        return {"finished": True}

    builder = StateGraph(State)
    try:
        builder.add_node("decide", decide, destinations=("finish",))
    except TypeError:
        builder.add_node("decide", decide)
    builder.add_node("finish", finish)
    builder.add_edge(START, "decide")
    builder.add_edge("finish", END)

    graph = builder.compile()
    return {"output": to_jsonable(graph.invoke({}))}


def run_python_interrupt_replay(
    StateGraph: Any,
    START: Any,
    END: Any,
    Command: Any,
    interrupt: Any,
    MemorySaver: Any,
) -> dict[str, Any]:
    class State(TypedDict, total=False):
        approved: bool

    def approve(_: State) -> dict[str, Any]:
        answer = interrupt({"question": "continue?"})
        if isinstance(answer, dict) and "approval" in answer:
            answer = answer["approval"]
        return {"approved": answer}

    builder = StateGraph(State)
    builder.add_node("approve", approve)
    builder.add_edge(START, "approve")
    builder.add_edge("approve", END)

    graph = builder.compile(checkpointer=MemorySaver())
    config = {"configurable": {"thread_id": "conformance-interrupt"}}
    paused = to_jsonable(graph.invoke({}, config))
    snapshot = graph.get_state(config)

    replayed = None
    checkpoint_id = None
    checkpoint_config = to_jsonable(getattr(snapshot, "config", {}))
    configurable = checkpoint_config.get("configurable", {}) if isinstance(checkpoint_config, dict) else {}
    if isinstance(configurable, dict):
        checkpoint_id = configurable.get("checkpoint_id")
    if checkpoint_id:
        replay_config = {"configurable": {"thread_id": "conformance-interrupt", "checkpoint_id": checkpoint_id}}
        try:
            replayed = to_jsonable(graph.invoke(None, replay_config))
        except Exception as error:  # pragma: no cover - version-dependent replay API
            replayed = {"error": str(error)}

    resumed = to_jsonable(graph.invoke(Command(resume={"approval": True}), config))
    return {
        "paused": paused,
        "replayed": replayed,
        "output": resumed,
    }


def run_python_multi_interrupt(
    StateGraph: Any,
    START: Any,
    END: Any,
    Command: Any,
    interrupt: Any,
    MemorySaver: Any,
) -> dict[str, Any]:
    class State(TypedDict, total=False):
        vals: Annotated[list[str], operator.add]

    def left(_: State) -> dict[str, list[str]]:
        answer = interrupt("left-question")
        return {"vals": [f"left:{answer}"]}

    def right(_: State) -> dict[str, list[str]]:
        answer = interrupt("right-question")
        return {"vals": [f"right:{answer}"]}

    builder = StateGraph(State)
    builder.add_node("left", left)
    builder.add_node("right", right)
    builder.add_edge(START, "left")
    builder.add_edge(START, "right")
    builder.add_edge("left", END)
    builder.add_edge("right", END)

    graph = builder.compile(checkpointer=MemorySaver())
    config = {"configurable": {"thread_id": "conformance-multi-interrupt"}}
    initial = graph.invoke({"vals": []}, config)
    interrupts = []
    if isinstance(initial, dict):
        interrupts = list(initial.get("__interrupt__", []) or [])
    else:
        interrupts = list(getattr(initial, "interrupts", []) or [])
    if len(interrupts) != 2:
        die(f"Python multi interrupt did not produce two interrupts: {to_jsonable(interrupts)}")

    resume_map: dict[str, str] = {}
    for item in interrupts:
        value = getattr(item, "value", None)
        item_id = getattr(item, "id", None)
        if not item_id:
            die(f"Python multi interrupt is missing interrupt id: {to_jsonable(item)}")
        if value == "left-question":
            resume_map[item_id] = "L"
        elif value == "right-question":
            resume_map[item_id] = "R"
        else:
            die(f"Unexpected Python multi interrupt value: {to_jsonable(item)}")

    resumed = graph.invoke(Command(resume=resume_map), config)
    return {
        "interrupts": to_jsonable(interrupts),
        "output": to_jsonable(getattr(resumed, "output", resumed)),
    }


def run_python_sequential_interrupt(
    StateGraph: Any,
    START: Any,
    END: Any,
    Command: Any,
    interrupt: Any,
    MemorySaver: Any,
) -> dict[str, Any]:
    class State(TypedDict, total=False):
        first: str
        second: str

    def ask(_: State) -> dict[str, str]:
        first = interrupt("first")
        second = interrupt("second")
        return {"first": first, "second": second}

    builder = StateGraph(State)
    builder.add_node("ask", ask)
    builder.add_edge(START, "ask")
    builder.add_edge("ask", END)

    graph = builder.compile(checkpointer=MemorySaver())
    config = {"configurable": {"thread_id": "conformance-sequential-interrupt"}}
    first_pause = to_jsonable(graph.invoke({}, config))
    second_pause = to_jsonable(graph.invoke(Command(resume="one"), config))
    output = to_jsonable(graph.invoke(Command(resume="two"), config))
    return {
        "first_pause": first_pause,
        "second_pause": second_pause,
        "output": output,
    }


def run_python_send_map_reduce(StateGraph: Any, START: Any, END: Any, Send: Any) -> dict[str, Any]:
    class State(TypedDict, total=False):
        items: Annotated[list[int], operator.add]
        item: int

    def fan(_: State) -> dict[str, list[int]]:
        return {}

    def worker(state: State) -> dict[str, list[int]]:
        return {"items": [state["item"]]}

    def fanout(_: Any) -> list[Any]:
        return [Send("worker", {"item": 1}), Send("worker", {"item": 2})]

    builder = StateGraph(State)
    builder.add_node("fan", fan)
    builder.add_node("worker", worker)
    builder.add_edge(START, "fan")
    builder.add_conditional_edges("fan", fanout, ["worker"])
    builder.add_edge("worker", END)

    graph = builder.compile()
    return {"output": to_jsonable(graph.invoke({}))}


def run_python_subgraph_boundary(StateGraph: Any, START: Any, END: Any, MemorySaver: Any) -> dict[str, Any]:
    class State(TypedDict, total=False):
        child: bool

    def child_node(_: State) -> dict[str, Any]:
        return {"child": True}

    child_builder = StateGraph(State)
    child_builder.add_node("child_node", child_node)
    child_builder.add_edge(START, "child_node")
    child_builder.add_edge("child_node", END)
    child_graph = child_builder.compile()

    parent_builder = StateGraph(State)
    parent_builder.add_node("child", child_graph)
    parent_builder.add_edge(START, "child")
    parent_builder.add_edge("child", END)
    parent_graph = parent_builder.compile(checkpointer=MemorySaver())
    config = {"configurable": {"thread_id": "conformance-subgraph", "checkpoint_ns": "root"}}
    return {"output": to_jsonable(parent_graph.invoke({}, config))}


def assert_true(name: str, condition: bool, details: str = "") -> None:
    if not condition:
        suffix = f": {details}" if details else ""
        die(f"{name}{suffix}")
    print(f"PASS: {name}")


def compare_history_snapshot(cpp: dict[str, Any], py: dict[str, Any]) -> None:
    assert_true(
        "history_snapshot.final_count",
        cpp["output"].get("count") == py["output"].get("count") == 1,
        f"cpp={cpp['output']} py={py['output']}",
    )
    cpp_sequence = history_count_sequence(cpp["history"], "count")
    py_sequence = history_count_sequence(py["history"], "count")
    assert_true(
        "history_snapshot.cpp_newest_first_order",
        cpp_sequence == [1, 0],
        f"cpp sequence={cpp_sequence}",
    )
    assert_true(
        "history_snapshot.python_newest_first_order",
        py_sequence[:2] == [1, 0],
        f"python sequence={py_sequence}",
    )
    required_cpp_fields = {"values", "next", "tasks", "writes", "pending_writes", "config", "parent_config", "metadata"}
    assert_true(
        "history_snapshot.cpp_snapshot_shape",
        required_cpp_fields.issubset(cpp["history"][0].keys()),
        f"fields={sorted(cpp['history'][0].keys())}",
    )
    required_py_fields = {"values", "next", "tasks", "writes", "pending_writes", "config", "parent_config", "metadata", "created_at"}
    assert_true(
        "history_snapshot.python_snapshot_shape",
        bool(py["history"]) and required_py_fields.issubset(py["history"][0].keys()),
        f"fields={sorted(py['history'][0].keys()) if py['history'] else []}",
    )
    assert_true(
        "history_snapshot.cpp_checkpoint_ns_format",
        (
            isinstance(cpp["history"][0].get("config", {}), dict)
            and isinstance(cpp["history"][0].get("config", {}).get("configurable", {}), dict)
            and cpp["history"][0]["config"]["configurable"].get("checkpoint_ns") == "root"
        ),
        f"config={cpp['history'][0].get('config')}",
    )
    first_py_config = py["history"][0].get("config", {}) if py["history"] else {}
    first_py_configurable = first_py_config.get("configurable", {}) if isinstance(first_py_config, dict) else {}
    assert_true(
        "history_snapshot.python_checkpoint_ns_present",
        isinstance(first_py_configurable, dict) and "checkpoint_ns" in first_py_configurable,
        f"config={first_py_config}",
    )


def compare_command_goto(cpp: dict[str, Any], py: dict[str, Any]) -> None:
    assert_true(
        "command_goto.final_state",
        cpp["output"].get("routed") is True
        and cpp["output"].get("finished") is True
        and py["output"].get("routed") is True
        and py["output"].get("finished") is True,
        f"cpp={cpp['output']} py={py['output']}",
    )


def compare_send_map_reduce(cpp: dict[str, Any], py: dict[str, Any]) -> None:
    cpp_items = sorted(cpp["output"].get("items", []))
    py_items = sorted(py["output"].get("items", []))
    assert_true(
        "send_map_reduce.items",
        cpp_items == py_items == [1, 2],
        f"cpp={cpp_items} py={py_items}",
    )


def compare_interrupt_replay(cpp: dict[str, Any], py: dict[str, Any]) -> None:
    assert_true(
        "interrupt_replay.cpp_pause_and_replay",
        cpp["paused_status"] == "paused"
        and cpp["pause_interrupt_id"] == "approval"
        and cpp["replay_status"] == "paused"
        and cpp["replay_interrupt_id"] == "approval",
        f"cpp={cpp}",
    )
    assert_true(
        "interrupt_replay.final_state",
        cpp["output"].get("approved") is True and py["output"].get("approved") is True,
        f"cpp={cpp['output']} py={py['output']}",
    )


def compare_multi_interrupt(cpp: dict[str, Any], py: dict[str, Any]) -> None:
    cpp_interrupts = cpp.get("interrupts", [])
    assert_true(
        "multi_interrupt.cpp_pause",
        cpp.get("paused_status") == "paused" and len(cpp_interrupts) == 2,
        f"cpp={cpp}",
    )
    cpp_ids = sorted(item.get("id") for item in cpp_interrupts)
    assert_true(
        "multi_interrupt.cpp_ids",
        cpp_ids == ["left-int", "right-int"],
        f"ids={cpp_ids}",
    )
    assert_true(
        "multi_interrupt.cpp_resume",
        cpp["output"].get("left") == "L" and cpp["output"].get("right") == "R",
        f"cpp={cpp['output']}",
    )
    py_vals = sorted(py["output"].get("vals", []))
    assert_true(
        "multi_interrupt.python_resume_map",
        py_vals == ["left:L", "right:R"],
        f"py={py['output']}",
    )


def compare_sequential_interrupt(cpp: dict[str, Any], py: dict[str, Any]) -> None:
    assert_true(
        "sequential_interrupt.cpp_order",
        cpp["first_status"] == "paused"
        and cpp["first_interrupt"].get("id") == "first"
        and cpp["second_status"] == "paused"
        and cpp["second_interrupt"].get("id") == "second"
        and cpp["replay_status"] == "paused"
        and cpp["replay_interrupt"].get("id") == "second",
        f"cpp={cpp}",
    )
    assert_true(
        "sequential_interrupt.cpp_resume_values",
        cpp["second_interrupt"].get("resume_values", {}).get("first") == "one",
        f"second_interrupt={cpp['second_interrupt']}",
    )
    assert_true(
        "sequential_interrupt.final_state",
        cpp["output"].get("first") == "one"
        and cpp["output"].get("second") == "two"
        and py["output"].get("first") == "one"
        and py["output"].get("second") == "two",
        f"cpp={cpp['output']} py={py['output']}",
    )


def compare_subgraph_boundary(cpp: dict[str, Any], py: dict[str, Any]) -> None:
    assert_true(
        "subgraph_boundary.final_state",
        cpp["output"].get("child") is True and py["output"].get("child") is True,
        f"cpp={cpp['output']} py={py['output']}",
    )
    assert_true(
        "subgraph_boundary.cpp_checkpoint_ns",
        cpp["output"].get("checkpoint_ns") == "root|child",
        f"cpp={cpp['output']}",
    )
    assert_true(
        "subgraph_boundary.cpp_child_checkpoint_namespace",
        cpp.get("child_history_size", 0) >= 2
        and cpp.get("child_thread_id") == "conformance-subgraph/child"
        and cpp.get("child_checkpoint_ns") == "root|child",
        f"cpp={cpp}",
    )


def compare_stream_envelope(cpp: dict[str, Any]) -> None:
    envelope = cpp.get("token_envelope") or {}
    assert_true(
        "stream_envelope.token_chunk",
        envelope.get("event") == "on_chat_model_stream"
        and envelope.get("data", {}).get("chunk", {}).get("type") == "AIMessageChunk"
        and envelope.get("data", {}).get("chunk", {}).get("content") == "hello",
        f"envelope={envelope}",
    )
    assert_true(
        "stream_envelope.required_fields",
        {"event", "name", "run_id", "parent_ids", "tags", "metadata", "data"}.issubset(envelope.keys()),
        f"fields={sorted(envelope.keys())}",
    )


def compare_stream_projection(cpp: dict[str, Any]) -> None:
    mode_counts = cpp.get("mode_counts", {})
    samples = cpp.get("samples", {})
    assert_true("stream_projection.completed", cpp.get("status") == "completed", f"cpp={cpp}")
    for mode in ("updates", "messages", "tasks", "checkpoints", "output"):
        assert_true(
            f"stream_projection.saw_{mode}",
            mode_counts.get(mode, 0) > 0,
            f"mode_counts={mode_counts}",
    )
    assert_true(
        "stream_projection.update_shape",
        "model" in samples.get("updates", {}) and "messages" in samples["updates"]["model"],
        f"samples={samples.get('updates')}",
    )
    assert_true(
        "stream_projection.message_shape",
        samples.get("messages", {}).get("text") == "hello"
        and samples.get("messages", {}).get("chunk", {}).get("type") == "AIMessageChunk"
        and samples.get("messages", {}).get("chunk", {}).get("content") == "hello"
        and isinstance(samples.get("messages", {}).get("metadata"), dict),
        f"samples={samples.get('messages')}",
    )
    assert_true(
        "stream_projection.checkpoint_shape",
        {"config", "parent_config", "values", "next", "tasks", "metadata"}.issubset(samples.get("checkpoints", {}).keys())
        and isinstance(samples.get("checkpoints", {}).get("config", {}).get("configurable", {}), dict)
        and isinstance(samples.get("checkpoints", {}).get("config", {}).get("configurable", {}).get("checkpoint_id"), str),
        f"samples={samples.get('checkpoints')}",
    )
    assert_true(
        "stream_projection.output_keys",
        "messages" in samples.get("output", {}) and "count" not in samples.get("output", {}),
        f"samples={samples.get('output')}",
    )


def compare_stream_projection_v2(cpp: dict[str, Any]) -> None:
    samples = cpp.get("samples", {})
    update = samples.get("updates", {})
    values = samples.get("final_values", {})
    interrupt_values = cpp.get("interrupt_values", {})
    interrupts = interrupt_values.get("interrupts") or [{}]
    assert_true("stream_projection_v2.completed", cpp.get("status") == "completed", f"cpp={cpp}")
    assert_true(
        "stream_projection_v2.update_envelope",
        update.get("type") == "updates"
        and isinstance(update.get("ns"), list)
        and update.get("data", {}).get("tick", {}).get("count") == 1,
        f"update={update}",
    )
    assert_true(
        "stream_projection_v2.values_envelope",
        values.get("type") == "values"
        and isinstance(values.get("ns"), list)
        and values.get("data", {}).get("count") == 1
        and values.get("interrupts") == [],
        f"values={values}",
    )
    assert_true(
        "stream_projection_v2.interrupt_status",
        cpp.get("interrupt_status") == "paused",
        f"cpp={cpp}",
    )
    assert_true(
        "stream_projection_v2.interrupt_lifted",
        interrupt_values.get("type") == "values"
        and isinstance(interrupt_values.get("ns"), list)
        and "__interrupt__" not in interrupt_values.get("data", {})
        and interrupts[0].get("id") == "approval"
        and interrupts[0].get("value", {}).get("question") == "continue?",
        f"interrupt_values={interrupt_values}",
    )


def compare_stream_interrupt_error(cpp: dict[str, Any]) -> None:
    interrupt_samples = cpp.get("interrupt_samples", {})
    error_samples = cpp.get("error_samples", {})
    assert_true(
        "stream_interrupt_error.interrupt_status",
        cpp.get("interrupt_status") == "paused",
        f"cpp={cpp}",
    )
    assert_true(
        "stream_interrupt_error.interrupt_projection",
        interrupt_samples.get("interrupt", {}).get("id") == "approval"
        and interrupt_samples.get("interrupt", {}).get("value", {}).get("question") == "approve?",
        f"interrupt_samples={interrupt_samples}",
    )
    assert_true(
        "stream_interrupt_error.interrupt_envelope",
        interrupt_samples.get("event_envelope", {}).get("event") == "on_custom_event"
        and interrupt_samples.get("event_envelope", {}).get("data", {}).get("interrupt", {}).get("id") == "approval",
        f"interrupt_samples={interrupt_samples}",
    )
    assert_true(
        "stream_interrupt_error.error_status",
        cpp.get("error_status") == "failed_precondition",
        f"cpp={cpp}",
    )
    assert_true(
        "stream_interrupt_error.error_envelopes",
        error_samples.get("node_error_envelope", {}).get("event") == "on_node_error"
        and error_samples.get("run_error_envelope", {}).get("event") == "on_chain_error"
        and isinstance(error_samples.get("failed_task", {}).get("error"), str),
        f"error_samples={error_samples}",
    )
    assert_true(
        "stream_interrupt_error.error_projection",
        "boom" in error_samples.get("error_projection", {}).get("error", {}).get("message", ""),
        f"error_samples={error_samples}",
    )


def compare_tool_returned_command(cpp: dict[str, Any]) -> None:
    assert_true(
        "tool_returned_command.final_state",
        cpp["output"].get("tool_routed") is True and cpp["output"].get("finished") is True,
        f"cpp={cpp['output']}",
    )


def compare_runnable_config(cpp: dict[str, Any]) -> None:
    merged = cpp.get("merged", {})
    patched = cpp.get("patched", {})
    applied = cpp.get("applied", {})
    envelope = cpp.get("event_envelope", {})

    assert_true(
        "runnable_config.merged_tags",
        merged.get("tags") == ["base", "call"] and merged.get("callbacks") == ["cb1"],
        f"merged={merged}",
    )
    merged_metadata = merged.get("metadata", {})
    assert_true(
        "runnable_config.metadata_merge_and_known_propagation",
        merged_metadata.get("tenant") == "tenant-b"
        and merged_metadata.get("thread_id") == "config-thread"
        and merged_metadata.get("checkpoint_id") == "cp-1"
        and merged_metadata.get("checkpoint_ns") == "root"
        and merged_metadata.get("assistant_id") == "assistant-a"
        and merged_metadata.get("graph_id") == "graph-a"
        and "custom" not in merged_metadata
        and "custom_top" not in merged_metadata,
        f"metadata={merged_metadata}",
    )
    merged_configurable = merged.get("configurable", {})
    assert_true(
        "runnable_config.arbitrary_configurable_preserved",
        merged_configurable.get("custom") == "call"
        and merged_configurable.get("custom_top") is False,
        f"configurable={merged_configurable}",
    )
    assert_true(
        "runnable_config.patch_rules",
        patched.get("tags") == ["base", "call", "patched"]
        and patched.get("callbacks") == ["cb2"]
        and "run_name" not in patched
        and "run_id" not in patched
        and patched.get("recursion_limit") == 8
        and patched.get("metadata", {}).get("request_id") == "req-1"
        and patched.get("metadata", {}).get("checkpoint_ns") == "root"
        and patched.get("configurable", {}).get("checkpoint_ns") == "patched-root",
        f"patched={patched}",
    )
    assert_true(
        "runnable_config.apply_to_run_options",
        applied.get("thread_id") == "config-thread"
        and applied.get("checkpoint_ns") == "root"
        and applied.get("run_id") == "run-config"
        and applied.get("run_name") == "base-run"
        and applied.get("max_steps") == 12
        and applied.get("max_concurrency") == 3,
        f"applied={applied}",
    )
    assert_true(
        "runnable_config.event_envelope",
        envelope.get("event") == "on_chain_start"
        and envelope.get("tags") == ["base", "call"]
        and envelope.get("metadata", {}).get("tenant") == "tenant-b"
        and envelope.get("metadata", {}).get("thread_id") == "config-thread"
        and envelope.get("metadata", {}).get("checkpoint_id") == "cp-1"
        and "custom_top" not in envelope.get("metadata", {}),
        f"envelope={envelope}",
    )


def compare_contract(cpp: dict[str, Any]) -> None:
    assert_true("contract.api_contract_version", cpp["api_contract_version"] == 24)
    assert_true("contract.schema_contract_version", cpp["schema_contract_version"] == 1)
    assert_true("contract.checkpoint_schema_version", cpp["checkpoint_schema_version"] == 3)
    assert_true("contract.content_envelope_version", cpp["content_envelope_version"] == 1)
    assert_true("contract.storage_schema_version", cpp["storage_schema_version"] == 1)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--cpp-probe", required=True)
    args = parser.parse_args()

    cpp = run_cpp_probe(args.cpp_probe)["scenarios"]
    require_scenarios(cpp)
    StateGraph, START, END, Command, Send, interrupt, MemorySaver = import_langgraph()

    py_history = run_python_history_snapshot(StateGraph, START, END, MemorySaver)
    py_command = run_python_command_goto(StateGraph, START, END, Command)
    py_interrupt = run_python_interrupt_replay(StateGraph, START, END, Command, interrupt, MemorySaver)
    py_multi_interrupt = run_python_multi_interrupt(StateGraph, START, END, Command, interrupt, MemorySaver)
    py_sequential_interrupt = run_python_sequential_interrupt(StateGraph, START, END, Command, interrupt, MemorySaver)
    py_send = run_python_send_map_reduce(StateGraph, START, END, Send)
    py_subgraph = run_python_subgraph_boundary(StateGraph, START, END, MemorySaver)

    compare_history_snapshot(cpp["history_snapshot"], py_history)
    compare_runnable_config(cpp["runnable_config"])
    compare_command_goto(cpp["command_goto"], py_command)
    compare_interrupt_replay(cpp["interrupt_replay"], py_interrupt)
    compare_multi_interrupt(cpp["multi_interrupt"], py_multi_interrupt)
    compare_sequential_interrupt(cpp["sequential_interrupt"], py_sequential_interrupt)
    compare_send_map_reduce(cpp["send_map_reduce"], py_send)
    compare_subgraph_boundary(cpp["subgraph_boundary"], py_subgraph)
    compare_stream_envelope(cpp["stream_envelope"])
    compare_stream_projection(cpp["stream_projection"])
    compare_stream_projection_v2(cpp["stream_projection_v2"])
    compare_stream_interrupt_error(cpp["stream_interrupt_error"])
    compare_tool_returned_command(cpp["tool_returned_command"])
    compare_contract(cpp["contract"])

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
