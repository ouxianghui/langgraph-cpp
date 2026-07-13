# LangGraph Golden Fixtures

This directory contains small, sanitized JSON fixtures for protocol-facing behavior.
They are intentionally independent of one test binary so humans and AI tools can
inspect the expected shapes directly.

Fixtures are examples of the current `langgraph-cpp` compatibility slice, not a
claim of complete Python LangGraph parity.

| Fixture | Covers |
| --- | --- |
| `stream_envelope_v3.json` | Projected stream v3 envelope shape. |
| `checkpoint_payload_v3.json` | LangGraph-style checkpoint stream payload shape. |
| `runnable_config_merge.json` | RunnableConfig merge/patch/apply behavior. |
| `interrupt_resume_payload.json` | Multi-interrupt resume payload shape. |
| `message_tool_call.json` | Message + tool-call JSON shape. |

