add_executable(fileflow_tests
    test_bytes.cpp
    test_hash.cpp
    test_gf256.cpp
    test_intra_fec.cpp
    test_link.cpp
    test_framing.cpp
    test_fuzz_regressions.cpp
    test_device.cpp
    test_fountain.cpp
    test_grid.cpp
    test_frame.cpp
    test_modulation.cpp
    test_llr_quality.cpp
    test_transfer.cpp
    test_session.cpp
    test_pipeline.cpp
    test_live.cpp
    test_geometry.cpp
    test_sampler.cpp
    test_photometric.cpp
    test_detect.cpp
    test_detect_stability.cpp
    test_tracker.cpp
    test_rendered_source.cpp
    test_harness.cpp
    test_end_to_end.cpp
)

target_link_libraries(fileflow_tests PRIVATE
    fileflow_core
    fileflow_sim
    fileflow_harness
    fileflow_warnings
    GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(fileflow_tests DISCOVERY_TIMEOUT 60)
