cmake_minimum_required(VERSION 3.20)

project(flowstate LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Threads REQUIRED)

add_library(
  flowstate_lib
  src/app.cpp
  src/buffer.cpp
  src/build.cpp
  src/command.cpp
  src/cursor.cpp
  src/diff.cpp
  src/editor_state.cpp
  src/git_status.cpp
  src/intellisense/clangd_client.cpp
  src/intellisense/completion.cpp
  src/intellisense/diagnostic.cpp
  src/json.cpp
  src/patch.cpp
  src/screen.cpp
  src/selection.cpp
  src/system_clipboard.cpp
  src/syntax/cpp_highlighter.cpp
  src/syntax/go_highlighter.cpp
  src/syntax/javascript_highlighter.cpp
  src/syntax/java_highlighter.cpp
  src/syntax/language.cpp
  src/syntax/markdown_highlighter.cpp
  src/syntax/plain_text_highlighter.cpp
  src/syntax/python_highlighter.cpp
  src/syntax/registry.cpp
  src/syntax/rust_highlighter.cpp
  src/syntax/theme.cpp
  src/terminal.cpp
  src/ai/codex_app_server_client.cpp
  src/ai/codex_client.cpp
  src/ai/no_ai_client.cpp
  src/ai/openai_client.cpp
  src/ai/prompt.cpp
)

target_include_directories(flowstate_lib PUBLIC src)
target_link_libraries(flowstate_lib PUBLIC Threads::Threads)

add_executable(flowstate src/main.cpp)
target_link_libraries(flowstate PRIVATE flowstate_lib)

enable_testing()

add_executable(
  flowstate_tests
  tests/test_main.cpp
)

target_link_libraries(flowstate_tests PRIVATE flowstate_lib)

add_test(NAME flowstate_tests COMMAND flowstate_tests)
