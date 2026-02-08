# Test executable
add_executable(basic_example
    main.c
)

target_link_libraries(basic_example
    PRIVATE inline
)
