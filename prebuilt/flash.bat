@echo off
cd /d "%~dp0"
set FW=goblin_buddy_v0.1.3.bin
echo ============================================
echo  Network Goblin DB-1  -  flashing %FW%
echo ============================================
echo  Back up your stock firmware first! (see README)
echo.
set /p PORT=Which COM port? (e.g. COM5): 
python -m esptool --chip esp32c2 -p %PORT% -b 460800 write-flash 0x0 bootloader.bin 0x8000 partition-table.bin 0xf000 ota_data_initial.bin 0x20000 %FW%
if errorlevel 1 (
  echo.
  echo *** FLASH FAILED - read the error above. ***
  echo Close anything else using %PORT% and try another USB cable/port.
) else (
  echo.
  echo *** SUCCESS - look for the Goblin-Setup hotspot in ~45 s ***
)
pause
