#pragma once

#include <mpi.h>

#define TRACKER_RANK 0
#define MAX_FILES 10
#define MAX_FILENAME 15
#define HASH_SIZE 32
#define MAX_CHUNKS 100
#define MAX_CLIENTS 10

struct file_metadata {
    char name[MAX_FILENAME + 1];

    int chunk_count;
    char hashes[MAX_CHUNKS][HASH_SIZE + 1];
};

struct message {
    enum {
        NACK,
        ACK,
        SWARM_REQUEST,
        UPLOAD_COUNT_REQUEST,
        SEGMENT_REQUEST,
        FILE_DONE,
        CLIENT_DONE,
        STOP
    } type;
    /* Only used when message type is `SEGMENT_REQUEST`, `SWARM_REQUEST` or `FILE_DONE`. */
    char filename[MAX_FILENAME + 1];
    /* Only used when message type is `SEGMENT_REQUEST`. */
    int hash_index;
};

enum client_type {
    NOT_APPLICABLE = 0,
    PEER,
    SEED,
};

/*
 * Duplicates of `MPI_COMM_WORLD` used to prevent miscorrelation of messages between parallel
 * `MPI_Recv`s. Download threads only receive on `response_comm` and upload threads only receive
 * on `request_comm`. `MPI_COMM_WORLD` is exclusively used for communication with the tracker.
 */
extern MPI_Comm request_comm;
extern MPI_Comm response_comm;
