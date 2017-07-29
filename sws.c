/*
 * File: sws.c
 * Author: Alex Brodsky
 * Purpose: This file contains the implementation of a simple web server.
 *          It consists of two functions: main() which contains the main
 *          loop accept client connections, and serve_client(), which
 *          processes each client request.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "network.h"

#define MAX_HTTP_SIZE 8192                 /* size of buffer to allocate */
#define MAX_THREADS 100
#define KB 1024

typedef struct RCB {
  int seq, fileDescriptor, bytesRemaining, fileSize, priority, quantum;
  FILE * handle;
  // Other information the schedule might need
} RCB;

RCB EMPTY_RCB = {0,0,0,0,0,0,NULL};
RCB requestTable[MAX_THREADS];
int scheduler=0;
int currentRequest=0;
int numRequests=0;

static void scheduleRCB(int len, FILE * fin, int fd);
static void scheduleSJF(int len, FILE * fin, int fd);
static void scheduleRR(int len, FILE * fin, int fd);
static void scheduleMLFQ(int len, FILE * fin, int fd);

static void sendPacketsToClientMLFQ(RCB *req);

static void processRCB();
static void processSJF();
static void processRR();
static void processMLFQ();

static int isRCBEmpty(RCB req);
static void initRequestTable();
static void processWholeRequest();
static void printRCBTable();
static void printRCB(RCB r);

/* This function takes a file handle to a client, reads in the request,
 *    parses the request, and sends back the requested file.  If the
 *    request is improper or the file is not available, the appropriate
 *    error is sent back.
 * Parameters:
 *             fd : the file descriptor to the client connection
 * Returns: None
 */
static void serve_client( int fd ) {
  static char *buffer;                              /* request buffer */
  char *req = NULL;                                 /* ptr to req file */
  char *brk;                                        /* state used by strtok */
  char *tmp;                                        /* error checking ptr */
  FILE *fin;                                        /* input file handle */
  int len;                                          /* length of data read */

  if( !buffer ) {                                   /* 1st time, alloc buffer */
    buffer = malloc( MAX_HTTP_SIZE );
    if( !buffer ) {                                 /* error check */
      perror( "Error while allocating memory" );
      abort();
    }
  }

  memset( buffer, 0, MAX_HTTP_SIZE );
  if( read( fd, buffer, MAX_HTTP_SIZE ) <= 0 ) {    /* read req from client */
    perror( "Error while reading request" );
    abort();
  }

  /* standard requests are of the form
   *   GET /foo/bar/qux.html HTTP/1.1
   * We want the second token (the file path).
   */
  tmp = strtok_r( buffer, " ", &brk );              /* parse request */
  if( tmp && !strcmp( "GET", tmp ) ) {
    req = strtok_r( NULL, " ", &brk );
  }

  if( !req ) {                                      /* is req valid? */
    len = sprintf( buffer, "HTTP/1.1 400 Bad request\n\n" );
    write( fd, buffer, len );                       /* if not, send err */
    close(fd);
  } else {                                          /* if so, open file */
    req++;                                          /* skip leading / */
    fin = fopen( req, "r" );                        /* open file */
    if( !fin ) {                                    /* check if successful */
      len = sprintf( buffer, "HTTP/1.1 404 File not found\n\n" );
      write( fd, buffer, len );                     /* if not, send err */
      close(fd);
    } else {                                        /* if so, send file */
      len = sprintf( buffer, "HTTP/1.1 200 OK\n\n" );/* send success code */
      write( fd, buffer, len );

      fseek(fin, 0, SEEK_END); // seek to end of file
      int size = ftell(fin); // get current file pointer
      fseek(fin, 0, SEEK_SET);
      scheduleRCB(size, fin, fd);

      /*
      do {                                          // loop, read & send file
        len = fread( buffer, 1, MAX_HTTP_SIZE, fin );  // read file chunk
        if( len < 0 ) {                             // check for errors
            perror( "Error while writing to client" );
        } else if( len > 0 ) {                      // if none, send chunk
          len = write( fd, buffer, len );
          if( len < 1 ) {                           // check for errors
            perror( "Error while writing to client" );
          }
        }
      } while( len == MAX_HTTP_SIZE );              // the last chunk < 8192
        */
    }
  }
}


