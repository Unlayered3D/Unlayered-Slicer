get_filename_component(_TEST_NAME ${CMAKE_CURRENT_LIST_DIR} NAME)
add_executable(${_TEST_NAME}_tests 
	${_TEST_NAME}_tests.cpp
	test_helpers.cpp
	test_helpers.hpp
	test_cooling.cpp
	test_cut_plane_apply.cpp
	test_segment_frames.cpp
	test_gcode_baseline.cpp
	test_hotend_collision.cpp
	test_virtual_tilt_limit.cpp
	test_auto_planes.cpp
	test_auto_planes_stack.cpp
	test_multiplanar_audit.cpp
	test_seam_continuation.cpp
	auto_planes_fixtures.hpp
	test_max_layer_z_multiplanar.cpp
	test_segment_support_cascade.cpp
	test_skeleton_containment.cpp
	test_extrusion_entity.cpp
	test_extrusion_processor.cpp
	test_fill.cpp
	test_mesh_infill.cpp
	test_flow.cpp
	test_gcode_timing.cpp
	test_gcodewriter.cpp
	test_curved_layer_engine.cpp
	test_model.cpp
	test_multifilament.cpp
	test_perimeters.cpp
	test_print.cpp
	test_printobject.cpp
	test_mixed_filament.cpp
	test_skirt_brim.cpp
	test_slicing_pipeline_hook.cpp
	test_support_material.cpp
	test_support_direction.cpp
	test_tree_support.cpp
	test_trianglemesh.cpp
	test_wave_overhangs.cpp
	test_wipe_tower.cpp
	)
target_link_libraries(${_TEST_NAME}_tests test_common libslic3r Catch2::Catch2WithMain)
set_property(TARGET ${_TEST_NAME}_tests PROPERTY FOLDER "tests")

unlayered_copy_test_dlls()

unlayered_discover_tests(${_TEST_NAME}_tests)
