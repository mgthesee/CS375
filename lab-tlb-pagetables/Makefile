CXX = g++
CXXFLAGS = -std=c++17 -O2 -Wall

all: lab generate

lab: main.cpp
	$(CXX) $(CXXFLAGS) -o lab main.cpp

generate: generate_addresses.cpp
	$(CXX) $(CXXFLAGS) -o generate generate_addresses.cpp

run: lab generate
	./generate
	./lab

experiment: lab generate
	./generate
	./lab --experiment

clean:
	rm -f lab generate addresses.txt *.csv
