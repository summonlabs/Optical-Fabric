set(OPTICAL_FABRIC_EXAMPLES
  ex_lifecycle
  ex_refusal
  ex_persistence_restart
)
foreach(example ${OPTICAL_FABRIC_EXAMPLES})
  add_executable(${example} ${example}.cpp ex_support.cpp)
  target_link_libraries(${example} PRIVATE OpticalFabric)
  target_include_directories(${example} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  optical_fabric_apply_warnings(${example})
endforeach()
