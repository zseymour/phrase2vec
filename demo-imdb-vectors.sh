make
cwd=$(pwd)
if [ ! -e aclImdb ]; then
  wget http://ai.stanford.edu/~amaas/data/sentiment/aclImdb_v1.tar.gz
  tar -zxf aclImdb_v1.tar.gz
  rm aclImdb_v1.tar.gz
  echo "Preprocessing files...."
  cd aclImdb/train
  pwd
  perl -pl -i.bak -e '$_ = lc' neg/*.txt pos/*.txt unsup/*.txt 
  perl -pl -i.bak -e 's/([[:punct:]])/ \1 /g' neg/*.txt pos/*.txt unsup/*.txt 
  cd ../test
  pwd
  perl -pl -i.bak -e '$_ = lc' pos/*.txt neg/*.txt
  perl -pl -i.bak -e 's/([[:punct:]])/ \1 /g' pos/*.txt neg/*.txt
  cd $cwd
  pwd
  find . -type f -name "*.bak" -exec rm -f {} \;
  echo "Finished preprocessing"
fi
time ./phrase2vec -train-dir aclImdb/train/ -test-dir aclImdb/test/ -output imdb-word-vectors.bin -nn-train imdbtrain.data -nn-test imdbtest.data -size 400 -window 10 -sample 1e-3 -threads 12 -binary 1
echo "============PHRASE2VEC ACCURACY================"
./paragraph_nn -train imdbtrain.data -test imdbtest.data -output imdb.net -epochs 1000

