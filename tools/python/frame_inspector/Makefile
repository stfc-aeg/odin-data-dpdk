CC = g++
CFLAGS = -fPIC -Wall -Werror $(shell pkg-config --cflags libdpdk)
LDFLAGS = $(shell pkg-config --libs libdpdk)

SRC_DIR = cpp/src
INCLUDE_DIR = cpp/include
LIB_DIR = lib

TARGET = $(LIB_DIR)/libdpdk_wrapper.so

all: $(TARGET)

$(TARGET): $(SRC_DIR)/dpdk_wrapper.cpp
	@mkdir -p $(LIB_DIR)
	$(CC) $(CFLAGS) -I$(INCLUDE_DIR) -shared -o $@ $< $(LDFLAGS)

clean:
	rm -f $(TARGET)

.PHONY: all clean