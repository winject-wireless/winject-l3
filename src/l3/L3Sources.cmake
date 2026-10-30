# winject-l3 node sources: Config, the node (App), its control API and the
# text console. Relative to this directory; built on top of the core
# (src/core/CoreSources.cmake).
set(WINJECT_L3_SOURCES
    WinjectL3.cpp
    App.cpp
    Control.cpp
    Config.cpp
    console/ConsoleCommands.cpp
    console/ConsoleService.cpp
    console/ConsoleParse.cpp
)
