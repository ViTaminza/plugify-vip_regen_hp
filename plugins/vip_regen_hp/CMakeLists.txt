# vip_regen_hp — Plugify C++ plugin: port of the Pisex VIP module "VIP_RegenHP".
#
#   plugify-vip_regen_hp  the plugin shared library (entry "bin/vip_regen_hp")

set(VIP_REGEN_HP_VERSION "1.0.1")

# Binding globals the plugify module fills from the plugins declared as dependencies:
#   __s2sdk_* / __hudmenu_* (s2sdk_bindings, hudmenu_bindings - both come in through
#   pisexcompat's PUBLIC link) and __vip_* (vip_bindings, the generated consumer bindings of
#   plugins/vip).
set(VIP_REGEN_HP_BINDING_OBJECTS "")
if(TARGET vip_bindings)
    list(APPEND VIP_REGEN_HP_BINDING_OBJECTS $<TARGET_OBJECTS:vip_bindings>)
else()
    message(WARNING "vip_regen_hp: vip_bindings is missing (build plugins/vip first)")
endif()

add_library(plugify-vip_regen_hp SHARED
        src/plugin.cpp
        $<TARGET_OBJECTS:s2sdk_bindings>
        ${VIP_REGEN_HP_BINDING_OBJECTS}
)
target_include_directories(plugify-vip_regen_hp PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
# SYSTEM: warnings from the generated vip/s2sdk/plg/pisexcompat headers must not fail -Werror.
target_include_directories(plugify-vip_regen_hp SYSTEM PRIVATE
        "${CMAKE_SOURCE_DIR}/plugins/vip/include"
        "${CMAKE_SOURCE_DIR}/plugins/vip/include/external/plugify/include"
        "${CMAKE_SOURCE_DIR}/sdk/include"
        "${CMAKE_SOURCE_DIR}/libs/pisexcompat/include")
target_link_libraries(plugify-vip_regen_hp PRIVATE
        pisexcompat
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
# Engine-free logic tests; run by ./test.sh (build/tests).
add_executable(vip_regen_hp_tests tests/vip_regen_hp_tests.cpp)
target_include_directories(vip_regen_hp_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_options(vip_regen_hp_tests PRIVATE -Wall -Wextra -Werror)
target_compile_definitions(vip_regen_hp_tests PRIVATE _GLIBCXX_USE_CXX11_ABI=1)
target_link_libraries(vip_regen_hp_tests PRIVATE -static-libstdc++ -static-libgcc)
set_target_properties(vip_regen_hp_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")
