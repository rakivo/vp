CC := g++
BIN := vp
CLIBS := -lraylib -lavformat -lavcodec -lavutil -lswscale -ltbb -lswresample
WFLAGS := -Wall -Wextra -Wpedantic -Wswitch-enum
CFLAGS := -std=c++20 -O3 -g

$(BIN): main.cpp
	$(CC) -o $(BIN) $< $(CLIBS) $(CFLAGS) $(WFLAGS)
