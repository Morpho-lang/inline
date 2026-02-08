# Test executable
add_executable(minimal_example
    main.c
)

target_link_libraries(minimal_example
    PRIVATE inline
)

install(TARGETS minimal_example
    RUNTIME DESTINATION bin
)
