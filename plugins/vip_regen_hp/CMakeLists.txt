# vip_regen_hp — Plugify C++ plugin: port of the Pisex VIP module "VIP_RegenHP".
#
# Pure s2sdk: no s2guard and no pisexcompat. All safety rules (map gate, slot/controller/pawn
# checks, the controller's m_iTeamNum read, the m_iHealth network state change, the per-slot
# countdown) live in src/plugin.cpp and the shared, engine-free src/regen_hp_core.hpp.
#
#   plugify-vip_regen_hp  the plugin shared library (entry "bin/vip_regen_hp" -> bin/libvip_regen_hp.so)
#   vip_regen_hp_tests    engine-free unit tests: the healing step, the regen-amount guard, the
#                         slot range, the per-slot countdown with the 1/64 s clamp and the
#                         "RegenHP_round_min" round rule (build/tests/vip_regen_hp_tests)

set(VIP_REGEN_HP_VERSION "1.2.0")

# Binding globals the plugify module fills from the plugins declared as dependencies:
#   __s2sdk_*   (s2sdk_bindings)
#   __vip_*     (vip consumer bindings, generated into plugins/vip/include/external/plugify/include)
# In the monorepo plugins/vip is added first ("vip" < "vip_regen_hp"), so vip_bindings already
# exists; a standalone exported repo only carries plugins/vip/include/ headers + vip.cpp and has no
# plugins/vip/CMakeLists.txt - define vip_bindings here when absent.
if(NOT TARGET vip_bindings)
    add_library(vip_bindings OBJECT
            "${CMAKE_SOURCE_DIR}/plugins/vip/include/external/plugify/include/vip/vip.cpp")
    target_include_directories(vip_bindings SYSTEM PUBLIC
            "${CMAKE_SOURCE_DIR}/plugins/vip/include/external/plugify/include"
            "${CMAKE_SOURCE_DIR}/sdk/include")
    target_compile_definitions(vip_bindings PUBLIC _GLIBCXX_USE_CXX11_ABI=1)
endif()

add_library(plugify-vip_regen_hp SHARED
        src/plugin.cpp
        $<TARGET_OBJECTS:s2sdk_bindings>
        $<TARGET_OBJECTS:vip_bindings>
)
target_include_directories(plugify-vip_regen_hp PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
# SYSTEM: warnings from the generated vip/s2sdk/plg headers must not fail -Werror on our sources.
target_include_directories(plugify-vip_regen_hp SYSTEM PRIVATE
        "${CMAKE_SOURCE_DIR}/plugins/vip/include"
        "${CMAKE_SOURCE_DIR}/plugins/vip/include/external/plugify/include"
        "${CMAKE_SOURCE_DIR}/sdk/include")
target_link_libraries(plugify-vip_regen_hp PRIVATE
        plugify_sdk
        -static-libstdc++ -static-libgcc
)
target_compile_options(plugify-vip_regen_hp PRIVATE -Wall -Wextra -Werror)
target_compile_definitions(plugify-vip_regen_hp PRIVATE
        _GLIBCXX_USE_CXX11_ABI=1
        PLUGIFY_MODULE="vip_regen_hp"
        PLUGIFY_IS_DEBUG=0
        PLUGIFY_IS_RELEASE=1
)
set_target_properties(plugify-vip_regen_hp PROPERTIES OUTPUT_NAME vip_regen_hp)

if(UNIX AND NOT APPLE)
    target_link_options(plugify-vip_regen_hp PRIVATE
            "-Wl,--version-script,${CMAKE_CURRENT_SOURCE_DIR}/sym/version_script.lds")
endif()

configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/vip_regen_hp.pplugin.in"
        "${CMAKE_CURRENT_BINARY_DIR}/vip_regen_hp.pplugin"
)

# --- tests --------------------------------------------------------------------
# Engine-free logic tests (no s2sdk binding, no pisexcompat); run by ./test.sh (build/tests).
add_executable(vip_regen_hp_tests tests/vip_regen_hp_tests.cpp)
target_include_directories(vip_regen_hp_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_options(vip_regen_hp_tests PRIVATE -Wall -Wextra -Werror)
target_compile_definitions(vip_regen_hp_tests PRIVATE _GLIBCXX_USE_CXX11_ABI=1)
target_link_libraries(vip_regen_hp_tests PRIVATE -static-libstdc++ -static-libgcc)
set_target_properties(vip_regen_hp_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")
