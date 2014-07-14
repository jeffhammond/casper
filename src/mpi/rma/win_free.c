#include <stdio.h>
#include <stdlib.h>
#include "mpiasp.h"

int MPI_Win_free(MPI_Win * win)
{
    static const char FCNAME[] = "MPIASP_Win_free";
    int mpi_errno = MPI_SUCCESS;
    MPIASP_Win *ua_win;
    int user_rank, user_nprocs, user_local_rank, user_local_nprocs;
    int ua_tag, i;

    MPIASP_DBG_PRINT_FCNAME();

    mpi_errno = get_ua_win(*win, &ua_win);
    if (mpi_errno != MPI_SUCCESS)
        goto fn_fail;

    /* Release additional resources if it is an MPIASP-window */
    if (ua_win > 0) {
        PMPI_Comm_rank(ua_win->user_comm, &user_rank);
        PMPI_Comm_size(ua_win->user_comm, &user_nprocs);
        PMPI_Comm_rank(ua_win->local_user_comm, &user_local_rank);
        PMPI_Comm_size(ua_win->local_user_comm, &user_local_nprocs);

        mpi_errno = MPIASP_Tag_format((int) ua_win->user_comm, &ua_tag);
        if (mpi_errno != MPI_SUCCESS)
            goto fn_fail;

        MPIASP_Func_start(MPIASP_FUNC_WIN_FREE, user_nprocs, user_local_nprocs, ua_tag,
                          ua_win->local_user_comm);

        /* Notify the handle of target ASP win */
        if (user_local_rank == 0) {
            mpi_errno = PMPI_Send(&ua_win->asp_win_handle, 1, MPI_UNSIGNED_LONG,
                                  MPIASP_RANK_IN_COMM_LOCAL, ua_tag, MPIASP_COMM_LOCAL);
            if (mpi_errno != MPI_SUCCESS)
                goto fn_fail;
        }

        /* Free ua_win before local_ua_win, because all the incoming operations
         * should be done before free shared buffers.
         *
         * We do not need additional barrier in Manticore for waiting all
         * operations complete, because Win_free already internally add a barrier
         * for waiting operations on that window complete.
         */
        if (ua_win->ua_wins) {
            MPIASP_DBG_PRINT("[%d] \t free ua windows\n", user_rank);
            for (i = 0; i < user_nprocs; i++) {
                if (ua_win->ua_wins[i]) {
                    mpi_errno = PMPI_Win_free(&ua_win->ua_wins[i]);
                    if (mpi_errno != MPI_SUCCESS)
                        goto fn_fail;
                }
            }
        }

        if (ua_win->local_ua_win) {
            MPIASP_DBG_PRINT("[%d] \t free shared window\n", user_rank);
            mpi_errno = PMPI_Win_free(&ua_win->local_ua_win);
            if (mpi_errno != MPI_SUCCESS)
                goto fn_fail;
        }

        if (ua_win->local_ua_group != MPI_GROUP_NULL) {
            mpi_errno = PMPI_Group_free(&ua_win->local_ua_group);
            if (mpi_errno != MPI_SUCCESS)
                goto fn_fail;
        }
        if (ua_win->ua_comm != MPI_COMM_NULL && ua_win->ua_comm != MPI_COMM_WORLD) {
            mpi_errno = PMPI_Comm_free(&ua_win->ua_comm);
            if (mpi_errno != MPI_SUCCESS)
                goto fn_fail;
        }
        if (ua_win->ua_group != MPI_GROUP_NULL) {
            mpi_errno = PMPI_Group_free(&ua_win->ua_group);
            if (mpi_errno != MPI_SUCCESS)
                goto fn_fail;
        }
        if (ua_win->user_group != MPI_GROUP_NULL) {
            mpi_errno = PMPI_Group_free(&ua_win->user_group);
            if (mpi_errno != MPI_SUCCESS)
                goto fn_fail;
        }

        if (ua_win->local_ua_comm && ua_win->local_ua_comm != MPIASP_COMM_LOCAL) {
            MPIASP_DBG_PRINT("[%d] \t free shared communicator\n", user_rank);
            mpi_errno = PMPI_Comm_free(&ua_win->local_ua_comm);
            if (mpi_errno != MPI_SUCCESS)
                goto fn_fail;
        }

        if (ua_win->local_user_comm && ua_win->local_user_comm != MPIASP_COMM_USER_LOCAL) {
            MPIASP_DBG_PRINT("[%d] \t free local USER communicator\n", user_rank);
            mpi_errno = PMPI_Comm_free(&ua_win->local_user_comm);
            if (mpi_errno != MPI_SUCCESS)
                goto fn_fail;
        }

        MPIASP_DBG_PRINT("[%d] \t free user window\n", user_rank);
        mpi_errno = PMPI_Win_free(win);
        if (mpi_errno != MPI_SUCCESS)
            goto fn_fail;

        /* ua_win->user_comm is created by user, will be freed by user. */

        if (ua_win->disp_units)
            free(ua_win->disp_units);
        if (ua_win->base_asp_offset)
            free(ua_win->base_asp_offset);
        if (ua_win->local_ua_win_param)
            free(ua_win->local_ua_win_param);
        if (ua_win->asp_ranks_in_ua)
            free(ua_win->asp_ranks_in_ua);
        if (ua_win->ua_wins)
            free(ua_win->ua_wins);

        free(ua_win);

        MPIASP_DBG_PRINT("[%d] Freed MPIASP window 0x%x\n", user_rank, *win);
    }
    else {
        PMPI_Comm_rank(MPI_COMM_WORLD, &user_rank);

        mpi_errno = PMPI_Win_free(win);
        if (mpi_errno != MPI_SUCCESS)
            goto fn_fail;

        MPIASP_DBG_PRINT("[%d] Freed MPI window 0x%x\n", user_rank, *win);
    }

  fn_exit:

    return mpi_errno;

  fn_fail:

    goto fn_exit;
}
