//  Copyright 2013 Google Inc. All Rights Reserved.
//
//  Licensed under the Apache License, Version 2.0 (the "License");
//  you may not use this file except in compliance with the License.
//  You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
//  Unless required by applicable law or agreed to in writing, software
//  distributed under the License is distributed on an "AS IS" BASIS,
//  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
//  See the License for the specific language governing permissions and
//  limitations under the License.

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <pthread.h>
#include <dirent.h>
#include <limits.h>
#include <fenv.h>


#define MAX_STRING 100
#define EXP_TABLE_SIZE 1000
#define MAX_EXP 6
#define MAX_SENTENCE_LENGTH 2000
#define MAX_CODE_LENGTH 40
#define EPSILON 1e-2

const int vocab_hash_size = 30000000;  // Maximum 30 * 0.7 = 21M words in the vocabulary

typedef float real;                    // Precision of float numbers

struct vocab_word {
  long long cn;
  int *point;
  char *word, *code, codelen;
};

struct paragraph {
  long long word_count;
  real *dbow_vector;
  real *dm_vector;
  int label_index;
  char *file_name;
  char **words;
};

char train_file[MAX_STRING], train_dir[MAX_STRING], test_file[MAX_STRING], test_dir[MAX_STRING], word_output_file[MAX_STRING], phrase_output_file[MAX_STRING];
//char save_vocab_file[MAX_STRING], read_vocab_file[MAX_STRING];
char para_file[MAX_STRING], para_file_test[MAX_STRING];
struct vocab_word *vocab;
struct paragraph *phrases;
char **labels;
int binary = 0, model = 2, debug_mode = 2, window = 5, min_count = 5, num_threads = 1, min_reduce = 1;
int *vocab_hash;
long long vocab_max_size = 10000, vocab_size = 0, phrase_max_size = 100000, phrase_size = 0, word_features = 200, paragraph_features = 200, layer1_size = 2000, label_max_size = 10, label_size = 0, labeled_instances = 0;
long long train_words = 0, word_count_actual = 0, sentence_count_actual = 0, file_size = 0, classes = 0;
real alpha = 0.025, initial_alpha, starting_alpha, sample = 0;
real *syn0, *syn1, *syn1neg, *expTable;
clock_t start;
const char* model_names[] = {"PV-DM", "PV-DBOW", "PV-DM + PV-DBOW"};
int hs = 1, negative = 0, freeze_words = 0;
const int table_size = 1e8;
int *table;

void ShufflePhrases() {
  size_t size = sizeof(struct paragraph);
  char tmp[size];
  char *arr = (char *)phrases;
  size_t stride = size * sizeof(char);

  if(phrase_size > 1) {
    size_t i;
    for (i = 0; i < phrase_size - 1; ++i) {
      size_t rnd = (size_t) rand();
      size_t j = i + rnd / (RAND_MAX / (phrase_size - i) + 1);

      memcpy(tmp, arr + j * stride, size);
      memcpy(arr + j * stride, arr + i * stride, size);
      memcpy(arr + i * stride, tmp, size);
    }
  }
}

int IsConverged(real* old_vector, real* new_vector, long long size) {
  long long i;
  for(i = 0; i < size; i++) {
    if (fabs(new_vector[i]-old_vector[i]) > EPSILON) {
      return 0;
    }
  }

  return 1;
}
void InitUnigramTable() {
  int a, i;
  long long train_words_pow = 0;
  real d1, power = 0.75;
  table = (int *)malloc(table_size * sizeof(int));
  for (a = 0; a < vocab_size; a++) train_words_pow += pow(vocab[a].cn, power);
  i = 0;
  d1 = pow(vocab[i].cn, power) / (real)train_words_pow;
  for (a = 0; a < table_size; a++) {
    table[a] = i;
    if (a / (real)table_size > d1) {
      i++;
      d1 += pow(vocab[i].cn, power) / (real)train_words_pow;
    }
    if (i >= vocab_size) i = vocab_size - 1;
  }
}

// Reads a single word from a file, assuming space + tab + EOL to be word boundaries
void ReadWord(char *word, FILE *fin) {
  int a = 0, ch;
  while (!feof(fin)) {
    ch = fgetc(fin);
    if (ch == 13) continue;
    if ((ch == ' ') || (ch == '\t') || (ch == '\n')) {
      if (a > 0) {
        if (ch == '\n') ungetc(ch, fin);
        break;
      }
      if (ch == '\n') {
        strcpy(word, (char *)"</s>");
        return;
      } else continue;
    }
    word[a] = ch;
    a++;
    if (a >= MAX_STRING - 1) a--;   // Truncate too long words
  }
  word[a] = 0;
}

// Returns hash value of a word
int GetWordHash(char *word) {
  unsigned long long a, hash = 0;
  for (a = 0; a < strlen(word); a++) hash = hash * 257 + word[a];
  hash = hash % vocab_hash_size;
  return hash;
}

// Returns position of a word in the vocabulary; if the word is not found, returns -1
int SearchVocab(char *word) {
  unsigned int hash = GetWordHash(word);
  while (1) {
    if (vocab_hash[hash] == -1) return -1;
    if (!strcmp(word, vocab[vocab_hash[hash]].word)) return vocab_hash[hash];
    hash = (hash + 1) % vocab_hash_size;
  }
  return -1;
}

int SearchLabels(const char *label) {
  long long i;
  for (i = 0; i < label_size; i++) {
    if(strcmp(label, labels[i]) == 0) return i;
  }
  return -1;
}

int AddLabel(const char *label) {
  if(strcmp(label, "unsup") == 0) return -1;
  int index = SearchLabels(label);
  if(index != -1) return index;
  
  unsigned int length = strlen(label) + 1;
  labels[label_size] = (char *)calloc(length, sizeof(char));
  strcpy(labels[label_size], label);
  label_size++;
  if(label_size + 2 >= label_max_size) {
    label_max_size += 10;
    labels = (char **)realloc(labels, label_max_size * sizeof(char *));
  }
  return label_size - 1;
}

// Reads a word and returns its index in the vocabulary
int ReadWordIndex(FILE *fin) {
  char word[MAX_STRING];
  ReadWord(word, fin);
  if (feof(fin)) return -1;
  return SearchVocab(word);
}

// Adds a word to the vocabulary
int AddWordToVocab(char *word) {
  unsigned int hash, length = strlen(word) + 1;
  if (length > MAX_STRING) length = MAX_STRING;
  vocab[vocab_size].word = (char *)calloc(length, sizeof(char));
  strcpy(vocab[vocab_size].word, word);
  vocab[vocab_size].cn = 0;
  vocab_size++;
  // Reallocate memory if needed
  if (vocab_size + 2 >= vocab_max_size) {
    vocab_max_size += 1000;
    vocab = (struct vocab_word *)realloc(vocab, vocab_max_size * sizeof(struct vocab_word));
  }
  hash = GetWordHash(word);
  while (vocab_hash[hash] != -1) hash = (hash + 1) % vocab_hash_size;
  vocab_hash[hash] = vocab_size - 1;
  return vocab_size - 1;
}

