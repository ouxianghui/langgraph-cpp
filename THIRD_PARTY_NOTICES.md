# Third-Party Notices

`langgraph-cpp` vendors or references the following third-party components under
`third_party/`. Their licenses remain their own.

| Component | Location | License File |
| --- | --- | --- |
| nlohmann/json | `third_party/nlohmann_json` | `third_party/nlohmann_json/LICENSE.MIT` |
| SQLite | `third_party/sqlite` | `third_party/sqlite/LICENSE` |
| cpp-httplib | `third_party/cpp-httplib` | `third_party/cpp-httplib/LICENSE` |

Optional integrations such as llama.cpp are not vendored by default and require
an external source tree, install prefix, or CMake target supplied by the user.
