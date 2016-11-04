/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil ; -*- */
/*
 *  (C) 2006 by Argonne National Laboratory.
 *      See COPYRIGHT in top-level directory.
 *
 *  Portions of this code were written by Intel Corporation.
 *  Copyright (C) 2011-2016 Intel Corporation.  Intel provides this material
 *  to Argonne National Laboratory subject to Software Grant and Corporate
 *  Contributor License Agreement dated February 8, 2012.
 */
#ifndef NETMOD_OFI_WIN_H_INCLUDED
#define NETMOD_OFI_WIN_H_INCLUDED

#include "ofi_impl.h"
#include <opa_primitives.h>


#undef FUNCNAME
#define FUNCNAME MPIDI_OFI_win_allgather
#undef FCNAME
#define FCNAME MPL_QUOTE(FUNCNAME)
static inline int MPIDI_OFI_win_allgather(MPIR_Win * win, void *base, int disp_unit)
{
    int i, same_disp, mpi_errno = MPI_SUCCESS;
    uint32_t first;
    MPIR_Errflag_t errflag = MPIR_ERR_NONE;
    MPIR_Comm *comm_ptr = win->comm_ptr;
    int raw_prefix, idx, bitpos;
    unsigned gen_id;
    MPIDI_OFI_win_targetinfo_t *winfo;

    MPIR_FUNC_VERBOSE_STATE_DECL(MPID_STATE_CH4_OFI_WIN_ALLGATHER);
    MPIR_FUNC_VERBOSE_ENTER(MPID_STATE_CH4_OFI_WIN_ALLGATHER);

    /* Calculate a canonical context id */
    raw_prefix = MPIR_CONTEXT_READ_FIELD(PREFIX, comm_ptr->context_id);
    idx = raw_prefix / MPIR_CONTEXT_INT_BITS;
    bitpos = raw_prefix % MPIR_CONTEXT_INT_BITS;
    gen_id = (idx * MPIR_CONTEXT_INT_BITS) + (31 - bitpos);

    int total_bits_avail = MPIDI_Global.max_mr_key_size * 8;
    uint64_t window_instance = (uint64_t) (MPIDI_OFI_WIN(win).win_id) >> 32;
    int bits_for_instance_id = MPIDI_Global.max_windows_bits;
    int bits_for_context_id;
    uint64_t max_contexts_allowed;
    uint64_t max_instances_allowed;

    bits_for_context_id = total_bits_avail -
        MPIDI_Global.max_windows_bits - MPIDI_Global.max_huge_rma_bits;
    max_contexts_allowed = 1 << (bits_for_context_id);
    max_instances_allowed = 1 << (bits_for_instance_id);
    MPIR_ERR_CHKANDSTMT(gen_id >= max_contexts_allowed, mpi_errno, MPI_ERR_OTHER,
                        goto fn_fail, "**ofid_mr_reg");
    MPIR_ERR_CHKANDSTMT(window_instance >= max_instances_allowed, mpi_errno, MPI_ERR_OTHER,
                        goto fn_fail, "**ofid_mr_reg");

    if (MPIDI_OFI_ENABLE_MR_SCALABLE) {
        /* Context id in lower bits, instance in upper bits */
        MPIDI_OFI_WIN(win).mr_key = (gen_id << MPIDI_Global.context_shift) | window_instance;
    }
    else {
        MPIDI_OFI_WIN(win).mr_key = 0;
    }

    MPIDI_OFI_CALL(fi_mr_reg(MPIDI_Global.domain,       /* In:  Domain Object       */
                             base,      /* In:  Lower memory address */
                             win->size, /* In:  Length              */
                             FI_REMOTE_READ | FI_REMOTE_WRITE,  /* In:  Expose MR for read  */
                             0ULL,      /* In:  offset(not used)    */
                             MPIDI_OFI_WIN(win).mr_key, /* In:  requested key       */
                             0ULL,      /* In:  flags               */
                             &MPIDI_OFI_WIN(win).mr,    /* Out: memregion object    */
                             NULL), mr_reg);    /* In:  context             */

    MPIDI_OFI_WIN(win).winfo = MPL_malloc(sizeof(*winfo) * comm_ptr->local_size);

    winfo = MPIDI_OFI_WIN(win).winfo;
    winfo[comm_ptr->rank].disp_unit = disp_unit;

#ifndef USE_OFI_MR_SCALABLE
    /* MR_BASIC */
    MPIDI_OFI_WIN(win).mr_key = fi_mr_key(MPIDI_OFI_WIN(win).mr);
    winfo[comm_ptr->rank].mr_key = MPIDI_OFI_WIN(win).mr_key;
    winfo[comm_ptr->rank].base = (uintptr_t) base;
#endif

    mpi_errno = MPIR_Allgather_impl(MPI_IN_PLACE, 0,
                                    MPI_DATATYPE_NULL,
                                    winfo, sizeof(*winfo), MPI_BYTE, comm_ptr, &errflag);
    if (mpi_errno)
        MPIR_ERR_POP(mpi_errno);

    if (MPIDI_OFI_ENABLE_MR_SCALABLE) {
        first = winfo[0].disp_unit;
        same_disp = 1;
        for (i = 1; i < comm_ptr->local_size; i++) {
            if (winfo[i].disp_unit != first) {
                same_disp = 0;
                break;
            }
        }
        if (same_disp) {
            MPL_free(MPIDI_OFI_WIN(win).winfo);
            MPIDI_OFI_WIN(win).winfo = NULL;
        }
    }

  fn_exit:
    MPIR_FUNC_VERBOSE_EXIT(MPID_STATE_CH4_OFI_PROGRESS_WIN_ALLGATHER);
    return mpi_errno;
  fn_fail:
    goto fn_exit;
}