int AddWordToSentence(int index, char *word) {
  if(phrases[index].word_count >= MAX_SENTENCE_LENGTH) return -1;
  unsigned int length = strlen(word) + 1;
  if (length > MAX_STRING) length = MAX_STRING;
  long long sen_index = phrases[index].word_count;
  //printf("Adding %s, the %lldth word of sentence #%d.\n", word, sen_index, index);
  phrases[index].words[sen_index] = (char *)calloc(length, sizeof(char));
  strcpy(phrases[index].words[sen_index], word);
  phrases[index].word_count++;
  return phrases[index].word_count - 1;
}

int AddSentenceToPhrases(const char *file_name, const char *label) {
  int index = AddLabel(label);
  phrases[phrase_size].label_index = index;

  unsigned int length = strlen(file_name) + 1;
  if(length > PATH_MAX) length = PATH_MAX;
  phrases[phrase_size].file_name = (char *)calloc(length, sizeof(char));
  strcpy(phrases[phrase_size].file_name, file_name);
  phrases[phrase_size].word_count = 0;
  phrases[phrase_size].words = (char **)calloc(MAX_SENTENCE_LENGTH, sizeof(char *));
  phrase_size++;
  if(strcmp(label, "unsup") != 0) labeled_instances++;
  if(phrase_size + 2 >= phrase_max_size) {
    phrase_max_size += 10000;
    phrases = (struct paragraph *)realloc(phrases, phrase_max_size * sizeof(struct paragraph));
  }
  return phrase_size - 1;
}

// Used later for sorting by word counts
int VocabCompare(const void *a, const void *b) {
    return ((struct vocab_word *)b)->cn - ((struct vocab_word *)a)->cn;
}

// Sorts the vocabulary by frequency using word counts
void SortVocab() {
  int a, size;
  unsigned int hash;
  // Sort the vocabulary and keep </s> at the first position
  qsort(&vocab[1], vocab_size - 1, sizeof(struct vocab_word), VocabCompare);
  for (a = 0; a < vocab_hash_size; a++) vocab_hash[a] = -1;
  size = vocab_size;
  train_words = 0;
  for (a = 0; a < size; a++) {
    // Words occuring less than min_count times will be discarded from the vocab
    if (vocab[a].cn < min_count) {
      vocab_size--;
      free(vocab[vocab_size].word);
    } else {
      // Hash will be re-computed, as after the sorting it is not actual
      hash=GetWordHash(vocab[a].word);
      while (vocab_hash[hash] != -1) hash = (hash + 1) % vocab_hash_size;
      vocab_hash[hash] = a;
      train_words += vocab[a].cn;
    }
  }
  vocab = (struct vocab_word *)realloc(vocab, (vocab_size + 1) * sizeof(struct vocab_word));
  // Allocate memory for the binary tree construction
  for (a = 0; a < vocab_size; a++) {
    vocab[a].code = (char *)calloc(MAX_CODE_LENGTH, sizeof(char));
    vocab[a].point = (int *)calloc(MAX_CODE_LENGTH, sizeof(int));
  }
}

// Reduces the vocabulary by removing infrequent tokens
void ReduceVocab() {
  int a, b = 0;
  unsigned int hash;
  for (a = 0; a < vocab_size; a++) if (vocab[a].cn > min_reduce) {
    vocab[b].cn = vocab[a].cn;
    vocab[b].word = vocab[a].word;
    b++;
  } else free(vocab[a].word);
  vocab_size = b;
  for (a = 0; a < vocab_hash_size; a++) vocab_hash[a] = -1;
  for (a = 0; a < vocab_size; a++) {
    // Hash will be re-computed, as it is not actual
    hash = GetWordHash(vocab[a].word);
    while (vocab_hash[hash] != -1) hash = (hash + 1) % vocab_hash_size;
    vocab_hash[hash] = a;
  }
  fflush(stdout);
  min_reduce++;
}

// Create binary Huffman tree using the word counts
// Frequent words will have short uniqe binary codes
void CreateBinaryTree() {
  long long a, b, i, min1i, min2i, pos1, pos2, point[MAX_CODE_LENGTH];
  char code[MAX_CODE_LENGTH];
  long long *count = (long long *)calloc(vocab_size * 2 + 1, sizeof(long long));
  long long *binary = (long long *)calloc(vocab_size * 2 + 1, sizeof(long long));
  long long *parent_node = (long long *)calloc(vocab_size * 2 + 1, sizeof(long long));
  for (a = 0; a < vocab_size; a++) count[a] = vocab[a].cn;
  for (a = vocab_size; a < vocab_size * 2; a++) count[a] = 1e15;
  pos1 = vocab_size - 1;
  pos2 = vocab_size;
  // Following algorithm constructs the Huffman tree by adding one node at a time
  for (a = 0; a < vocab_size - 1; a++) {
    // First, find two smallest nodes 'min1, min2'
    if (pos1 >= 0) {
      if (count[pos1] < count[pos2]) {
        min1i = pos1;
        pos1--;
      } else {
        min1i = pos2;
        pos2++;
      }
    } else {
      min1i = pos2;
      pos2++;
    }
    if (pos1 >= 0) {
      if (count[pos1] < count[pos2]) {
        min2i = pos1;
        pos1--;
      } else {
        min2i = pos2;
        pos2++;
      }
    } else {
      min2i = pos2;
      pos2++;
    }
    count[vocab_size + a] = count[min1i] + count[min2i];
    parent_node[min1i] = vocab_size + a;
    parent_node[min2i] = vocab_size + a;
    binary[min2i] = 1;
  }
  // Now assign binary code to each vocabulary word
  for (a = 0; a < vocab_size; a++) {
    b = a;
    i = 0;
    while (1) {
      code[i] = binary[b];
      point[i] = b;
      i++;
      b = parent_node[b];
      if (b == vocab_size * 2 - 2) break;
    }
    vocab[a].codelen = i;
    vocab[a].point[0] = vocab_size - 2;
    for (b = 0; b < i; b++) {
      vocab[a].code[i - b - 1] = code[b];
      vocab[a].point[i - b] = point[b] - vocab_size;
    }
  }
  free(count);
  free(binary);
  free(parent_node);
}

