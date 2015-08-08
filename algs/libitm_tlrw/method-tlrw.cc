#include <stdio.h>
#include <stdlib.h>

#include "libitm_i.h"

using namespace GTM;

namespace {

enum {
    READ_TIMEOUT = 32,
    ACQUIRE_TIMEOUT = 32,
    BYTE_ARRAY_FIELD_SIZE = 48,
    NUMBER_SLOTTED_THREADS = BYTE_ARRAY_FIELD_SIZE,
};

// The bytelock structure.
typedef struct bytelock_s {
    atomic<gtm_word> owner;
    atomic<gtm_word> reader_counter;
    char byte_array[BYTE_ARRAY_FIELD_SIZE];
} __attribute__((aligned(HW_CACHELINE_SIZE))) bytelock_t;

// This group consists of all TM methods that synchronize via multiple locks
// (or ownership records).
struct tlrw_mg : public method_group
{
  // The array of ownership records.
  bytelock_t* bytelocks __attribute__((aligned(HW_CACHELINE_SIZE)));
  char tailpadding[HW_CACHELINE_SIZE - sizeof(bytelock_t*)];

  // Location-to-orec mapping.  Stripes of 16B mapped to 2^19 orecs.
  static const gtm_word L2O_BYTELOCK = 1 << 19;
  static const gtm_word L2O_SHIFT = 4;
  static size_t get_bytelock(const void* addr)
  {
    return ((uintptr_t)addr >> L2O_SHIFT) & (L2O_BYTELOCK - 1);
  }
  static size_t get_next_bytelock(size_t bytelock)
  {
    return (bytelock + 1) & (L2O_BYTELOCK - 1);
  }
  // Returns the next orec after the region.
  static size_t get_bytelock_end(const void* addr, size_t len)
  {
    return (((uintptr_t)addr + len + (1 << L2O_SHIFT) - 1) >> L2O_SHIFT)
        & (L2O_BYTELOCK - 1);
  }

  static bool lookup_bytelocks_log(size_t el, vector<size_t>::iterator begin,
                                   vector<size_t>::iterator end)
  {
      for ( ; begin != end; begin++)
          if (*begin == el)
              return true;

      return false;
  }

  virtual void init()
  {
    // We assume that an atomic<gtm_word> is backed by just a gtm_word, so
    // starting with zeroed memory is fine.
    bytelocks = (bytelock_t*) xcalloc(
        sizeof(bytelock_t) * L2O_BYTELOCK, true);
  }

  virtual void fini()
  {
      for (unsigned int i = 0; i < L2O_BYTELOCK; i++) {
          if (bytelocks[i].owner.load() != 0)
              printf("%s:%d error due to writer!\n", __func__, __LINE__);
          for (unsigned int j = 0; j < BYTE_ARRAY_FIELD_SIZE; j++) {
              if (bytelocks[i].byte_array[j] != 0)
                  printf("%s:%d error due to reader!\n", __func__, __LINE__);
          }
      }
      free(bytelocks);
  }

