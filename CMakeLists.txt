cmake_minimum_required(VERSION 3.12)
project(sqlite-mcp-server C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

include_directories(include)

add_definitions(-DNDEBUG -DSQLITE_THREADSAFE=0 -DSQLITE_OMIT_LOAD_EXTENSION -D_CRT_SECURE_NO_WARNINGS -D_CRT_NONSTDC_NO_DEPRECATE)

set(SERVER_SOURCES
    src/main.c
    src/json_rpc.c
    src/rdbms_tools.c
    src/db_sqlite.c
    src/sqlite3.c
)

add_executable(sqlite-mcp-server ${SERVER_SOURCES})
add_executable(mcp_test_runner src/test_runner.c)

if(MSVC)
    target_compile_options(sqlite-mcp-server PRIVATE
        /W3
        $<$<CONFIG:Release>:/O2 /GL /Gy /Gw /MD>
        $<$<CONFIG:MinSizeRel>:/O1 /GL /Gy /Gw /MD>
    )
    target_link_options(sqlite-mcp-server PRIVATE
        $<$<CONFIG:Release>:/LTCG /OPT:REF /OPT:ICF>
        $<$<CONFIG:MinSizeRel>:/LTCG /OPT:REF /OPT:ICF>
    )

    target_compile_options(mcp_test_runner PRIVATE
        /W3
        $<$<CONFIG:Release>:/O2 /GL /Gy /Gw /MD>
        $<$<CONFIG:MinSizeRel>:/O1 /GL /Gy /Gw /MD>
    )
    target_link_options(mcp_test_runner PRIVATE
        $<$<CONFIG:Release>:/LTCG /OPT:REF /OPT:ICF>
        $<$<CONFIG:MinSizeRel>:/LTCG /OPT:REF /OPT:ICF>
    )
else()
    target_compile_options(sqlite-mcp-server PRIVATE
        -Wall -Wextra
        $<$<CONFIG:Release>:-O2 -flto -fomit-frame-pointer -ffunction-sections -fdata-sections>
        $<$<CONFIG:MinSizeRel>:-Os -flto -fomit-frame-pointer -ffunction-sections -fdata-sections>
    )
    target_link_options(sqlite-mcp-server PRIVATE
        $<$<CONFIG:Release>:-O2 -flto -Wl,--gc-sections -Wl,-s>
        $<$<CONFIG:MinSizeRel>:-Os -flto -Wl,--gc-sections -Wl,-s>
    )

    target_compile_options(mcp_test_runner PRIVATE
        -Wall -Wextra
        $<$<CONFIG:Release>:-O2 -flto -fomit-frame-pointer -ffunction-sections -fdata-sections>
        $<$<CONFIG:MinSizeRel>:-Os -flto -fomit-frame-pointer -ffunction-sections -fdata-sections>
    )
    target_link_options(mcp_test_runner PRIVATE
        $<$<CONFIG:Release>:-O2 -flto -Wl,--gc-sections -Wl,-s>
        $<$<CONFIG:MinSizeRel>:-Os -flto -Wl,--gc-sections -Wl,-s>
    )
endif()