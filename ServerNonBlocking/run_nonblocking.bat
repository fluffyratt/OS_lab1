@echo off
setlocal

cd /d "%~dp0"

set "BIN=%~dp0x64\Release"
set "SERVER=%BIN%\ServerNonBlocking.exe"
set "CLIENT=%BIN%\ClientNonBlocking.exe"
set "OUT=%~dp0results_nonblocking.csv"

echo impl,transport,mode,size,conns,count,secs,rate_per_s,mb_per_s,X_us,Y_us,Z_us,failed>"%OUT%"

echo ========================================
echo NON-BLOCKING SOCKET BENCHMARK
echo ========================================

call :transport tcp tcp:127.0.0.1:9000
call :transport unix unix:bench.sock

echo.
echo ========================================
echo DONE
echo Results: %OUT%
echo ========================================
pause
exit /b


:transport
set "NAME=%~1"
set "ADDR=%~2"

echo.
echo ----------------------------------------
echo Starting %NAME% server
echo ----------------------------------------

taskkill /F /IM ServerNonBlocking.exe >nul 2>&1
del bench.sock >nul 2>&1

start "" /B "%SERVER%" %ADDR%

timeout /t 1 /nobreak >nul

rem ===== SMALL MESSAGES =====

call :run "%ADDR%" echo 64 200000 1
call :run "%ADDR%" echo 64 50000 4
call :run "%ADDR%" echo 64 20000 16

rem ===== MEDIUM MESSAGES =====

call :run "%ADDR%" echo 1024 100000 1
call :run "%ADDR%" echo 1024 25000 4
call :run "%ADDR%" echo 1024 10000 16

rem ===== LARGE MESSAGES =====

call :run "%ADDR%" echo 65536 20000 1
call :run "%ADDR%" echo 65536 5000 4
call :run "%ADDR%" echo 65536 2000 16

rem ===== VERY LARGE MESSAGES =====

call :run "%ADDR%" echo 1048576 500 1
call :run "%ADDR%" echo 1048576 200 4

rem ===== CONNECTION SETUP =====

call :run "%ADDR%" conn 0 1000 1

taskkill /F /IM ServerNonBlocking.exe >nul 2>&1
del bench.sock >nul 2>&1

timeout /t 1 /nobreak >nul

exit /b


:run
echo.
echo %~1 mode=%~2 size=%~3 n=%~4 conns=%~5

"%CLIENT%" -a %~1 -m %~2 -s %~3 -n %~4 -c %~5 > tmp_nonblocking.txt

type tmp_nonblocking.txt

for /f "tokens=1,* delims=," %%A in ('findstr /B "CSV," tmp_nonblocking.txt') do (
    echo %%B>>"%OUT%"
)

exit /b