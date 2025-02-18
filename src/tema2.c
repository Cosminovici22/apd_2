#include <mpi.h>
#include <stdio.h>
#include <stdlib.h>

#include "common.h"
#include "tracker.h"
#include "peer.h"

MPI_Comm request_comm;
MPI_Comm response_comm;

int main(int argc, char *argv[])
{
    int numtasks, rank, provided;

    MPI_Init_thread(&argc, &argv, MPI_THREAD_MULTIPLE, &provided);
    if (provided < MPI_THREAD_MULTIPLE) {
        fprintf(stderr, "MPI nu are suport pentru multi-threading\n");
        exit(-1);
    }
    MPI_Comm_size(MPI_COMM_WORLD, &numtasks);
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);

    MPI_Comm_dup(MPI_COMM_WORLD, &request_comm);
    MPI_Comm_dup(MPI_COMM_WORLD, &response_comm);

    if (rank == TRACKER_RANK) {
        tracker(numtasks, rank);
    } else {
        peer(numtasks, rank);
    }

    MPI_Comm_free(&request_comm);
    MPI_Comm_free(&response_comm);

    MPI_Finalize();
}
