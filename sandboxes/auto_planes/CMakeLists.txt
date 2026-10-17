# UNLAYERED_MOD -- headless automatic-plane planner and multiplanar toolpath audit driver.
# Development/validation only (SLIC3R_BUILD_SANDBOXES=ON); never shipped.
add_executable(auto_planes_cli main.cpp)
target_link_libraries(auto_planes_cli libslic3r)