#undef FUNCNAME
#define FUNCNAME MPIDI_OFI_win_init_generic
#undef FCNAME
#define FCNAME MPL_QUOTE(FUNCNAME)
static inline int MPIDI_OFI_win_init_generic(MPI_Aint length,
                                             int disp_unit,
                                             MPIR_Win ** win_ptr,
                                             MPIR_Info * info,
                                             MPIR_Comm * comm_ptr,
                                             int create_flavor, int model, int do_stx_rma)
{
    int mpi_errno = MPI_SUCCESS;
    uint64_t window_instance;
    MPIR_Win *win;
    struct fi_info *finfo;
    struct fi_cntr_attr cntr_attr;

    CH4_COMPILE_TIME_ASSERT(sizeof(MPIDI_Devwin_t) >= sizeof(MPIDI_OFI_win_t));
    CH4_COMPILE_TIME_ASSERT(sizeof(MPIDI_Devdt_t) >= sizeof(MPIDI_OFI_datatype_t));

    /* Note: MPIDI_CH4U_win_init will interpret the info object */
    mpi_errno = MPIDI_CH4R_win_init(length, disp_unit, &win, info, comm_ptr, create_flavor, model);
    MPIR_ERR_CHKANDSTMT(mpi_errno != MPI_SUCCESS,
                        mpi_errno, MPI_ERR_NO_MEM, goto fn_fail, "**nomem");
    *win_ptr = win;

    memset(&MPIDI_OFI_WIN(win), 0, sizeof(MPIDI_OFI_win_t));

    /* context id lower bits, window instance upper bits */
    window_instance =
        MPIDI_OFI_index_allocator_alloc(MPIDI_OFI_COMM(win->comm_ptr).win_id_allocator);
    MPIDI_OFI_WIN(win).win_id = ((uint64_t) comm_ptr->context_id) | (window_instance << 32);
    MPIDI_OFI_map_set(MPIDI_Global.win_map, MPIDI_OFI_WIN(win).win_id, win);

    if (do_stx_rma && MPIDI_Global.stx_ctx != NULL) {
        /* Activate per-window EP/counter */
        int ret;

        finfo = fi_dupinfo(MPIDI_Global.prov_use);
        MPIR_Assert(finfo);
        finfo->ep_attr->tx_ctx_cnt = FI_SHARED_CONTEXT; /* Request a shared context */
        MPIDI_OFI_CALL_RETURN(fi_endpoint(MPIDI_Global.domain,
                                          finfo, &MPIDI_OFI_WIN(win).ep, NULL), ret);
        if (ret < 0) {
            MPL_DBG_MSG(MPIDI_CH4_DBG_GENERAL, VERBOSE,
                        "Failed to create per-window EP (with completion), "
                        "falling back to global EP/counter scheme");
            fi_freeinfo(finfo);
            goto fallback_global;
        }

        memset(&cntr_attr, 0, sizeof(cntr_attr));
        cntr_attr.events = FI_CNTR_EVENTS_COMP;
        MPIDI_OFI_CALL(fi_cntr_open(MPIDI_Global.domain,        /* In:  Domain Object        */
                                    &cntr_attr, /* In:  Configuration object */
                                    &MPIDI_OFI_WIN(win).cmpl_cntr,      /* Out: Counter Object       */
                                    NULL), openct);     /* Context: counter events   */
        MPIDI_OFI_WIN(win).issued_cntr = &MPIDI_OFI_WIN(win).issued_cntr_v;

        MPIDI_OFI_CALL(fi_ep_bind(MPIDI_OFI_WIN(win).ep, &MPIDI_Global.stx_ctx->fid, 0), bind);
        MPIDI_OFI_CALL(fi_ep_bind(MPIDI_OFI_WIN(win).ep,
                                  &MPIDI_Global.p2p_cq->fid, FI_TRANSMIT | FI_SELECTIVE_COMPLETION),
                       bind);
        MPIDI_OFI_CALL(fi_ep_bind
                       (MPIDI_OFI_WIN(win).ep, &MPIDI_OFI_WIN(win).cmpl_cntr->fid,
                        FI_READ | FI_WRITE), bind);
        MPIDI_OFI_CALL(fi_ep_bind(MPIDI_OFI_WIN(win).ep, &MPIDI_Global.av->fid, 0), bind);

        MPIDI_OFI_CALL_RETURN(fi_endpoint(MPIDI_Global.domain,
                                          finfo, &MPIDI_OFI_WIN(win).ep_nocmpl, NULL), ret);
        fi_freeinfo(finfo);
        if (ret < 0) {
            MPL_DBG_MSG(MPIDI_CH4_DBG_GENERAL, VERBOSE,
                        "Failed to create an EP alias, "
                        "falling back to global EP/counter scheme");
            MPIDI_OFI_CALL(fi_close(&MPIDI_OFI_WIN(win).ep->fid), epclose);
            MPIDI_OFI_CALL(fi_close(&MPIDI_OFI_WIN(win).cmpl_cntr->fid), epclose);
            goto fallback_global;
        }

        MPIDI_OFI_CALL(fi_ep_bind(MPIDI_OFI_WIN(win).ep_nocmpl,
                                  &MPIDI_Global.stx_ctx->fid, 0), bind);
        MPIDI_OFI_CALL(fi_ep_bind(MPIDI_OFI_WIN(win).ep_nocmpl,
                                  &MPIDI_OFI_WIN(win).cmpl_cntr->fid, FI_READ | FI_WRITE), bind);
        MPIDI_OFI_CALL(fi_ep_bind(MPIDI_OFI_WIN(win).ep_nocmpl, &MPIDI_Global.av->fid, 0), bind);

        MPIDI_OFI_CALL(fi_enable(MPIDI_OFI_WIN(win).ep), ep_enable);
        MPIDI_OFI_CALL(fi_enable(MPIDI_OFI_WIN(win).ep_nocmpl), ep_enable);
    }
    else {
      fallback_global:
        /* Fallback for the traditional global EP/counter model */
        MPIDI_OFI_WIN(win).ep = MPIDI_OFI_EP_TX_RMA(0);
        MPIDI_OFI_WIN(win).ep_nocmpl = MPIDI_OFI_EP_TX_CTR(0);
        MPIDI_OFI_WIN(win).cmpl_cntr = MPIDI_Global.rma_cmpl_cntr;
        MPIDI_OFI_WIN(win).issued_cntr = &MPIDI_Global.rma_issued_cntr;
    }

  fn_exit:
    return mpi_errno;
  fn_fail:
    goto fn_exit;
}

#undef FUNCNAME
#define FUNCNAME MPIDI_OFI_win_init
#undef FCNAME
#define FCNAME MPL_QUOTE(FUNCNAME)
static inline int MPIDI_OFI_win_init(MPI_Aint length,
                                     int disp_unit,
                                     MPIR_Win ** win_ptr,
                                     MPIR_Info * info,
                                     MPIR_Comm * comm_ptr, int create_flavor, int model)
{
    int mpi_errno;
    int use_stx_rma = MPIDI_OFI_ENABLE_SCALABLE_ENDPOINTS ? 0 : MPIDI_OFI_ENABLE_STX_RMA;

    MPIR_FUNC_VERBOSE_STATE_DECL(MPID_STATE_CH4_OFI_WIN_INIT);
    MPIR_FUNC_VERBOSE_ENTER(MPID_STATE_CH4_OFI_WIN_INIT);

    mpi_errno = MPIDI_OFI_win_init_generic(length,
                                           disp_unit,
                                           win_ptr,
                                           info, comm_ptr, create_flavor, model, use_stx_rma);

    MPIR_FUNC_VERBOSE_EXIT(MPID_STATE_CH4_OFI_PROGRESS_WIN_INIT);

    return mpi_errno;
}

#undef FUNCNAME
#define FUNCNAME MPIDI_Win_progress_fence
#undef FCNAME
#define FCNAME MPL_QUOTE(FUNCNAME)
static inline int MPIDI_Win_progress_fence(MPIR_Win * win)
{
    int mpi_errno = MPI_SUCCESS;
    int itercount = 0;
    int ret;
    uint64_t tcount, donecount;
    MPIDI_OFI_win_request_t *r;

    MPIR_FUNC_VERBOSE_STATE_DECL(MPID_STATE_CH4_OFI_PROGRESS_WIN_COUNTER_FENCE);
    MPIR_FUNC_VERBOSE_ENTER(MPID_STATE_CH4_OFI_PROGRESS_WIN_COUNTER_FENCE);

    MPID_THREAD_CS_ENTER(POBJ, MPIDI_OFI_THREAD_FI_MUTEX);
    tcount = *MPIDI_OFI_WIN(win).issued_cntr;
    donecount = fi_cntr_read(MPIDI_OFI_WIN(win).cmpl_cntr);

    MPIR_Assert(donecount <= tcount);

    while (tcount > donecount) {
        MPIR_Assert(donecount <= tcount);
        MPID_THREAD_CS_EXIT(POBJ, MPIDI_OFI_THREAD_FI_MUTEX);
        MPIDI_OFI_PROGRESS();
        MPID_THREAD_CS_ENTER(POBJ, MPIDI_OFI_THREAD_FI_MUTEX);
        donecount = fi_cntr_read(MPIDI_OFI_WIN(win).cmpl_cntr);
        itercount++;

        if (itercount == 1000) {
            ret = fi_cntr_wait(MPIDI_OFI_WIN(win).cmpl_cntr, tcount, 0);
            MPIDI_OFI_ERR(ret < 0 && ret != -FI_ETIMEDOUT,
                          mpi_errno,
                          MPI_ERR_RMA_RANGE,
                          "**ofid_cntr_wait",
                          "**ofid_cntr_wait %s %d %s %s",
                          __SHORT_FILE__, __LINE__, FCNAME, fi_strerror(-ret));
            itercount = 0;
        }
    }

    while (OPA_load_int(&MPIDI_CH4U_WIN(win, outstanding_ops)) != 0)
        MPIDI_OFI_PROGRESS();

    r = MPIDI_OFI_WIN(win).syncQ;

    while (r) {
        MPIDI_OFI_win_request_t *next = r->next;
        MPIDI_OFI_rma_done_event(NULL, (MPIR_Request *) r);
        r = next;
    }

    MPIDI_OFI_WIN(win).syncQ = NULL;
  fn_exit:
    MPID_THREAD_CS_EXIT(POBJ, MPIDI_OFI_THREAD_FI_MUTEX);
    MPIR_FUNC_VERBOSE_EXIT(MPID_STATE_CH4_OFI_PROGRESS_WIN_COUNTER_FENCE);
    return mpi_errno;
  fn_fail:
    goto fn_exit;
}