  // NOTE!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!! May be not neccessary.
  // We only re-initialize when our time base overflows.  Thus, only reset
  // the time base and the orecs but do not re-allocate the orec array.
  virtual void reinit()
  {
    // This store is only executed while holding the serial lock, so relaxed
    // memory order is sufficient here.  Same holds for the memset.
    memset(bytelocks, 0, sizeof(bytelock_t) * L2O_BYTELOCK);
  }
};

static tlrw_mg o_tlrw_mg;

class tlrw_dispatch : public abi_dispatch
{
protected:
  static void pre_write(gtm_thread *tx, const void *addr, size_t len)
  {
      unsigned thread_id = tx->thread_id;
      bytelock_t *&bls = o_tlrw_mg.bytelocks;
      int tries = 0;

      /*
       * bl - byte lock
       */
      size_t bl = tlrw_mg::get_bytelock(addr);
      size_t bl_end = tlrw_mg::get_bytelock_end(addr, len);
      gtm_word expected = 0;
      do {
          if (bls[bl].owner.load(memory_order_relaxed) != thread_id) {
              tries = 0;
              while (!bls[bl].owner.compare_exchange_weak(
                      expected, thread_id, memory_order_acquire)) {
                  if (++tries >= ACQUIRE_TIMEOUT)
                      tx->restart(RESTART_LOCKED_WRITE);
                  expected = 0;

              }
              size_t *e = tx->writebytelocks.push();
              *e = bl;

              // If current thread is unslotted, we need decrease the reader_counter,
              // if the thread is reader.
              if (thread_id < NUMBER_SLOTTED_THREADS) {
                  bls[bl].byte_array[thread_id] = 0;
              } else {
                  if (tlrw_mg::lookup_bytelocks_log(bl,
                                 tx->readbytelocks.begin(),
                                 tx->readbytelocks.end()) == true) {
                      bls[bl].reader_counter--;
                  }
              }

              // Wait readers
              for (int i = 0; i < BYTE_ARRAY_FIELD_SIZE; i = i+(sizeof(gtm_word))) {
                  volatile gtm_word *tmp =
                      (gtm_word *)&bls[bl].byte_array[i];

                  tries = 0;
                  while( *tmp != 0) {
                      if (++tries >= ACQUIRE_TIMEOUT)
                          tx->restart(RESTART_LOCKED_WRITE);
                  }
              }

              tries = 0;
              while(bls[bl].reader_counter.load(memory_order_acquire) != 0) {
                  if (++tries >= ACQUIRE_TIMEOUT)
                      tx->restart(RESTART_LOCKED_WRITE);
              }
          }

          bl = o_tlrw_mg.get_next_bytelock(bl);
      } while (bl != bl_end);

      tx->undolog.log(addr, len);
  }

  static gtm_rwlog_entry* pre_load(gtm_thread *tx, const void* addr,
      size_t len)
  {
      unsigned thread_id = tx->thread_id;
      bool slotted = thread_id < NUMBER_SLOTTED_THREADS ? true : false;
      bytelock_t *&bls = o_tlrw_mg.bytelocks;
      int tries = 0;

      /*
       * bl - byte lock
       */
      size_t bl = tlrw_mg::get_bytelock(addr);
      size_t bl_end = tlrw_mg::get_bytelock_end(addr, len);
      do {
          if (bls[bl].owner.load(memory_order_relaxed) != thread_id) {
              if (slotted && (bls[bl].byte_array[thread_id] == 1)) {
                  bl = o_tlrw_mg.get_next_bytelock(bl);
                  continue;
              }

              if (unlikely(!slotted && (tlrw_mg::lookup_bytelocks_log(bl,
                                   tx->readbytelocks.begin(),
                                   tx->readbytelocks.end()) == true))) {
                  bl = o_tlrw_mg.get_next_bytelock(bl);
                  continue;
              }

              while (true) {
                  if (likely(slotted)) {
                      bls[bl].byte_array[thread_id] = 1;
                      atomic_thread_fence(memory_order_release);
                  } else {
                      bls[bl].reader_counter++;
                  }
          
                  if (bls[bl].owner.load(memory_order_seq_cst) == 0) {
                      break;
                  }
          
                  if (likely(slotted)) {
                      bls[bl].byte_array[thread_id] = 0;
                      atomic_thread_fence(memory_order_release);
                  } else {
                      bls[bl].reader_counter--;
                  }

                  tries = 0;
                  //WARN! Need a timeout....
                  while (bls[bl].owner.load(memory_order_acquire) != 0) {
                      if (++tries >= READ_TIMEOUT) {
                          tx->restart(RESTART_LOCKED_READ);
                      }
                  }
              }
          }
          size_t *e = tx->readbytelocks.push();
          *e = bl;
          bl = o_tlrw_mg.get_next_bytelock(bl);
      } while (bl != bl_end);

      return NULL;
  }