/* This function is where the program starts running.
 *    The function first parses its command line parameters to determine port #
 *    Then, it initializes, the network and enters the main loop.
 *    The main loop waits for a client (1 or more to connect, and then processes
 *    all clients by calling the seve_client() function for each one.
 * Parameters:
 *             argc : number of command line parameters (including program name
 *             argv : array of pointers to command line parameters
 * Returns: an integer status code, 0 for success, something else for error.
 */
int main( int argc, char **argv ) {
  int port = -1;                                    /* server port # */
  int fd;                                           /* client file descriptor */

  /* check for and process parameters
   */
  if( ( argc < 2 ) || ( sscanf( argv[1], "%d", &port ) < 1 ) ) {
    printf( "usage: <port> <schedule>\n" );
    return 0;
  }

  if (port < 1024 || port > 65335) {
    printf("port must be greater than 1024 and less than 65336\n");
    return 0;
  }

  if (strcmp(argv[2], "SJF") == 0) {
    scheduler = 1;
  } else if (strcmp(argv[2], "RR") == 0) {
    scheduler = 2;
  } else if (strcmp(argv[2], "MLFQ") == 0) {
    scheduler = 3;
  } else {
    printf("Incorrect schedule name\n");
    return 1;
  }
  initRequestTable();
  network_init( port );                             /* init network module */

  for( ;; ) {                                       /* main loop */
    network_wait();                                 /* wait for clients */

    for( fd = network_open(); fd >= 0; fd = network_open() ) { /* get clients */
      serve_client( fd );                           /* process each client */
    }
     while (numRequests > 0) processRCB();
  }

}

void scheduleRCB(int len, FILE *fin, int fd) {
  switch (scheduler) {
    case 1:
      scheduleSJF(len, fin, fd);
      break;
    case 2:
      scheduleRR(len, fin, fd);
      break;
    case 3:
      scheduleMLFQ(len, fin, fd);
      break;
    default:
      printf("schedule var not valid option\n");
  }
  numRequests++;
}

void processRCB() {
  switch (scheduler) {
    case 1:
      processSJF();
      break;
    case 2:
      processRR();
      break;
    case 3:
      processMLFQ();
      break;
    default:
    printf("schedule var not valid option\n");
  }
}


void scheduleSJF(int len, FILE* fin, int fd) {
  //get file size
  fseek(fin, 0, SEEK_END);
  int sz = ftell(fin);
  fseek(fin, 0, SEEK_SET);

  RCB req = { -1, fd, len, len, 1, 0, fin};

    size_t i = 0;

  for(i=0; i < numRequests; i++)
  {
    if(!isRCBEmpty(requestTable[i]))
    {
      if(requestTable[i].bytesRemaining > sz)
      {
        requestTable[i+1] = requestTable[i];
        requestTable[i+1].seq = i+1;
        requestTable[i] = req;
        req.seq = i;
      }
    }
  }
}

void scheduleRR(int len, FILE* fin, int fd) {

}

// priority 1=8KB, 2=64KB,3=Remainder of the response
void scheduleMLFQ(int len, FILE* fin, int fd) {
    int rcbCount = 0;
    int reqIndex = -1;
    int priorityOneCount = 1; // seq start at 1 to account for RCB to be added
    size_t i = 0;
    for (i = 0; rcbCount < numRequests; i++) { // loop until you find all rcb in the table
      if (!isRCBEmpty(requestTable[i])) rcbCount++; // inc rcbCount when RCB found
      else if (reqIndex < 0) reqIndex = i; // if an index location is not found && location is empty, set index to add request to empty location
      if (requestTable[i].priority == 1) priorityOneCount++; // count requests with priority 1 to set sequence for RCB
    }
    RCB req = {
      priorityOneCount, fd, len, len, 1, 0,
      fin
    };
    int indexToSet = (reqIndex < 0) ? ((rcbCount == 0) ? 0 : i+1) : reqIndex;
    requestTable[indexToSet] = req;
}

