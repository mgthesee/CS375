CC = gcc
CFLAGS = -g

all: main wc map

main: main.c
	$(CC) $(CFLAGS) main.c -o main

wc: wc.c
	$(CC) $(CFLAGS) wc.c -o wc

map: map.c recurse.c
	$(CC) $(CFLAGS) map.c recurse.c -o map

clean:
	rm -f main wc map