void LearnVocabFromTrainFile() {
  char word[MAX_STRING];
  FILE *fin;
  long long a, i;
  for (a = 0; a < vocab_hash_size; a++) vocab_hash[a] = -1;
  fin = fopen(train_file, "rb");
  if (fin == NULL) {
    printf("ERROR: training data file not found!\n");
    exit(1);
  }
  vocab_size = 0;
  long long eol_index = AddWordToVocab((char *)"</s>");
  long long curr_sen = AddSentenceToPhrases("none","none");
  while (1) {
    ReadWord(word, fin);
    if (feof(fin)) break;
    train_words++;
    if ((debug_mode > 1) && (train_words % 100000 == 0)) {
      printf("%lldK%c", train_words / 1000, 13);
      fflush(stdout);
    }
    i = SearchVocab(word);
    if (i == -1) {
      a = AddWordToVocab(word);
      vocab[a].cn = 1;
    } else vocab[i].cn++;
    if(i == eol_index || (train_words % MAX_SENTENCE_LENGTH == 0)) 
      curr_sen = AddSentenceToPhrases(train_file, "none");
    else AddWordToSentence(curr_sen, word);
    if (vocab_size > vocab_hash_size * 0.7) ReduceVocab();
  }
  SortVocab();
  if (debug_mode > 0) {
    printf("Vocab size: %lld\n", vocab_size);
    printf("Words in train file: %lld\n", train_words);
    printf("Number of sentences: %lld\n", phrase_size);
  }
  file_size = ftell(fin);
  fclose(fin);
}

void LearnVocabFromTestFile() {
  char word[MAX_STRING];
  FILE *fin;
  long long i;
  //for (a = 0; a < vocab_hash_size; a++) vocab_hash[a] = -1;
  fin = fopen(test_file, "rb");
  if (fin == NULL) {
    printf("ERROR: training data file not found!\n");
    exit(1);
  }
  vocab_size = 0;
  long long eol_index = SearchVocab((char *)"</s>");
  if (eol_index == -1) {
    printf("ERROR: must initialize vocab before training test vectors.\n");
    exit(1);
  }
  long long curr_sen = AddSentenceToPhrases("none","none");
  while (1) {
    ReadWord(word, fin);
    if (feof(fin)) break;
    train_words++;
    if ((debug_mode > 1) && (train_words % 100000 == 0)) {
      printf("%lldK%c", train_words / 1000, 13);
      fflush(stdout);
    }
    i = SearchVocab(word);
    /* if (i == -1) { */
    /*   a = AddWordToVocab(word); */
    /*   vocab[a].cn = 1; */
    /* } else vocab[i].cn++; */
    if(i == eol_index || (train_words % 100 == 0)) 
      curr_sen = AddSentenceToPhrases(test_file, "none");
    if (i != eol_index) AddWordToSentence(curr_sen, word);
    //if (vocab_size > vocab_hash_size * 0.7) ReduceVocab();
  }
  //SortVocab();
  if (debug_mode > 0) {
    printf("Vocab size: %lld\n", vocab_size);
    printf("Words in train file: %lld\n", train_words);
    printf("Number of sentences: %lld\n", phrase_size);
  }
  file_size = ftell(fin);
  fclose(fin);
}

void LearnVocabFromTrainDir() {
  char word[MAX_STRING];
  DIR *d;
  long long a, i;
  for (a = 0; a < vocab_hash_size; a++) vocab_hash[a] = -1;
  d = opendir(train_dir);
  if (d == NULL) {
    printf("ERROR: training data file not found!\n");
    exit(1);
  }
  vocab_size = 0;
  AddWordToVocab((char *)"</s>");
  while (1) {
    struct dirent *entry;
    const char *label_name;
    entry = readdir(d);
    if(!entry) {
      break;
    }
    label_name = entry->d_name;
    if(entry->d_type & DT_DIR) {
      if(strcmp(label_name, "..") != 0 && strcmp(label_name,".") != 0) {
	char path[PATH_MAX];
	snprintf(path, PATH_MAX, "%s%s", train_dir, label_name);
	FILE *fin;
	
	DIR *dir;
	printf("INFO: Now opening %s.\n", path);
	dir = opendir(path);
	if(!d) {
	  printf("ERROR: training subdir not found!\n");
	  exit(1);
	}
	
	while(1) {
	  struct dirent *f;
	  f = readdir(dir);
	  if(!f)
	    break;
	  if(strcmp(f->d_name, "..") == 0 || strcmp(f->d_name,".") == 0)
	    continue;
	  
	  char file[PATH_MAX];
	  snprintf(file, PATH_MAX, "%s/%s", path, f->d_name);
	  
	  fin = fopen(file,"rb");
	  if(fin == NULL)
	    continue;
	  int sentence_index = AddSentenceToPhrases(file, label_name);
	  //printf("INFO: Reading sentence #%d from %s.\n", sentence_index, file);
	  while (1) {
	    ReadWord(word, fin);
	    if (feof(fin)) break;
	    train_words++;
	    AddWordToSentence(sentence_index, word);
	    if ((debug_mode > 1) && (train_words % 100000 == 0)) {
	      printf("%lldK%c", train_words / 1000, 13);
	      fflush(stdout);
	    }
	    i = SearchVocab(word);
	    if (i == -1) {
	      a = AddWordToVocab(word);
	      vocab[a].cn = 1;
	    } else vocab[i].cn++;
	    if (vocab_size > vocab_hash_size * 0.7) ReduceVocab();
	  }
	  fclose(fin);
	}
	closedir(dir);
      }
    }
  }
  closedir(d);
  SortVocab();
  if (debug_mode > 0) {
    printf("Vocab size: %lld\n", vocab_size);
    printf("Words in train file: %lld\n", train_words);
    printf("Number of sentences: %lld\n", phrase_size);
    printf("Number of labels: %lld\n", label_size);
  }
  //file_size = ftell(fin);
}

void LearnVocabFromTestDir() {
  char word[MAX_STRING];
  DIR *d;
  
  //long long a, i;
  //for (a = 0; a < vocab_hash_size; a++) vocab_hash[a] = -1;
  d = opendir(test_dir);
  if (d == NULL) {
    printf("ERROR: test data file not found!\n");
    exit(1);
  }
  //vocab_size = 0;
  //AddWordToVocab((char *)"</s>");
  while (1) {
    struct dirent *entry;
    const char *label_name;
    entry = readdir(d);
    if(!entry) {
      break;
    }
    label_name = entry->d_name;
    if(entry->d_type & DT_DIR) {
      if(strcmp(label_name, "..") != 0 && strcmp(label_name,".") != 0) {
	char path[PATH_MAX];
	snprintf(path, PATH_MAX, "%s%s", test_dir, label_name);
	FILE *fin;
	
	DIR *dir;
	printf("INFO: Now opening %s.\n", path);
	dir = opendir(path);
	if(!d) {
	  printf("ERROR: test subdir not found!\n");
	  exit(1);
	}
	
	while(1) {
	  struct dirent *f;
	  f = readdir(dir);
	  if(!f)
	    break;
	  if(strcmp(f->d_name, "..") == 0 || strcmp(f->d_name,".") == 0)
	    continue;
	  
	  char file[PATH_MAX];
	  snprintf(file, PATH_MAX, "%s/%s", path, f->d_name);
	  
	  fin = fopen(file,"rb");
	  if(fin == NULL)
	    continue;
	  int sentence_index = AddSentenceToPhrases(file, label_name);
	  //printf("INFO: Reading sentence #%d from %s.\n", sentence_index, file);
	  while (1) {
	    ReadWord(word, fin);
	    if (feof(fin)) break;
	    train_words++;
	    AddWordToSentence(sentence_index, word);
	    if ((debug_mode > 1) && (train_words % 100000 == 0)) {
	      printf("%lldK%c", train_words / 1000, 13);
	      fflush(stdout);
	    }
	    /* i = SearchVocab(word); */
	    /* if (i == -1) { */
	    /*   a = AddWordToVocab(word); */
	    /*   vocab[a].cn = 1; */
	    /* } else vocab[i].cn++; */
	    /* if (vocab_size > vocab_hash_size * 0.7) ReduceVocab(); */
	  }
	  fclose(fin);
	}
	closedir(dir);
      }
    }
  }
  closedir(d);
  //SortVocab();
  if (debug_mode > 0) {
    printf("Vocab size: %lld\n", vocab_size);
    printf("Words in train file: %lld\n", train_words);
    printf("Number of sentences: %lld\n", phrase_size);
  }
  //file_size = ftell(fin);
}