void processMLFQ() {
  int rcbCount = 0;
  int indexToProcess = -1;
  int lowestSeqOfPriority[4] = {MAX_THREADS + 1};
  int indexOfLowestSeq[4] = {-1};
  int largestSeqOfPriority[4] = {0};
  int priority;
  size_t i;

  // finds highest seq and lowest seq number for each priority
  for (i = 0; rcbCount < numRequests; i++) { // loop until you find all rcb in the table
    if (!isRCBEmpty(requestTable[i])) {
      rcbCount++; // inc rcbCount when RCB found
      priority = requestTable[i].priority;
      if (requestTable[i].seq < lowestSeqOfPriority[priority]) {
        lowestSeqOfPriority[priority] = requestTable[i].seq;
        indexOfLowestSeq[priority] = i;
      }

      if (requestTable[i].seq > largestSeqOfPriority[priority])
        largestSeqOfPriority[priority] = requestTable[i].seq;

    }
  }

  // Starting with p=1 use the lowest sequence as the index to process
  size_t p;
  for (p = 1; p <= 3; p++) {
    if (indexOfLowestSeq[p] != -1) {
      indexToProcess = indexOfLowestSeq[p];
      break;
    }
  }

  if (indexToProcess < 0) {
    printf("No RCB find in request Table\n");
    return;
  }
  if (requestTable[indexToProcess].bytesRemaining != 0) sendPacketsToClientMLFQ(&requestTable[indexToProcess]);
  if (requestTable[indexToProcess].bytesRemaining == 0) {
    numRequests--;
    if (requestTable[indexToProcess].handle == NULL) {
    } else
      fclose( requestTable[indexToProcess].handle );
    close( requestTable[indexToProcess].fileDescriptor );
    requestTable[indexToProcess] = EMPTY_RCB;

  } else {
    if (requestTable[indexToProcess].priority != 3)
      requestTable[indexToProcess].priority++;
    requestTable[indexToProcess].seq = largestSeqOfPriority[requestTable[indexToProcess].priority]+1; // find the largest seq for that priority and add 1
  }
}

void processRR() {

}

void processSJF() {
    size_t i = 0;
  for(i=0; i < sizeof(requestTable); i++)
  {
    processWholeRequest();
    numRequests--;
  }
}

int getEmptyIndexInRCBTable() {
  int i=0;
  while (!isRCBEmpty(requestTable[i]) && i < numRequests)
    i++;
  return (i==numRequests) ? -1 : i;
}

int isRCBEmpty(RCB req) {
  int isRCBEmpty = req.seq == 0
      && req.fileDescriptor == 0
      && req.bytesRemaining == 0
      && req.fileSize == 0
      && req.priority == 0
      && req.handle == NULL;
  return isRCBEmpty;
}

void initRequestTable() {
  size_t i = 0;
  for (i = 0; i < MAX_THREADS; i++) {
    requestTable[i] = EMPTY_RCB;
  }
}

void sendPacketsToClientMLFQ(RCB *req) {
  static char *buffer;                              /* request buffer */
  FILE *fin = req->handle;                          /* input file handle */
  int len = req->fileSize;                          /* length of data read */
  int fd = req->fileDescriptor;
  int packetCounter = 0;
  int packetLimit = (req->priority == 1) ? 1 : 8;

  buffer = malloc( MAX_HTTP_SIZE );
  if( !buffer ) {                                   /* 1st time, alloc buffer */
    buffer = malloc( MAX_HTTP_SIZE );
    if( !buffer ) {                                 /* error check */
      perror( "Error while allocating memory" );
      abort();
    }
  }

  memset( buffer, 0, MAX_HTTP_SIZE );
  do {                                             // loop, read & send file
    len = fread( buffer, 1, MAX_HTTP_SIZE, fin );  // read file chunk
    if( len < 0 ) {                                // check for errors
        perror( "Error while writing to client" );
    } else if( len > 0 ) {                         // if none, send chunk
      len = write( fd, buffer, len );
      if( len < 1 ) {                              // check for errors
        perror( "Error while writing to client" );
      }
    }
    req->bytesRemaining -= len;
    packetCounter++;
  } while( len == MAX_HTTP_SIZE && packetCounter <= packetLimit ); // the last chunk < 8192
}

void processWholeRequest() {

}

void printRCBTable() {
  int rcbCount =0;
  size_t i=0;
  for (i = 0; rcbCount < numRequests; i++) { // loop until you find all rcb in the table
    if (!isRCBEmpty(requestTable[i]))
      rcbCount++; // inc rcbCount when RCB found
      printRCB(requestTable[i]);
  }
  printf("\n");
}

void printRCB(RCB r) {
  int seq = r.seq;
  int br = r.bytesRemaining;
  int fs = r.fileSize;
  int p = r.priority;
  int fd = r.fileDescriptor;

  printf("seq:%d fd:%d fs:%d br:%d p:%d\n", seq, fd, fs, br, p );
}
