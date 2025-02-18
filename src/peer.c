#include "peer.h"

#include <mpi.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common.h"

/* Stores the combined number of owned and wanted files. */
static int file_count;
/* Stores only the number of owned files. */
static int owned_file_count;
/* Stores both owned and wanted files. */
static struct file_metadata files[MAX_FILES];

/*
 * Stores the swarms of wanted files received from the tracker.
 * The file `files[i]` has its swarm stored in `swarms[i]`.
 */
static enum client_type swarms[MAX_FILES][MAX_CLIENTS];

/*
 * Stores the number of owned segments for every file in `files`.
 * The file `files[i]` has its number of owned segments stored in `owned_segment_count[i]`.
 */
static int owned_segment_count[MAX_FILES];
/*
 * Locks which prevent simultaneous reading of `owned_segment_count[i]` by upload thread and
 * writing of `owned_segment_count[i]` by download thread.
 */
static pthread_mutex_t segment_count_locks[MAX_FILES];

/* Stores the number of file uploads made by this client */
static int upload_count;
/* Stores the number of file uploads made by other clients. Used for segment request fairness. */
static int peer_upload_counts[MAX_CLIENTS];

void scan_input(int rank)
{
    FILE *input_file;
    char input_filename[17];

    sprintf(input_filename, "in%d.txt", rank);
    input_file = fopen(input_filename, "rt");

    fscanf(input_file, "%d", &owned_file_count);
    for (int i = 0; i < owned_file_count; i++) {
        fscanf(input_file, "%s %d", files[i].name, &files[i].chunk_count);
        for (int j = 0; j < files[i].chunk_count; j++) {
            fscanf(input_file, "%s", files[i].hashes[j]);
        }
        pthread_mutex_init(&segment_count_locks[i], NULL);
        owned_segment_count[i] = files[i].chunk_count;
    }

    fscanf(input_file, "%d", &file_count);
    file_count += owned_file_count;
    for (int i = owned_file_count; i < file_count; i++) {
        fscanf(input_file, "%s\n", files[i].name);
        pthread_mutex_init(&segment_count_locks[i], NULL);
        owned_segment_count[i] = 0;
    }

    fclose(input_file);
}

/* Sends metadata of files for which this client is initially a seed. */
void notify_tracker()
{
    struct message response;

    MPI_Send(files, owned_file_count * sizeof *files, MPI_BYTE, TRACKER_RANK, 0, MPI_COMM_WORLD);
    MPI_Recv(&response, sizeof response, MPI_BYTE, TRACKER_RANK, 0, MPI_COMM_WORLD, NULL);
}

void request_swarms()
{
    struct message request;

    request.type = SWARM_REQUEST;
    for (int i = owned_file_count; i < file_count; i++) {
        strcpy(request.filename, files[i].name);

        MPI_Send(&request, sizeof request, MPI_BYTE, TRACKER_RANK, 0, MPI_COMM_WORLD);
        MPI_Recv(&files[i], sizeof *files, MPI_BYTE, TRACKER_RANK, 0, MPI_COMM_WORLD, NULL);
        MPI_Recv(&swarms[i], sizeof *swarms, MPI_BYTE, TRACKER_RANK, 0, MPI_COMM_WORLD, NULL);
    }
}

/* Populates the `peer_upload_counts` array for use in `download_thread_func()`. */
void request_upload_count(int rank)
{
    struct message request;
    int numtasks;

    request.type = UPLOAD_COUNT_REQUEST;
    MPI_Comm_size(MPI_COMM_WORLD, &numtasks);
    for (int client = 0; client < numtasks; client++) {
        if (client == TRACKER_RANK || client == rank) {
            continue;
        }

        MPI_Send(&request, sizeof request, MPI_BYTE, client, 0, request_comm);
        MPI_Recv(&peer_upload_counts[client], 1, MPI_INT, client, 0, response_comm, NULL);
    }
}

void print_output(int index, int rank)
{
    char output_filename[33];
    FILE *output_file;

    sprintf(output_filename, "client%d_%s", rank, files[index].name);
    output_file = fopen(output_filename, "wt");

    for (int i = 0; i < owned_segment_count[index]; i++) {
        fprintf(output_file, "%s\n", files[index].hashes[i]);
    }

    fclose(output_file);
}

