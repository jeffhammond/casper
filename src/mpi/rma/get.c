#include <stdio.h>
#include <stdlib.h>
#include "csp.h"

static int CSP_Get_shared_impl(void *origin_addr, int origin_count,
                                  MPI_Datatype origin_datatype, int target_rank,
                                  MPI_Aint target_disp, int target_count,
                                  MPI_Datatype target_datatype, MPI_Win win, CSP_Win * ug_win)
{
    int mpi_errno = MPI_SUCCESS;
    MPI_Win *win_ptr = &ug_win->my_ug_win;

    CSP_Get_epoch_local_win(ug_win, win_ptr);

    /* Issue operation to the target through local window, because shared
     * communication is fully handled by local process.
     */
    mpi_errno = PMPI_Get(origin_addr, origin_count, origin_datatype,
                         ug_win->my_rank_in_ug_comm, target_disp,
                         target_count, target_datatype, *win_ptr);
    CSP_DBG_PRINT("CASPER Get from self(%d, in local win 0x%x)\n",
                     ug_win->my_rank_in_ug_comm, *win_ptr);

    goto fn_exit;

  fn_exit:
    return mpi_errno;

  fn_fail:
    goto fn_exit;
}

static int CSP_Get_segment_impl(const void *origin_addr, int origin_count,
                                   MPI_Datatype origin_datatype,
                                   int target_rank, MPI_Aint target_disp,
                                   int target_count, MPI_Datatype target_datatype,
                                   MPI_Win win, CSP_Win * ug_win)
{
    int mpi_errno = MPI_SUCCESS;
    int num_segs = 0, i;
    CSP_OP_Segment *decoded_ops = NULL;

    /* TODO : Eliminate operation division for some special cases, see pptx */
    mpi_errno = CSP_Op_segments_decode(origin_addr, origin_count,
                                          origin_datatype, target_rank, target_disp, target_count,
                                          target_datatype, ug_win, &decoded_ops, &num_segs);
    if (mpi_errno != MPI_SUCCESS)
        goto fn_fail;

    CSP_DBG_PRINT("CASPER Get from target %d, num_segs=%d\n", target_rank, num_segs);

    for (i = 0; i < num_segs; i++) {
        int target_g_rank_in_ug = -1;
        int data_size = 0;
        MPI_Aint target_g_offset = 0;
        MPI_Aint ug_target_disp = 0;
        int seg_off = decoded_ops[i].target_seg_off;
        MPI_Win seg_ug_win = ug_win->targets[target_rank].segs[seg_off].ug_win;

        mpi_errno = CSP_Get_gp_rank(target_rank, seg_off, 0, decoded_ops[i].target_dtsize,
                                           ug_win, &target_g_rank_in_ug, &target_g_offset);
        if (mpi_errno != MPI_SUCCESS)
            goto fn_fail;

        ug_target_disp = target_g_offset
            + ug_win->targets[target_rank].disp_unit * decoded_ops[i].target_disp;

        /* Issue operation to the ghost process in corresponding ug-window of target process. */
        mpi_errno = PMPI_Get(decoded_ops[i].origin_addr, decoded_ops[i].origin_count,
                             decoded_ops[i].origin_datatype, target_g_rank_in_ug, ug_target_disp,
                             decoded_ops[i].target_count, decoded_ops[i].target_datatype,
                             seg_ug_win);
        if (mpi_errno != MPI_SUCCESS)
            goto fn_fail;

        CSP_DBG_PRINT("CASPER Get from (ghost %d, win 0x%x) instead of "
                         "target %d, seg %d \n"
                         "(origin.addr %p, count %d, datatype 0x%x, "
                         "target.disp 0x%lx(0x%lx + %d * %ld), count %d, datatype 0x%x)\n",
                         target_g_rank_in_ug, seg_ug_win, target_rank, seg_off,
                         decoded_ops[i].origin_addr, decoded_ops[i].origin_count,
                         decoded_ops[i].origin_datatype, ug_target_disp, target_g_offset,
                         ug_win->targets[target_rank].disp_unit, decoded_ops[i].target_disp,
                         decoded_ops[i].target_count, decoded_ops[i].target_datatype);
    }

  fn_exit:
    CSP_Op_segments_destroy(&decoded_ops);
    return mpi_errno;

  fn_fail:
    goto fn_exit;
}

