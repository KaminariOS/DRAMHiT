#ifndef __CACHEMISS_TEST_HPP__
#define __CACHEMISS_TEST_HPP__

#include "base_kht.hpp"
#include "types.hpp"

namespace kvstore {

class CacheMissTest {
 public:
  void cache_miss_run(Shard *sh, BaseHashTable *kmer_ht);
};

}  // namespace kvstore

#endif  // __CACHEMISS_TEST_HPP__