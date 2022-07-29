/*-------------------------------------------------------------------------
 *
 * cxxmapam_handler.cpp
 *	  Table access method using C++ std::map.
 *
 * Author: Youki Shiraishi <youki.shiraishi@ntt.com>
 * Copyright (c) 2022, Nippon Telegraph and Telephone Corporation
 *
 * IDENTIFICATION
 *    src/cxxmapam_handler.cpp
 *
 *-------------------------------------------------------------------------
 */

#include <atomic>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>

#if __cplusplus > 199711L
#define register    // deprecated in C++11
#endif

extern "C" {
#include "postgres.h"
#include "access/heapam.h"
#include "access/multixact.h"
#include "access/tableam.h"
}

extern "C" {
PG_MODULE_MAGIC;

PG_FUNCTION_INFO_V1(cxxmap_tableam_handler);
}


/* ------------------------------------------------------------------------
 * In-memory storage implementation.
 * ------------------------------------------------------------------------
 */

/*
 * In-memory storage is implemented using std::map, which stores pairs of
 * ItemKey and TupData. The RowId of an ItemKey is used as a logical row
 * identifier for the table of the TableId. ItemKeys are converted to/from
 * ItemPointers. The conversion ensures that the resulting ItemPointer
 * looks valid, i.e. TableId 0 and RowId 0 are never used.
 */
using TableId = Oid;
using RowId = OffsetNumber;
using ItemKey = std::pair<TableId, RowId>;
using TupData = std::pair<AttrNumber, Datum *>;
using Storage = std::map<ItemKey, TupData>;

/*
 * Maintain a monotonically increasing row counter for each table to
 * retrieve available row numbers.
 */
static std::map<Oid, std::atomic<RowId>> row_counters;

/* Singleton storage is implemented using double-checked locking. */
static std::unique_ptr<Storage> cxxmap_instance = nullptr;
static std::mutex lock;

static void cxxmap_init_storage()
{
    if (cxxmap_instance == nullptr) {
        std::lock_guard<std::mutex> guard(lock);
        if (cxxmap_instance == nullptr) {
            cxxmap_instance = std::make_unique<Storage>();
        }
    }
}


/* ------------------------------------------------------------------------
 * Table access method implementation.
 * ------------------------------------------------------------------------
 */

/* Structures for scans. */
typedef struct CXXMapScanDescData
{
    TableScanDescData rs_base;  /* AM independent part of the descriptor */
    RowId rs_cursor;
} CXXMapScanDescData;

typedef struct CXXMapScanDescData *CXXMapScanDesc;


static const TupleTableSlotOps *
cxxmap_slot_callbacks(Relation rel)
{
    /*
     * Since "virtual" (not "physical") tuple table slots are used, the
     * table access method takes responsibility for resource management.
     */
    return &TTSOpsVirtual;
}

static TableScanDesc
cxxmap_scan_begin(Relation rel,
                  Snapshot snapshot,
                  int nkeys,
                  ScanKey key,
                  ParallelTableScanDesc pscan,
                  uint32 flags)
{
    auto cscan = (CXXMapScanDesc) palloc(sizeof(CXXMapScanDescData));
    cscan->rs_base.rs_rd = rel;
    cscan->rs_cursor = FirstOffsetNumber;

    return (TableScanDesc) cscan;
}

static void
cxxmap_scan_end(TableScanDesc scan)
{
    auto cscan = (CXXMapScanDesc) scan;
    pfree(cscan);
}

static void
cxxmap_scan_rescan(TableScanDesc scan,
                   ScanKey key,
                   bool set_params,
                   bool allow_strat,
                   bool allow_sync,
                   bool allow_pagemode)
{
    /* Nothing to do. */
}

