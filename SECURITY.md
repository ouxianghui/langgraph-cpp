# Security Policy

`langgraph-cpp` is a pre-1.0 developer preview. The default runtime should build
and test without real cloud providers, real hardware access, or privileged tools.

## Supported Versions

Security fixes are handled on the default branch until the project has tagged
stable releases. Once stable releases exist, this file will list supported
release lines.

## Reporting a Vulnerability

Please do not publish a working exploit before maintainers have had a reasonable
chance to respond.

If GitHub private vulnerability reporting is enabled for the repository, use it.
Otherwise, open a minimal public issue that says a vulnerability report is
available, without including secrets, exploit payloads, credentials, or private
user data. A maintainer can then arrange a private disclosure channel.

## Security Boundaries

- The core runtime only executes user-registered node handlers and tools.
- Dangerous shell, filesystem, network, and hardware tools are not built in by
  default.
- Applications are responsible for registering only the tools they want to
  expose and for applying their own authorization policies.
- Tool inputs should be validated with JSON Schema before execution.
- Secrets, authorization headers, raw request/response bodies, and private user
  content should not be logged or emitted as metrics.

The detailed runtime safety model is documented in
[`docs/SECURITY_MODEL.md`](docs/SECURITY_MODEL.md).

## Out of Scope for the Current Preview

- Production secret rotation infrastructure.
- Hosted execution sandboxing.
- Full LangSmith-compatible trace backends.
- Real ROS2, UART, CAN, I2C, or cloud checkpoint-store deployments.