/* void SaveVocab() { */
/*   long long i; */
/*   FILE *fo = fopen(save_vocab_file, "wb"); */
/*   for (i = 0; i < vocab_size; i++) fprintf(fo, "%s %lld\n", vocab[i].word, vocab[i].cn); */
/*   fclose(fo); */
/* } */

/* void ReadVocab() { */
/*   long long a, i = 0; */
/*   char c; */
/*   char word[MAX_STRING]; */
/*   FILE *fin = fopen(read_vocab_file, "rb"); */
/*   if (fin == NULL) { */
/*     printf("Vocabulary file not found\n"); */
/*     exit(1); */
/*   } */
/*   for (a = 0; a < vocab_hash_size; a++) vocab_hash[a] = -1; */
/*   vocab_size = 0; */
/*   while (1) { */
/*     ReadWord(word, fin); */
/*     if (feof(fin)) break; */
/*     a = AddWordToVocab(word); */
/*     fscanf(fin, "%lld%c", &vocab[a].cn, &c); */
/*     i++; */
/*   } */
/*   SortVocab(); */
/*   if (debug_mode > 0) { */
/*     printf("Vocab size: %lld\n", vocab_size); */
/*     printf("Words in train file: %lld\n", train_words); */
/*   } */
/*   fin = fopen(train_file, "rb"); */
/*   if (fin == NULL) { */
/*     printf("ERROR: training data file not found!\n"); */
/*     exit(1); */
/*   } */
/*   fseek(fin, 0, SEEK_END); */
/*   file_size = ftell(fin); */
/*   fclose(fin); */
/* } */

void InitNet() {
  long long a, b, c;
  a = posix_memalign((void **)&syn0, 128, (long long)vocab_size * word_features * sizeof(real));
  if (syn0 == NULL) {printf("Memory allocation failed\n"); exit(1);}
  if (hs) {
    a = posix_memalign((void **)&syn1, 128, (long long)vocab_size * layer1_size * sizeof(real));
    if (syn1 == NULL) {printf("Memory allocation failed\n"); exit(1);}
    for (b = 0; b < layer1_size; b++) for (a = 0; a < vocab_size; a++)
     syn1[a * layer1_size + b] = 0;
  }
  if (negative>0) {
    a = posix_memalign((void **)&syn1neg, 128, (long long)vocab_size * layer1_size * sizeof(real));
    if (syn1neg == NULL) {printf("Memory allocation failed\n"); exit(1);}
    for (b = 0; b < layer1_size; b++) for (a = 0; a < vocab_size; a++)
     syn1neg[a * layer1_size + b] = 0;
  }
  for (b = 0; b < word_features; b++) for (a = 0; a < vocab_size; a++)
   syn0[a * word_features + b] = (rand() / (real)RAND_MAX - 0.5) / word_features;
  for (b = 0; b < phrase_size; b++) {
    a = posix_memalign((void **)&phrases[b].dm_vector, 128, (long long)paragraph_features * sizeof(real));
    if(phrases[b].dm_vector == NULL) {printf("Memory allocation failed\n"); exit(1);}
    for(c = 0; c < paragraph_features; c++) {
      phrases[b].dm_vector[c] = (rand() / (real)RAND_MAX - 0.5) / paragraph_features;
    }
    a = posix_memalign((void **)&phrases[b].dbow_vector, 128, (long long)paragraph_features * sizeof(real));
    if(phrases[b].dbow_vector == NULL) {printf("Memory allocation failed\n"); exit(1);}
    for(c = 0; c < paragraph_features; c++) {
      phrases[b].dbow_vector[c] = (rand() / (real)RAND_MAX - 0.5) / paragraph_features;
    }
  }
  CreateBinaryTree();
}

