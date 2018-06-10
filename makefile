all:
	g++ -std=c++14 -pthread -O0 -g -fsanitize=address,undefined -fsanitize-undefined-trap-on-error *.cpp

opt:
	g++ -std=c++14 -pthread -O2 -fsanitize=address,undefined -fsanitize-undefined-trap-on-error *.cpp

.PHONY: all opt