static bool
cxxmap_scan_getnextslot(TableScanDesc scan,
                        ScanDirection direction,
                        TupleTableSlot *slot)
{
    auto cscan = (CXXMapScanDesc) scan;

    /* Mark the slot empty. */
    ExecClearTuple(slot);

    /* Retrieve tuple data. */
    TableId table_id = cscan->rs_base.rs_rd->rd_id;
    RowId cur_row_id = cscan->rs_cursor;
    ItemKey key = std::make_pair(table_id, cur_row_id);

    TupData t_data;

    try {
        t_data = cxxmap_instance->at(key);
        cscan->rs_cursor += 1;
    } catch(std::out_of_range&) {
        /* No more tuples. */
        return false;
    }

    /* Fill isnull/values array for the slot. */
    AttrNumber nattrs = t_data.first;
    Datum *values = t_data.second;

    for (int i = 0; i < nattrs; ++i) {
        slot->tts_isnull[i] = false;
        slot->tts_values[i] = values[i];
    }

    /* Mark the slot valid. */
    ExecStoreVirtualTuple(slot);

    /* May be more tuples. */
    return true;
}

static IndexFetchTableData *
cxxmap_index_fetch_begin(Relation rel)
{
    /* Nothing to do. */
    return nullptr;
}

static void
cxxmap_index_fetch_reset(IndexFetchTableData *data)
{
    /* Nothing to do. */
}

static void
cxxmap_index_fetch_end(IndexFetchTableData *data)
{
    /* Nothing to do. */
}

static bool
cxxmap_index_fetch_tuple(IndexFetchTableData *scan,
                         ItemPointer tid,
                         Snapshot snapshot,
                         TupleTableSlot *slot,
                         bool *call_again,
                         bool *all_dead)
{
    /* Nothing to do. */
    return false;
}

static bool
cxxmap_fetch_row_version(Relation rel,
                         ItemPointer tid,
                         Snapshot snapshot,
                         TupleTableSlot *slot)
{
    /* Nothing to do. */
    return false;
}

static void
cxxmap_get_latest_tid(TableScanDesc scan, ItemPointer tid)
{
    /* Nothing to do. */
}

static bool
cxxmap_tuple_tid_valid(TableScanDesc scan, ItemPointer tid)
{
    /* Nothing to do. */
    return false;
}

static bool
cxxmap_tuple_satisfies_snapshot(Relation rel,
                                TupleTableSlot *slot,
                                Snapshot snapshot)
{
    /* Nothing to do. */
    return false;
}

static TransactionId
cxxmap_index_delete_tuples(Relation rel, TM_IndexDeleteOp *delstate)
{
    /* Nothing to do. */
    return InvalidTransactionId;
}

static void
cxxmap_tuple_insert(Relation rel,
                    TupleTableSlot *slot,
                    CommandId cid,
                    int options,
                    BulkInsertState bistate)
{
    TableId table_id = RelationGetRelid(rel);
    slot->tts_tableOid = table_id;

    /*
     * Ensure that the table ID is also valid in terms of BlockNumber,
     * since the table ID is also used as a BlockNumber.
     */
    Assert(BlockNumberIsValid(table_id));

    if (!row_counters.contains(table_id)) {
        row_counters[table_id] = FirstOffsetNumber;
    }

    /* Retrieve an available row ID. */
    uint16_t row_id = row_counters[table_id]
            .fetch_add(1, std::memory_order_relaxed);

    ItemPointerData tid = { { 0 } };
    ItemPointerSetBlockNumber(&tid, table_id);
    ItemPointerSetOffsetNumber(&tid, row_id);

    slot->tts_tid = tid;

    /* The inserted value is copied to the newly allocated memory area. */
    auto *values = new Datum[slot->tts_nvalid];
    std::memcpy(values, slot->tts_values, slot->tts_nvalid * sizeof(Datum));
    TupData tupdata = std::make_pair(slot->tts_nvalid, values);

    /* Perform insertion. */
    ItemKey key = std::make_pair(table_id, row_id);
    cxxmap_instance->insert(std::make_pair(key, tupdata));
}

static void
cxxmap_tuple_insert_speculative(Relation rel,
                                TupleTableSlot *slot,
                                CommandId cid,
                                int options,
                                BulkInsertState bistate,
                                uint32 specToken)
{
    /* Nothing to do. */
}

static void
cxxmap_tuple_complete_speculative(Relation rel,
                                  TupleTableSlot *slot,
                                  uint32 specToken,
                                  bool succeeded)
{
    /* Nothing to do. */
}

static void
cxxmap_multi_insert(Relation rel,
                    TupleTableSlot **slots,
                    int nslots,
                    CommandId cid,
                    int options,
                    BulkInsertState bistate)
{
    /* Nothing to do. */
}