void request_segment(int index, int rank)
{
    struct message request;

    request.type = SEGMENT_REQUEST;
	strcpy(request.filename, files[index].name);
	/* Segments are requested in order. */
	request.hash_index = owned_segment_count[index];

	/* Sends segment requests to clients in order of upload count until it receives an ACK. */
	while (1) {
        struct message response;
	    int leech;

	    leech = -1;
	    /* Determines the client with least uploads. */
	    for (int client = 0; client < MAX_CLIENTS; client++) {
	        if (swarms[index][client] != NOT_APPLICABLE && client != rank)
	        if (leech == -1 || peer_upload_counts[client] <= peer_upload_counts[leech]) {
	            leech = client;
	        }
	    }

	    MPI_Send(&request, sizeof request, MPI_BYTE, leech, 0, request_comm);
	    MPI_Recv(&response, sizeof response, MPI_BYTE, leech, 0, response_comm, NULL);

	    if (response.type == ACK) {
	        pthread_mutex_lock(&segment_count_locks[index]);
	        /* Effectively marks the segment as owned, since they are requested in order. */
	        owned_segment_count[index]++;
	        pthread_mutex_unlock(&segment_count_locks[index]);
	        break;
	    }

	    /* If a NACK was received, mark client as N/A so it is skipped in the next iteration. */
	    swarms[index][leech] = NOT_APPLICABLE;
	}
}

void *download_thread_func(void *arg)
{
    int rank, downloaded_file_count, downloaded_segment_count;
    struct message request;

    rank = *(int *) arg;
    downloaded_file_count = 0;
    downloaded_segment_count = 0;
    request.type = FILE_DONE;

    /*
     * Alternates file after each downloaded segment. For three wanted files, the download order is
     * (file 1, segment 1) -> (file 2, segment 1) -> (file 3, segment 1) ->
     * (file 1, segment 2) -> (file 2, segment 2) -> (file 3, segment 2) ->
     * (file 1, segment 3) etc.
     */
    for (int i = owned_file_count; 1; i = file_count - 1 == i ? owned_file_count : i + 1) {
        /* Only stop when the number of downloaded files is equal to the number of wanted files. */
        if (downloaded_file_count == file_count - owned_file_count) {
            break;
        }

        if (downloaded_segment_count % 10 == 0) {
            request_swarms();
            /* Request the number of uploads of each client to fairily distribute downloads. */
            request_upload_count(rank);
        }

    	/* Skip files which have finished downloading. */
        if (owned_segment_count[i] == files[i].chunk_count) {
            continue;
        }

        request_segment(i, rank);

        downloaded_segment_count++;
        /* Check if `files[i]` has finished downloading. */
        if (owned_segment_count[i] == files[i].chunk_count) {
            MPI_Send(&request, sizeof request, MPI_BYTE, TRACKER_RANK, 0, MPI_COMM_WORLD);
            downloaded_file_count++;
            print_output(i, rank);
        }
    }

    request.type = CLIENT_DONE;
    MPI_Send(&request, sizeof request, MPI_BYTE, TRACKER_RANK, 0, MPI_COMM_WORLD);

    return NULL;
}

void *upload_thread_func(void *arg)
{
    while (1) {
        MPI_Status status;
        struct message request, response;

        MPI_Recv(&request, sizeof request, MPI_BYTE, MPI_ANY_SOURCE, 0, request_comm, &status);

        switch (request.type) {
        case UPLOAD_COUNT_REQUEST:
            MPI_Send(&upload_count, 1, MPI_INT, status.MPI_SOURCE, 0, response_comm);
            break;

        case SEGMENT_REQUEST:
            response.type = NACK;
            for (int i = 0; i < file_count; i++) {
                int aux;

                if (strcmp(files[i].name, request.filename) != 0) {
                	continue;
                }

                pthread_mutex_lock(&segment_count_locks[i]);
                aux = owned_segment_count[i];
                pthread_mutex_unlock(&segment_count_locks[i]);

                /*
                 * Segment download is done in order, so a requested segment is owned if it's index
                 * is lesser than the number of segments this client owns.
                 */
                if (request.hash_index < aux) {
                    upload_count++;
                    response.type = ACK;
                    break;
                }
            }

            MPI_Send(&response, sizeof response, MPI_BYTE, status.MPI_SOURCE, 0, response_comm);
            break;

        case STOP:
            return NULL;

        default:
            break;
        }
    }

    return NULL;
}

void peer(int numtasks, int rank)
{
    pthread_t download_thread;
    pthread_t upload_thread;

    scan_input(rank);
    notify_tracker();

    pthread_create(&download_thread, NULL, download_thread_func, (void *) &rank);
    pthread_create(&upload_thread, NULL, upload_thread_func, NULL);

    pthread_join(download_thread, NULL);
    pthread_join(upload_thread, NULL);

    for (int i = 0; i < owned_file_count; i++) {
        pthread_mutex_destroy(&segment_count_locks[i]);
    }
}
