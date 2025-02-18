#include "tracker.h"

#include <mpi.h>
#include <string.h>

#include "common.h"

static int file_count;
/* Stores tracked files. */
static struct file_metadata files[MAX_FILES];

static enum client_type swarms[MAX_FILES][MAX_CLIENTS];

void add_file(struct file_metadata *file, int source)
{
    for (int i = 0; i < file_count; i++) {
        if (strcmp(files[i].name, file->name) == 0) {
            swarms[i][source] = SEED;
            return;
        }
    }

    swarms[file_count][source] = SEED;
    memcpy(&files[file_count++], file, sizeof *file);
}

/* Awaits the initial information regarding owned files from each client. */
void await_clients(int numtasks, int rank)
{
    struct message response;

    for (int i = 0; i < numtasks - 1; i++) {
        MPI_Status status;
        int file_count;
        struct file_metadata files[MAX_FILES];

        MPI_Recv(&files, sizeof files, MPI_BYTE, MPI_ANY_SOURCE, 0, MPI_COMM_WORLD, &status);

        MPI_Get_count(&status, MPI_BYTE, &file_count);
        file_count /= sizeof *files;
        for (int j = 0; j < file_count; j++) {
            add_file(&files[j], status.MPI_SOURCE);
        }
    }

    response.type = ACK;
    for (int i = 0; i < numtasks; i++) {
        if (i != rank) {
            MPI_Send(&response, sizeof response, MPI_BYTE, i, 0, MPI_COMM_WORLD);
        }
    }
}

void tracker(int numtasks, int rank)
{
    int done_client_count;

    file_count = 0;
    done_client_count = 0;

    await_clients(numtasks, rank);

    while (1) {
        MPI_Status status;
        struct message msg;

        MPI_Recv(&msg, sizeof msg, MPI_BYTE, MPI_ANY_SOURCE, 0, MPI_COMM_WORLD, &status);

        switch (msg.type) {
        case SWARM_REQUEST:
            for (int i = 0; i < file_count; i++) {
                if (strcmp(files[i].name, msg.filename) != 0) {
                    continue;
                }

                MPI_Send(&files[i], sizeof *files, MPI_BYTE, status.MPI_SOURCE, 0, MPI_COMM_WORLD);
                MPI_Send(&swarms[i], sizeof *swarms, MPI_BYTE, status.MPI_SOURCE, 0,MPI_COMM_WORLD);

                swarms[i][status.MPI_SOURCE] = PEER;
                break;
            }
            break;

        case FILE_DONE:
            for (int i = 0; i < file_count; i++) {
                if (strcmp(files[i].name, msg.filename) != 0) {
                    continue;
                }

                swarms[i][status.MPI_SOURCE] = SEED;
                break;
            }
            break;

        case CLIENT_DONE:
            done_client_count++;
            /* Check wether all clients have finished downloading their wanted files. */
            if (done_client_count == numtasks - 1) {
                msg.type = STOP;
                for (int i = 0; i < numtasks; i++) {
                    if (i != rank) {
                        MPI_Send(&msg, sizeof msg, MPI_BYTE, i, 0, request_comm);
                    }
                }

                return;
            }
            break;

        default:
            break;
        }
    }
}