#undef FUNCNAME
#define FUNCNAME MPIDI_NM_mpi_win_set_info
#undef FCNAME
#define FCNAME MPL_QUOTE(FUNCNAME)
static inline int MPIDI_NM_mpi_win_set_info(MPIR_Win * win, MPIR_Info * info)
{
    int mpi_errno;
    MPIR_FUNC_VERBOSE_STATE_DECL(MPID_STATE_NETMOD_OFI_WIN_SET_INFO);
    MPIR_FUNC_VERBOSE_ENTER(MPID_STATE_NETMOD_OFI_WIN_SET_INFO);

    mpi_errno = MPIDI_CH4R_mpi_win_set_info(win, info);
    if (mpi_errno)
        MPIR_ERR_POP(mpi_errno);

  fn_exit:
    MPIR_FUNC_VERBOSE_EXIT(MPID_STATE_NETMOD_OFI_WIN_SET_INFO);
    return mpi_errno;
  fn_fail:
    goto fn_exit;
}

#undef FUNCNAME
#define FUNCNAME MPIDI_NM_mpi_win_start
#undef FCNAME
#define FCNAME MPL_QUOTE(FUNCNAME)
static inline int MPIDI_NM_mpi_win_start(MPIR_Group * group, int assert, MPIR_Win * win)
{
    int mpi_errno = MPI_SUCCESS;

    MPIR_FUNC_VERBOSE_STATE_DECL(MPID_STATE_NETMOD_OFI_WIN_START);
    MPIR_FUNC_VERBOSE_ENTER(MPID_STATE_NETMOD_OFI_WIN_START);

    MPIDI_CH4U_EPOCH_CHECK_TYPE(win, mpi_errno, goto fn_fail);

    MPIR_Group_add_ref(group);

    MPIDI_OFI_PROGRESS_WHILE(group->size != (int) MPIDI_CH4U_WIN(win, sync).pw.count);

    MPIDI_CH4U_WIN(win, sync).pw.count = 0;

    MPIR_ERR_CHKANDJUMP((MPIDI_CH4U_WIN(win, sync).sc.group != NULL),
                        mpi_errno, MPI_ERR_GROUP, "**group");
    MPIDI_CH4U_WIN(win, sync).sc.group = group;
    MPIDI_CH4U_WIN(win, sync).origin_epoch_type = MPIDI_CH4U_EPOTYPE_START;

  fn_exit:
    MPIR_FUNC_VERBOSE_EXIT(MPID_STATE_NETMOD_OFI_WIN_START);
    return mpi_errno;
  fn_fail:
    goto fn_exit;
}


#undef FUNCNAME
#define FUNCNAME MPIDI_NM_mpi_win_complete
#undef FCNAME
#define FCNAME MPL_QUOTE(FUNCNAME)
static inline int MPIDI_NM_mpi_win_complete(MPIR_Win * win)
{
    int mpi_errno = MPI_SUCCESS;

    MPIR_FUNC_VERBOSE_STATE_DECL(MPID_STATE_NETMOD_OFI_WIN_COMPLETE);
    MPIR_FUNC_VERBOSE_ENTER(MPID_STATE_NETMOD_OFI_WIN_COMPLETE);

    MPIDI_CH4U_EPOCH_START_CHECK2(win, mpi_errno, goto fn_fail);

    MPIDI_OFI_MPI_CALL_POP(MPIDI_Win_progress_fence(win));

    MPIR_Group *group;
    group = MPIDI_CH4U_WIN(win, sync).sc.group;
    MPIR_Assert(group != NULL);
    MPIDI_OFI_win_control_t msg;
    msg.type = MPIDI_OFI_CTRL_COMPLETE;

    int index, peer;

    for (index = 0; index < group->size; ++index) {
        peer = group->lrank_to_lpid[index].lpid;
        mpi_errno = MPIDI_OFI_do_control_win(&msg, peer, win, 0, 1);

        if (mpi_errno != MPI_SUCCESS)
            MPIR_ERR_SETANDSTMT(mpi_errno, MPI_ERR_RMA_SYNC, goto fn_fail, "**rmasync");
    }

    MPIDI_CH4U_EPOCH_TARGET_EVENT(win);

    MPIR_Group_release(MPIDI_CH4U_WIN(win, sync).sc.group);
    MPIDI_CH4U_WIN(win, sync).sc.group = NULL;

  fn_exit:
    MPIR_FUNC_VERBOSE_EXIT(MPID_STATE_NETMOD_OFI_WIN_COMPLETE);
    return mpi_errno;
  fn_fail:
    goto fn_exit;
}

#undef FUNCNAME
#define FUNCNAME MPIDI_NM_mpi_win_post
#undef FCNAME
#define FCNAME MPL_QUOTE(FUNCNAME)
static inline int MPIDI_NM_mpi_win_post(MPIR_Group * group, int assert, MPIR_Win * win)
{
    int peer, index, mpi_errno = MPI_SUCCESS;
    MPIDI_OFI_win_control_t msg;

    MPIR_FUNC_VERBOSE_STATE_DECL(MPID_STATE_NETMOD_OFI_WIN_POST);
    MPIR_FUNC_VERBOSE_ENTER(MPID_STATE_NETMOD_OFI_WIN_POST);

    MPIDI_CH4U_EPOCH_POST_CHECK(win, mpi_errno, goto fn_fail);

    MPIR_Group_add_ref(group);
    MPIR_ERR_CHKANDJUMP((MPIDI_CH4U_WIN(win, sync).pw.group != NULL),
                        mpi_errno, MPI_ERR_GROUP, "**group");

    MPIDI_CH4U_WIN(win, sync).pw.group = group;
    MPIR_Assert(group != NULL);

    msg.type = MPIDI_OFI_CTRL_POST;

    for (index = 0; index < group->size; ++index) {
        peer = group->lrank_to_lpid[index].lpid;
        mpi_errno = MPIDI_OFI_do_control_win(&msg, peer, win, 0, 1);

        if (mpi_errno != MPI_SUCCESS)
            MPIR_ERR_SETANDSTMT(mpi_errno, MPI_ERR_RMA_SYNC, goto fn_fail, "**rmasync");
    }

    MPIDI_CH4U_WIN(win, sync).target_epoch_type = MPIDI_CH4U_EPOTYPE_POST;

  fn_exit:
    MPIR_FUNC_VERBOSE_EXIT(MPID_STATE_NETMOD_OFI_WIN_POST);
    return mpi_errno;
  fn_fail:
    goto fn_exit;
}

