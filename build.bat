@echo off
echo ===================================================
echo Building Standalone SQLite MCP Server for Windows
echo ===================================================

where cl >nul 2>nul
if %ERRORLEVEL% NEQ 0 goto TRY_CMAKE

echo [INFO] Found MSVC compiler (cl.exe). Compiling Release binary...
if not exist bin mkdir bin
cl.exe /nologo /std:c11 /O2 /GL /Gy /Gw /MD /DNDEBUG /DSQLITE_THREADSAFE=0 /DSQLITE_OMIT_LOAD_EXTENSION /D_CRT_SECURE_NO_WARNINGS /D_CRT_NONSTDC_NO_DEPRECATE /Iinclude src\main.c src\json_rpc.c src\rdbms_tools.c src\db_sqlite.c src\sqlite3.c /link /LTCG /OPT:REF /OPT:ICF /OUT:bin\sqlite-mcp-server.exe
if %ERRORLEVEL% EQU 0 (
    echo [SUCCESS] Binary compiled successfully: bin\sqlite-mcp-server.exe
    exit /b 0
)
echo [ERROR] MSVC compilation failed.
exit /b 1

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
cd ..
if %ERRORLEVEL% EQU 0 (
    echo [SUCCESS] CMake build completed successfully.
    exit /b 0
)
exit /b 1

:NO_TOOLS
echo [ERROR] Neither MSVC (cl.exe) nor CMake was found in PATH.
echo [INFO] Please run this batch script from Developer Command Prompt for Visual Studio.
exit /b 1