void *TrainModelThread(void *id) {
  
  long long i,j, a, b, d, word, last_word, sentence_length = 0, sentence_position = 0;
  long long word_count = 0, last_word_count = 0, sen[MAX_SENTENCE_LENGTH + 1];
  long long sentence_count = 0, last_sentence_count = 0;
  long long /*l1,*/ l2, c, target, label;
  long long start_index, layer_index;
  unsigned long long next_random = (long long)id;
  real f, g;
  clock_t now;
  real *neu1 = (real *)calloc(layer1_size, sizeof(real));
  real *neu1e = (real *)calloc(layer1_size, sizeof(real));
  //real *last_dbow = (real *)calloc(paragraph_features, sizeof(real));
  //real *last_dm = (real *)calloc(paragraph_features, sizeof(real));
  //FILE *fi = fopen(train_file, "rb");
  //fseek(fi, file_size / (long long)num_threads * (long long)id, SEEK_SET);
  
  long long first_phrase = phrase_size / (long long) num_threads * (long long)(id);
  long long last_phrase = phrase_size / (long long) num_threads * (long long)(id+1);
  if(debug_mode > 2)
    printf("Thread %lld started with sentences %lld through %lld.\n", (long long)id, first_phrase, last_phrase);
  for(i = first_phrase; i < last_phrase; i++) {
    sentence_count++;
    //int iterations = 0;
    //while(1) {
    //iterations++;
    /*if(model == 0 || model == 2) memcpy(last_dm, phrases[i].dm_vector, paragraph_features*sizeof(real)); */
      /* if(model == 1 || model == 2) memcpy(last_dbow, phrases[i].dbow_vector, paragraph_features*sizeof(real)); */
      if (word_count - last_word_count > 10000) {
	word_count_actual += word_count - last_word_count;
	last_word_count = word_count;
	sentence_count_actual += sentence_count - last_sentence_count;
	last_sentence_count = sentence_count;
	if ((debug_mode > 1)) {
	  now=clock();
	  printf("%cAlpha: %f  Progress: %.2f%%  Sentence Progress : %.2f%%  Words/thread/sec: %.2fk", 13, alpha,
	  	 word_count_actual / (real)(train_words + 1) * 100,
		 sentence_count_actual / (real)(phrase_size + 1) * 100,
	  	 word_count_actual / ((real)(now - start + 1) / (real)CLOCKS_PER_SEC * 1000));
	  fflush(stdout);
	}
      
	alpha = starting_alpha * (1 - word_count_actual / (real)(train_words + 1));
	if (alpha < starting_alpha * 0.0001) alpha = starting_alpha * 0.0001;
      }
      sentence_length = 0;
      for(j = 0; j < phrases[i].word_count; j++) {
	word = SearchVocab(phrases[i].words[j]);
	//if (feof(fi)) break;
	if (word == -1) {
	  continue;
	}
	word_count++;
	if (word == 0) break;
	// The subsampling randomly discards frequent words while keeping the ranking same
	if (sample > 0) {
	  real ran = (sqrt(vocab[word].cn / (sample * train_words)) + 1) * (sample * train_words) / vocab[word].cn;
	  next_random = next_random * (unsigned long long)25214903917 + 11;
	  if (ran < (next_random & 0xFFFF) / (real)65536) continue;
	}
	sen[sentence_length] = word;
	sentence_length++;
	if (sentence_length >= MAX_SENTENCE_LENGTH) break;
      }
    
      for(sentence_position = 0; sentence_position < sentence_length; sentence_position++) {
	//if (feof(fi)) break;
	//if (word_count > train_words / num_threads) break;
	word = sen[sentence_position];
	if (word == -1) continue;
	for (c = 0; c < layer1_size; c++) neu1[c] = 0;
	for (c = 0; c < layer1_size; c++) neu1e[c] = 0;
	next_random = next_random * (unsigned long long)25214903917 + 11;
	b = next_random % window;
	if (model == 0 || model == 2) {  //train pv-dm
	  // in -> hidden
	  
	  for (layer_index = 0; layer_index < paragraph_features; layer_index++) neu1[layer_index] += phrases[i].dm_vector[layer_index];
	  for (a = b; a < window * 2 + 1 - b; a++) {
	    if (a != window) {
	      c = sentence_position - window + a;
	      if (c < 0) continue;
	      if (c >= sentence_length) continue;
	      last_word = sen[c];
	      if (last_word == -1) continue;
	      start_index = layer_index;
	      for (layer_index = start_index; (layer_index < start_index + word_features) && (layer_index < layer1_size); layer_index++) {
		neu1[layer_index] += syn0[(layer_index - start_index) + last_word * word_features];
	      }
	    }
	  }
	  if(layer_index < layer1_size) {
	    //append NULL symbol
	    long long last_index = layer_index;
	    for(a = 0; a < (layer1_size - last_index) / word_features; a++) {
	      for(c = layer_index; c < layer1_size; c++)
		neu1[c] += syn0[(c - layer_index) + 0 * word_features];
	      layer_index += word_features;
	    }
	  }
	  //for (c = 0; c < layer1_size; c++) neu1[c] += phrases[i].dm_vector[c];
	  if (hs) for (d = 0; d < vocab[word].codelen; d++) {
	      f = 0;
	      l2 = vocab[word].point[d] * word_features;
	      // Propagate hidden -> output
	      for (c = 0; c < layer1_size; c++) {
		f += neu1[c] * syn1[c + l2];
	      }
	      if (f <= -MAX_EXP) continue;
	      else if (f >= MAX_EXP) continue;
	      else f = expTable[(int)((f + MAX_EXP) * (EXP_TABLE_SIZE / MAX_EXP / 2))];
	      // 'g' is the gradient multiplied by the learning rate
	      g = (1 - vocab[word].code[d] - f) * alpha;
	      // Propagate errors output -> hidden
	      for (c = 0; c < layer1_size; c++) neu1e[c] += g * syn1[c + l2];
	      // Learn weights hidden -> output
	      for (c = 0; c < layer1_size; c++) syn1[c + l2] += g * neu1[c];
	    }
	  // NEGATIVE SAMPLING
	  if (negative > 0) for (d = 0; d < negative + 1; d++) {
	      if (d == 0) {
		target = word;
		label = 1;
	      } else {
		next_random = next_random * (unsigned long long)25214903917 + 11;
		target = table[(next_random >> 16) % table_size];
		if (target == 0) target = next_random % (vocab_size - 1) + 1;
		if (target == word) continue;
		label = 0;
	      }
	      l2 = target * layer1_size;
	      f = 0;
	      for (c = 0; c < layer1_size; c++) f += neu1[c] * syn1neg[c + l2];
	      if (f > MAX_EXP) g = (label - 1) * alpha;
	      else if (f < -MAX_EXP) g = (label - 0) * alpha;
	      else g = (label - expTable[(int)((f + MAX_EXP) * (EXP_TABLE_SIZE / MAX_EXP / 2))]) * alpha;
	      for (c = 0; c < layer1_size; c++) neu1e[c] += g * syn1neg[c + l2];
	      for (c = 0; c < layer1_size; c++) syn1neg[c + l2] += g * neu1[c];
	    }
	  // hidden -> in
	  for (layer_index = 0; layer_index < paragraph_features; layer_index++) phrases[i].dm_vector[layer_index] += neu1e[layer_index];
	  if(!freeze_words) {
	    for (a = b; a < window * 2 + 1 - b; a++) {
	      if (a != window) {
		c = sentence_position - window + a;
		if (c < 0) continue;
		if (c >= sentence_length) continue;
		last_word = sen[c];
		if (last_word == -1) continue;
		start_index = layer_index;
		for (layer_index = start_index; layer_index < (start_index + word_features) && (layer_index < layer1_size); layer_index++) {
		  syn0[(layer_index-start_index) + last_word * word_features] += neu1e[layer_index];
		}
	      }
	    }
	  }
	  /* if(!freeze_words) { */
	  /*   for (a = b; a < window * 2 + 1 - b; a++) if (a != window) { */
	  /* 	c = sentence_position - window + a; */
	  /* 	if (c < 0) continue; */
	  /* 	if (c >= sentence_length) continue; */
	  /* 	last_word = sen[c]; */
	  /* 	if (last_word == -1) continue; */
	  /* 	for (c = 0; c < layer1_size; c++) syn0[c + last_word * layer1_size] += neu1e[c]; */
	  /*     } */
	  /* } */
	  /* for (c = 0; c < layer1_size; c++) phrases[i].dm_vector[c] += neu1e[c]; */
	
	} 
	for (c = 0; c < layer1_size; c++) neu1[c] = 0;
	for (c = 0; c < layer1_size; c++) neu1e[c] = 0;
	/* if (model == 1 || model == 2) {  //train pv-dbow */
	/*   for (a = b; a < window * 2 + 1 - b; a++) if (a != window) { */
	/*       c = sentence_position - window + a; */
	/*       if (c < 0) continue; */
	/*       if (c >= sentence_length) continue; */
	/*       last_word = sen[c]; */
	/*       if (last_word == -1) continue; */
	/*       l1 = last_word * layer1_size; */
	/*       for (c = 0; c < layer1_size; c++) neu1e[c] = 0; */
	/*       // HIERARCHICAL SOFTMAX */
	/*       if (hs) for (d = 0; d < vocab[word].codelen; d++) { */
	/* 	  f = 0; */
	/* 	  l2 = vocab[word].point[d] * layer1_size; */
	/* 	  // Propagate hidden -> output */
	/* 	  for (c = 0; c < layer1_size; c++) f += phrases[i].dbow_vector[c] * syn1[c + l2]; */
	/* 	  if (f <= -MAX_EXP) continue; */
	/* 	  else if (f >= MAX_EXP) continue; */
	/* 	  else f = expTable[(int)((f + MAX_EXP) * (EXP_TABLE_SIZE / MAX_EXP / 2))]; */
	/* 	  // 'g' is the gradient multiplied by the learning rate */
	/* 	  g = (1 - vocab[word].code[d] - f) * alpha; */
	/* 	  // Propagate errors output -> hidden */
	/* 	  for (c = 0; c < layer1_size; c++) neu1e[c] += g * syn1[c + l2]; */
	/* 	  // Learn weights hidden -> output */
	/* 	  for (c = 0; c < layer1_size; c++) syn1[c + l2] += g * syn0[c + l1]; */
	/* 	} */
	/*       // NEGATIVE SAMPLING */
	/*       if (negative > 0) for (d = 0; d < negative + 1; d++) { */
	/* 	  if (d == 0) { */
	/* 	    target = word; */
	/* 	    label = 1; */
	/* 	  } else { */
	/* 	    next_random = next_random * (unsigned long long)25214903917 + 11; */
	/* 	    target = table[(next_random >> 16) % table_size]; */
	/* 	    if (target == 0) target = next_random % (vocab_size - 1) + 1; */
	/* 	    if (target == word) continue; */
	/* 	    label = 0; */
	/* 	  } */
	/* 	  l2 = target * layer1_size; */
	/* 	  f = 0; */
	/* 	  for (c = 0; c < layer1_size; c++) f += syn0[c + l1] * syn1neg[c + l2]; */
	/* 	  if (f > MAX_EXP) g = (label - 1) * alpha; */
	/* 	  else if (f < -MAX_EXP) g = (label - 0) * alpha; */
	/* 	  else g = (label - expTable[(int)((f + MAX_EXP) * (EXP_TABLE_SIZE / MAX_EXP / 2))]) * alpha; */
	/* 	  for (c = 0; c < layer1_size; c++) neu1e[c] += g * syn1neg[c + l2]; */
	/* 	  for (c = 0; c < layer1_size; c++) syn1neg[c + l2] += g * syn0[c + l1]; */
	/* 	} */
	/*       // Learn weights input -> hidden */
	/*       //for (c = 0; c < layer1_size; c++) syn0[c + l1] += neu1e[c]; */
	/*       for(c = 0; c < layer1_size; c++) phrases[i].dbow_vector[c] += neu1e[c]; */
	/*     } */
	/* } */
      }
      //sentence_position++;
      //if (sentence_position >= sentence_length) {
      //  sentence_length = 0;
      //  continue;
      //}
      /* if (iterations > 10) */
    /* 	break; */
    /*   else if (model == 0 && IsConverged(last_dm, phrases[i].dm_vector, paragraph_features)) */
    /* 	break; */
    /*   else if (model == 1 && IsConverged(last_dbow, phrases[i].dbow_vector, paragraph_features)) */
    /* 	break; */
    /*   else if (model == 2 && IsConverged(last_dm, phrases[i].dm_vector, paragraph_features) && IsConverged(last_dbow, phrases[i].dbow_vector, paragraph_features)) */
    /* 	break; */
    /* } */
    //if(debug_mode > 1)
    //  printf("Sentence %lld trained through %d iterations.\n", i, iterations);
  }
  //fclose(fi);
  free(neu1);
  free(neu1e);
  pthread_exit(NULL);
}

