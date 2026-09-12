CC = gcc
CFLAGS = -g -Wall

all: main wc map

main: limits.c
	$(CC) $(CFLAGS) limits.c -o main

wc: wc.c
	$(CC) $(CFLAGS) wc.c -o wc

map: map.c recurse.c
	$(CC) $(CFLAGS) map.c recurse.c -o map

clean:
	rm -f main wc map
