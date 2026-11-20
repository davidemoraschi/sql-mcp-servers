cmake_minimum_required(VERSION 3.12)
project(sqlite-mcp-server C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

include_directories(include)

add_definitions(-DNDEBUG -DSQLITE_THREADSAFE=0 -DSQLITE_OMIT_LOAD_EXTENSION -DHAS_EMBEDDED_SQLITE -D_CRT_SECURE_NO_WARNINGS -D_CRT_NONSTDC_NO_DEPRECATE)

set(SERVER_SOURCES
    src/main.c
    src/mcp_loop.c
    src/json_rpc.c
    src/rdbms_tools.c
    src/db_sqlite.c
    src/sqlite3.c
)

add_executable(sqlite-mcp-server ${SERVER_SOURCES})
add_executable(mcp_test_runner src/test_runner.c)

set(MCP_TARGETS sqlite-mcp-server mcp_test_runner)

# Snowflake MCP server. Needs WinHTTP on Windows (part of the OS) and libcurl
# on other platforms; without libcurl the target is skipped so the SQLite
# server still builds.
set(BUILD_SNOWFLAKE ON)
if(NOT WIN32)
    find_package(CURL)
    if(NOT CURL_FOUND)
        message(WARNING "libcurl not found: skipping snowflake-mcp-server (install libcurl4-openssl-dev or equivalent)")
        set(BUILD_SNOWFLAKE OFF)
    endif()
endif()

if(BUILD_SNOWFLAKE)
    add_executable(snowflake-mcp-server
        src/main_snowflake.c
        src/mcp_loop.c
        src/json_rpc.c
        src/rdbms_tools.c
        src/db_snowflake.c
        src/sf_query.c
        src/sf_auth.c
        src/sf_buf.c
        src/sf_http.c
        src/sf_loopback.c
        src/sf_browser.c
    )
    if(WIN32)
        target_link_libraries(snowflake-mcp-server PRIVATE winhttp ws2_32 shell32)
    else()
        target_link_libraries(snowflake-mcp-server PRIVATE CURL::libcurl)
    endif()
    list(APPEND MCP_TARGETS snowflake-mcp-server)
endif()

foreach(tgt IN LISTS MCP_TARGETS)
    if(MSVC)
        target_compile_options(${tgt} PRIVATE
            /W3
            $<$<CONFIG:Release>:/O2 /GL /Gy /Gw /MD>
            $<$<CONFIG:MinSizeRel>:/O1 /GL /Gy /Gw /MD>
        )
        target_link_options(${tgt} PRIVATE
            $<$<CONFIG:Release>:/LTCG /OPT:REF /OPT:ICF>
            $<$<CONFIG:MinSizeRel>:/LTCG /OPT:REF /OPT:ICF>
        )
    else()
        target_compile_options(${tgt} PRIVATE
            -Wall -Wextra
            $<$<CONFIG:Release>:-O2  -fomit-frame-pointer -ffunction-sections -fdata-sections>
            $<$<CONFIG:MinSizeRel>:-Os  -fomit-frame-pointer -ffunction-sections -fdata-sections>
        )
        target_link_options(${tgt} PRIVATE
            $<$<CONFIG:Release>:-O2  -Wl,--gc-sections -Wl,-s>
            $<$<CONFIG:MinSizeRel>:-Os  -Wl,--gc-sections -Wl,-s>
        )
    endif()
endforeach()
