CC = gcc
CXX = g++
CFLAGS = -Wall -Wextra
CXXFLAGS = -Wall -Wextra
LDFLAGS = -pthread

TARGETS = server_ex1 client_ex1 \
          server_ex2 client_ex2 \
          server_ex3 client_ex3 \
          server_ex4 client_ex4 \
          server_ex5 client_ex5 \
          server_ex6 client_ex6 \
          server_ex7 client_ex7

all: $(TARGETS)

server_ex1: server_ex1.c
	$(CC) $(CFLAGS) -o $@ $<

client_ex1: client_ex1.c
	$(CC) $(CFLAGS) -o $@ $<

server_ex2: server_ex2.cpp
	$(CXX) $(CXXFLAGS) -o $@ $<

client_ex2: client_ex2.cpp
	$(CXX) $(CXXFLAGS) -o $@ $<

server_ex3: server_ex3.c
	$(CC) $(CFLAGS) -o $@ $<

client_ex3: client_ex3.c
	$(CC) $(CFLAGS) -o $@ $<

server_ex4: server_ex4.cpp
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ $<

client_ex4: client_ex2.cpp
	$(CXX) $(CXXFLAGS) -o $@ $<

server_ex5: server_ex5.c
	$(CC) $(CFLAGS) -o $@ $<

client_ex5: client_ex1.c
	$(CC) $(CFLAGS) -o $@ $<

server_ex6: server_ex6.cpp
	$(CXX) $(CXXFLAGS) -o $@ $<

client_ex6: client_ex2.cpp
	$(CXX) $(CXXFLAGS) -o $@ $<

server_ex7: server_ex7.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<

client_ex7: client_ex7.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f $(TARGETS) server_errors.log

.PHONY: all clean
