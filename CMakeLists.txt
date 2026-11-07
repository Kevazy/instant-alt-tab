cmake_minimum_required(VERSION 3.25)
project(instant-alt-tab VERSION 1.0.0 LANGUAGES C CXX)

if(NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "Instant Alt+Tab is Windows-only and builds with MSVC for x64.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Static runtime: the binaries run anywhere without a VC++ redistributable.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# MinHook, compiled into the hook DLL.
include(FetchContent)
FetchContent_Declare(minhook
    URL https://github.com/TsudaKageyu/minhook/archive/refs/tags/v1.3.4.tar.gz
    URL_HASH SHA256=1aebeae4ca898330c507860acc2fca2eb335fe446a3a2b8444c3bf8b2660a14e
    SOURCE_SUBDIR none)  # download only; its own build files aren't used
FetchContent_MakeAvailable(minhook)
add_library(minhook STATIC
    ${minhook_SOURCE_DIR}/src/buffer.c
    ${minhook_SOURCE_DIR}/src/hook.c
    ${minhook_SOURCE_DIR}/src/trampoline.c
    ${minhook_SOURCE_DIR}/src/hde/hde64.c)
target_include_directories(minhook PUBLIC ${minhook_SOURCE_DIR}/include)

# Zydis, an x86-64 instruction decoder, for the agent's discovery code (only its decoder is used).
FetchContent_Declare(zydis
    URL https://github.com/zyantific/zydis/releases/download/v4.1.0/zydis-amalgamated.tar.gz
    URL_HASH SHA256=aa9b82be3a37a2998bd8e16cf583bbf2b6c3d80e97dc20504169dc32ca1ced59
    SOURCE_SUBDIR none)
FetchContent_MakeAvailable(zydis)
add_library(zydis STATIC ${zydis_SOURCE_DIR}/Zydis.c)
target_include_directories(zydis PUBLIC ${zydis_SOURCE_DIR})
target_compile_definitions(zydis PUBLIC ZYDIS_STATIC_BUILD)

function(set_project_warnings target)
    target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8)
    target_compile_definitions(${target} PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
endfunction()

add_library(shared STATIC src/shared/Log.cpp src/shared/TextFile.cpp)
set_project_warnings(shared)

# Loaded into Explorer by the agent.
add_library(hook SHARED
    src/hook/DllMain.cpp
    src/hook/Hooking.cpp
    src/hook/QuietCardEdits.cpp
    src/hook/KeptMenu.cpp
    src/hook/KeptMenuChecks.cpp
    src/hook/MenuSelection.cpp
    src/hook/ComObjects.cpp)
set_target_properties(hook PROPERTIES OUTPUT_NAME instant-alt-tab-hook)
set_project_warnings(hook)
target_link_libraries(hook PRIVATE shared minhook windowsapp shcore)

# Finding and injecting into Explorer, and the crash guard; used by the agent and the CLI.
add_library(agent_core STATIC
    src/agent/CrashGuard.cpp
    src/agent/ExplorerProcess.cpp)
set_project_warnings(agent_core)

# Runs in the background from sign-in; no window.
add_executable(agent WIN32 src/agent/main.cpp src/agent/RegisteredDllPath.cpp src/agent/Alert.cpp)
set_target_properties(agent PROPERTIES OUTPUT_NAME instant-alt-tab-agent)
set_project_warnings(agent)
target_link_libraries(agent PRIVATE agent_core discovery shared)

# The command-line tool: install, uninstall, status, start, stop. It is the one file users
# download: the agent and the hook DLL are built into it, as resources named after their files,
# and installing writes them next to it.
file(GENERATE OUTPUT ${CMAKE_BINARY_DIR}/embedded-$<CONFIG>.rc CONTENT
    "$<TARGET_FILE_NAME:agent> RCDATA \"$<TARGET_FILE:agent>\"\n$<TARGET_FILE_NAME:hook> RCDATA \"$<TARGET_FILE:hook>\"\n")
add_executable(cli
    src/cli/main.cpp
    src/cli/Installer.cpp
    src/cli/AgentClient.cpp
    src/cli/Console.cpp
    src/cli/FirstAltTab.cpp
    ${CMAKE_BINARY_DIR}/embedded-$<CONFIG>.rc)
add_dependencies(cli agent hook)
set_target_properties(cli PROPERTIES OUTPUT_NAME instant-alt-tab)
set_project_warnings(cli)
target_compile_definitions(cli PRIVATE "PROJECT_VERSION=L\"${PROJECT_VERSION}\"")
target_link_libraries(cli PRIVATE agent_core shared shell32 ole32)

# Finds the Windows internals the hooks need in the DLLs Explorer has loaded; used by the agent.
add_library(discovery STATIC
    src/discovery/CodeAnalysis.cpp
    src/discovery/Discovery.cpp
    src/discovery/FindDataModelInternals.cpp
    src/discovery/PeFile.cpp
    src/discovery/FindSwitcherInternals.cpp
    src/discovery/Symbols.cpp
    src/discovery/FindTwinuiInternals.cpp)
set_project_warnings(discovery)
target_link_libraries(discovery PUBLIC zydis PRIVATE winhttp)

# Optional: runs discovery on any copies of the two DLLs (cmake --build --preset release --target discover).
add_executable(discover EXCLUDE_FROM_ALL tools/Discover.cpp)
set_project_warnings(discover)
target_link_libraries(discover PRIVATE discovery ole32)

# Optional: measures how fast the menu appears (cmake --build --preset release --target measure-latency).
add_executable(measure-latency EXCLUDE_FROM_ALL tools/MeasureLatency.cpp)
set_project_warnings(measure-latency)
target_link_libraries(measure-latency PRIVATE d3d11 dxgi)
