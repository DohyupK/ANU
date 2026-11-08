# Country Search Map — MinGW / Windows
# 사용: mingw32-make   또는   make

CC      = gcc
RAY     = deps/raylib-5.5_win64_mingw-w64
CFLAGS  = -O2 -I$(RAY)/include
LDFLAGS = -L$(RAY)/lib -lraylib -lopengl32 -lgdi32 -lwinmm
TARGET  = country.exe
SRCS    = country.c sqlite3.c

.PHONY: all clean run

all: $(TARGET) raylib.dll

$(TARGET): $(SRCS)
	$(CC) $(SRCS) -o $(TARGET) $(CFLAGS) $(LDFLAGS)

# 실행 파일과 같은 폴더에 DLL 배치 (런타임 로드용)
raylib.dll: $(RAY)/lib/raylib.dll
	cmd /c copy /Y "$(subst /,\,$(RAY))\lib\raylib.dll" raylib.dll

run: all
	./$(TARGET)

clean:
	cmd /c del /Q $(TARGET) raylib.dll countries.db 2>nul || exit 0