static TM_Result
cxxmap_tuple_delete(Relation rel,
                    ItemPointer tid,
                    CommandId cid,
                    Snapshot snapshot,
                    Snapshot crosscheck,
                    bool wait,
                    TM_FailureData *tmfd,
                    bool changingPart)
{
    /* Nothing to do. */
    return TM_Ok;
}


static TM_Result
cxxmap_tuple_update(Relation rel,
                    ItemPointer otid,
                    TupleTableSlot *slot,
                    CommandId cid,
                    Snapshot snapshot,
                    Snapshot crosscheck,
                    bool wait,
                    TM_FailureData *tmfd,
                    LockTupleMode *lockmode,
                    bool *update_indexes)
{
    /* Nothing to do. */
    return TM_Ok;
}

static TM_Result
cxxmap_tuple_lock(Relation rel,
                  ItemPointer tid,
                  Snapshot snapshot,
                  TupleTableSlot *slot,
                  CommandId cid,
                  LockTupleMode mode,
                  LockWaitPolicy wait_policy,
                  uint8 flags,
                  TM_FailureData *tmfd)
{
    /* Nothing to do. */
    return TM_Ok;
}

static void
cxxmap_finish_bulk_insert(Relation rel, int options)
{
    /* Nothing to do. */
}

static void
cxxmap_relation_set_new_filenode(Relation rel,
                                 const RelFileNode *newrnode,
                                 char persistence,
                                 TransactionId *freezeXid,
                                 MultiXactId *minmulti)
{
    /* Nothing to do. */
    *freezeXid = InvalidTransactionId;
    *minmulti = InvalidMultiXactId;
}

static void
cxxmap_relation_nontransactional_truncate(Relation rel)
{
    /* Nothing to do. */
}

static void
cxxmap_copy_data(Relation rel, const RelFileNode *newrnode)
{
    /* Nothing to do. */
}

static void
cxxmap_copy_for_cluster(Relation OldTable,
                        Relation NewTable,
                        Relation OldIndex,
                        bool use_sort,
                        TransactionId OldestXmin,
                        TransactionId *xid_cutoff,
                        MultiXactId *multi_cutoff,
                        double *num_tuples,
                        double *tups_vacuumed,
                        double *tups_recently_dead)
{
    /* Nothing to do. */
}

static void
cxxmap_vacuum(Relation rel,
              VacuumParams *params,
              BufferAccessStrategy bstrategy)
{
    /* Nothing to do. */
}

static bool
cxxmap_scan_analyze_next_block(TableScanDesc scan,
                               BlockNumber blockno,
                               BufferAccessStrategy bstrategy)
{
    /* Nothing to do. */
    return false;
}

static bool
cxxmap_scan_analyze_next_tuple(TableScanDesc scan,
                               TransactionId OldestXmin,
                               double *liverows,
                               double *deadrows,
                               TupleTableSlot *slot)
{
    /* Nothing to do. */
    return false;
}

static double
cxxmap_index_build_range_scan(Relation table_rel,
                              Relation index_rel,
                              IndexInfo *index_info,
                              bool allow_sync,
                              bool anyvisible,
                              bool progress,
                              BlockNumber start_blockno,
                              BlockNumber numblocks,
                              IndexBuildCallback callback,
                              void *callback_state,
                              TableScanDesc scan)
{
    /* Nothing to do. */
    return 0;
}

static void
cxxmap_index_validate_scan(Relation table_rel,
                           Relation index_rel,
                           IndexInfo *index_info,
                           Snapshot snapshot,
                           ValidateIndexState *state)
{
    /* Nothing to do. */
}

static uint64
cxxmap_relation_size(Relation rel, ForkNumber forkNumber)
{
    /* Nothing to do. */
    return 0;
}

static bool
cxxmap_relation_needs_toast_table(Relation rel)
{
    /* Nothing to do. */
    return false;
}

static void
cxxmap_relation_estimate_size(Relation rel,
                              int32 *attr_widths,
                              BlockNumber *pages,
                              double *tuples,
                              double *allvisfrac)
{
    /* Nothing to do. */
}

static bool
cxxmap_scan_bitmap_next_block(TableScanDesc scan, TBMIterateResult *tbmres)
{
    /* Nothing to do. */
    return false;
}

