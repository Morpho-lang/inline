
if (MSVC)
    add_compile_options(/wd5045) # QSpectre warnings
    add_compile_options(/wd4668) # Macro warnings from windows libraries
    add_compile_options(/wd4710) # inlining of functions
    add_compile_options(/wd4711) # inlining of functions
endif()

# Library source files
add_library(inline STATIC
    inline.c
)

target_include_directories(inline
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
)

# Enable warnings for the library
target_compile_options(inline PRIVATE
    -Wall
)

# Test executable
add_executable(test_inline
    main.c
)

target_link_libraries(test_inline
    PRIVATE inline
)