#undef FUNCNAME
#define FUNCNAME MPIDI_NM_mpi_win_wait
#undef FCNAME
#define FCNAME MPL_QUOTE(FUNCNAME)
static inline int MPIDI_NM_mpi_win_wait(MPIR_Win * win)
{
    int mpi_errno = MPI_SUCCESS;

    MPIR_FUNC_VERBOSE_STATE_DECL(MPID_STATE_NETMOD_OFI_WIN_WAIT);
    MPIR_FUNC_VERBOSE_ENTER(MPID_STATE_NETMOD_OFI_WIN_WAIT);

    MPIDI_CH4U_EPOCH_TARGET_CHECK(win, MPIDI_CH4U_EPOTYPE_POST, mpi_errno, return mpi_errno);

    MPIR_Group *group;
    group = MPIDI_CH4U_WIN(win, sync).pw.group;

    MPIDI_OFI_PROGRESS_WHILE(group->size != (int) MPIDI_CH4U_WIN(win, sync).sc.count);

    MPIDI_CH4U_WIN(win, sync).sc.count = 0;
    MPIDI_CH4U_WIN(win, sync).pw.group = NULL;

    MPIR_Group_release(group);

    MPIDI_CH4U_EPOCH_ORIGIN_EVENT(win);

  fn_exit:
    MPIR_FUNC_VERBOSE_EXIT(MPID_STATE_NETMOD_OFI_WIN_WAIT);
    return mpi_errno;
  fn_fail:
    goto fn_exit;
}

#undef FUNCNAME
#define FUNCNAME MPIDI_NM_mpi_win_test
#undef FCNAME
#define FCNAME MPL_QUOTE(FUNCNAME)
static inline int MPIDI_NM_mpi_win_test(MPIR_Win * win, int *flag)
{
    int mpi_errno = MPI_SUCCESS;

    MPIR_FUNC_VERBOSE_STATE_DECL(MPID_STATE_NETMOD_OFI_WIN_TEST);
    MPIR_FUNC_VERBOSE_ENTER(MPID_STATE_NETMOD_OFI_WIN_TEST);

    MPIDI_CH4U_EPOCH_TARGET_CHECK(win, MPIDI_CH4U_EPOTYPE_POST, mpi_errno, return mpi_errno);

    MPIR_Group *group;
    group = MPIDI_CH4U_WIN(win, sync).pw.group;

    if (group->size == (int) MPIDI_CH4U_WIN(win, sync).sc.count) {
        MPIDI_CH4U_WIN(win, sync).sc.count = 0;
        MPIDI_CH4U_WIN(win, sync).pw.group = NULL;
        *flag = 1;
        MPIR_Group_release(group);
        MPIDI_CH4U_EPOCH_ORIGIN_EVENT(win);
    }
    else {
        MPIDI_OFI_PROGRESS();
        *flag = 0;
    }

  fn_exit:
    MPIR_FUNC_VERBOSE_EXIT(MPID_STATE_NETMOD_OFI_WIN_TEST);
    return mpi_errno;
  fn_fail:
    goto fn_exit;
}

#undef FUNCNAME
#define FUNCNAME MPIDI_NM_mpi_win_lock
#undef FCNAME
#define FCNAME MPL_QUOTE(FUNCNAME)
static inline int MPIDI_NM_mpi_win_lock(int lock_type, int rank, int assert, MPIR_Win * win)
{
    int mpi_errno = MPI_SUCCESS;

    MPIR_FUNC_VERBOSE_STATE_DECL(MPID_STATE_NETMOD_OFI_WIN_LOCK);
    MPIR_FUNC_VERBOSE_ENTER(MPID_STATE_NETMOD_OFI_WIN_LOCK);

    MPIDI_CH4U_win_sync_lock *slock = &MPIDI_CH4U_WIN(win, sync).lock;
    MPIDI_CH4U_EPOCH_CHECK_TYPE(win, mpi_errno, goto fn_fail);

    if (rank == MPI_PROC_NULL)
        goto fn_exit0;

    MPIDI_OFI_win_control_t msg;

    msg.type = MPIDI_OFI_CTRL_LOCKREQ;
    msg.lock_type = lock_type;

    mpi_errno = MPIDI_OFI_do_control_win(&msg, rank, win, 1, 1);

    if (mpi_errno != MPI_SUCCESS)
        MPIR_ERR_SETANDSTMT(mpi_errno, MPI_ERR_RMA_SYNC, goto fn_fail, "**rmasync");

    MPIDI_OFI_PROGRESS_WHILE(!slock->remote.locked);

  fn_exit0:
    MPIDI_CH4U_WIN(win, sync).origin_epoch_type = MPIDI_CH4U_EPOTYPE_LOCK;

  fn_exit:
    MPIR_FUNC_VERBOSE_EXIT(MPID_STATE_NETMOD_OFI_WIN_LOCK);
    return mpi_errno;
  fn_fail:
    goto fn_exit;
}

#undef FUNCNAME
#define FUNCNAME MPIDI_NM_mpi_win_unlock
#undef FCNAME
#define FCNAME MPL_QUOTE(FUNCNAME)
static inline int MPIDI_NM_mpi_win_unlock(int rank, MPIR_Win * win)
{
    int mpi_errno = MPI_SUCCESS;
    MPIR_FUNC_VERBOSE_STATE_DECL(MPID_STATE_NETMOD_OFI_WIN_UNLOCK);
    MPIR_FUNC_VERBOSE_ENTER(MPID_STATE_NETMOD_OFI_WIN_UNLOCK);

    MPIDI_CH4U_EPOCH_ORIGIN_CHECK(win, MPIDI_CH4U_EPOTYPE_LOCK, mpi_errno, return mpi_errno);

    if (rank == MPI_PROC_NULL)
        goto fn_exit0;

    MPIDI_OFI_MPI_CALL_POP(MPIDI_Win_progress_fence(win));

    MPIDI_OFI_win_control_t msg;
    msg.type = MPIDI_OFI_CTRL_UNLOCK;
    mpi_errno = MPIDI_OFI_do_control_win(&msg, rank, win, 1, 1);

    if (mpi_errno != MPI_SUCCESS)
        MPIR_ERR_SETANDSTMT(mpi_errno, MPI_ERR_RMA_SYNC, goto fn_fail, "**rmasync");

    MPIDI_OFI_PROGRESS_WHILE(MPIDI_CH4U_WIN(win, sync).lock.remote.locked);
  fn_exit0:
    MPIDI_CH4U_WIN(win, sync).origin_epoch_type = MPIDI_CH4U_EPOTYPE_NONE;
    MPIDI_CH4U_WIN(win, sync).target_epoch_type = MPIDI_CH4U_EPOTYPE_NONE;

  fn_exit:
    MPIR_FUNC_VERBOSE_EXIT(MPID_STATE_NETMOD_OFI_WIN_UNLOCK);
    return mpi_errno;
  fn_fail:
    goto fn_exit;

}

#undef FUNCNAME
#define FUNCNAME MPIDI_NM_mpi_win_get_info
#undef FCNAME
#define FCNAME MPL_QUOTE(FUNCNAME)
static inline int MPIDI_NM_mpi_win_get_info(MPIR_Win * win, MPIR_Info ** info_p_p)
{
    int mpi_errno = MPI_SUCCESS;
    MPIR_FUNC_VERBOSE_STATE_DECL(MPID_STATE_NETMOD_OFI_WIN_GET_INFO);
    MPIR_FUNC_VERBOSE_ENTER(MPID_STATE_NETMOD_OFI_WIN_GET_INFO);

    mpi_errno = MPIDI_CH4R_mpi_win_get_info(win, info_p_p);
    if (mpi_errno)
        MPIR_ERR_POP(mpi_errno);

  fn_exit:
    MPIR_FUNC_VERBOSE_EXIT(MPID_STATE_NETMOD_OFI_WIN_GET_INFO);
    return mpi_errno;
  fn_fail:
    goto fn_exit;
}