void TrainModel() {
  long a, b, c;// d;
  FILE *fo;
  pthread_t *pt = (pthread_t *)malloc(num_threads * sizeof(pthread_t));
  initial_alpha = alpha;
  starting_alpha = alpha;
  printf("Learning paragraph vectors with parameters:\n");
  printf("\tModel: %s\n", model_names[model]);
  printf("\tInput layer size: %lld\n", layer1_size);
  printf("\tWindow size: %d\n", window);
  printf("\tSampling rate: %f\n\n", sample);
  if (train_file[0] != 0) {
    printf("Starting training using file %s\n", train_file);
    LearnVocabFromTrainFile();
  } else {
    printf("Starting training using directory %s\n", train_dir);
    LearnVocabFromTrainDir();
  }
  //if (save_vocab_file[0] != 0) SaveVocab();
  if (para_file[0] == 0) return;
  InitNet();
  if (negative > 0) InitUnigramTable();
  ShufflePhrases();
  start = clock();
  for (a = 0; a < num_threads; a++) pthread_create(&pt[a], NULL, TrainModelThread, (void *)a);
  for (a = 0; a < num_threads; a++) pthread_join(pt[a], NULL);
  printf("\nCompleted training. Now outputting vectors.\n");
  fo = fopen(para_file, "wb");
    
  fprintf(fo, "%lld %lld %lld\n", labeled_instances, paragraph_features, label_size);
  long long vector_output = 0;
  for (a = 0; a < phrase_size; a++) {
    int arr[label_size];
    memset(arr, 0, sizeof(arr));
    if(phrases[a].label_index == -1) continue;
    if(model == 0)
      for (b = 0; b < paragraph_features; b++) fprintf(fo, "%lf ", phrases[a].dm_vector[b]);
    else if (model == 1)
      for (b = 0; b < paragraph_features; b++) fprintf(fo, "%lf ", phrases[a].dbow_vector[b]);
    else
      for (b = 0; b < paragraph_features; b++) fprintf(fo, "%lf ", phrases[a].dm_vector[b] + phrases[a].dbow_vector[b]);
    fprintf(fo, "\n");
    arr[phrases[a].label_index] = 1;
    for (b = 0; b < label_size; b++) fprintf(fo, "%d ", arr[b]);
    fprintf(fo, "\n");
    vector_output++;
  }
  if(vector_output != labeled_instances) {
    printf("Number of vectors output is not equal to the count of labeled instances.\n");
    exit(1);
  }
  fclose(fo);
  if (word_output_file[0] != 0) {
    // Save the word vectors
    fo = fopen(word_output_file, "wb");
    fprintf(fo, "%lld %lld\n", vocab_size, word_features);
    for (a = 0; a < vocab_size; a++) {
      fprintf(fo, "%s ", vocab[a].word);
      if (binary) for (b = 0; b < word_features; b++) fwrite(&syn0[a * word_features + b], sizeof(real), 1, fo);
      else for (b = 0; b < word_features; b++) fprintf(fo, "%lf ", syn0[a * word_features + b]);
      fprintf(fo, "\n");
    }
    /* for (a = 0; a < phrase_size; a++) { */
    /*   long count = phrases[a].word_count; */
      
    /*   for (b = 0; b < count; b++) { */
    /* 	fprintf(fo, "%s ", phrases[a].words[b]); */
    /*   } */
    /*   if (binary) for (b = 0; b < layer1_size; b++) fwrite(&phrases[a].vector[b], sizeof(real), 1, fo); */
    /*   else for (b = 0; b < layer1_size; b++) fprintf(fo, "%lf ", phrases[a].vector[b]); */
    /*   fprintf(fo, "\n"); */
    /* } */
    fclose(fo);
  } 

  if (phrase_output_file[0] != 0) {
    // Save the paragraph vectors
    fo = fopen(phrase_output_file, "wb");
    fprintf(fo, "%lld %lld\n", phrase_size, paragraph_features);
    for (a = 0; a < phrase_size; a++) {
      //long count = phrases[a].word_count;
      //int arr[label_size];
      //memset(arr, 0, sizeof(arr));
      fprintf(fo, "%s ", phrases[a].file_name);
      //for (b = 0; b < count; b++) {
      //fprintf(fo, "%s ", phrases[a].words[b]);
      //}
      //fprintf(fo, "\n");
      if (binary) for (b = 0; b < paragraph_features; b++) fwrite(&phrases[a].dm_vector[b], sizeof(real), 1, fo);
      else for (b = 0; b < paragraph_features; b++) fprintf(fo, "%lf ", phrases[a].dm_vector[b]);
      fprintf(fo, "\n");
      //if(phrases[a].label_index != -1)
      //arr[phrases[a].label_index] = 1;
      //for (b = 0; b < label_size; b++) fprintf(fo, "%d ", arr[b]);
      //fprintf(fo, "\n");
    }

    fclose(fo);
  }
  
  if(para_file_test[0] == 0) return;
  printf("Now learning vectors for test paragraphs.\n");
  //Reset to starting conditions
  pt = (pthread_t *)malloc(num_threads * sizeof(pthread_t));
  phrases = (struct paragraph *)calloc(phrase_max_size, sizeof(struct paragraph));
  for (b = 0; b < phrase_size; b++) {
    a = posix_memalign((void **)&phrases[b].dm_vector, 128, (long long)paragraph_features * sizeof(real));
    if(phrases[b].dm_vector == NULL) {printf("Memory allocation failed\n"); exit(1);}
    for(c = 0; c < paragraph_features; c++) {
      phrases[b].dm_vector[c] = (rand() / (real)RAND_MAX - 0.5) / paragraph_features;
    }
    a = posix_memalign((void **)&phrases[b].dbow_vector, 128, (long long)paragraph_features * sizeof(real));
    if(phrases[b].dbow_vector == NULL) {printf("Memory allocation failed\n"); exit(1);}
    for(c = 0; c < paragraph_features; c++) {
      phrases[b].dbow_vector[c] = (rand() / (real)RAND_MAX - 0.5) / paragraph_features;
    }
  }
  phrase_size = 0;
  train_words = 0;
  labeled_instances = 0;
  //freeze_words to true so that we don't change any word vectors
  freeze_words = 1;
  alpha = initial_alpha;
  starting_alpha = initial_alpha;
  word_count_actual = 0;
  if (test_file[0] != 0) {
    printf("Starting training using file %s\n", test_file);
    LearnVocabFromTestFile();
  } else {
    printf("Starting training using directory %s\n", test_dir);
    LearnVocabFromTestDir();
  }
  ShufflePhrases();
  start = clock();
  for (a = 0; a < num_threads; a++) pthread_create(&pt[a], NULL, TrainModelThread, (void *)a);
  for (a = 0; a < num_threads; a++) pthread_join(pt[a], NULL);
  printf("\nCompleted training. Now outputting vectors.\n");
  fo = fopen(para_file_test, "wb");
  fprintf(fo, "%lld %lld %lld\n", labeled_instances, paragraph_features, label_size);
  for (a = 0; a < phrase_size; a++) {
    int arr[label_size];
    memset(arr, 0, sizeof(arr));
    if(phrases[a].label_index == -1) continue;

    if(model == 0)
      for (b = 0; b < paragraph_features; b++) fprintf(fo, "%lf ", phrases[a].dm_vector[b]);
    else if (model == 1)
      for (b = 0; b < paragraph_features; b++) fprintf(fo, "%lf ", phrases[a].dbow_vector[b]);
    else
      for (b = 0; b < paragraph_features; b++) fprintf(fo, "%lf ", phrases[a].dm_vector[b] + phrases[a].dbow_vector[b]);
    fprintf(fo, "\n");
    arr[phrases[a].label_index] = 1;
    for (b = 0; b < label_size; b++) fprintf(fo, "%d ", arr[b]);
    fprintf(fo, "\n");
  }
  fclose(fo);
}

