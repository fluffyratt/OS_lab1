@echo off
setlocal

cd /d "%~dp0"

set "BIN=%~dp0..\ServerBlocking\x64\Release"
set "SERVER=%BIN%\ServerAsync.exe"
set "CLIENT=%BIN%\ClientAsync.exe"

set "OUT=results_async.csv"
set "TMP=tmp_async.txt"

echo impl,transport,mode,size,conns,count,secs,rate_per_s,mb_per_s,X_us,Y_us,Z_us,failed>"%OUT%"

echo ========================================
echo ASYNC SOCKET BENCHMARK
echo ========================================

call :transport tcp tcp:127.0.0.1:9000
call :transport unix unix:bench.sock

del "%TMP%" >nul 2>&1

echo.
echo ========================================
echo DONE
echo Results:
echo %OUT%
echo ========================================

pause
exit /b


:transport

set "NAME=%~1"
set "ADDR=%~2"

echo.
echo ----------------------------------------
echo Starting %NAME% async server
echo ----------------------------------------

taskkill /F /IM ServerAsync.exe >nul 2>&1
del bench.sock >nul 2>&1

start "" /B "%SERVER%" %ADDR%

timeout /t 1 /nobreak >nul


rem ========================================
rem SMALL MESSAGES - 64 B
rem ========================================

call :run "%ADDR%" echo 64 200000 1
call :run "%ADDR%" echo 64 50000 4
call :run "%ADDR%" echo 64 20000 16


rem ========================================
rem MEDIUM MESSAGES - 1 KB
rem ========================================

call :run "%ADDR%" echo 1024 100000 1
call :run "%ADDR%" echo 1024 25000 4
call :run "%ADDR%" echo 1024 10000 16


rem ========================================
rem LARGE MESSAGES - 64 KB
rem ========================================

call :run "%ADDR%" echo 65536 20000 1
call :run "%ADDR%" echo 65536 5000 4
call :run "%ADDR%" echo 65536 2000 16


rem ========================================
rem VERY LARGE MESSAGES - 1 MB
rem ========================================

call :run "%ADDR%" echo 1048576 500 1
call :run "%ADDR%" echo 1048576 200 4


rem ========================================
rem CONNECTION SETUP
rem In async implementation asynchronous I/O
rem is used for WSARecv / WSASend.
rem connect() is measured separately.
rem ========================================

call :run "%ADDR%" conn 0 1000 1


taskkill /F /IM ServerAsync.exe >nul 2>&1
del bench.sock >nul 2>&1

timeout /t 1 /nobreak >nul

exit /b


:run

echo.
echo Address=%~1 mode=%~2 size=%~3 n=%~4 conns=%~5

"%CLIENT%" ^
-a %~1 ^
-m %~2 ^
-s %~3 ^
-n %~4 ^
-c %~5 > "%TMP%"

type "%TMP%"

for /f "usebackq tokens=1,* delims=," %%A in ("%TMP%") do (
    if "%%A"=="CSV" (
        echo %%B>>"%OUT%"
    )
)

exit /b