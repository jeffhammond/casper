/* -*- Mode: C; c-basic-offset:4 ; -*- */
/*
 * (C) 2017 by Argonne National Laboratory.
 *     See COPYRIGHT in top-level directory.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "mpi.h"

/* This benchmark evaluates 2D halo exchange with cart and derived datatype.*/

#define DEFAULT_ITERS  (1024)
#define DEFAULT_DIM    (1024)

static int iters = DEFAULT_ITERS;
static int dim = DEFAULT_DIM;
static char testname[128] = { 0 };

static void usage(void)
{
    printf("./a.out\n");
    printf("     --iters [iterations; default %d] --dim [dimension size %d]\n",
           DEFAULT_ITERS, DEFAULT_DIM);
    exit(1);
}

static void set_testname(void)
{
    char *val = getenv("TEST_NAME");
    if (val && strlen(val) > 0) {
        strncpy(testname, val, 128);
    }
}

#define ind(x,y)  ((x) * (dim + 2) + (y))

int main(int argc, char **argv)
{
    int i, j, k;
    double start, end, local_time, avg_time;
    int comm_rank, comm_size;
    int dims[2] = { 0, 0 }, periods[2] = {
    1, 1};
    int north, south, east, west;
    double *inbuf, *outbuf, *tmp;
    MPI_Comm comm;
    MPI_Request req[8];
    MPI_Datatype type;

    MPI_Init(NULL, NULL);
    MPI_Comm_rank(MPI_COMM_WORLD, &comm_rank);
    MPI_Comm_size(MPI_COMM_WORLD, &comm_size);

    set_testname();

    while (--argc && ++argv) {
        if (!strcmp(*argv, "--iters")) {
            --argc;
            ++argv;
            iters = atoi(*argv);
        }
        else if (!strcmp(*argv, "--dim")) {
            --argc;
            ++argv;
            dim = atoi(*argv);
        }
        else {
            usage();
        }
    }

    inbuf = (double *) malloc((dim + 2) * (dim + 2) * sizeof(double));
    outbuf = (double *) malloc((dim + 2) * (dim + 2) * sizeof(double));

    MPI_Type_vector(dim, 1, dim + 2, MPI_DOUBLE, &type);
    MPI_Type_commit(&type);

    MPI_Dims_create(comm_size, 2, dims);

    MPI_Info info = MPI_INFO_NULL;
    MPI_Info_create(&info);
    MPI_Info_set(info, (char *) "wildcard_used", (char *) "none");
    MPI_Comm_set_info(MPI_COMM_WORLD, info);
    MPI_Info_free(&info);

    MPI_Cart_create(MPI_COMM_WORLD, 2, dims, periods, 1, &comm);
    assert(comm != MPI_COMM_NULL);

    MPI_Cart_shift(comm, 0, 1, &north, &south);
    MPI_Cart_shift(comm, 1, 1, &west, &east);

#ifdef STEP_TIME
    double cp_t0 = 0, cp_time = 0, avg_cp_time = 0;
#endif

    start = MPI_Wtime();
    for (i = 0; i < iters; i++) {
        MPI_Irecv(&outbuf[ind(0, 1)], dim, MPI_DOUBLE, north, 0, comm, &req[0]);
        MPI_Irecv(&outbuf[ind(dim + 1, 1)], dim, MPI_DOUBLE, south, 0, comm, &req[1]);
        MPI_Irecv(&outbuf[ind(1, 0)], 1, type, west, 0, comm, &req[2]);
        MPI_Irecv(&outbuf[ind(1, dim + 1)], 1, type, east, 0, comm, &req[3]);

#ifdef STEP_TIME
        cp_t0 = MPI_Wtime();
#endif
        for (j = 1; j <= dim; j++) {
            for (k = 1; k <= dim; k++) {
                outbuf[ind(j, k)] =
                    (inbuf[ind(j, k - 1)] + inbuf[ind(j, k + 1)] + inbuf[ind(j - 1, k)] +
                     inbuf[ind(j + 1, k)] + inbuf[ind(j, k)]) / 5.0;
            }
        }

        MPI_Isend(&outbuf[ind(1, 1)], dim, MPI_DOUBLE, north, 0, comm, &req[4]);
        MPI_Isend(&outbuf[ind(dim, 1)], dim, MPI_DOUBLE, south, 0, comm, &req[5]);
        MPI_Isend(&outbuf[ind(1, 1)], 1, type, west, 0, comm, &req[6]);
        MPI_Isend(&outbuf[ind(1, dim)], 1, type, east, 0, comm, &req[7]);
#ifdef STEP_TIME
        cp_time += MPI_Wtime() - cp_t0;
#endif

        MPI_Waitall(8, req, MPI_STATUSES_IGNORE);

        /* swap in and out buffers */
        tmp = outbuf;
        outbuf = inbuf;
        inbuf = tmp;
    }
    end = MPI_Wtime();

    local_time = end - start;
    MPI_Reduce(&local_time, &avg_time, 1, MPI_DOUBLE, MPI_SUM, 0, MPI_COMM_WORLD);
    avg_time /= comm_size;

#ifdef STEP_TIME
    MPI_Reduce(&cp_time, &avg_cp_time, 1, MPI_DOUBLE, MPI_SUM, 0, MPI_COMM_WORLD);
    avg_cp_time /= comm_size;
#endif

    if (comm_rank == 0) {
        printf("%s average_time: %.3f us\n", testname, 1e6 * avg_time / iters);
#ifdef STEP_TIME
        printf("%s compute_time: %.3f us\n", testname, 1e6 * avg_cp_time / iters);
#endif
    }

    free(inbuf);
    free(outbuf);

    MPI_Comm_free(&comm);
    MPI_Type_free(&type);

    MPI_Finalize();

    return 0;
}
