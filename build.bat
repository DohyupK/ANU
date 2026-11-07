@echo off
set RAY=deps\raylib-5.5_win64_mingw-w64
gcc country.c sqlite3.c -o country.exe -I "%RAY%\include" -L "%RAY%\lib" -lraylib -lopengl32 -lgdi32 -lwinmm -O2
if errorlevel 1 exit /b 1
copy /Y "%RAY%\lib\raylib.dll" raylib.dll >nul
echo BUILD OK
