CC = gcc
#The -Ofast might not work with older versions of gcc; in that case, use -O2
CFLAGS = -lm -pthread -O0 -ggdb -march=native -Wall -funroll-loops -Wno-unused-result

all: phrase2vec paragraph_nn

phrase2vec : phrase2vec.c
	$(CC) phrase2vec.c -o phrase2vec $(CFLAGS)

paragraph_nn : paragraph_nn.c
	$(CC) -O3 $< -o $@ -lfann -lm

clean:
	rm -rf phrase2vec
