# Build configuration for Terminator (X11 terminal emulator)

CC       ?= gcc
CFLAGS   ?= -Wall -Wextra -pthread
LDFLAGS  ?=
LDLIBS   ?= -lX11 -lrt

TARGET   := terminator
SOURCE   := temp.c
OBJECTS  := $(SOURCE:.c=.o)

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $(OBJECTS) $(LDLIBS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(TARGET) $(OBJECTS)