int ArgPos(char *str, int argc, char **argv) {
  int a;
  for (a = 1; a < argc; a++) if (!strcmp(str, argv[a])) {
    if (a == argc - 1) {
      printf("Argument missing for %s\n", str);
      exit(1);
    }
    return a;
  }
  return -1;
}

int main(int argc, char **argv) {
  int i;
  if (argc == 1) {
    printf("WORD VECTOR estimation toolkit v 0.1b\n\n");
    printf("Options:\n");
    printf("Parameters for training:\n");
    printf("\t-train-file <file>\n");
    printf("\t\tUse text data from <file> (one sentence/phrase per line) to train the model\n");
    printf("\t-train-dir <dir>\n");
    printf("\t\tUse text data from <dir> (one sentence/phrase per file) to train the model\n");
    printf("\t-test-file <file>\n");
    printf("\t\tUse text data from <file> to learn test vectors\n");
    printf("\t-test-dir <file>\n");
    printf("\t\tUse text data from <dir> to learn test vectors\n");
    printf("\t-output-words <file>\n");
    printf("\t\tUse <file> to save the resulting word vectors\n");
    printf("\t-output-phrases <file>\n");
    printf("\t\tUse <file> to save the resulting paragraph vectors (Only for PV-DM model\n)");
    printf("\t-nn-train <file>\n");
    printf("\t\tUse <file> to save paragraph vectors in FANN format\n");
    printf("\t-nn-test <file>\n");
    printf("\t\tUse <file> to save paragraph vectors in FANN format\n");
    printf("\t-word-size <int>\n");
    printf("\t\tSet size of word vectors; default is 200\n");
    printf("\t-paragraph-size <int>\n");
    printf("\t\tSet size of paragraph vectors; default is 200\n");
    printf("\t-window <int>\n");
    printf("\t\tSet max skip length between words; default is 5\n");
    printf("\t-sample <float>\n");
    printf("\t\tSet threshold for occurrence of words. Those that appear with higher frequency");
    printf(" in the training data will be randomly down-sampled; default is 0 (off), useful value is 1e-5\n");
    printf("\t-hs <int>\n");
    printf("\t\tUse Hierarchical Softmax; default is 1 (0 = not used)\n");
    printf("\t-negative <int>\n");
    printf("\t\tNumber of negative examples; default is 0, common values are 5 - 10 (0 = not used)\n");
    printf("\t-threads <int>\n");
    printf("\t\tUse <int> threads (default 1)\n");
    printf("\t-min-count <int>\n");
    printf("\t\tThis will discard words that appear less than <int> times; default is 5\n");
    printf("\t-alpha <float>\n");
    printf("\t\tSet the starting learning rate; default is 0.025\n");
    //printf("\t-classes <int>\n");
    //printf("\t\tOutput word classes rather than word vectors; default number of classes is 0 (vectors are written)\n");
    printf("\t-debug <int>\n");
    printf("\t\tSet the debug mode (default = 2 = more info during training)\n");
    printf("\t-binary <int>\n");
    printf("\t\tSave the resulting vectors in binary moded; default is 0 (off)\n");
    //printf("\t-save-vocab <file>\n");
    //printf("\t\tThe vocabulary will be saved to <file>\n");
    //printf("\t-read-vocab <file>\n");
    //printf("\t\tThe vocabulary will be read from <file>, not constructed from the training data\n");
    printf("\t-model <int>\n");
    printf("\t\t0 = PV-DM, 1 = PV-DBOW, 2 = Both (concatenate); default is 2\n");
    printf("\nExamples:\n");
    printf("./phrase2vec -train-dir dir -nn-train train.data -test-dir dir -nn-test test.data -debug 2 -word-size 200 -paragraph-size 400 -window 5 -sample 1e-4 -negative 5 -hs 0 -binary 0 -model 0\n\n");
    return 0;
  }
  word_output_file[0] = 0;
  phrase_output_file[0] = 0;
  //save_vocab_file[0] = 0;
  //read_vocab_file[0] = 0;
  train_file[0] = 0;
  train_dir[0] = 0;
  test_file[0] = 0;
  test_dir[0] = 0;
  para_file[0] = 0;
  para_file_test[0] = 0;
  if ((i = ArgPos((char *)"-word-size", argc, argv)) > 0) word_features = atoi(argv[i + 1]);
  if ((i = ArgPos((char *)"-paragraph-size", argc, argv)) > 0) paragraph_features = atoi(argv[i + 1]);
  if ((i = ArgPos((char *)"-train-file", argc, argv)) > 0) strcpy(train_file, argv[i + 1]);
  if ((i = ArgPos((char *)"-train-dir", argc, argv)) > 0) strcpy(train_dir, argv[i + 1]);
  if ((i = ArgPos((char *)"-test-file", argc, argv)) > 0) strcpy(test_file, argv[i + 1]);
  if ((i = ArgPos((char *)"-test-dir", argc, argv)) > 0) strcpy(test_dir, argv[i + 1]);
  if ((i = ArgPos((char *)"-nn-train", argc, argv)) > 0) strcpy(para_file, argv[i + 1]);
  if ((i = ArgPos((char *)"-nn-test", argc, argv)) > 0) strcpy(para_file_test, argv[i + 1]);
  //if ((i = ArgPos((char *)"-save-vocab", argc, argv)) > 0) strcpy(save_vocab_file, argv[i + 1]);
  //if ((i = ArgPos((char *)"-read-vocab", argc, argv)) > 0) strcpy(read_vocab_file, argv[i + 1]);
  if ((i = ArgPos((char *)"-debug", argc, argv)) > 0) debug_mode = atoi(argv[i + 1]);
  if ((i = ArgPos((char *)"-binary", argc, argv)) > 0) binary = atoi(argv[i + 1]);
  if ((i = ArgPos((char *)"-model", argc, argv)) > 0) model = atoi(argv[i + 1]);
  if ((i = ArgPos((char *)"-alpha", argc, argv)) > 0) alpha = atof(argv[i + 1]);
  if ((i = ArgPos((char *)"-output-words", argc, argv)) > 0) strcpy(word_output_file, argv[i + 1]);
  if ((i = ArgPos((char *)"-output-phrases", argc, argv)) > 0) strcpy(phrase_output_file, argv[i + 1]);
  if ((i = ArgPos((char *)"-window", argc, argv)) > 0) window = atoi(argv[i + 1]);
  if ((i = ArgPos((char *)"-sample", argc, argv)) > 0) sample = atof(argv[i + 1]);
  if ((i = ArgPos((char *)"-hs", argc, argv)) > 0) hs = atoi(argv[i + 1]);
  if ((i = ArgPos((char *)"-negative", argc, argv)) > 0) negative = atoi(argv[i + 1]);
  if ((i = ArgPos((char *)"-threads", argc, argv)) > 0) num_threads = atoi(argv[i + 1]);
  if ((i = ArgPos((char *)"-min-count", argc, argv)) > 0) min_count = atoi(argv[i + 1]);
  //if ((i = ArgPos((char *)"-classes", argc, argv)) > 0) classes = atoi(argv[i + 1]);

  layer1_size = (word_features * (2*window - 1)) + paragraph_features;

  feenableexcept(FE_INVALID | FE_OVERFLOW | FE_UNDERFLOW);
  phrases = (struct paragraph *)calloc(phrase_max_size, sizeof(struct paragraph));
  vocab = (struct vocab_word *)calloc(vocab_max_size, sizeof(struct vocab_word));
  vocab_hash = (int *)calloc(vocab_hash_size, sizeof(int));
  labels = (char **)calloc(label_max_size, sizeof(char *));
  expTable = (real *)malloc((EXP_TABLE_SIZE + 1) * sizeof(real));
  for (i = 0; i < EXP_TABLE_SIZE; i++) {
    expTable[i] = exp((i / (real)EXP_TABLE_SIZE * 2 - 1) * MAX_EXP); // Precompute the exp() table
    expTable[i] = expTable[i] / (expTable[i] + 1);                   // Precompute f(x) = x / (x + 1)
  }
  TrainModel();
  free(phrases);
  free(vocab);
  free(vocab_hash);
  free(expTable);
  return 0;
}
