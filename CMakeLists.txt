cmake_minimum_required(VERSION 3.12)
project(sqlite-mcp-server C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

include_directories(include)

add_definitions(
    -DNDEBUG -DSQLITE_THREADSAFE=0 -DSQLITE_OMIT_LOAD_EXTENSION -DHAS_EMBEDDED_SQLITE
    -D_CRT_SECURE_NO_WARNINGS -D_CRT_NONSTDC_NO_DEPRECATE
)

# ---------------------------------------------------------------------------
# SQLite MCP Server
# ---------------------------------------------------------------------------
add_executable(sqlite-mcp-server
    src/sqlite/main.c
    src/shared/mcp_loop.c
    src/shared/json_rpc.c
    src/shared/rdbms_tools.c
    src/sqlite/db_sqlite_core.c
    src/sqlite/db_sqlite_transfer.c
    src/sqlite/sqlite3.c
)

# ---------------------------------------------------------------------------
# Test runners
# ---------------------------------------------------------------------------
add_executable(mcp_test_runner_sqlite src/sqlite/test_runner_sqlite.c)
add_executable(mcp_test_runner_postgres src/postgres/test_runner_postgres.c)

set(MCP_TARGETS sqlite-mcp-server mcp_test_runner_sqlite mcp_test_runner_postgres)

# Embed the application icon and version information in every Windows executable.
if(WIN32)
    target_sources(sqlite-mcp-server PRIVATE res/sqlite.rc)
    target_sources(mcp_test_runner_sqlite PRIVATE res/test_runner_sqlite.rc)
    target_sources(mcp_test_runner_postgres PRIVATE res/test_runner_postgres.rc)
endif()

# ---------------------------------------------------------------------------
# Snowflake MCP Server (optional: needs WinHTTP on Windows, libcurl elsewhere)
# ---------------------------------------------------------------------------
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
        src/snowflake/main_snowflake.c
        src/shared/mcp_loop.c
        src/shared/json_rpc.c
        src/shared/rdbms_tools.c
        src/snowflake/db_snowflake_core.c
        src/snowflake/db_snowflake_transfer.c
        src/snowflake/sf_query.c
        src/snowflake/sf_auth.c
        src/snowflake/sf_buf.c
        src/snowflake/sf_http.c
        src/snowflake/sf_loopback.c
        src/snowflake/sf_browser.c
    )
    if(WIN32)
        target_link_libraries(snowflake-mcp-server PRIVATE winhttp ws2_32 shell32)
    else()
        target_link_libraries(snowflake-mcp-server PRIVATE CURL::libcurl)
    endif()
    list(APPEND MCP_TARGETS snowflake-mcp-server)
    if(WIN32)
        target_sources(snowflake-mcp-server PRIVATE res/snowflake.rc)
    endif()
endif()

# ---------------------------------------------------------------------------
# BigQuery MCP Server (OAuth 2.0 + BigQuery REST API)
# ---------------------------------------------------------------------------
set(BUILD_BIGQUERY ON)
if(NOT WIN32)
    find_package(CURL)
    if(NOT CURL_FOUND)
        message(WARNING "libcurl not found: skipping bigquery-mcp-server (install libcurl4-openssl-dev)")
        set(BUILD_BIGQUERY OFF)
    endif()
endif()

if(BUILD_BIGQUERY)
    add_executable(bigquery-mcp-server
        src/bigquery/main_bigquery.c
        src/bigquery/bq_oauth.c
        src/bigquery/db_bigquery.c
        src/bigquery/db_bigquery_schema.c
        src/bigquery/db_bigquery_transfer.c
        src/shared/mcp_loop.c
        src/shared/json_rpc.c
        src/shared/rdbms_tools.c
        src/snowflake/sf_buf.c
        src/snowflake/sf_http.c
    )
    if(WIN32)
        target_link_libraries(bigquery-mcp-server PRIVATE winhttp ws2_32 shell32 bcrypt)
        target_sources(bigquery-mcp-server PRIVATE res/bigquery.rc)
        target_include_directories(bigquery-mcp-server PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/res")
    else()
        target_link_libraries(bigquery-mcp-server PRIVATE CURL::libcurl)
    endif()
    list(APPEND MCP_TARGETS bigquery-mcp-server)
endif()

# ---------------------------------------------------------------------------
# PostgreSQL MCP Server (no external dependencies - pure wire protocol)
# ---------------------------------------------------------------------------
add_executable(postgres-mcp-server
    src/postgres/main_postgres.c
    src/shared/mcp_loop.c
    src/shared/json_rpc.c
    src/shared/rdbms_tools.c
    src/postgres/db_postgres.c
    src/postgres/db_postgres_transfer.c
    src/postgres/pg_md5.c
    src/postgres/pg_scram.c
    src/snowflake/sf_buf.c
)
if(WIN32)
    target_link_libraries(postgres-mcp-server PRIVATE ws2_32)
endif()
list(APPEND MCP_TARGETS postgres-mcp-server)
if(WIN32)
    target_sources(postgres-mcp-server PRIVATE res/postgres.rc)
endif()

# ---------------------------------------------------------------------------
# Compiler flags (applied to all targets)
# ---------------------------------------------------------------------------
foreach(tgt IN LISTS MCP_TARGETS)
    if(WIN32)
        target_include_directories(${tgt} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/res")
    endif()
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
            $<$<CONFIG:Release>:-O2 -fomit-frame-pointer -ffunction-sections -fdata-sections>
            $<$<CONFIG:MinSizeRel>:-Os -fomit-frame-pointer -ffunction-sections -fdata-sections>
        )
        target_link_options(${tgt} PRIVATE
            $<$<CONFIG:Release>:-O2 -Wl,--gc-sections -Wl,-s>
            $<$<CONFIG:MinSizeRel>:-Os -Wl,--gc-sections -Wl,-s>
        )
    endif()
endforeach()