#undef FUNCNAME
#define FUNCNAME MPIDI_NM_mpi_win_free
#undef FCNAME
#define FCNAME MPL_QUOTE(FUNCNAME)
static inline int MPIDI_NM_mpi_win_free(MPIR_Win ** win_ptr)
{
    int mpi_errno = MPI_SUCCESS;
    MPIR_Errflag_t errflag = MPIR_ERR_NONE;
    MPIR_Win *win = *win_ptr;
    uint32_t window_instance;
    MPIR_FUNC_VERBOSE_STATE_DECL(MPID_STATE_NETMOD_OFI_WIN_FREE);
    MPIR_FUNC_VERBOSE_ENTER(MPID_STATE_NETMOD_OFI_WIN_FREE);

    MPIDI_CH4U_EPOCH_FREE_CHECK(win, mpi_errno, return mpi_errno);

    mpi_errno = MPIR_Barrier_impl(win->comm_ptr, &errflag);

    if (mpi_errno != MPI_SUCCESS)
        goto fn_fail;

    window_instance = (uint32_t) (MPIDI_OFI_WIN(win).win_id >> 32);

    MPIDI_OFI_index_allocator_free(MPIDI_OFI_COMM(win->comm_ptr).win_id_allocator, window_instance);
    MPIDI_OFI_map_erase(MPIDI_Global.win_map, MPIDI_OFI_WIN(win).win_id);
    if (MPIDI_OFI_WIN(win).ep_nocmpl != MPIDI_OFI_EP_TX_CTR(0))
        MPIDI_OFI_CALL(fi_close(&MPIDI_OFI_WIN(win).ep_nocmpl->fid), epclose);
    if (MPIDI_OFI_WIN(win).ep != MPIDI_OFI_EP_TX_RMA(0))
        MPIDI_OFI_CALL(fi_close(&MPIDI_OFI_WIN(win).ep->fid), epclose);
    if (MPIDI_OFI_WIN(win).cmpl_cntr != MPIDI_Global.rma_cmpl_cntr)
        MPIDI_OFI_CALL(fi_close(&MPIDI_OFI_WIN(win).cmpl_cntr->fid), cqclose);
    MPIDI_OFI_CALL(fi_close(&MPIDI_OFI_WIN(win).mr->fid), mr_unreg);
    if (MPIDI_OFI_WIN(win).winfo) {
        MPL_free(MPIDI_OFI_WIN(win).winfo);
        MPIDI_OFI_WIN(win).winfo = NULL;
    }

    MPIDI_CH4R_win_finalize(win_ptr);

  fn_exit:
    MPIR_FUNC_VERBOSE_EXIT(MPID_STATE_NETMOD_OFI_WIN_FREE);
    return mpi_errno;
  fn_fail:
    goto fn_exit;
}

#undef FUNCNAME
#define FUNCNAME MPIDI_NM_mpi_win_fence
#undef FCNAME
#define FCNAME MPL_QUOTE(FUNCNAME)
static inline int MPIDI_NM_mpi_win_fence(int massert, MPIR_Win * win)
{
    int mpi_errno = MPI_SUCCESS;
    MPIR_Errflag_t errflag = MPIR_ERR_NONE;
    MPIR_FUNC_VERBOSE_STATE_DECL(MPID_STATE_NETMOD_OFI_WIN_FENCE);
    MPIR_FUNC_VERBOSE_ENTER(MPID_STATE_NETMOD_OFI_WIN_FENCE);

    MPIDI_CH4U_EPOCH_FENCE_CHECK(win, mpi_errno, goto fn_fail);

    MPIDI_OFI_MPI_CALL_POP(MPIDI_Win_progress_fence(win));

    MPIDI_CH4U_EPOCH_FENCE_EVENT(win, massert);

    /*
     * We always make a barrier even if MPI_MODE_NOPRECEDE is specified.
     * This is necessary because we no longer defer executions of RMA ops
     * until synchronization calls as CH3 did. Otherwise, the code like
     * this won't work correctly (cf. f77/rma/wingetf)
     *
     * Rank 0                          Rank 1
     * ----                            ----
     * Store to local mem in window
     * MPI_Win_fence(MODE_NOPRECEDE)   MPI_Win_fence(MODE_NOPRECEDE)
     * MPI_Get(from rank 1)
     */
    mpi_errno = MPIR_Barrier_impl(win->comm_ptr, &errflag);

  fn_exit:
    MPIR_FUNC_VERBOSE_EXIT(MPID_STATE_NETMOD_OFI_WIN_FENCE);
    return mpi_errno;
  fn_fail:
    goto fn_exit;
}

#undef FUNCNAME
#define FUNCNAME MPIDI_NM_mpi_win_create
#undef FCNAME
#define FCNAME MPL_QUOTE(FUNCNAME)
static inline int MPIDI_NM_mpi_win_create(void *base,
                                          MPI_Aint length,
                                          int disp_unit,
                                          MPIR_Info * info, MPIR_Comm * comm_ptr,
                                          MPIR_Win ** win_ptr)
{
    int mpi_errno = MPI_SUCCESS;
    MPIR_Errflag_t errflag = MPIR_ERR_NONE;
    MPIR_Win *win;

    MPIR_FUNC_VERBOSE_STATE_DECL(MPID_STATE_NETMOD_OFI_WIN_CREATE);
    MPIR_FUNC_VERBOSE_ENTER(MPID_STATE_NETMOD_OFI_WIN_CREATE);

    mpi_errno = MPIDI_OFI_win_init(length,
                                   disp_unit,
                                   win_ptr, info, comm_ptr, MPI_WIN_FLAVOR_CREATE, MPI_WIN_UNIFIED);

    if (mpi_errno != MPI_SUCCESS)
        goto fn_fail;

    win = *win_ptr;
    win->base = base;

    mpi_errno = MPIDI_OFI_win_allgather(win, base, disp_unit);

    if (mpi_errno != MPI_SUCCESS)
        goto fn_fail;

    mpi_errno = MPIR_Barrier_impl(comm_ptr, &errflag);

    if (mpi_errno != MPI_SUCCESS)
        goto fn_fail;

  fn_exit:
    MPIR_FUNC_VERBOSE_EXIT(MPID_STATE_NETMOD_OFI_WIN_CREATE);
    return mpi_errno;
  fn_fail:
    goto fn_exit;
}

#undef FUNCNAME
#define FUNCNAME MPIDI_NM_mpi_win_attach
#undef FCNAME
#define FCNAME MPL_QUOTE(FUNCNAME)
static inline int MPIDI_NM_mpi_win_attach(MPIR_Win * win, void *base, MPI_Aint size)
{
    int mpi_errno = MPI_SUCCESS;
    MPIR_FUNC_VERBOSE_STATE_DECL(MPID_STATE_NETMOD_OFI_WIN_ATTACH);
    MPIR_FUNC_VERBOSE_ENTER(MPID_STATE_NETMOD_OFI_WIN_ATTACH);

    MPIR_ERR_CHKANDSTMT((win->create_flavor != MPI_WIN_FLAVOR_DYNAMIC), mpi_errno,
                        MPI_ERR_RMA_FLAVOR, goto fn_fail, "**rmaflavor");
  fn_exit:
    MPIR_FUNC_VERBOSE_EXIT(MPID_STATE_NETMOD_OFI_WIN_ATTACH);
    return mpi_errno;
  fn_fail:
    goto fn_exit;
}

