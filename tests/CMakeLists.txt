# Optical Fabric - test suite.
#
# No test carries a TIMEOUT property. A hanging test is a defect and must
# surface as a hang rather than be masked by a timer.

set(OPTICAL_FABRIC_TEST_SUPPORT test_harness.cpp test_support.cpp)

set(OPTICAL_FABRIC_TESTS
  test_identity
  test_evidence
  test_lifecycle
  test_authority
  test_persistence
  test_protocol
  test_concurrency
  test_property
  test_adversarial
  test_scale
)

foreach(test ${OPTICAL_FABRIC_TESTS})
  add_executable(${test} ${test}.cpp ${OPTICAL_FABRIC_TEST_SUPPORT})
  target_link_libraries(${test} PRIVATE OpticalFabric)
  target_include_directories(${test} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  optical_fabric_apply_warnings(${test})
  add_test(NAME ${test} COMMAND ${test})
endforeach()

# Real operating-system process proof. It needs the runtime daemon and the
# inspection utility, so it is only built when they are.
if(TARGET of_node AND TARGET of_cli)
  add_executable(test_multiprocess test_multiprocess.cpp ${OPTICAL_FABRIC_TEST_SUPPORT})
  target_link_libraries(test_multiprocess PRIVATE OpticalFabric)
  target_include_directories(test_multiprocess PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  optical_fabric_apply_warnings(test_multiprocess)
  target_compile_definitions(test_multiprocess PRIVATE
    OF_NODE_EXE="$<TARGET_FILE:of_node>"
    OF_CLI_EXE="$<TARGET_FILE:of_cli>")
  add_test(NAME test_multiprocess COMMAND test_multiprocess)
endif()
