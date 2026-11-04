# winject-l3 sources: the public API (include/winject_l3/*.h) over the node,
# Config, and the text console (console/: execute, format, the UDP Service).
# Relative to this directory; built on top of the core
# (src/core/CoreSources.cmake).
set(WINJECT_L3_SOURCES
    Api.cpp
    Run.cpp
    L3Core.cpp
    L3Impl.cpp
    Config.cpp
    console/Console.cpp
    console/ConsoleCommands.cpp
    console/ConsoleService.cpp
    console/ConsoleParse.cpp
)