#undef FUNCNAME
#define FUNCNAME MPIDI_NM_mpi_win_allocate_shared
#undef FCNAME
#define FCNAME MPL_QUOTE(FUNCNAME)
static inline int MPIDI_NM_mpi_win_allocate_shared(MPI_Aint size,
                                                   int disp_unit,
                                                   MPIR_Info * info_ptr,
                                                   MPIR_Comm * comm_ptr,
                                                   void **base_ptr, MPIR_Win ** win_ptr)
{
    int i = 0, fd = -1, rc, first = 0, mpi_errno = MPI_SUCCESS;
    MPIR_Errflag_t errflag = MPIR_ERR_NONE;
    void *baseP = NULL;
    MPIR_Win *win = NULL;
    ssize_t total_size = 0LL;
    MPI_Aint size_out = 0;
    char shm_key[64];
    void *map_ptr;
    MPIDI_CH4U_win_shared_info_t *shared_table = NULL;

    MPIR_FUNC_VERBOSE_STATE_DECL(MPID_STATE_NETMOD_OFI_WIN_ALLOCATE_SHARED);
    MPIR_FUNC_VERBOSE_ENTER(MPID_STATE_NETMOD_OFI_WIN_ALLOCATE_SHARED);

    mpi_errno = MPIDI_OFI_win_init(size, disp_unit, win_ptr, info_ptr, comm_ptr,
                                   MPI_WIN_FLAVOR_SHARED, MPI_WIN_UNIFIED);
    if (mpi_errno != MPI_SUCCESS)
        MPIR_ERR_POP(mpi_errno);
    win = *win_ptr;
    MPIDI_CH4U_WIN(win, shared_table) =
        (MPIDI_CH4U_win_shared_info_t *) MPL_malloc(sizeof(MPIDI_CH4U_win_shared_info_t) *
                                                    comm_ptr->local_size);
    shared_table = MPIDI_CH4U_WIN(win, shared_table);

    shared_table[comm_ptr->rank].size = size;
    shared_table[comm_ptr->rank].disp_unit = disp_unit;

    mpi_errno = MPIR_Allgather_impl(MPI_IN_PLACE,
                                    0,
                                    MPI_DATATYPE_NULL,
                                    shared_table,
                                    sizeof(MPIDI_CH4U_win_shared_info_t),
                                    MPI_BYTE, comm_ptr, &errflag);

    if (mpi_errno != MPI_SUCCESS)
        goto fn_fail;

    /* No allreduce here because this is a shared memory domain
     * and should be a relatively small number of processes
     * and a non performance sensitive API.
     */
    for (i = 0; i < comm_ptr->local_size; i++)
        total_size += shared_table[i].size;

    if (total_size == 0)
        goto fn_zero;

    sprintf(shm_key, "/mpi-%X-%" PRIx64, MPIDI_Global.jobid, MPIDI_OFI_WIN(win).win_id);

    rc = shm_open(shm_key, O_CREAT | O_EXCL | O_RDWR, 0600);
    first = (rc != -1);

    if (!first) {
        rc = shm_open(shm_key, O_RDWR, 0);

        if (rc == -1) {
            shm_unlink(shm_key);
            MPIR_ERR_SETANDSTMT(mpi_errno, MPI_ERR_NO_MEM, goto fn_fail, "**nomem");
        }
    }

    /* Make the addresses symmetric by using MAP_FIXED */
    size_t page_sz, mapsize;

    mapsize = MPIDI_CH4R_get_mapsize(total_size, &page_sz);
    fd = rc;
    rc = ftruncate(fd, mapsize);

    if (rc == -1) {
        close(fd);

        if (first)
            shm_unlink(shm_key);

        MPIR_ERR_SETANDSTMT(mpi_errno, MPI_ERR_NO_MEM, goto fn_fail, "**nomem");
    }

    if (comm_ptr->rank == 0) {
        map_ptr = MPIDI_CH4R_generate_random_addr(mapsize);
        map_ptr = mmap(map_ptr, mapsize, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, 0);

        if (map_ptr == NULL || map_ptr == MAP_FAILED) {
            close(fd);

            if (first)
                shm_unlink(shm_key);

            MPIR_ERR_SETANDSTMT(mpi_errno, MPI_ERR_NO_MEM, goto fn_fail, "**nomem");
        }

        mpi_errno = MPIR_Bcast_impl(&map_ptr, 1, MPI_UNSIGNED_LONG, 0, comm_ptr, &errflag);

        if (mpi_errno != MPI_SUCCESS)
            goto fn_fail;

        MPIDI_CH4U_WIN(win, mmap_addr) = map_ptr;
        MPIDI_CH4U_WIN(win, mmap_sz) = mapsize;
    }
    else {
        mpi_errno = MPIR_Bcast_impl(&map_ptr, 1, MPI_UNSIGNED_LONG, 0, comm_ptr, &errflag);

        if (mpi_errno != MPI_SUCCESS)
            goto fn_fail;

        rc = MPIDI_CH4R_check_maprange_ok(map_ptr, mapsize);
        /* If we hit this assert, we need to iterate
         * trying more addresses
         */
        MPIR_Assert(rc == 1);
        map_ptr = mmap(map_ptr, mapsize, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, 0);
        MPIDI_CH4U_WIN(win, mmap_addr) = map_ptr;
        MPIDI_CH4U_WIN(win, mmap_sz) = mapsize;

        if (map_ptr == NULL || map_ptr == MAP_FAILED) {
            close(fd);

            if (first)
                shm_unlink(shm_key);

            MPIR_ERR_SETANDSTMT(mpi_errno, MPI_ERR_NO_MEM, goto fn_fail, "**nomem");
        }
    }

    /* Scan for my offset into the buffer             */
    /* Could use exscan if this is expensive at scale */
    for (i = 0; i < comm_ptr->rank; i++)
        size_out += shared_table[i].size;

  fn_zero:

    baseP = (size == 0) ? NULL : (void *) ((char *) map_ptr + size_out);
    win->base = baseP;
    win->size = size;
    mpi_errno = MPIDI_OFI_win_allgather(win, baseP, disp_unit);

    if (mpi_errno != MPI_SUCCESS)
        return mpi_errno;

    *(void **) base_ptr = (void *) win->base;
    mpi_errno = MPIR_Barrier_impl(comm_ptr, &errflag);

    close(fd);

    if (first)
        shm_unlink(shm_key);

  fn_exit:
    MPIR_FUNC_VERBOSE_EXIT(MPID_STATE_NETMOD_OFI_WIN_ALLOCATE_SHARED);
    return mpi_errno;
  fn_fail:
    goto fn_exit;
}

#undef FUNCNAME
#define FUNCNAME MPIDI_NM_mpi_win_detach
#undef FCNAME
#define FCNAME MPL_QUOTE(FUNCNAME)
static inline int MPIDI_NM_mpi_win_detach(MPIR_Win * win, const void *base)
{
    int mpi_errno = MPI_SUCCESS;
    MPIR_FUNC_VERBOSE_STATE_DECL(MPID_STATE_NETMOD_OFI_WIN_DETACH);
    MPIR_FUNC_VERBOSE_ENTER(MPID_STATE_NETMOD_OFI_WIN_DETACH);
    MPIR_ERR_CHKANDSTMT((win->create_flavor != MPI_WIN_FLAVOR_DYNAMIC), mpi_errno,
                        MPI_ERR_RMA_FLAVOR, goto fn_fail, "**rmaflavor");
  fn_exit:
    MPIR_FUNC_VERBOSE_EXIT(MPID_STATE_NETMOD_OFI_WIN_DETACH);
    return mpi_errno;
  fn_fail:
    goto fn_exit;
}

