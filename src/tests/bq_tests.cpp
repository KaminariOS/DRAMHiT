#include "BQueueTest.hpp"
#include "misc_lib.h"
#include "print_stats.h"
#include "sync.h"

#if defined(XX_HASH)
#include "xx/xxhash.h"
#endif

#ifdef WITH_VTUNE_LIB
#include <ittnotify.h>
#endif

#define BQ_MAGIC_64BIT 0xD221A6BE96E04673UL
#define BQ_TESTS_BATCH_LENGTH 16
#define BQ_TESTS_DEQUEUE_ARR_LENGTH 16

namespace kmercounter {
extern uint64_t HT_TESTS_HT_SIZE;
extern uint64_t HT_TESTS_NUM_INSERTS;

// Test Variables
[[maybe_unused]] static uint64_t transactions = 100000000;
// static uint64_t transactions = 200000000;

uint32_t producer_count = 1;
uint32_t consumer_count = 1;

uint64_t batch_size = 1;

uint64_t mem_pool_order = 16;
uint64_t mem_pool_size;

extern BaseHashTable *init_ht(uint64_t, uint8_t);
extern void get_ht_stats(Shard *, BaseHashTable *);

#ifdef BQ_TESTS_USE_HALT
int *bqueue_halt;
#endif

struct bq_kmer {
  char data[KMER_DATA_LENGTH];
} __attribute__((aligned(64)));

struct bq_kmer bq_kmers[BQ_TESTS_DEQUEUE_ARR_LENGTH];
// thread-local since we have multiple consumers
__thread int data_idx = 0;
__thread uint64_t keys[BQ_TESTS_DEQUEUE_ARR_LENGTH];
__attribute__((
    aligned(64))) __thread Keys _items[BQ_TESTS_DEQUEUE_ARR_LENGTH] = {0};
alignas(64) uint64_t cons_buffers[64][64][BQ_TESTS_BATCH_LENGTH];
alignas(64) uint64_t buf_idx[64][64];

uint64_t num_enq_failures[64][64] = {0};
uint64_t num_deq_failures[64][64] = {0};

#if defined(DOUBLE_BUFFERING)
void BQueueTest::producer_thread(int tid, int n_prod, int n_cons) {
  Shard *sh = &this->shards[tid];

  sh->stats =
      //(thread_stats *)std::aligned_alloc(CACHE_LINE_SIZE,
      // sizeof(thread_stats));
      (thread_stats *)calloc(1, sizeof(thread_stats));
  alignas(64) uint64_t k = 0;
  uint8_t this_prod_id = sh->shard_idx;
  uint32_t cons_id = 0;
  uint64_t transaction_id;
  queue_t **q = this->prod_queues[this_prod_id];
#ifdef WITH_VTUNE_LIB
  std::string thread_name("producer_thread" + std::to_string(tid));
  __itt_thread_set_name(thread_name.c_str());
#endif
#ifdef BQ_TESTS_INSERT_XORWOW
  struct xorwow_state xw_state;
  xorwow_init(&xw_state);
#endif

  fipc_test_FAI(ready_producers);
  while (!test_ready) fipc_test_pause();
  fipc_test_mfence();

  // HT_TESTS_NUM_INSERTS enqueues per consumer
  cons_id = 0;
  auto mult_factor = static_cast<double>(n_cons) / n_prod;
  auto _num_messages = HT_TESTS_NUM_INSERTS * mult_factor;
  uint64_t num_messages = static_cast<uint64_t>(_num_messages);
  uint64_t key_start = static_cast<uint64_t>(_num_messages) * tid;

  printf("%s, mult_factor %f _num_messages %f | num_messages %lu\n", __func__,
         mult_factor, _num_messages, num_messages);
  if (key_start == 0) key_start = 1;
  printf(
      "[INFO] Producer %u starting. Sending %lu messages to %d consumers | "
      "key_start %lu\n",
      this_prod_id, num_messages, consumer_count, key_start);

  auto hash_to_cpu = [&](auto hash) {
    // return (hash * 11400714819323198485llu) & (n_cons - 1);
    // return (hash * 11400714819323198485llu) % n_cons;
    return hash % n_cons;
  };

  for (transaction_id = 0u; transaction_id < num_messages;) {
    /* BQ_TESTS_BATCH_LENGTH enqueues in one batch, then move on to next
     * consumer */

    for (auto i = 0u; i < BQ_TESTS_BATCH_LENGTH; i++) {
#ifdef BQ_TESTS_INSERT_XORWOW
      k = xorwow(&xw_state);
      k = k << 32 | xorwow(&xw_state);
#else
      k = key_start++;
      uint64_t hash_val = XXH64(&k, sizeof(k), 0);

      cons_id = hash_to_cpu(k);
      // cons_id = hash_to_cpu(hash_val);
      // k has the computed hash in upper 32 bits
      // and the actual key value in lower 32 bits
      k |= (hash_val << 32);
      cons_buffers[this_prod_id][cons_id][buf_idx[this_prod_id][cons_id]++] = k;
#endif
      // *((uint64_t *)&kmers[i].data) = k;

      if (buf_idx[this_prod_id][cons_id] == (BQ_TESTS_BATCH_LENGTH - 2)) {
        prefetch_queue(q[cons_id]);
      }
      if (buf_idx[this_prod_id][cons_id] == (BQ_TESTS_BATCH_LENGTH - 1)) {
        prefetch_queue_data(q[cons_id], true);
      }

      if (buf_idx[this_prod_id][cons_id] == BQ_TESTS_BATCH_LENGTH) {
      retry:
        for (auto m = 0u; m < BQ_TESTS_BATCH_LENGTH; m++) {
          if (enqueue(q[cons_id],
                      (data_t)cons_buffers[this_prod_id][cons_id][m]) !=
              SUCCESS) {
            /* if enqueue fails, move to next consumer queue */
            // printf("[ERROR] Producer %u -> Consumer %u \n", this_prod_id,
            // cons_id);
            // num_enq_failures[this_prod_id][cons_id]++;
            // break;
            goto retry;
          }
          transaction_id++;
        }
        buf_idx[this_prod_id][cons_id] = 0;
      }
#ifdef CALC_STATS
      if (transaction_id % (HT_TESTS_NUM_INSERTS * consumer_count / 10) == 0) {
        printf("[INFO] Producer %u, transaction_id %lu\n", this_prod_id,
               transaction_id);
      }
#endif
    }

    /* ++cons_id;
    if (cons_id >= consumer_count) cons_id = 0;

    auto get_next_cons = [&](auto inc) {
      auto next_cons_id = cons_id + inc;
      if (next_cons_id >= consumer_count) next_cons_id -= consumer_count;
      return next_cons_id;
    };

    prefetch_queue(q[get_next_cons(2)], true);
    prefetch_queue_data(q[get_next_cons(1)], true);
    */
  }

#ifdef BQ_TESTS_USE_HALT
  /* Tell consumers to halt */
  for (auto i = 0u; i < consumer_count; ++i) {
    bqueue_halt[i] = 1;
  }
#else
  /* enqueue halt messages */
  for (cons_id = 0; cons_id < consumer_count; cons_id++) {
    while (enqueue(q[cons_id], (data_t)BQ_MAGIC_64BIT) != SUCCESS)
      ;
    transaction_id++;
  }
#endif

  fipc_test_FAI(completed_producers);
}
#else
void BQueueTest::producer_thread(int tid, int n_prod, int n_cons) {
  Shard *sh = &this->shards[tid];

  sh->stats =
      //(thread_stats *)std::aligned_alloc(CACHE_LINE_SIZE,
      // sizeof(thread_stats));
      (thread_stats *)calloc(1, sizeof(thread_stats));
  alignas(64) uint64_t k = 0;
  uint8_t this_prod_id = sh->shard_idx;
  uint32_t cons_id = 0;
  uint64_t transaction_id;
  queue_t **q = this->prod_queues[this_prod_id];
#ifdef WITH_VTUNE_LIB
  std::string thread_name("producer_thread" + std::to_string(tid));
  __itt_thread_set_name(thread_name.c_str());
#endif
#ifdef BQ_TESTS_INSERT_XORWOW
  struct xorwow_state xw_state;
  xorwow_init(&xw_state);
#endif

  fipc_test_FAI(ready_producers);
  while (!test_ready) fipc_test_pause();
  fipc_test_mfence();

  // HT_TESTS_NUM_INSERTS enqueues per consumer
  cons_id = 0;
  auto mult_factor = static_cast<double>(n_cons) / n_prod;
  auto _num_messages = HT_TESTS_NUM_INSERTS * mult_factor;
  uint64_t num_messages = static_cast<uint64_t>(_num_messages);
  uint64_t key_start = static_cast<uint64_t>(_num_messages) * tid;

  printf("%s, mult_factor %f _num_messages %f | num_messages %lu\n", __func__,
         mult_factor, _num_messages, num_messages);
  if (key_start == 0) key_start = 1;
  printf(
      "[INFO] Producer %u starting. Sending %lu messages to %d consumers | "
      "key_start %lu\n",
      this_prod_id, num_messages, consumer_count, key_start);

  auto hash_to_cpu = [&](auto hash) {
    // return (hash * 11400714819323198485llu) & (n_cons - 1);
    // return (hash * 11400714819323198485llu) % n_cons;
    return hash % n_cons;
  };

  for (transaction_id = 0u; transaction_id < num_messages;) {
    /* BQ_TESTS_BATCH_LENGTH enqueues in one batch, then move on to next
     * consumer */

    for (auto i = 0u; i < BQ_TESTS_BATCH_LENGTH; i++) {
#ifdef BQ_TESTS_INSERT_XORWOW
      k = xorwow(&xw_state);
      k = k << 32 | xorwow(&xw_state);
#else
      k = key_start++;
      uint64_t hash_val = XXH64(&k, sizeof(k), 0);

      cons_id = hash_to_cpu(k);
      // k has the computed hash in upper 32 bits
      // and the actual key value in lower 32 bits
      k |= (hash_val << 32);
#endif
      // *((uint64_t *)&kmers[i].data) = k;
    retry:
      if (enqueue(q[cons_id], (data_t)k) != SUCCESS) {
        /* if enqueue fails, move to next consumer queue */
        // printf("[ERROR] Producer %u -> Consumer %u \n", this_prod_id,
        // cons_id);
        // num_enq_failures[this_prod_id][cons_id]++;
        goto retry;
        // break;
      }
      {
        // | 0 | 1 | .... | 7 |
        auto *_q = q[cons_id];
        if ((_q->head & 7) == 0) {
          auto new_head = (_q->head + 8) & (QUEUE_SIZE - 1);
          __builtin_prefetch(&_q->data[new_head], 1, 3);
        }
        auto _cons_id = cons_id + 1;
        if (_cons_id >= consumer_count) _cons_id = 0;
        __builtin_prefetch(q[_cons_id], 1, 3);
      }
      transaction_id++;
#ifdef CALC_STATS
      if (transaction_id % (HT_TESTS_NUM_INSERTS * consumer_count / 10) == 0) {
        printf("[INFO] Producer %u, transaction_id %lu\n", this_prod_id,
               transaction_id);
      }
#endif
    }

    /* ++cons_id;
    if (cons_id >= consumer_count) cons_id = 0;

    auto get_next_cons = [&](auto inc) {
      auto next_cons_id = cons_id + inc;
      if (next_cons_id >= consumer_count) next_cons_id -= consumer_count;
      return next_cons_id;
    };

    prefetch_queue(q[get_next_cons(2)], true);
    prefetch_queue_data(q[get_next_cons(1)], true);
    */
  }

#ifdef BQ_TESTS_USE_HALT
  /* Tell consumers to halt */
  for (auto i = 0u; i < consumer_count; ++i) {
    bqueue_halt[i] = 1;
  }
#else
  /* enqueue halt messages */
  for (cons_id = 0; cons_id < consumer_count; cons_id++) {
    q[cons_id]->backtrack_flag = 1;
    while (enqueue(q[cons_id], (data_t)BQ_MAGIC_64BIT) != SUCCESS)
      ;
    transaction_id++;
  }
#endif

  fipc_test_FAI(completed_producers);
}
#endif

void BQueueTest::consumer_thread(int tid, uint32_t num_nops) {
  Shard *sh = &this->shards[tid];
  sh->stats =
      //(thread_stats *)std::aligned_alloc(CACHE_LINE_SIZE,
      // sizeof(thread_stats));
      (thread_stats *)calloc(1, sizeof(thread_stats));

  uint64_t t_start, t_end;
  BaseHashTable *kmer_ht = NULL;
  uint8_t finished_producers = 0;
  alignas(64) uint64_t k = 0;
  uint64_t transaction_id = 0;
  uint32_t prod_id = 0;
  uint8_t this_cons_id = sh->shard_idx - producer_count;
  queue_t **q = this->cons_queues[this_cons_id];
  uint64_t inserted = 0u;
#ifdef WITH_VTUNE_LIB
  std::string thread_name("consumer_thread" + std::to_string(tid));
  printf("%s, thread_name %s\n", __func__, thread_name.c_str());
  __itt_thread_set_name(thread_name.c_str());
#endif
  // bq_kmer[BQ_TESTS_BATCH_LENGTH*consumer_count];

  kmer_ht = init_ht(HT_TESTS_HT_SIZE, sh->shard_idx);
  fipc_test_FAI(ready_consumers);
  while (!test_ready) fipc_test_pause();
  fipc_test_mfence();

  printf("[INFO] Consumer %u starting\n", this_cons_id);

  t_start = RDTSC_START();

  prod_id = 0;
#ifdef BQ_TESTS_USE_HALT
  while (!bqueue_halt[this_cons_id]) {
#else
  while (finished_producers < producer_count) {
#endif

    auto get_next_prod = [&](auto inc) {
      auto next_prod_id = prod_id + inc;
      if (next_prod_id >= producer_count) next_prod_id -= producer_count;
      return next_prod_id;
    };

    auto submit_batch = [&](auto num_elements) {
      KeyPairs kp = std::make_pair(num_elements, &_items[0]);

      prefetch_queue(q[get_next_prod(2)]);
      prefetch_queue_data(q[get_next_prod(1)], false);

      kmer_ht->insert_batch(kp);
      inserted += kp.first;

      data_idx = 0;
    };

    kmer_ht->prefetch_queue(QueueType::insert_queue);
    for (auto i = 0u; i < 1 * BQ_TESTS_BATCH_LENGTH; i++) {
      /* Receive and unmarshall */
      if (dequeue(q[prod_id], (data_t *)&k) != SUCCESS) {
        /* move on to next producer queue to dequeue */
        // printf("[ERROR] Consumer %u <- Producer %u \n", sh->shard_idx,
        // prod_id);
        // num_deq_failures[tid][prod_id]++;

        if (data_idx > 0) {
          submit_batch(data_idx);
        }
        break;
      }

      auto *_q = q[prod_id];
      if ((_q->tail & 7) == 0) {
        auto new_tail = (_q->tail + 8) & (QUEUE_SIZE - 1);
        __builtin_prefetch(&_q->data[new_tail], 1, 3);
      }

      if ((data_t)k == BQ_MAGIC_64BIT) {
        fipc_test_FAI(finished_producers);
        printf(
            "[INFO] Consumer %u, received HALT from prod_id %u. "
            "finished_producers :%u\n",
            this_cons_id, prod_id, finished_producers);
        continue;
      }

      if constexpr (bq_load == BQUEUE_LOAD::HtInsert) {
        _items[data_idx].key = _items[data_idx].id = k;

        // for (auto i = 0u; i < num_nops; i++) asm volatile("nop");

        if (++data_idx == BQ_TESTS_DEQUEUE_ARR_LENGTH) {
          submit_batch(BQ_TESTS_DEQUEUE_ARR_LENGTH);
        }
      }

      transaction_id++;
#ifdef CALC_STATS
      if (transaction_id % (HT_TESTS_NUM_INSERTS * consumer_count / 10) == 0) {
        printf("[INFO] Consumer %u, transaction_id %lu\n", this_cons_id,
               transaction_id);
      }
#endif
    }
    ++prod_id;
    if (prod_id >= producer_count) {
      prod_id = 0;
    }
  }

  t_end = RDTSCP();
  sh->stats->insertion_cycles = (t_end - t_start);
  sh->stats->num_inserts = transaction_id;
  get_ht_stats(sh, kmer_ht);

  printf("%s, cons_id %d | inserted %lu elements\n", __func__, this_cons_id,
         inserted);
  printf(
      "[INFO] Quick Stats: Consumer %u finished, receiving %lu messages "
      "(cycles per message %lu) prod_count %u | finished %u\n",
      this_cons_id, transaction_id, (t_end - t_start) / transaction_id,
      producer_count, finished_producers);

  /* Write to file */
  if (!this->cfg->ht_file.empty()) {
    std::string outfile = this->cfg->ht_file + std::to_string(sh->shard_idx);
    printf("[INFO] Shard %u: Printing to file: %s\n", sh->shard_idx,
           outfile.c_str());
    kmer_ht->print_to_file(outfile);
  }

  // End test
  fipc_test_FAI(completed_consumers);
}

void BQueueTest::init_queues(uint32_t nprod, uint32_t ncons) {
  uint32_t i, j;
  // Queue Allocation
  queue_t *queues = (queue_t *)std::aligned_alloc(
      FIPC_CACHE_LINE_SIZE, nprod * ncons * sizeof(queue_t));

  for (i = 0; i < nprod * ncons; ++i) init_queue(&queues[i]);

  this->prod_queues = (queue_t ***)std::aligned_alloc(
      FIPC_CACHE_LINE_SIZE, nprod * sizeof(queue_t **));
  this->cons_queues = (queue_t ***)std::aligned_alloc(
      FIPC_CACHE_LINE_SIZE, ncons * sizeof(queue_t **));

#ifdef BQ_TESTS_USE_HALT
  bqueue_halt = (int *)calloc(ncons, sizeof(*bqueue_halt));
#endif

  /* For each producer allocate a queue connecting it to <ncons>
   * consumers */
  for (i = 0; i < nprod; ++i)
    this->prod_queues[i] = (queue_t **)std::aligned_alloc(
        FIPC_CACHE_LINE_SIZE, ncons * sizeof(queue_t *));

  for (i = 0; i < ncons; ++i) {
    this->cons_queues[i] = (queue_t **)std::aligned_alloc(
        FIPC_CACHE_LINE_SIZE, nprod * sizeof(queue_t *));
#ifdef BQ_TESTS_USE_HALT
    bqueue_halt[i] = 0;
#endif
  }

  /* Queue Linking */
  for (i = 0; i < nprod; ++i) {
    for (j = 0; j < ncons; ++j) {
      this->prod_queues[i][j] = &queues[i * ncons + j];
      printf("[INFO] prod_queues[%u][%u] = %p\n", i, j, &queues[i * ncons + j]);
    }
  }

  for (i = 0; i < ncons; ++i) {
    for (j = 0; j < nprod; ++j) {
      this->cons_queues[i][j] = &queues[i + j * ncons];
      printf("[INFO] cons_queues[%u][%u] = %p\n", i, j, &queues[i + j * ncons]);
    }
  }

  // cons_buffers =  (uint64_t *) std::aligned_alloc(FIPC_CACHE_LINE_SIZE, nprod
  // * ncons * BQ_TESTS_BATCH_LENGTH * sizeof(uint64_t)); buf_idx =  (uint64_t
  // *) std::aligned_alloc(FIPC_CACHE_LINE_SIZE, nprod * ncons *
  // sizeof(uint64_t));

  // memset(buf_idx[i], 0x0, nprod * ncons * sizeof(uint64_t));
  memset(cons_buffers, 0x0, sizeof(cons_buffers));
  memset(buf_idx, 0x0, sizeof(buf_idx));
}

void BQueueTest::no_bqueues(Shard *sh, BaseHashTable *kmer_ht) {
  [[maybe_unused]] uint64_t k = 0;
  // uint64_t num_inserts = 0;
  uint64_t t_start, t_end;
  uint64_t transaction_id;

#ifdef BQ_TESTS_INSERT_XORWOW
  struct xorwow_state xw_state;
  xorwow_init(&xw_state);
#endif

  printf(
      "[INFO] no_bqueues thread %u starting. Total inserts in this "
      "thread:%lu \n",
      sh->shard_idx, HT_TESTS_NUM_INSERTS);

  t_start = RDTSC_START();

  for (transaction_id = 0u; transaction_id < HT_TESTS_NUM_INSERTS;
       transaction_id++) {
#ifdef BQ_TESTS_INSERT_XORWOW
    k = xorwow(&xw_state);
    k = k << 32 | xorwow(&xw_state);
#else
    k = transaction_id;
#endif

#ifdef BQ_TESTS_DO_HT_INSERTS
    /* Save kmer into array*/
    memcpy(&bq_kmers[data_idx].data, &k, sizeof(k));
    /* insert kmer into HT */
    kmer_ht->insert((void *)&bq_kmers[data_idx]);
    data_idx++;
    if (data_idx == BQ_TESTS_DEQUEUE_ARR_LENGTH) data_idx = 0;
#endif

    if (transaction_id % (HT_TESTS_NUM_INSERTS) == 0) {
      printf("[INFO] no_bqueues thread %u, transaction_id %lu\n", sh->shard_idx,
             transaction_id);
    }
  }

  t_end = RDTSCP();
  sh->stats->insertion_cycles = (t_end - t_start);
  sh->stats->num_inserts = transaction_id;
  get_ht_stats(sh, kmer_ht);

  printf(
      "[INFO] Quick Stats: no_bqueues thread %u finished, sending %lu messages "
      "(cycles per message %lu)\n",
      sh->shard_idx, transaction_id, (t_end - t_start) / transaction_id);
}

void BQueueTest::run_test(Configuration *cfg, Numa *n, NumaPolicyQueues *npq) {
  cpu_set_t cpuset;
  uint32_t i = 0, j = 0;

  this->n = n;
  this->nodes = this->n->get_node_config();
  this->npq = npq;
  this->cfg = cfg;

  uint32_t num_nodes = static_cast<uint32_t>(this->n->get_num_nodes());
  uint32_t num_cpus = static_cast<uint32_t>(this->n->get_num_total_cpus());

  /* num_nodes cpus not available TODO Verify this logic*/
  if (this->cfg->n_prod + this->cfg->n_cons + num_nodes > num_cpus) {
    fprintf(stderr,
            "[ERROR] producers (%u) + consumers (%u) exceeded number of "
            "available CPUs (%u)\n",
            this->cfg->n_prod, this->cfg->n_cons, num_cpus);
    fprintf(stderr,
            "[ERROR] Note: %u core(s) not available, one of which "
            "is assigned completely for synchronization\n",
            num_nodes);
    exit(-1);
  }

  producer_count = cfg->n_prod;
  consumer_count = cfg->n_cons;
  printf("[INFO]: Controller starting ... nprod: %u, ncons: %u\n",
         producer_count, consumer_count);

  /* Stats data structures */
  // this->shards = (Shard *)std::aligned_alloc(
  //    FIPC_CACHE_LINE_SIZE, sizeof(Shard) * (producer_count +
  //    consumer_count));

  this->shards =
      (Shard *)calloc(sizeof(Shard), (producer_count + consumer_count));

  // memset(this->shards, 0, sizeof(Shard) * (producer_count + consumer_count));

  /* Init queues */
  this->init_queues(cfg->n_prod, cfg->n_cons);

  fipc_test_mfence();

  /* Thread Allocation */
  this->prod_threads = new std::thread[producer_count];
  this->cons_threads = new std::thread[consumer_count];

  /* Spawn producer threads */
  for (uint32_t assigned_cpu : this->npq->get_assigned_cpu_list_producers()) {
    Shard *sh = &this->shards[i];
    sh->shard_idx = i;
    this->prod_threads[i] = std::thread(&BQueueTest::producer_thread, this, i,
                                        cfg->n_prod, cfg->n_cons);
    CPU_ZERO(&cpuset);
    CPU_SET(assigned_cpu, &cpuset);
    pthread_setaffinity_np(prod_threads[i].native_handle(), sizeof(cpu_set_t),
                           &cpuset);
    printf("[INFO]: Thread producer_thread: %u, affinity: %u\n", i,
           assigned_cpu);
    i += 1;
  }

  /* Spawn consumer threads */
  i = producer_count;
  for (uint32_t assigned_cpu : this->npq->get_assigned_cpu_list_consumers()) {
    Shard *sh = &this->shards[i];
    sh->shard_idx = i;
    this->cons_threads[j] =
        std::thread(&BQueueTest::consumer_thread, this, i, cfg->num_nops);
    CPU_ZERO(&cpuset);
    CPU_SET(assigned_cpu, &cpuset);
    pthread_setaffinity_np(this->cons_threads[j].native_handle(),
                           sizeof(cpu_set_t), &cpuset);
    printf("[INFO]: Thread consumer_thread: %u, affinity: %u\n", i,
           assigned_cpu);
    i += 1;
    j += 1;
  }

  /* pin this thread to last cpu of last node. */
  /* TODO don't waste one thread on synchronization  */
  CPU_ZERO(&cpuset);
  uint32_t last_cpu = this->npq->get_unassigned_cpu_list()[0];
  CPU_SET(last_cpu, &cpuset);
  sched_setaffinity(0, sizeof(cpu_set_t), &cpuset);
  printf("[INFO]: Thread 'controller': affinity: %u\n", last_cpu);

  /* Wait for threads to be ready for test */
  while (ready_consumers < consumer_count) fipc_test_pause();
  while (ready_producers < producer_count) fipc_test_pause();

  fipc_test_mfence();

  /* Begin Test */
  test_ready = 1;
  fipc_test_mfence();

  /* Wait for producers to complete */
  while (completed_producers < producer_count) fipc_test_pause();

  fipc_test_mfence();

  /* Wait for consumers to complete */
  while (completed_consumers < consumer_count) fipc_test_pause();

  fipc_test_mfence();

  cfg->num_threads = producer_count + consumer_count;
  print_stats(this->shards, *cfg);

  /* Tell consumers to halt once producers are done */
  // return 0;
  for (auto i = 0u; i < cfg->n_prod; i++) {
    if (this->prod_threads[i].joinable()) {
      this->prod_threads[i].join();
    }
  }

  for (auto i = 0u; i < cfg->n_cons; i++) {
    if (this->cons_threads[i].joinable()) {
      this->cons_threads[i].join();
    }
  }
  /* TODO free everything */
}

}  // namespace kmercounter
