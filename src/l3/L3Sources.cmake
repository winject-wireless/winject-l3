# winject-l3 node sources: Config, the node (App), its control API and the
# text console. Relative to this directory; built on top of the core
# (src/core/CoreSources.cmake).
set(WINJECT_L3_SOURCES
    WinjectL3.cpp
    App.cpp
    Config.cpp
    console/ConsoleService.cpp
    console/ConsoleParse.cpp
)
