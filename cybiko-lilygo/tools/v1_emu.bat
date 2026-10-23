@echo off
rem SDL Classic for Windows (MSYS2 MinGW or similar gcc).
rem   tools\v1_emu.bat boot.bin flash.bin [--nvram save.flash] [--radio COM3] [--cyid HEX]
rem Omit --radio to auto-pick the first Heltec CP2102 (VID 10C4 PID EA60).
rem Needs: mingw-w64 gcc, SDL2, libpng. PlatformIO for Settings flash.
rem   pacman -S mingw-w64-x86_64-gcc mingw-w64-x86_64-SDL2 mingw-w64-x86_64-libpng
setlocal
set HERE=%~dp0..
set CORE=%HERE%\lib\cybiko_core\src
set RADIO=%HERE%\..\cybiko-c-emulator\src\radio
if not defined OUT set OUT=%HERE%\.pio\v1_emu.exe
if not exist "%HERE%\.pio" mkdir "%HERE%\.pio"
if not defined STRIDE set STRIDE=12
gcc -std=c11 -D_DEFAULT_SOURCE -DEMU_TIMER_STRIDE=%STRIDE% -DCYBIKO_MACHINE_V1 -O2 -Wall -Wextra ^
  -I"%CORE%" -I"%CORE%\core" -I"%RADIO%" -I"%HERE%\tools" ^
  -o "%OUT%" "%HERE%\tools\v1_emu.c" "%HERE%\tools\host_serial.c" ^
  "%CORE%\cybiko_core_unity.c" ^
  "%RADIO%\sci2_xt.c" "%RADIO%\sci0_v1.c" "%RADIO%\radio_coproc.c" ^
  "%RADIO%\radio_link.c" "%RADIO%\radio_lora.c" "%RADIO%\radio_serial_phy.c" ^
  -lmingw32 -lSDL2main -lSDL2 -lpng -lsetupapi -lwinmm -limm32 -lole32 -loleaut32 -lversion -luuid -lgdi32 -luser32 -ladvapi32 -lshell32
if errorlevel 1 exit /b 1
"%OUT%" %*
