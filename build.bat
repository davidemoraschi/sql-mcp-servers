@echo off
echo =======================================
echo Building Standalone SQLite MCP Server for Windows
echo =======================================

where cl >nul 2>nul
if %ERRORLEVEL% NEQ 0 goto TRY_CMAKE

echo [INFO] Found MSVC compiler (cl.exe). Compiling Release binary...
if not exist bin mkdir bin

cl.exe /nologo /std:c11 /O2 /GL /Gy /Gw /MD /DNDEBUG /DSQLITE_THREADSAFE=0 /DSQLITE_OMIT_LOAD_EXTENSION /DHAS_EMBEDDED_SQLITE /D_CRT_SECURE_NO_WARNINGS /D_CRT_NONSTDC_NO_DEPRECATE /Iinclude ^
    src\\sqlite\\main.c src\\shared\\mcp_loop.c src\\shared\\json_rpc.c src\\shared\\rdbms_tools.c ^
    src\\sqlite\\db_sqlite_core.c src\\sqlite\\db_sqlite_transfer.c src\\sqlite\\sqlite3.c ^
    /link /LTCG /OPT:REF /OPT:ICF /OUT:bin\\sqlite-mcp-server.exe
if %ERRORLEVEL% NEQ 0 (
    echo [ERROR] SQLite server compilation failed.
    exit /b 1
)

echo [INFO] Compiling Snowflake MCP Server...
cl.exe /nologo /std:c11 /O2 /GL /Gy /Gw /MD /DNDEBUG /D_CRT_SECURE_NO_WARNINGS /D_CRT_NONSTDC_NO_DEPRECATE /Iinclude ^
    src\\snowflake\\main_snowflake.c src\\shared\\mcp_loop.c src\\shared\\json_rpc.c src\\shared\\rdbms_tools.c ^
    src\\snowflake\\db_snowflake_core.c src\\snowflake\\db_snowflake_transfer.c ^
    src\\snowflake\\sf_query.c src\\snowflake\\sf_auth.c src\\snowflake\\sf_buf.c ^
    src\\snowflake\\sf_http.c src\\snowflake\\sf_loopback.c src\\snowflake\\sf_browser.c ^
    /link /LTCG /OPT:REF /OPT:ICF winhttp.lib ws2_32.lib shell32.lib /OUT:bin\\snowflake-mcp-server.exe
if %ERRORLEVEL% NEQ 0 (
    echo [ERROR] Snowflake server compilation failed.
    exit /b 1
)

echo [INFO] Compiling PostgreSQL MCP Server...
cl.exe /nologo /std:c11 /O2 /GL /Gy /Gw /MD /DNDEBUG /D_CRT_SECURE_NO_WARNINGS /D_CRT_NONSTDC_NO_DEPRECATE /Iinclude ^
    src\\postgres\\main_postgres.c src\\shared\\mcp_loop.c src\\shared\\json_rpc.c src\\shared\\rdbms_tools.c ^
    src\\postgres\\db_postgres.c src\\postgres\\db_postgres_transfer.c src\\postgres\\pg_md5.c src\\postgres\\pg_scram.c src\\snowflake\\sf_buf.c ^
    /link /LTCG /OPT:REF /OPT:ICF ws2_32.lib /OUT:bin\\postgres-mcp-server.exe
if %ERRORLEVEL% NEQ 0 (
    echo [ERROR] PostgreSQL server compilation failed.
    exit /b 1
)

echo [INFO] Compiling Test Runner (SQLite)...
cl.exe /nologo /std:c11 /O2 /GL /Gy /Gw /MD /D_CRT_SECURE_NO_WARNINGS /D_CRT_NONSTDC_NO_DEPRECATE /Iinclude ^
    src\\sqlite\\test_runner_sqlite.c ^
    /link /LTCG /OPT:REF /OPT:ICF /OUT:bin\\mcp_test_runner_sqlite.exe
if %ERRORLEVEL% NEQ 0 (
    echo [ERROR] SQLite test runner compilation failed.
    exit /b 1
)

echo [INFO] Compiling Test Runner (PostgreSQL)...
cl.exe /nologo /std:c11 /O2 /GL /Gy /Gw /MD /D_CRT_SECURE_NO_WARNINGS /D_CRT_NONSTDC_NO_DEPRECATE /Iinclude ^
    src\\postgres\\test_runner_postgres.c ^
    /link /LTCG /OPT:REF /OPT:ICF /OUT:bin\\mcp_test_runner_postgres.exe
if %ERRORLEVEL% NEQ 0 (
    echo [ERROR] PostgreSQL test runner compilation failed.
    exit /b 1
)

echo [SUCCESS] Build passed!
exit /b 0

:TRY_CMAKE
where cmake >nul 2>nul
if %ERRORLEVEL% NEQ 0 goto NO_TOOLS

echo [INFO] cl.exe not in PATH, attempting CMake build...
if not exist build mkdir build
cd build
cmake ..
if %ERRORLEVEL% NEQ 0 ( cd .. & echo [ERROR] CMake configuration failed. & exit /b 1 )
cmake --build . --config Release
if %ERRORLEVEL% NEQ 0 ( cd .. & echo [ERROR] CMake build failed. & exit /b 1 )
cd ..
echo [SUCCESS] CMake build passed!
exit /b 0

:NO_TOOLS
echo [ERROR] Neither MSVC (cl.exe) nor CMake was found in PATH.
echo [INFO] Please run this script from Developer Command Prompt for Visual Studio.
exit /b 1
