CC := g++
BIN := vp
CLIBS := -lraylib -lavformat -lavcodec -lavutil -lswscale -ltbb -lswresample
CFLAGS := -O3 -g

$(BIN): main.cpp
	$(CC) -o $(BIN) $< $(CLIBS) $(CFLAGS)