static bool
cxxmap_scan_bitmap_next_tuple(TableScanDesc scan,
                              TBMIterateResult *tbmres,
                              TupleTableSlot *slot)
{
    /* Nothing to do. */
    return false;
}

static bool
cxxmap_scan_sample_next_block(TableScanDesc scan,
                              SampleScanState *scanstate)
{
    /* Nothing to do. */
    return false;
}

static bool
cxxmap_scan_sample_next_tuple(TableScanDesc scan,
                              SampleScanState *scanstate,
                              TupleTableSlot *slot)
{
    /* Nothing to do. */
    return false;
}


static const TableAmRoutine cxxmapam_methods = {
        /* This must be set to T_TableAmRoutine */
        .type = T_TableAmRoutine,

        /* Slot related callbacks. */
        .slot_callbacks = cxxmap_slot_callbacks,

        /* Table scan callbacks. */
        .scan_begin = cxxmap_scan_begin,
        .scan_end = cxxmap_scan_end,
        .scan_rescan = cxxmap_scan_rescan,
        .scan_getnextslot = cxxmap_scan_getnextslot,

        /* Parallel table scan related functions. */
        .parallelscan_estimate = table_block_parallelscan_estimate,
        .parallelscan_initialize = table_block_parallelscan_initialize,
        .parallelscan_reinitialize = table_block_parallelscan_reinitialize,

        /* Index scan callbacks. */
        .index_fetch_begin = cxxmap_index_fetch_begin,
        .index_fetch_reset = cxxmap_index_fetch_reset,
        .index_fetch_end = cxxmap_index_fetch_end,
        .index_fetch_tuple = cxxmap_index_fetch_tuple,

        /* Callbacks for non-modifying operations on individual tuples. */
        .tuple_fetch_row_version = cxxmap_fetch_row_version,
        .tuple_tid_valid = cxxmap_tuple_tid_valid,
        .tuple_get_latest_tid = cxxmap_get_latest_tid,
        .tuple_satisfies_snapshot = cxxmap_tuple_satisfies_snapshot,
        .index_delete_tuples = cxxmap_index_delete_tuples,

        /* Manipulations of physical tuples. */
        .tuple_insert = cxxmap_tuple_insert,
        .tuple_insert_speculative = cxxmap_tuple_insert_speculative,
        .tuple_complete_speculative = cxxmap_tuple_complete_speculative,
        .multi_insert = cxxmap_multi_insert,
        .tuple_delete = cxxmap_tuple_delete,
        .tuple_update = cxxmap_tuple_update,
        .tuple_lock = cxxmap_tuple_lock,
        .finish_bulk_insert = cxxmap_finish_bulk_insert,

        /* DDL related functionality. */
        .relation_set_new_filenode = cxxmap_relation_set_new_filenode,
        .relation_nontransactional_truncate = cxxmap_relation_nontransactional_truncate,
        .relation_copy_data = cxxmap_copy_data,
        .relation_copy_for_cluster = cxxmap_copy_for_cluster,
        .relation_vacuum = cxxmap_vacuum,
        .scan_analyze_next_block = cxxmap_scan_analyze_next_block,
        .scan_analyze_next_tuple = cxxmap_scan_analyze_next_tuple,
        .index_build_range_scan = cxxmap_index_build_range_scan,
        .index_validate_scan = cxxmap_index_validate_scan,

        /* Miscellaneous functions. */
        .relation_size = cxxmap_relation_size,
        .relation_needs_toast_table = cxxmap_relation_needs_toast_table,

        /* Planner related functions. */
        .relation_estimate_size = cxxmap_relation_estimate_size,

        /* Executor related functions. */
        .scan_bitmap_next_block = cxxmap_scan_bitmap_next_block,
        .scan_bitmap_next_tuple = cxxmap_scan_bitmap_next_tuple,
        .scan_sample_next_block = cxxmap_scan_sample_next_block,
        .scan_sample_next_tuple = cxxmap_scan_sample_next_tuple
};


Datum
cxxmap_tableam_handler(PG_FUNCTION_ARGS)
{
    cxxmap_init_storage();
    PG_RETURN_POINTER(&cxxmapam_methods);
}
