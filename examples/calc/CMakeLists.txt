# Test executable
add_executable(calc
    main.c
)

target_link_libraries(calc PRIVATE inline)

if (UNIX AND NOT APPLE)
  target_link_libraries(calc PRIVATE m)
endif()

install(TARGETS calc
    RUNTIME DESTINATION bin
)
