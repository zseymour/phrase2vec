make
if [ ! -e text8 ]; then
  wget http://mattmahoney.net/dc/text8.zip -O text8.gz
  gzip -d text8.gz -f
fi
time ./word2vec -train text8 -output word-vectors.bin -cbow 0 -size 200 -window 5 -negative 0 -hs 1 -sample 1e-3 -threads 12 -binary 1
time ./phrase2vec -train-file text8 -output phrase-vectors.bin -nn-train para.data -size 200 -window 5 -dbow 0 -threads 12 -binary 1
echo "=============WORD2VEC ACCURACY================="
./compute-accuracy word-vectors.bin 30000 < questions-words.txt
echo "============PHRASE2VEC ACCURACY================"
./compute-accuracy phrase-vectors.bin 30000 < questions-words.txt
