cmake_minimum_required(VERSION 3.16)
project(curved_transition_geometry LANGUAGES CXX)
add_executable(curved_transition_probe probe.cpp)
target_compile_features(curved_transition_probe PRIVATE cxx_std_17)
enable_testing()
add_test(NAME curved_transition_geometry COMMAND curved_transition_probe --self-test)
add_executable(curved_transition_tests
    ../../tests/libslic3r/test_curved_transition.cpp
    ../../tests/libslic3r/test_curved_ramp.cpp
    ../../tests/libslic3r/test_curved_plane_program.cpp
    ../../tests/libslic3r/test_curved_regions.cpp
    ../../tests/libslic3r/test_curved_joint_planning.cpp
    ../../tests/libslic3r/test_underside_analysis.cpp
    ../../tests/catch2/extras/catch_amalgamated.cpp)
target_compile_features(curved_transition_tests PRIVATE cxx_std_17)
target_compile_definitions(curved_transition_tests PRIVATE CURVED_TRANSITION_STANDALONE)
target_include_directories(curved_transition_tests PRIVATE ../../src)
add_test(NAME curved_transition_regressions COMMAND curved_transition_tests "[curved_transition]")
add_test(NAME curved_ramp_regressions COMMAND curved_transition_tests "[curved-blend]")
