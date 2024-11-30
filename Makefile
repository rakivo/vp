CC := g++
BIN := vp
CLIBS := -lraylib -lavformat -lavcodec -lavutil -lswscale -ltbb -lswresample -lavfilter
WFLAGS := -Wall -Wextra -Wpedantic -Wswitch-enum -Wno-missing-field-initializers
CFLAGS := -std=c++20 -g -O2

$(BIN): main.cpp
	$(CC) -o $(BIN) $< $(CLIBS) $(CFLAGS) $(WFLAGS)