#undef FUNCNAME
#define FUNCNAME MPIDI_NM_mpi_win_shared_query
#undef FCNAME
#define FCNAME MPL_QUOTE(FUNCNAME)
static inline int MPIDI_NM_mpi_win_shared_query(MPIR_Win * win,
                                                int rank,
                                                MPI_Aint * size, int *disp_unit, void *baseptr)
{
    int mpi_errno = MPI_SUCCESS;

    MPIR_FUNC_VERBOSE_STATE_DECL(MPID_STATE_NETMOD_OFI_WIN_SHARED_QUERY);
    MPIR_FUNC_VERBOSE_ENTER(MPID_STATE_NETMOD_OFI_WIN_SHARED_QUERY);
    int offset = rank;
    int i;
    uintptr_t base = (uintptr_t) MPIDI_CH4U_WIN(win, mmap_addr);

    MPIDI_CH4U_win_shared_info_t *shared_table = MPIDI_CH4U_WIN(win, shared_table);

    if (rank < 0)
        offset = 0;

    *size = shared_table[offset].size;
    *disp_unit = shared_table[offset].disp_unit;
    if (*size > 0) {
        for (i = 0; i < offset; i++)
            base += shared_table[i].size;
        *(void **) baseptr = (void *) base;
    }
    else
        *(void **) baseptr = NULL;

    MPIR_FUNC_VERBOSE_EXIT(MPID_STATE_NETMOD_OFI_WIN_SHARED_QUERY);
    return mpi_errno;
}


#undef FUNCNAME
#define FUNCNAME MPIDI_NM_mpi_win_allocate
#undef FCNAME
#define FCNAME MPL_QUOTE(FUNCNAME)
static inline int MPIDI_NM_mpi_win_allocate(MPI_Aint size,
                                            int disp_unit,
                                            MPIR_Info * info,
                                            MPIR_Comm * comm, void *baseptr, MPIR_Win ** win_ptr)
{
    int mpi_errno = MPI_SUCCESS;
    MPIR_Errflag_t errflag = MPIR_ERR_NONE;
    void *baseP;
    MPIR_Win *win;

    MPIR_FUNC_VERBOSE_STATE_DECL(MPID_STATE_NETMOD_OFI_WIN_ALLOCATE);
    MPIR_FUNC_VERBOSE_ENTER(MPID_STATE_NETMOD_OFI_WIN_ALLOCATE);

    mpi_errno = MPIDI_OFI_win_init(size, disp_unit, win_ptr, info, comm,
                                   MPI_WIN_FLAVOR_ALLOCATE, MPI_WIN_UNIFIED);

    if (mpi_errno != MPI_SUCCESS)
        goto fn_fail;

    mpi_errno = MPIDI_CH4R_get_symmetric_heap(size, comm, &baseP, *win_ptr);

    if (mpi_errno != MPI_SUCCESS)
        goto fn_fail;

    win = *win_ptr;
    win->base = baseP;
    mpi_errno = MPIDI_OFI_win_allgather(win, baseP, disp_unit);

    if (mpi_errno != MPI_SUCCESS)
        goto fn_fail;

    *(void **) baseptr = (void *) win->base;
    mpi_errno = MPIR_Barrier_impl(comm, &errflag);

    if (mpi_errno != MPI_SUCCESS)
        goto fn_fail;

  fn_exit:
    MPIR_FUNC_VERBOSE_EXIT(MPID_STATE_NETMOD_OFI_WIN_ALLOCATE);
    return mpi_errno;
  fn_fail:
    goto fn_exit;
}

#undef FUNCNAME
#define FUNCNAME MPIDI_NM_mpi_win_flush
#undef FCNAME
#define FCNAME MPL_QUOTE(FUNCNAME)
static inline int MPIDI_NM_mpi_win_flush(int rank, MPIR_Win * win)
{
    int mpi_errno = MPI_SUCCESS;
    MPIR_FUNC_VERBOSE_STATE_DECL(MPID_STATE_NETMOD_OFI_WIN_FLUSH);
    MPIR_FUNC_VERBOSE_ENTER(MPID_STATE_NETMOD_OFI_WIN_FLUSH);

    MPIDI_CH4U_EPOCH_LOCK_CHECK(win, mpi_errno, goto fn_fail);

    MPIDI_OFI_MPI_CALL_POP(MPIDI_Win_progress_fence(win));

  fn_exit:
    MPIR_FUNC_VERBOSE_EXIT(MPID_STATE_NETMOD_OFI_WIN_FLUSH);
    return mpi_errno;
  fn_fail:
    goto fn_exit;
}

#undef FUNCNAME
#define FUNCNAME MPIDI_NM_mpi_win_flush_local_all
#undef FCNAME
#define FCNAME MPL_QUOTE(FUNCNAME)
static inline int MPIDI_NM_mpi_win_flush_local_all(MPIR_Win * win)
{
    int mpi_errno = MPI_SUCCESS;
    MPIR_FUNC_VERBOSE_STATE_DECL(MPID_STATE_NETMOD_OFI_WIN_FLUSH_LOCAL_ALL);
    MPIR_FUNC_VERBOSE_ENTER(MPID_STATE_NETMOD_OFI_WIN_FLUSH_LOCAL_ALL);

    MPIDI_CH4U_EPOCH_LOCK_CHECK(win, mpi_errno, goto fn_fail);
    MPIDI_OFI_MPI_CALL_POP(MPIDI_Win_progress_fence(win));

  fn_exit:
    MPIR_FUNC_VERBOSE_EXIT(MPID_STATE_NETMOD_OFI_WIN_FLUSH_LOCAL_ALL);
    return mpi_errno;
  fn_fail:
    goto fn_exit;
}

#undef FUNCNAME
#define FUNCNAME MPIDI_NM_mpi_win_unlock_all
#undef FCNAME
#define FCNAME MPL_QUOTE(FUNCNAME)
static inline int MPIDI_NM_mpi_win_unlock_all(MPIR_Win * win)
{
    int mpi_errno = MPI_SUCCESS;
    MPIR_FUNC_VERBOSE_STATE_DECL(MPID_STATE_NETMOD_OFI_WIN_UNLOCK_ALL);
    MPIR_FUNC_VERBOSE_ENTER(MPID_STATE_NETMOD_OFI_WIN_UNLOCK_ALL);
    int i;
    MPIDI_CH4U_win_lock_info *lockQ;

    MPIDI_CH4U_EPOCH_ORIGIN_CHECK(win, MPIDI_CH4U_EPOTYPE_LOCK_ALL, mpi_errno, return mpi_errno);

    MPIDI_OFI_MPI_CALL_POP(MPIDI_Win_progress_fence(win));

    MPIR_Assert(MPIDI_CH4U_WIN(win, lockQ) != NULL);

    lockQ = (MPIDI_CH4U_win_lock_info *) MPIDI_CH4U_WIN(win, lockQ);

    for (i = 0; i < win->comm_ptr->local_size; i++) {
        MPIDI_OFI_win_control_t msg;
        lockQ[i].done = 0;
        lockQ[i].peer = i;
        lockQ[i].win = win;
        msg.type = MPIDI_OFI_CTRL_UNLOCKALL;
        mpi_errno = MPIDI_OFI_do_control_win(&msg, lockQ[i].peer, win, 1, 1);

        if (mpi_errno != MPI_SUCCESS)
            MPIR_ERR_SETANDSTMT(mpi_errno, MPI_ERR_RMA_SYNC, goto fn_fail, "**rmasync");

        if (MPIDI_CH4U_WIN(win, sync).lock.remote.allLocked == 1)
            lockQ[i].done = 1;
    }

    MPIDI_OFI_PROGRESS_WHILE(MPIDI_CH4U_WIN(win, sync).lock.remote.allLocked);

    MPIDI_CH4U_WIN(win, sync).origin_epoch_type = MPIDI_CH4U_EPOTYPE_NONE;
    MPIDI_CH4U_WIN(win, sync).target_epoch_type = MPIDI_CH4U_EPOTYPE_NONE;

  fn_exit:
    MPIR_FUNC_VERBOSE_EXIT(MPID_STATE_NETMOD_OFI_WIN_UNLOCK_ALL);
    return mpi_errno;
  fn_fail:
    goto fn_exit;
}

