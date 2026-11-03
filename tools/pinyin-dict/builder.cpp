// Host tool: raw word list + valid-character list -> dict_pinyin.dat.
// The engine's own jni/command/pinyinime_dictbuilder.cpp with the paths as arguments.
#include <stdio.h>
#include "dicttrie.h"
using namespace ime_pinyin;
int main(int argc, char *argv[]) {
  if (argc != 4) { fprintf(stderr, "usage: builder RAWDICT_UTF16 VALID_UTF16 OUT.dat\n"); return 2; }
  DictTrie *trie = new DictTrie();
  if (!trie->build_dict(argv[1], argv[2])) { fprintf(stderr, "build failed\n"); return 1; }
  if (!trie->save_dict(argv[3])) { fprintf(stderr, "save failed\n"); return 1; }
  return 0;
}
