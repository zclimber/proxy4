all:
	g++ -std=c++14 -pthread -O0 -g -fsanitize=address,undefined *.cpp

.PHONY: all