#undef FUNCNAME
#define FUNCNAME MPIDI_NM_mpi_win_create_dynamic
#undef FCNAME
#define FCNAME MPL_QUOTE(FUNCNAME)
static inline int MPIDI_NM_mpi_win_create_dynamic(MPIR_Info * info,
                                                  MPIR_Comm * comm, MPIR_Win ** win_ptr)
{
    int mpi_errno = MPI_SUCCESS;
    int rc = MPI_SUCCESS;
    MPIR_Errflag_t errflag = MPIR_ERR_NONE;
    MPIR_Win *win;
    MPIR_FUNC_VERBOSE_STATE_DECL(MPID_STATE_NETMOD_OFI_WIN_CREATE_DYNAMIC);
    MPIR_FUNC_VERBOSE_ENTER(MPID_STATE_NETMOD_OFI_WIN_CREATE_DYNAMIC);

    rc = MPIDI_OFI_win_init((uintptr_t) UINTPTR_MAX - (uintptr_t) MPI_BOTTOM,
                            1, win_ptr, info, comm, MPI_WIN_FLAVOR_DYNAMIC, MPI_WIN_UNIFIED);

    if (rc != MPI_SUCCESS)
        goto fn_fail;

    win = *win_ptr;
    win->base = MPI_BOTTOM;

    rc = MPIDI_OFI_win_allgather(win, win->base, 1);

    if (rc != MPI_SUCCESS)
        goto fn_fail;

    mpi_errno = MPIR_Barrier_impl(comm, &errflag);

  fn_exit:
    MPIR_FUNC_VERBOSE_EXIT(MPID_STATE_NETMOD_OFI_WIN_CREATE_DYNAMIC);
    return mpi_errno;
  fn_fail:
    goto fn_exit;
}

#undef FUNCNAME
#define FUNCNAME MPIDI_NM_mpi_win_flush_local
#undef FCNAME
#define FCNAME MPL_QUOTE(FUNCNAME)
static inline int MPIDI_NM_mpi_win_flush_local(int rank, MPIR_Win * win)
{
    int mpi_errno = MPI_SUCCESS;
    MPIR_FUNC_VERBOSE_STATE_DECL(MPID_STATE_NETMOD_OFI_WIN_FLUSH_LOCAL);
    MPIR_FUNC_VERBOSE_ENTER(MPID_STATE_NETMOD_OFI_WIN_FLUSH_LOCAL);

    MPIDI_CH4U_EPOCH_LOCK_CHECK(win, mpi_errno, goto fn_fail);

    MPIDI_OFI_MPI_CALL_POP(MPIDI_Win_progress_fence(win));

  fn_exit:
    MPIR_FUNC_VERBOSE_EXIT(MPID_STATE_NETMOD_OFI_WIN_FLUSH_LOCAL);
    return mpi_errno;
  fn_fail:
    goto fn_exit;
}


#undef FUNCNAME
#define FUNCNAME MPIDI_NM_mpi_win_sync
#undef FCNAME
#define FCNAME MPL_QUOTE(FUNCNAME)
static inline int MPIDI_NM_mpi_win_sync(MPIR_Win * win)
{
    int mpi_errno = MPI_SUCCESS;
    MPIR_FUNC_VERBOSE_STATE_DECL(MPID_STATE_NETMOD_OFI_WIN_SYNC);
    MPIR_FUNC_VERBOSE_ENTER(MPID_STATE_NETMOD_OFI_WIN_SYNC);

    MPIDI_CH4U_EPOCH_LOCK_CHECK(win, mpi_errno, goto fn_fail);

    OPA_read_write_barrier();

  fn_exit:
    MPIR_FUNC_VERBOSE_EXIT(MPID_STATE_NETMOD_OFI_WIN_SYNC);
    return mpi_errno;
  fn_fail:
    goto fn_exit;
}

#undef FUNCNAME
#define FUNCNAME MPIDI_NM_mpi_win_flush_all
#undef FCNAME
#define FCNAME MPL_QUOTE(FUNCNAME)
static inline int MPIDI_NM_mpi_win_flush_all(MPIR_Win * win)
{
    int mpi_errno = MPI_SUCCESS;
    MPIR_FUNC_VERBOSE_STATE_DECL(MPID_STATE_NETMOD_OFI_WIN_FLUSH_ALL);
    MPIR_FUNC_VERBOSE_ENTER(MPID_STATE_NETMOD_OFI_WIN_FLUSH_ALL);

    MPIDI_CH4U_EPOCH_LOCK_CHECK(win, mpi_errno, goto fn_fail);
    MPIDI_OFI_MPI_CALL_POP(MPIDI_Win_progress_fence(win));

  fn_exit:
    MPIR_FUNC_VERBOSE_EXIT(MPID_STATE_NETMOD_OFI_WIN_FLUSH_ALL);
    return mpi_errno;
  fn_fail:
    goto fn_exit;
}

#undef FUNCNAME
#define FUNCNAME MPIDI_NM_mpi_win_lock_all
#undef FCNAME
#define FCNAME MPL_QUOTE(FUNCNAME)
static inline int MPIDI_NM_mpi_win_lock_all(int assert, MPIR_Win * win)
{
    int mpi_errno = MPI_SUCCESS;
    MPIDI_CH4U_win_lock_info *lockQ;

    MPIR_FUNC_VERBOSE_STATE_DECL(MPID_STATE_NETMOD_OFI_WIN_LOCK_ALL);
    MPIR_FUNC_VERBOSE_ENTER(MPID_STATE_NETMOD_OFI_WIN_LOCK_ALL);

    MPIDI_CH4U_EPOCH_CHECK_TYPE(win, mpi_errno, goto fn_fail);

    int size;
    size = win->comm_ptr->local_size;

    if (!MPIDI_CH4U_WIN(win, lockQ)) {
        MPIDI_CH4U_WIN(win, lockQ) =
            (MPIDI_CH4U_win_lock_info *) MPL_calloc(size, sizeof(MPIDI_CH4U_win_lock_info));
        MPIR_Assert(MPIDI_CH4U_WIN(win, lockQ) != NULL);
    }

    lockQ = MPIDI_CH4U_WIN(win, lockQ);
    int i;

    for (i = 0; i < size; i++) {
        MPIDI_OFI_win_control_t msg;

        lockQ[i].done = 0;
        lockQ[i].peer = i;
        lockQ[i].win = win;
        lockQ[i].lock_type = MPI_LOCK_SHARED;

        msg.type = MPIDI_OFI_CTRL_LOCKALLREQ;
        msg.lock_type = MPI_LOCK_SHARED;
        mpi_errno = MPIDI_OFI_do_control_win(&msg, lockQ[i].peer, lockQ[i].win, 1, 1);

        if (mpi_errno != MPI_SUCCESS)
            MPIR_ERR_SETANDSTMT(mpi_errno, MPI_ERR_RMA_SYNC, goto fn_fail, "**rmasync");

        if (MPIDI_CH4U_WIN(win, sync).lock.remote.allLocked == 1)
            lockQ[i].done = 1;
    }

    MPIDI_OFI_PROGRESS_WHILE(size != (int) MPIDI_CH4U_WIN(win, sync).lock.remote.allLocked);

    MPIDI_CH4U_WIN(win, sync).origin_epoch_type = MPIDI_CH4U_EPOTYPE_LOCK_ALL;

  fn_exit:
    MPIR_FUNC_VERBOSE_EXIT(MPID_STATE_NETMOD_OFI_WIN_LOCK_ALL);
    return mpi_errno;
  fn_fail:
    goto fn_exit;
}


#endif /* NETMOD_OFI_WIN_H_INCLUDED */