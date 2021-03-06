// Author: Ziqi Fan
// cdbb.c: Collaborative Distributed Burst Buffer
// Note: After 01/18/2017, the name of BB monitor rank is changed to BB coordiantor rank.
//

#include <mpi.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <pthread.h>
#include <limits.h>
#include <stdbool.h>
#include <assert.h>

#define debug 1

#if debug
#define dbg_print(format,args...)\
    do\
    {\
        printf("[%s][%d]"format,__FUNCTION__, __LINE__,## args);\
    }while(0)
#else
#define dbg_print(format,args...)
#endif

//unsigned long burstBufferMaxSize = 3145728; // 3MB = 3*1024*1024
unsigned long burstBufferMaxSize = 4294967296; // 4GB = 4*1024*1024*1024

struct threadParams {
    int rank; // the rank of current process
    int totalRank; // the total number of ranks (processes)
    char* burstBuffer;
    int size; // the size of one burst buffer
    int fileSize; // the size of incoming data
    char* readBuffer; // checkpointing data buffer to write
    unsigned long* localBBmonitor; // length of one
    int ckptRun; // keep track how many ckpts have been performed
    MPI_Win* win; // to manage shared memory
};

unsigned long fsize(char* file)
{
    FILE * f = fopen(file, "r");
    fseek(f, 0, SEEK_END);
    unsigned long len = (unsigned long)ftell(f);
    fclose(f);
    return len;
}

int findSmallest(unsigned long* array, int size) {
    int i = 0;
    int ans;
    unsigned long smallest = INT_MAX;
    for(i=0; i<size; i++) {
        if(smallest > array[i]) {
            smallest = array[i];
            ans = i;
        }
    }
    dbg_print("Rank of smallest burst buffer offset is %d, offset is %lld\n", ans, smallest);
    return ans;
}

void* xMPI_Alloc_mem(size_t nbytes) {
    void* p;
    MPI_Alloc_mem(nbytes, MPI_INFO_NULL, &p);
    if (nbytes != 0 && !p) {
        fprintf(stderr, "MPI_Alloc_mem failed for size %zu\n", nbytes);
        abort();
    }
    return p;
}

// a FIFO queue for producer and consumer to manage accepting and draining data
// start

#define MAX 2000

unsigned long queue[MAX];
int front = 0;
int rear = -1;
int itemCount = 0;

unsigned long peek() {
    return queue[front];
}

bool isEmpty() {
    return itemCount == 0;
}

bool isFull() {
    return itemCount == MAX;
}

int size() {
    return itemCount;
}

void insert(unsigned long data) {
    if(!isFull()) {
        if(rear == MAX-1) {
            rear = -1;
        }
        queue[++rear] = data;
        itemCount++;
    }
    else {
        printf("FIFO queue is full. Enlarge it!!!\n");
    }
}

unsigned long removeData() {
    unsigned long data = queue[front++];

    if(front == MAX) {
        front = 0;
    }

    itemCount--;
    return data;
}
// end
// FIFO queue implementation

void* producer(void *ptr) {
    struct threadParams *tp = ptr;

    dbg_print("BB producer %d: just entered, nothing been done yet\n", tp->rank);

    MPI_Status status;
    int i;

    while(1) {
        MPI_Win_lock(MPI_LOCK_SHARED, 0, 0, *tp->win);

        // receive from writer how much data it wants to write
        int incomingDataSize;
        MPI_Recv(&incomingDataSize, 1, MPI_INT, MPI_ANY_SOURCE, 4, MPI_COMM_WORLD, &status);

        // receive the real data from writer
        MPI_Recv(tp->burstBuffer, incomingDataSize, MPI_CHAR, MPI_ANY_SOURCE, 5, MPI_COMM_WORLD, &status);

        *tp->localBBmonitor += incomingDataSize;

        insert(incomingDataSize);

        dbg_print("BB producer %d: receive %lld amount of data, localBBmonitor is %lld\n", tp->rank, incomingDataSize, *tp->localBBmonitor);

        MPI_Win_unlock(0, *tp->win);
    }
    pthread_exit(0);
}

void* consumer(void *ptr) {
    struct threadParams *tp = ptr;

    dbg_print("BB consumer %d: just entered, nothing been done yet\n", tp->rank);

    while(1) {
        if(*tp->localBBmonitor > 0) {
            char filename[64];
            char *prefix="/scratch.global/fan/rank";
            strcpy(filename, prefix);
            char buf[sizeof(int)+1];
            snprintf(buf, sizeof buf, "%d", tp->rank);
            strcat(filename, buf);
            strcat(filename, ".out");
            FILE *fp;
            fp = fopen(filename, "a+");
            if(fp == NULL) {
                printf("cannot open file for write. Exit!\n");
                return;
            }

            assert(!isEmpty());
            unsigned long drainSize = removeData();

            fwrite(tp->burstBuffer, 1, drainSize, fp);
            fclose(fp);

            *tp->localBBmonitor -= drainSize;

            int BBmonitorRank = 0;

            // tell BB monitor rank I am a BB rank
            int senderID = 0;
            MPI_Send(&senderID, 1, MPI_INT, BBmonitorRank, 0, MPI_COMM_WORLD);

            MPI_Send(tp->localBBmonitor, 1, MPI_UNSIGNED_LONG, BBmonitorRank, 6, MPI_COMM_WORLD);

            dbg_print("BB consumer %d: drained %lld amount of data to PFS, localBBmonitor is %lld\n", tp->rank, drainSize, *tp->localBBmonitor);
        }
    }
    pthread_exit(0);
}

void* writer(void *ptr) {
    // using MPI timer to get the start and end time
    double timeStart, timeEnd;
    timeStart = MPI_Wtime();

    struct threadParams *tp = ptr;

    // before sending the real data, send fileSize to local BB to check global BB status
    // if local BB is not full, send real data to local BB
    // if local BB is full but remote BB is not, send to remote BB
    // else send to PFS directly
    int BBmonitorRank = 0; // BB monitor rank

    // tell BB monitor rank I am a writer
    int senderID = 1;
    MPI_Send(&senderID, 1, MPI_INT, BBmonitorRank, 0, MPI_COMM_WORLD);

    // tell BB monitor how much data I want to write
    MPI_Send(&tp->fileSize, 1, MPI_INT, BBmonitorRank, 1, MPI_COMM_WORLD);

    // 1 means space left in at least one BB, may not be local BB
    int checkResult;
    MPI_Recv(&checkResult, 1, MPI_INT, MPI_ANY_SOURCE, 2, MPI_COMM_WORLD, MPI_STATUS_IGNORE);

    // the returned BB rank number could be local BB or a remote BB
    int returnedBBrank2send;
    MPI_Recv(&returnedBBrank2send, 1, MPI_INT, MPI_ANY_SOURCE, 3, MPI_COMM_WORLD, MPI_STATUS_IGNORE);

    dbg_print("Writer %d: checkResult from BB monitor is %d, returnedBBrank2send is %d\n", tp->rank, checkResult, returnedBBrank2send);

    // there is enough space left in local BB or remote BB
    if(checkResult == 1) {
        // tell BB how much data I want to write
        MPI_Send(&tp->fileSize, 1, MPI_INT, returnedBBrank2send, 4, MPI_COMM_WORLD);

        // send real data
        MPI_Send(tp->readBuffer, tp->fileSize, MPI_CHAR, returnedBBrank2send, 5, MPI_COMM_WORLD);
        dbg_print("Writer %d: send %lld amount of data to BB on rank %d\n", tp->rank, tp->fileSize, returnedBBrank2send);
    }
    else {
        char filename[64];
        char *prefix="/scratch.global/fan/rank";
        strcpy(filename, prefix);
        char buf[sizeof(int)+1];
        snprintf(buf, sizeof buf, "%d", tp->rank);
        strcat(filename, buf);
        strcat(filename, ".out");
        FILE *fp;
        fp = fopen(filename, "a+");
        if(fp == NULL) {
            printf("cannot open file for write. Exit!\n");
            return;
        }
        fwrite(tp->readBuffer, 1, tp->fileSize, fp);
        fclose(fp);

        dbg_print("Writer %d: Not enough space left in any BBs -> write %u to PFS\n", tp->rank, tp->fileSize);
    }

    timeEnd = MPI_Wtime();
    printf( "$$ CKPT Run %d: Elapsed time for writer rank %d is %f, timeStart %f, timeEnd %f\n", tp->ckptRun, tp->rank, timeEnd - timeStart, timeStart, timeEnd);
}

int main(int argc, char** argv) {
    if(argc != 6) {
        printf("USAGE: ./cdbb <CKPT_size * 5>\n");
        exit(1);
    }

    // Initialize the MPI environment. The two arguments to MPI Init are not
    // currently used by MPI implementations, but are there in case future
    // implementations might need the arguments.
    MPI_Init(NULL, NULL);

    // Get the number of processes
    int size;
    MPI_Comm_size(MPI_COMM_WORLD, &size);

    // Get the rank of the process
    int rank;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);

    // Get the name of the processor
    char processor_name[MPI_MAX_PROCESSOR_NAME];
    int name_len;
    MPI_Get_processor_name(processor_name, &name_len);

    MPI_Status status;

    // create window to manage global BB monitor
    MPI_Win win_BB_monitor;
    int BBmonitorSize = size / 8;
    unsigned long* BBmonitor = (unsigned long*)xMPI_Alloc_mem(BBmonitorSize * sizeof(unsigned long));
    int i;
    for(i=0; i<BBmonitorSize; i++) {
        BBmonitor[i] = 0;
    }
    MPI_Win_create(BBmonitor, sizeof(unsigned long) * BBmonitorSize, sizeof(unsigned long), MPI_INFO_NULL, MPI_COMM_WORLD, &win_BB_monitor);

    // create window to manage local BB monitor
    MPI_Win win_local_BB;
    unsigned long* localBBmonitor = (unsigned long*)xMPI_Alloc_mem(sizeof(unsigned long));
    *localBBmonitor = 0;
    MPI_Win_create(localBBmonitor, sizeof(unsigned long), sizeof(unsigned long), MPI_INFO_NULL, MPI_COMM_WORLD, &win_local_BB);

    FILE *fp;
    //fp = fopen("/home/dudh/fanxx234/CDBB/ICC2011.pdf", "r");
    fp = fopen("/home/dudh/fanxx234/CDBB/ddFile.input", "r");
    if(fp == NULL) {
        printf("cannot open file for read. Exit!\n");
        return 1;
    }

    // read file to buffer
    //unsigned long fileSize = fsize("/home/dudh/fanxx234/CDBB/ICC2011.pdf");
    unsigned long fileSize = fsize("/home/dudh/fanxx234/CDBB/ddFile.input");
    char *readBuffer;
    fseek(fp, 0, SEEK_END);
    rewind(fp);
    readBuffer = (char*) malloc(sizeof(char) * fileSize);
    if (readBuffer == 0) {
        printf("ERROR: Out of memory when malloc readBuffer\n");
        return 1;
    }
    fread(readBuffer, 1, fileSize, fp);
    fclose(fp);

    // Print off a hello world message
    dbg_print("Hello world from processor %s, rank %d out of %d processors\n", processor_name, rank, size);

    MPI_Barrier(MPI_COMM_WORLD);

    // BB coordinator rank
    if(rank == 0) {
        while(1) {
            int senderID; // who is sending me information? 0 means from BB; 1 means from writer
            MPI_Recv(&senderID, 1, MPI_INT, MPI_ANY_SOURCE, 0, MPI_COMM_WORLD, &status);

            // talking with BB
            if(senderID == 0) {
                unsigned long newBBmonitor;
                MPI_Recv(&newBBmonitor, 1, MPI_UNSIGNED_LONG, MPI_ANY_SOURCE, 6, MPI_COMM_WORLD, &status);
                int localBB = status.MPI_SOURCE / 8; // calculate localBB offset in BBmonitor
                BBmonitor[localBB] = newBBmonitor;
            }
            // talking with writer
            if(senderID == 1) {
                // receive from writer how much data it wants to write
                int incomingDataSize;
                MPI_Recv(&incomingDataSize, 1, MPI_INT, MPI_ANY_SOURCE, 1, MPI_COMM_WORLD, &status);

                // calculate localBB offset in BBmonitor
                int localBB = status.MPI_SOURCE / 8;

                int checkResult = 0; // denote whether any BB has space left; 1 means yes and 0 means no

                MPI_Win_lock(MPI_LOCK_SHARED, 0, 0, win_BB_monitor);

                int rankOfSmallestBurstBufferOffset = findSmallest(BBmonitor, BBmonitorSize);

                // local BB has enough space; let writer send the real data
                if(BBmonitor[localBB] + incomingDataSize < burstBufferMaxSize) {
                    BBmonitor[localBB] += incomingDataSize;

                    checkResult = 1;
                    MPI_Send(&checkResult, 1, MPI_INT, status.MPI_SOURCE, 2, MPI_COMM_WORLD);
                    int BBrank2send = localBB * 8 + 7;
                    MPI_Send(&BBrank2send, 1, MPI_INT, status.MPI_SOURCE, 3, MPI_COMM_WORLD);

                    dbg_print("BB monitor: let writer %d send its data to it local BB on rank %d\n", status.MPI_SOURCE, BBrank2send);
                }
                // local BB is full, but remote BB has enough space;
                // let writer know which remote BB to try
                else if(BBmonitor[rankOfSmallestBurstBufferOffset] + incomingDataSize < burstBufferMaxSize) {
                    BBmonitor[rankOfSmallestBurstBufferOffset] += incomingDataSize;

                    checkResult = 1;
                    MPI_Send(&checkResult, 1, MPI_INT, status.MPI_SOURCE, 2, MPI_COMM_WORLD);
                    int BBrank2send  = rankOfSmallestBurstBufferOffset * 8 + 7;
                    MPI_Send(&BBrank2send, 1, MPI_INT, status.MPI_SOURCE, 3, MPI_COMM_WORLD);

                    dbg_print("BB monitor: local BB is full, let writer %d send its data to it remote BB on rank %d\n", status.MPI_SOURCE, BBrank2send);
                }
                // all BBs do not have enough space; writer has to bypass BB and write to PFS
                else {
                    checkResult = 0;
                    MPI_Send(&checkResult, 1, MPI_INT, status.MPI_SOURCE, 2, MPI_COMM_WORLD);
                    int BBrank2send  = 666;
                    MPI_Send(&BBrank2send, 1, MPI_INT, status.MPI_SOURCE, 3, MPI_COMM_WORLD);
                    dbg_print("BB monitor: all BBs are full for writer %d\n", status.MPI_SOURCE);
                }
                MPI_Win_unlock(0, win_BB_monitor);
            }
        }
    }
    // BB rank
    if(rank % 8 == 7) {
        // writer processes do not expose BBmoniror memory in the window
        // [!!!] Note that if not do this, the whole program will hang
        //MPI_Win_create(NULL, 0, 1, MPI_INFO_NULL, MPI_COMM_WORLD, &win);

        char *burstBuffer;
        //burstBuffer = (char*) malloc(sizeof(char) * 3 *  1024 * 1024); // malloc 3MB as the local burst buffer
        burstBuffer = (char*) malloc(sizeof(char) * 4 *  1024 * 1024 *1024 ); // malloc 4GB as the local burst buffer

        pthread_t pro, con;

        struct threadParams tp;
        tp.rank = rank;
        tp.totalRank = size;
        tp.burstBuffer = burstBuffer;
        tp.size = burstBufferMaxSize;
        tp.fileSize = fileSize;
        tp.localBBmonitor = localBBmonitor;
        tp.readBuffer = readBuffer;
        tp.win = &win_local_BB;

        // Create the threads
        pthread_create(&con, NULL, consumer, &tp);
        pthread_create(&pro, NULL, producer, &tp);

        // Wait for the threads to finish
        // [!!!] but currently pro and con threads are in infinite loop
        //       they will not exit naturally
        pthread_join(pro, NULL);
        pthread_join(con, NULL);

        free(burstBuffer);
    }
    // 1st application from with 64 ranks from 1 to 73
    else if (rank >= 1 && rank <= 73) {
        int ckptRun = 0; // keep track how many ckpts have been performed

        sleep(0);
        dbg_print("1st application start after sleep for 0 seconds\n");

        while(1) {
            pthread_t wrtr;

            struct threadParams tp;
            tp.rank = rank;
            tp.totalRank = size;
            tp.burstBuffer = NULL;
            tp.size = burstBufferMaxSize;

            tp.fileSize = atoi(argv[1]);
            //tp.fileSize = 167772160; // small
            //tp.fileSize = 872415232; // large
            //tp.fileSize = 167772160; // mix

            tp.localBBmonitor = NULL;
            tp.readBuffer = readBuffer;
            tp.ckptRun = ckptRun;
            tp.win = NULL;

            // Create the threads
            pthread_create(&wrtr, NULL, writer, &tp);

            ckptRun++;

            sleep(600); // checkpointing frequency
        }
    }
    // 2nd application from with 64 ranks from 74 to 146
    else if (rank >= 74 && rank <= 146) {
        int ckptRun = 0; // keep track how many ckpts have been performed

        sleep(120);
        dbg_print("2nd application start after sleep for 120 seconds\n");

        while(1) {
            pthread_t wrtr;

            struct threadParams tp;
            tp.rank = rank;
            tp.totalRank = size;
            tp.burstBuffer = NULL;
            tp.size = burstBufferMaxSize;

            tp.fileSize = atoi(argv[2]);
            //tp.fileSize = 285212672; // small
            //tp.fileSize = 1258291200; // large
            //tp.fileSize = 285212672; // mix

            tp.localBBmonitor = NULL;
            tp.readBuffer = readBuffer;
            tp.ckptRun = ckptRun;
            tp.win = NULL;

            // Create the threads
            pthread_create(&wrtr, NULL, writer, &tp);

            ckptRun++;

            sleep(600); // checkpointing frequency
        }
    }
    // 3rd application from with 64 ranks from 147 to 219
    else if (rank >= 147 && rank <= 219) {
        int ckptRun = 0; // keep track how many ckpts have been performed

        sleep(240);
        dbg_print("3rd application start after sleep for 240 seconds\n");

        while(1) {
            pthread_t wrtr;

            struct threadParams tp;
            tp.rank = rank;
            tp.totalRank = size;
            tp.burstBuffer = NULL;
            tp.size = burstBufferMaxSize;

            tp.fileSize = atoi(argv[3]);
            //tp.fileSize = 285212672; // small
            //tp.fileSize = 1577058304; // large
            //tp.fileSize = 654311424; // mix

            tp.localBBmonitor = NULL;
            tp.readBuffer = readBuffer;
            tp.ckptRun = ckptRun;
            tp.win = NULL;

            // Create the threads
            pthread_create(&wrtr, NULL, writer, &tp);

            ckptRun++;

            sleep(600); // checkpointing frequency
        }
    }
    // 4th application from with 64 ranks from 220 to 292
    else if (rank >= 220 && rank <= 292) {
        int ckptRun = 0; // keep track how many ckpts have been performed

        sleep(360);
        dbg_print("4th application start after sleep for 360 seconds\n");

        while(1) {
            pthread_t wrtr;

            struct threadParams tp;
            tp.rank = rank;
            tp.totalRank = size;
            tp.burstBuffer = NULL;
            tp.size = burstBufferMaxSize;

            tp.fileSize = atoi(argv[4]);
            //tp.fileSize = 301989888; // small
            //tp.fileSize = 1660944384; // large
            //tp.fileSize = 1660944384; // mix

            tp.localBBmonitor = NULL;
            tp.readBuffer = readBuffer;
            tp.ckptRun = ckptRun;
            tp.win = NULL;

            // Create the threads
            pthread_create(&wrtr, NULL, writer, &tp);

            ckptRun++;

            sleep(600); // checkpointing frequency
        }
    }
    // 5th application from with 64 ranks from 293 to 365
    else if (rank >= 293 && rank <= 365) {
        int ckptRun = 0; // keep track how many ckpts have been performed

        sleep(480);
        dbg_print("5th application start after sleep for 480 seconds\n");

        while(1) {
            pthread_t wrtr;

            struct threadParams tp;
            tp.rank = rank;
            tp.totalRank = size;
            tp.burstBuffer = NULL;
            tp.size = burstBufferMaxSize;

            tp.fileSize = atoi(argv[5]);
            //tp.fileSize = 553648128; // small
            //tp.fileSize = 2147483646; // large
            //tp.fileSize = 2147483646; // mix

            tp.localBBmonitor = NULL;
            tp.readBuffer = readBuffer;
            tp.ckptRun = ckptRun;
            tp.win = NULL;

            // Create the threads
            pthread_create(&wrtr, NULL, writer, &tp);

            ckptRun++;

            sleep(600); // checkpointing frequency
        }
    }
    // rest ranks do nothing
    else {
        dbg_print("Rank %d does nothing\n", rank);
    }

    free(readBuffer);
    MPI_Win_free(&win_BB_monitor);
    MPI_Win_free(&win_local_BB);
    MPI_Free_mem(BBmonitor);
    MPI_Free_mem(localBBmonitor);

    // Finalize the MPI environment. No more MPI calls can be made after this
    MPI_Finalize();
    return 0;
}
