CC	= gcc

SRC_DIR		:= ./

C_SRC	:= $(shell find $(SRC_DIR) -name '*.c' -print)

TARGET		:= libcacao.so

CFLAGS	:= -Wall -O3 -g -fPIC
LDFLAGS	:= -shared

all: $(TARGET)

C_STANDARD	:= -std=gnu11

OBJECTS 	:= $(C_SRC:.c=.o)

$(TARGET): $(OBJECTS)
	$(CC) $(CFLAGS) $(C_STANDARD) $^ -o $@ $(LDFLAGS)

clean:
	@rm -rf $(OBJECTS) $(TARGET)
