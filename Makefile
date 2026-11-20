TARGET  := dirtree
SRC     := src/main.c
OBJ     := $(SRC:.c=.o)

PKGS    := gtk+-3.0

CC      ?= cc
CFLAGS  ?= -O2 -g
CFLAGS  += -Wall -Wextra -Wpedantic -std=c11
CFLAGS  += $(shell pkg-config --cflags $(PKGS))
LDLIBS  += $(shell pkg-config --libs $(PKGS))

.PHONY: all clean run

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

run: $(TARGET)
	./$(TARGET)

clean:
	rm -f $(TARGET) $(OBJ)
