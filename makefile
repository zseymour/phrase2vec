CC = gcc
#The -Ofast might not work with older versions of gcc; in that case, use -O2
CFLAGS = -lm -pthread -Ofast -march=native -Wall -funroll-loops -Wno-unused-result

all: phrase2vec paragraph_nn distance word-analogy compute-accuracy word2vec

phrase2vec : phrase2vec.c
	$(CC) phrase2vec.c -o phrase2vec $(CFLAGS)
paragraph_nn : paragraph_nn.c
	$(CC) -O3 $< -o $@ -lfann -lm
distance : distance.c
	$(CC) distance.c -o distance $(CFLAGS)
word-analogy : word-analogy.c
	$(CC) word-analogy.c -o word-analogy $(CFLAGS)
compute-accuracy : compute-accuracy.c
	$(CC) compute-accuracy.c -o compute-accuracy $(CFLAGS)
word2vec : word2vec.c
	$(CC) word2vec.c -o word2vec $(CFLAGS)
clean:
	rm -rf phrase2vec paragraph_nn distance word-analogy compute-accuracy
