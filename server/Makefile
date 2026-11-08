
CROSS_COMPILE ?=
CC = $(CROSS_COMPILE)gcc

CFLAGS ?= -Wall -Wextra -Werror -O0 -g
LDFLAGS ?= 

.PHONY: all clean 

all: aesdsocket


aesdsocket:aesdsocket.o
	$(CC) -o $@ $^ $(LDFLAGS)
aesdsocket.o:aesdsocket.c
	$(CC) -c -o $@ $< $(CFLAGS)
	
clean:
	rm -f aesdsocket aesdsocket.o
