CROSS_COMPILE ?=
CC = $(CROSS_COMPILE)gcc

SRC = writer.c
OBJ = writer.o

writer:writer.o
	$(CC) -o $@ $^

writer.o:writer.c
	$(CC) -c -o $@ $<
.PHONY: clean
clean:
	rm -f writer writer.o
