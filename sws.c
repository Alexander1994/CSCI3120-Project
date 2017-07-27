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
#define MAX_THREADS 1024
#define KB 1024

typedef struct RCB {
  int seq, fileDescriptor, bytesRemaining, fileSize, priority, quantum;
  FILE * handle;
  // Other information the schedule might need
} RCB;

RCB EMPTY_RCB = {0,0,0,0,0,NULL};
RCB requestTable[MAX_THREADS];
int scheduler=0;
int currentRequest=0;
int numRequests=0;

static void scheduleRCB(int len, FILE * fin, int fd);
static void scheduleSJF(int len, FILE * fin, int fd);
static void scheduleRR(int len, FILE * fin, int fd);
static void scheduleMLFQ(int len, FILE * fin, int fd);

static void processRCB();
static void processSJF();
static void processRR();
static void processMLFQ();

static int isRCBEmpty(RCB req);
static void initRequestTable();
static void processWholeRequest();

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
  } else {                                          /* if so, open file */
    req++;                                          /* skip leading / */
    fin = fopen( req, "r" );                        /* open file */
    if( !fin ) {                                    /* check if successful */
      len = sprintf( buffer, "HTTP/1.1 404 File not found\n\n" );
      write( fd, buffer, len );                     /* if not, send err */
    } else {                                        /* if so, send file */
      len = sprintf( buffer, "HTTP/1.1 200 OK\n\n" );/* send success code */
      write( fd, buffer, len );
      scheduleRCB(len, fin, fd);
      fclose( fin );

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
  close( fd );                                     /* close client connectuin*/
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
     if (numRequests > 0) processRCB();
  }

}

void scheduleRCB(int len, FILE *fin, int fd) {
  numRequests++;
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
int queueSize = 0;
int priority = 0;
  fseek(fp, 0L, SEEK_END);
int sz = ftell(fp);
    if (queueSize <= numRequests) {
      priority++;
      RCB req = {
      priority, fd, len, len, 1, sz
      fin
    };
		
		
		queueSize++;
		return 1;
	}
	return 0;			
}

void scheduleRR(int len, FILE* fin, int fd) {

}

// priority 1=8KB, 2=64KB,3=Remainder of the response
void scheduleMLFQ(int len, FILE* fin, int fd) {

    int rcbCount = 0;
    int reqIndex = -1;
    int priorityOneCount = 1; // seq start at 1
    for (size_t i = 0; rcbCount < numRequests; i++) { // loop until you find all rcb in the table
      if (!isRCBEmpty(requestTable[i])) rcbCount++; // inc rcbCount when RCB found
      else if (reqIndex < 0) reqIndex = i; // if an index location is not found && location is empty, set index to add request to empty location
      if (requestTable[i].priority == 1) priorityOneCount++; // count requests with priority 1 to set sequence for RCB
    }
    RCB req = {
      priorityOneCount, fd, len, len, 1,
      fin
    };

    requestTable[reqIndex] = req;
}

void processMLFQ() {
  int rcbCount = 0;
  int indexToProcess = -1;
  int lowestSequenceFound = MAX_THREADS + 1;

  // finds RCB with highest priority and smallest sequence number
  for (size_t priority = 1; priority <= 3 && indexToProcess < 0; priority++) { // search for each priority starting with 1
    for (size_t i = 0; rcbCount < numRequests; i++) { // loop until you find all rcb in the table
      if (!isRCBEmpty(requestTable[i])) rcbCount++; // inc rcbCount when RCB found
      if (requestTable[i].priority == priority && requestTable[i].seq < lowestSequenceFound) { // only track index if lowest sequence
        lowestSequenceFound = requestTable[i].seq;
        indexToProcess = i;
      }
    }
  }

  if (indexToProcess < 0) printf("No RCB find in request Table\n");

  // retrieve request with index and processes request
  if ( requestTable[indexToProcess].priority == 3 ) {
    processWholeRequest();
    numRequests--;
  } else if (requestTable[indexToProcess].priority == 2) {
    if (requestTable[indexToProcess].bytesRemaining <= 64 * KB )  {
      processWholeRequest();
      numRequests--;
    } else {

    }
  } else if (requestTable[indexToProcess].priority == 1) {
    if (requestTable[indexToProcess].bytesRemaining <= 8 * KB )  {
      processWholeRequest();
      numRequests--;
    } else {

    }
  }
}

void processRR() {

}

void processSJF() {

}

int getEmptyIndexInRCBTable() {
  int i=0;
  while (!isRCBEmpty(requestTable[i]) && i < numRequests)
    i++;
  return (i==numRequests) ? -1 : i;
}

int isRCBEmpty(RCB req) {
  return req.seq == 0
      && req.fileDescriptor == 0
      && req.bytesRemaining == 0
      && req.fileSize == 0
      && req.priority == 0
      && req.handle == NULL;
}

void initRequestTable() {
  for (size_t i = 0; i < MAX_THREADS; i++) {
    requestTable[i] = EMPTY_RCB;
  }
}

void processWholeRequest() {

}
