.PHONY: all
all:
	gcc -O3 -Wno-int-conversion -o code main.c buddy.c
