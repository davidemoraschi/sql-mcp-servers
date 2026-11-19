@echo off
echo =======================================
echo Building Standalone SQLite MCP Server for Windows
echo =======================================

where cl >nul 2>nul
if %ERRORLEVEL% NEQ 0 goto TRY_CMAKE

echo [INFO] Found MSVC compiler (cl.exe). Compiling Release binary...
if not exist bin mkdir bin
cl.exe /nologo /std:c11 /O2 /GL /Gy /Gw /MD /DNDEBUG /DSQLITE_THREADSAFE=0 /DSQLITE_OMIT_LOAD_EXTENSION /D_CRT_SECURE_NO_WARNINGS /D_CRT_NONSTDC_NO_DEPRECATE /Iinclude src\main.c src\json_rpc.c src\rdbms_tools.c src\db_sqlite.c /link /LTCG /OPT:REF /OPT:ICF /OUT:bin\sqlite-mcp-server.exe
if %ERRORLEVEL% NEQ 0 (
    echo [ERROR] Server compilation failed.
    exit /b 1
)

echo [INFO] Compiling Test Runner...
cl.exe /nologo /std:c11 /O2 /GL /Gy /Gw /MD /D_CRT_SECURE_NO_WARNINGS /D_CRT_NONSTDC_NO_DEPRECATE /Iinclude src\test_runner.c /link /LTCG /OPT:REF /OPT:ICF /OUT:bin\mcp_test_runner.exe
if %ERRORLEVEL% NEQ 0 (
    echo [ERROR] Test runner compilation failed.
    exit /b 1
)

rem echo ===================================================
rem echo Running Integration Tests...
rem echo ===================================================
rem bin\mcp_test_runner.exe bin\sqlite-mcp-server.exe
rem if %ERRORLEVEL% EQU 0 (
rem     echo [SUCCESS] Build and all integration tests passed!
rem     exit /b 0
rem ) else (
rem     echo [ERROR] Integration tests failed.
rem     exit /b 1
rem )

:TRY_CMAKE
where cmake >nul 2>nul
if %ERRORLEVEL% NEQ 0 goto NO_TOOLS

echo [INFO] cl.exe not in PATH, attempting CMake build...
if not exist build mkdir build
cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
if %ERRORLEVEL% NEQ 0 (
    cd ..
    echo [ERROR] CMake configuration failed.
    exit /b 1
)
cmake --build . --config Release
if %ERRORLEVEL% NEQ 0 (
    cd ..
    echo [ERROR] CMake build failed.
    exit /b 1
)
rem Release\mcp_test_runner.exe Release\sqlite-mcp-server.exe
if %ERRORLEVEL% EQU 0 (
    cd ..
    echo [SUCCESS] CMake build passed!
    exit /b 0
)
cd ..
exit /b 1

:NO_TOOLS
echo [ERROR] Neither MSVC (cl.exe) nor CMake was found in PATH.
echo [INFO] Please run this batch script from Developer Command Prompt for Visual Studio.
exit /b 1