static int CSP_Get_impl(void *origin_addr, int origin_count,
                           MPI_Datatype origin_datatype,
                           int target_rank, MPI_Aint target_disp,
                           int target_count,
                           MPI_Datatype target_datatype, MPI_Win win, CSP_Win * ug_win)
{
    int mpi_errno = MPI_SUCCESS;
    MPI_Aint ug_target_disp = 0;
    int target_node_id = -1;
    int rank;

    PMPI_Comm_rank(ug_win->user_comm, &rank);
#ifdef CSP_ENABLE_LOCAL_LOCK_OPT
    if (target_rank == rank && ug_win->is_self_locked) {
        /* If target is itself, we do not need translate it to any Ghosts because
         * win_lock(self) will force lock(ghost) to be granted so that it is safe
         * to send operations to the real target.
         */
        mpi_errno = CSP_Get_shared_impl(origin_addr, origin_count,
                                           origin_datatype, target_rank, target_disp, target_count,
                                           target_datatype, win, ug_win);
        if (mpi_errno != MPI_SUCCESS)
            return mpi_errno;
    }
    else
#endif
    {
        /* TODO: Do we need segment load balancing in fence ?
         * 1. No lock issue.
         * 2. overhead of data range checking and division */
        if (CSP_ENV.lock_binding == CSP_LOCK_BINDING_SEGMENT &&
            ug_win->targets[target_rank].num_segs > 1 &&
            ug_win->epoch_stat == CSP_WIN_EPOCH_LOCK) {
            mpi_errno = CSP_Get_segment_impl(origin_addr, origin_count,
                                                origin_datatype, target_rank, target_disp,
                                                target_count, target_datatype, win, ug_win);
            if (mpi_errno != MPI_SUCCESS)
                return mpi_errno;
        }
        else {
            /* Translation for intra/inter-node operations.
             *
             * We do not use force flush + shared window for optimizing operations to local targets.
             * Because: 1) we lose lock optimization on force flush; 2) Although most implementation
             * does shared-communication for operations on shared windows, MPI standard doesn’t
             * require it. Some implementation may use network even for shared targets for
             * shorter CPU occupancy.
             */
            int target_g_rank_in_ug = -1;
            int data_size = 0;
            MPI_Aint target_g_offset = 0;
            MPI_Win *win_ptr = NULL;

            CSP_Get_epoch_win(target_rank, 0, ug_win, win_ptr);

#if defined(CSP_ENABLE_RUNTIME_LOAD_OPT)
            if (CSP_ENV.load_opt == CSP_LOAD_BYTE_COUNTING) {
                PMPI_Type_size(origin_datatype, &data_size);
                data_size *= origin_count;
            }
#endif

            mpi_errno = CSP_Get_gp_rank(target_rank, 0, 0, data_size, ug_win,
                                               &target_g_rank_in_ug, &target_g_offset);
            if (mpi_errno != MPI_SUCCESS)
                goto fn_fail;

            ug_target_disp = target_g_offset + ug_win->targets[target_rank].disp_unit * target_disp;

            /* Issue operation to the ghost process in corresponding ug-window of target process. */
            mpi_errno = PMPI_Get(origin_addr, origin_count, origin_datatype,
                                 target_g_rank_in_ug, ug_target_disp,
                                 target_count, target_datatype, *win_ptr);
            if (mpi_errno != MPI_SUCCESS)
                goto fn_fail;

            CSP_DBG_PRINT("CASPER Get from (ghost %d, win 0x%x  [%s]) instead of "
                             "target %d, 0x%lx(0x%lx + %d * %ld)\n",
                             target_g_rank_in_ug, *win_ptr,
                             CSP_Win_epoch_stat_name[ug_win->epoch_stat],
                             target_rank, ug_target_disp, target_g_offset,
                             ug_win->targets[target_rank].disp_unit, target_disp);
        }
    }
  fn_exit:
    return mpi_errno;

  fn_fail:
    goto fn_exit;
}

int MPI_Get(void *origin_addr, int origin_count,
            MPI_Datatype origin_datatype,
            int target_rank, MPI_Aint target_disp,
            int target_count, MPI_Datatype target_datatype, MPI_Win win)
{
    static const char FCNAME[] = "MPI_Get";
    int mpi_errno = MPI_SUCCESS;
    CSP_Win *ug_win;

    CSP_DBG_PRINT_FCNAME();

    CSP_Fetch_ug_win_from_cache(win, ug_win);

    if (ug_win) {
        /* casper window */
        mpi_errno = CSP_Get_impl(origin_addr, origin_count, origin_datatype,
                                    target_rank, target_disp, target_count, target_datatype, win,
                                    ug_win);
    }
    else {
        /* normal window */
        mpi_errno = PMPI_Get(origin_addr, origin_count, origin_datatype,
                             target_rank, target_disp, target_count, target_datatype, win);
    }

  fn_exit:
    return mpi_errno;

  fn_fail:
    goto fn_exit;
}
