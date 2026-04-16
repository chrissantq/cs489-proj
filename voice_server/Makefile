# Compiler
CXX = g++
CXXFLAGS = -O2 -std=c++17 -Wall -Werror -I../whisper.cpp/include \
					 -I../whisper.cpp/ggml/include

# Paths
WHISPER_DIR = ../whisper.cpp
WHISPER_BUILD = $(WHISPER_DIR)/build/src

SRC = voice_server.cc
BIN = voice_server

# Default target
all: $(BIN)

# Build whisper (if needed)
$(WHISPER_BUILD)/libwhisper.so:
	$(MAKE) -C $(WHISPER_DIR)

# Build server
$(BIN): $(SRC) $(WHISPER_BUILD)/libwhisper.so
	$(CXX) $(SRC) $(CXXFLAGS) -L$(WHISPER_BUILD) -lwhisper -Wl,-rpath=$(WHISPER_BUILD) -o $(BIN)

# Clean
clean:
	rm -f $(BIN)

# Rebuild
rebuild: clean all