  template <typename V> static V load(const V* addr, ls_modifier mod)
  {
      gtm_thread *tx = gtm_thr();
      
      if (unlikely(mod == RfW)) {
          pre_write(tx, addr, sizeof(V));
          return *addr;
      }
      if (unlikely(mod == RaW)) {
          return *addr;
      }
      pre_load(tx, addr, sizeof(V));

      V v = *addr;

      return v; 
  }

  template <typename V> static void store(V* addr, const V value,
      ls_modifier mod)
  {
      gtm_thread *tx = gtm_thr();
      
      if (likely(mod != WaW)) {
          pre_write(tx, addr, sizeof(V));          
      }
      *addr = value;
  }

public:
  static void memtransfer_static(void *dst, const void* src, size_t size,
      bool may_overlap, ls_modifier dst_mod, ls_modifier src_mod)
  {
      gtm_thread *tx = gtm_thr();
      
      if (src_mod == RfW) {
          pre_write(tx, src, size);
      } else if (src_mod != RaW && src_mod != NONTXNAL) {
          pre_load(tx, src, size);
      }

      if (dst_mod != NONTXNAL && dst_mod != WaW) {
          pre_write(tx, dst, size);
      }

      if (!may_overlap) {
          ::memcpy(dst, src, size);
      } else {
          ::memmove(dst, src, size);
      }
  }

  static void memset_static(void *dst, int c, size_t size, ls_modifier mod)
  {
      gtm_thread *tx = gtm_thr();

      if (mod != WaW) {
          pre_write(tx, dst, size);
      }

      ::memset(dst, c, size);
  }

  virtual gtm_restart_reason begin_or_restart()
  {
      return NO_RESTART;
  }

  virtual bool trycommit(gtm_word& priv_time)
  {
      gtm_thread *tx = gtm_thr();
      unsigned thread_id = tx->thread_id;
      bool slotted = thread_id < NUMBER_SLOTTED_THREADS ? true : false;
      bytelock_t *&bls = o_tlrw_mg.bytelocks;

      for (size_t *i = tx->writebytelocks.begin(),
               *ie = tx->writebytelocks.end(); i != ie; i++) {
          bls[*i].owner.store(0, memory_order_seq_cst);
      }

      for (size_t *i = tx->readbytelocks.begin(),
               *ie = tx->readbytelocks.end(); i != ie; i++) {
          if (slotted) {
              bls[*i].byte_array[thread_id] = 0;
              atomic_thread_fence(memory_order_seq_cst);
          } else {
              bls[*i].reader_counter--;
          }
      }
      
      tx->writebytelocks.clear();
      tx->readbytelocks.clear();

      return true;
  }

  virtual void rollback()
  {
      gtm_thread *tx = gtm_thr();
      unsigned thread_id = tx->thread_id;
      bool slotted = thread_id < NUMBER_SLOTTED_THREADS ? true : false;
      bytelock_t *&bls = o_tlrw_mg.bytelocks;

      for (size_t *i = tx->writebytelocks.begin(),
               *ie = tx->writebytelocks.end(); i != ie; i++) {
          bls[*i].owner.store(0, memory_order_seq_cst);
      }

      for (size_t *i = tx->readbytelocks.begin(),
               *ie = tx->readbytelocks.end(); i != ie; i++) {
          if (slotted) {
              bls[*i].byte_array[thread_id] = 0;
              atomic_thread_fence(memory_order_seq_cst);
          } else {
              bls[*i].reader_counter--;
          }
      }
      
      tx->writebytelocks.clear();
      tx->readbytelocks.clear();
  }

  virtual bool supports(unsigned number_of_threads)
  {
      return true;
  }

  CREATE_DISPATCH_METHODS(virtual, )
  CREATE_DISPATCH_METHODS_MEM()

  tlrw_dispatch() : abi_dispatch(false, true, false, 0, &o_tlrw_mg)
  { }
};

} // anon namespace

static const tlrw_dispatch o_tlrw_dispatch;

abi_dispatch *
GTM::dispatch_tlrw ()
{
    return const_cast<tlrw_dispatch *>(&o_tlrw_dispatch);
}
