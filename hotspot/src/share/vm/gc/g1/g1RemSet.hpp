/*
 * Copyright (c) 2001, 2015, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 *
 */

#ifndef SHARE_VM_GC_G1_G1REMSET_HPP
#define SHARE_VM_GC_G1_G1REMSET_HPP

#include "gc/g1/dirtyCardQueue.hpp"
#include "gc/g1/g1RemSetSummary.hpp"
#include "gc/g1/heapRegion.hpp"
#include "memory/allocation.hpp"
#include "memory/iterator.hpp"

// A G1RemSet provides ways of iterating over pointers into a selected
// collection set.

class BitMap;
class CardTableModRefBS;
class G1BlockOffsetSharedArray;
class ConcurrentG1Refine;
class CodeBlobClosure;
class G1CollectedHeap;
class G1CollectorPolicy;
class G1ParPushHeapRSClosure;
class G1SATBCardTableModRefBS;
class HeapRegionClaimer;

// A G1RemSet in which each heap region has a rem set that records the
// external heap references into it.  Uses a mod ref bs to track updates,
// so that they can be used to update the individual region remsets.

class G1RemSet: public CHeapObj<mtGC> {
private:
  G1RemSetSummary _prev_period_summary;

  // A DirtyCardQueueSet that is used to hold cards that contain
  // references into the current collection set. This is used to
  // update the remembered sets of the regions in the collection
  // set in the event of an evacuation failure.
  DirtyCardQueueSet _into_cset_dirty_card_queue_set;

protected:
  G1CollectedHeap* _g1;
  size_t _conc_refine_cards;
  uint n_workers();

protected:
  CardTableModRefBS*     _ct_bs;
  G1CollectorPolicy*     _g1p;

  ConcurrentG1Refine*    _cg1r;

  // Used for caching the closure that is responsible for scanning
  // references into the collection set.
  G1ParPushHeapRSClosure** _cset_rs_update_cl;

public:
  // Gives an approximation on how many threads can be expected to add records to
  // a remembered set in parallel. This can be used for sizing data structures to
  // decrease performance losses due to data structure sharing.
  // Examples for quantities that influence this value are the maximum number of
  // mutator threads, maximum number of concurrent refinement or GC threads.
  static uint num_par_rem_sets();

  // Initialize data that depends on the heap size being known.
  static void initialize(uint max_regions);

  // This is called to reset dual hash tables after the gc pause
  // is finished and the initial hash table is no longer being
  // scanned.
  void cleanupHRRS();

  G1RemSet(G1CollectedHeap* g1, CardTableModRefBS* ct_bs);
  ~G1RemSet();

  // Invoke "blk->do_oop" on all pointers into the collection set
  // from objects in regions outside the collection set (having
  // invoked "blk->set_region" to set the "from" region correctly
  // beforehand.)
  //
  // Apply non_heap_roots on the oops of the unmarked nmethods
  // on the strong code roots list for each region in the
  // collection set.
  //
  // The "worker_i" param is for the parallel case where the id
  // of the worker thread calling this function can be helpful in
  // partitioning the work to be done. It should be the same as
  // the "i" passed to the calling thread's work(i) function.
  // In the sequential case this param will be ignored.
  //
  // Returns the number of cards scanned while looking for pointers
  // into the collection set.
  size_t oops_into_collection_set_do(G1ParPushHeapRSClosure* blk,
                                     CodeBlobClosure* heap_region_codeblobs,
                                     uint worker_i);

  // Prepare for and cleanup after an oops_into_collection_set_do
  // call.  Must call each of these once before and after (in sequential
  // code) any threads call oops_into_collection_set_do.  (This offers an
  // opportunity to sequential setup and teardown of structures needed by a
  // parallel iteration over the CS's RS.)
  void prepare_for_oops_into_collection_set_do();
  void cleanup_after_oops_into_collection_set_do();

  size_t scanRS(G1ParPushHeapRSClosure* oc,
                CodeBlobClosure* heap_region_codeblobs,
                uint worker_i);

  void updateRS(DirtyCardQueue* into_cset_dcq, uint worker_i);

  CardTableModRefBS* ct_bs() { return _ct_bs; }

  // Record, if necessary, the fact that *p (where "p" is in region "from",
  // which is required to be non-NULL) has changed to a new non-NULL value.
  template <class T> void par_write_ref(HeapRegion* from, T* p, uint tid);

  // Requires "region_bm" and "card_bm" to be bitmaps with 1 bit per region
  // or card, respectively, such that a region or card with a corresponding
  // 0 bit contains no part of any live object.  Eliminates any remembered
  // set entries that correspond to dead heap ranges. "worker_num" is the
  // parallel thread id of the current thread, and "hrclaimer" is the
  // HeapRegionClaimer that should be used.
  void scrub(BitMap* region_bm, BitMap* card_bm, uint worker_num, HeapRegionClaimer* hrclaimer);

  // Refine the card corresponding to "card_ptr".
  // If check_for_refs_into_cset is true, a true result is returned
  // if the given card contains oops that have references into the
  // current collection set.
  virtual bool refine_card(jbyte* card_ptr,
                           uint worker_i,
                           bool check_for_refs_into_cset);

  // Print accumulated summary info from the start of the VM.
  virtual void print_summary_info();

  // Print accumulated summary info from the last time called.
  virtual void print_periodic_summary_info(const char* header, uint period_count);

  // Prepare remembered set for verification.
  virtual void prepare_for_verify();

  size_t conc_refine_cards() const {
    return _conc_refine_cards;
  }
};

class ScanRSClosure : public HeapRegionClosure {
  size_t _cards_done, _cards;
  G1CollectedHeap* _g1h;

  G1ParPushHeapRSClosure* _oc;
  CodeBlobClosure* _code_root_cl;

  G1BlockOffsetSharedArray* _bot_shared;
  G1SATBCardTableModRefBS *_ct_bs;

  double _strong_code_root_scan_time_sec;
  uint   _worker_i;
  size_t _block_size;
  bool   _try_claimed;

public:
  ScanRSClosure(G1ParPushHeapRSClosure* oc,
                CodeBlobClosure* code_root_cl,
                uint worker_i);

  bool doHeapRegion(HeapRegion* r);

  double strong_code_root_scan_time_sec() {
    return _strong_code_root_scan_time_sec;
  }
  size_t cards_done() { return _cards_done;}
  size_t cards_looked_up() { return _cards;}
  void set_try_claimed() { _try_claimed = true; }
private:
  void scanCard(size_t index, HeapRegion *r);
  void printCard(HeapRegion* card_region, size_t card_index,
                 HeapWord* card_start);
  void scan_strong_code_roots(HeapRegion* r);
};

class UpdateRSOopClosure: public ExtendedOopClosure {
  HeapRegion* _from;
  G1RemSet* _rs;
  uint _worker_i;

  template <class T> void do_oop_work(T* p);

public:
  UpdateRSOopClosure(G1RemSet* rs, uint worker_i = 0) :
    _from(NULL), _rs(rs), _worker_i(worker_i)
  {}

  void set_from(HeapRegion* from) {
    assert(from != NULL, "from region must be non-NULL");
    _from = from;
  }

  virtual void do_oop(narrowOop* p) { do_oop_work(p); }
  virtual void do_oop(oop* p)       { do_oop_work(p); }
};

#endif // SHARE_VM_GC_G1_G1REMSET_HPP
