/*
 * proxy.c - CS:APP Web proxy
 *
 * Student Information:
 *     JangHo Seo<jangho.se@snu.ac.kr>, 2014-18790
 *
 * How this concurrent proxy works:
 *  - function main
 *      - parse port number from CLI input
 *      - listen from INADDR_ANY:portnumber
 *      - for each accepted connection (i.e. browser connection), create a thread with handleClientRequest
 *  - function handleClientRequest
 *      - read browser request until blank line encountered, for reading HTTP header
 *      - from request header extract end server host and port number
 *      - translate server host to ip address by calling getaddrinfo(3)
 *      - connect to end server and forward the HTTP header which was previously saved
 *      - pump server response to browser by repeatedly calling read(2) and write(2)
 *      - close the server socket
 *      - generate a log entry
 *      - close connection socket
 *      - free job payload and return
 */

#include "csapp.h"

/* basic configuration */
#define BUFSIZE         (1024*1024)
#define LOGFILENAME     ("proxy.log")

/* for pretty terminal output */
#define START_INFO      do {printf("\033[36m"); fflush(stdout);} while (0)
#define START_SUCCESS   do {printf("\033[32m"); fflush(stdout);} while (0)
#define START_NOTICE    do {printf("\033[33m"); fflush(stdout);} while (0)
#define START_ERROR     do {printf("\033[31m"); fflush(stdout);} while (0)
#define START_QUOTE     do {printf("\033[35m"); fflush(stdout);} while (0)
#define END_MESSAGE     do {printf("\033[0m"); fflush(stdout);} while (0)


/* typedefs */
typedef struct handlerJob
{
    int clientFD;
    struct sockaddr_in clientAddr;
}
handlerJob_t;


/*
 * Function prototypes
 */
void *handleClientRequest(void *job);
int parse_uri(char *uri, char *target_addr, in_port_t *port);
void format_log_entry(char *logstring, struct sockaddr_in *sockaddr, char *uri, int size);
int readAll(int fd, void *buf, const size_t count);
int readUntil(int fd, void *buf, const size_t count, const char *pattern);
int writeAll(int fd, const void *buf, const size_t count);
int pump(int from, int to);
void fatal(char *message);
void error(char *message);


/* global variables */
FILE *logFile;
sem_t logSem;


/*
 * main - Main routine for the proxy program
 */
int main(int argc, char **argv)
{
    uint16_t listenPort;
    int listenFD;
    struct sockaddr_in listenAddr;
    int optval;

    /* Check arguments */
    if (argc != 2)
    {
        fprintf(stderr, "Usage: %s <port number>\n", argv[0]);
        exit(EXIT_FAILURE);
    }
    listenPort = atoi(argv[1]);

    /* ignore SIGPIPE */
    signal(SIGPIPE, SIG_IGN);

    /* open the log file */
    if ((logFile = fopen(LOGFILENAME, "a")) == NULL)
    {
        fatal("fopen");
    }

    /* semaphore */
    if (sem_init(&logSem, 0, 1)  == -1)
    {
        fatal("sem_init");
    }

    /* initalize listen socket */
    if ((listenFD = socket(AF_INET, SOCK_STREAM, 0)) == -1)
    {
        fatal("socket");
    }

    /* option for listen socket */
    optval = 1;
    if (setsockopt(listenFD, SOL_SOCKET, SO_REUSEADDR,
                (const void*) &optval, sizeof(optval)) == -1)
    {
        fatal("setsockopt");
    }

    /* prepare address */
    memset(&listenAddr, 0, sizeof(listenAddr));
    listenAddr.sin_family = AF_INET;
    listenAddr.sin_addr.s_addr = htonl(INADDR_ANY);
    listenAddr.sin_port = htons(listenPort);

    /* bind */
    if (bind(listenFD, (const struct sockaddr*) &listenAddr, sizeof(listenAddr)) == -1)
    {
        fatal("bind");
    }

    /* listen */
    if (listen(listenFD, LISTENQ) == -1)
    {
        fatal("listen");
    }

    for(;;)
    {
        handlerJob_t *job;
        socklen_t clientAddr_len;
        pthread_t dummy;
        pthread_attr_t threadAttr;

        /* allocate */
        job = malloc(sizeof(handlerJob_t));

        /* accept */
        clientAddr_len = sizeof(struct sockaddr);
        job->clientFD = accept(listenFD, (struct sockaddr*) &(job->clientAddr), &clientAddr_len);

        /* thread attribute - detached */
        pthread_attr_init(&threadAttr);
        pthread_attr_setdetachstate(&threadAttr, PTHREAD_CREATE_DETACHED);

        /* create thread */
        pthread_create(&dummy, &threadAttr, handleClientRequest, job);
    }

    return 0;
}

/* handleClientRequest

DESCRIPTION
Assumes HTTP header stream from clientFD and acts like a middleman between client and end-server.
It parses end-server information from clientFD stream and connect accordingly, and forward the response to the client.
For this function, socket to client and file pointer to log file should be available.

ARGUMENTS
handlerJob_t *job
    int clientFD
        connection file descriptor from accept(2) system call
    struct sockaddr_in *clientAddr
        sockaddr descriptor from accept(2) system call

LIMITATIONS
This function will fail if HTTP header is biggern than BUFSIZE.

SIDE EFFECTS
This function can cause termination of the entire program in some cases of system call failure.
This function logs real-time status to STDOUT, and writes a log entry for each request to LOGFILENAME.
This function calls subroutines with side effects, such as readAll, readUntil and writeAll.
*/

void *handleClientRequest(void *job)
{
    /* argument */
    int clientFD = ((handlerJob_t*)job)->clientFD;
    struct sockaddr_in* clientAddr = &(((handlerJob_t*)job)->clientAddr);

    /* for saving and parsing HTTP request from client */
    const char *headerDelimiter = "\r\n\r\n";
    char clientRequestHeader[BUFSIZE];
    char *http, *request_host;
    in_port_t request_port;

    /* server connection information */
    struct addrinfo *serverAddrInfo;
    struct sockaddr_in serverAddr;
    int serverFD;

    /* misc. */
    int readResult;
    int getaddrinfoResult;
    char logEntry[MAXLINE];
    int responseSize;

    do
    {
        /* read HTTP header */
        readResult = readUntil(clientFD, clientRequestHeader, sizeof(clientRequestHeader), headerDelimiter);
        if (readResult == -1)
        {
            break;
        }
        if (readResult == -2)
        {
            START_ERROR;
            printf("Buffer for clientRequestHeader is full\n");
            END_MESSAGE;
            break;
        }

        /* analyze the request */
        request_host = malloc(MAXLINE);
        http = strstr(clientRequestHeader, "http://");

        if (http == NULL || parse_uri(http, request_host, &request_port) == -1)
        {
            break;
        }

        /* DNS lookup & get serverAddr */
        if ((getaddrinfoResult = getaddrinfo(request_host, NULL, NULL, &serverAddrInfo)) != 0)
        {
            free(request_host);
            START_ERROR;
            printf("DNS lookup failure\n");
            END_MESSAGE;
            break;
        }
        else
        {
            memcpy(&serverAddr, serverAddrInfo->ai_addr, sizeof(struct sockaddr));
            freeaddrinfo(serverAddrInfo);
        }
        free(request_host);

        /* prepare serverAddr */
        serverAddr.sin_port = htons(request_port);

        /* connect to end server */
        if ((serverFD = socket(serverAddr.sin_family, SOCK_STREAM, 0)) == -1)
        {
            error("socket");
            break;
        }

        if (connect(serverFD, (struct sockaddr*)(&serverAddr), sizeof(serverAddr)) == -1)
        {
            error("connect");
            close(serverFD);
            break;
        }

        /* forward request */
        if (writeAll(serverFD, clientRequestHeader, readResult) == -1)
        {
            close(serverFD);
            break;
        }
        if (writeAll(serverFD, headerDelimiter, sizeof(headerDelimiter)) == -1)
        {
            close(serverFD);
            break;
        }

        /* forward response */
        responseSize = pump(serverFD, clientFD);
        if (responseSize == -1)
        {
            close(serverFD);
            break;
        }

        /* close serverFD */
        close(serverFD);

        /* make log */
        *strstr(http, " ") = '\0';
        format_log_entry(logEntry, clientAddr, http, responseSize);
        if (sem_wait(&logSem) == -1)
        {
            fatal("sem_wait");
        }
        fprintf(logFile, "%s\n", logEntry);
        fflush(logFile);
        if (sem_post(&logSem) == -1)
        {
            fatal("sem_post");
        }
    }
    while (0);

    /* finally */
    free(job);
    close(clientFD);
    return NULL;
}

/*
 * parse_uri - URI parser
 *
 * Given a URI from an HTTP proxy GET request (i.e., a URL), extract
 * the host name,  and port.  The memory for hostname
 * must already be allocated and should be at least MAXLINE
 * bytes. Return -1 if there are any problems.
 */

int parse_uri(char *uri, char *hostname, in_port_t *port)
{
    char *hostbegin;
    char *hostend;
    int len;

    if (strncasecmp(uri, "http://", 7) != 0)
    {
        hostname[0] = '\0';
        return -1;
    }

    /* Extract the host name */
    hostbegin = uri + 7;
    hostend = strpbrk(hostbegin, " :/\r\n\0");
    len = hostend - hostbegin;
    strncpy(hostname, hostbegin, len);
    hostname[len] = '\0';

    /* Extract the port number */
    *port = 80; /* default */
    if (*hostend == ':')
    {
        *port = atoi(hostend + 1);
    }

    return 0;
}

/*
 * format_log_entry - Create a formatted log entry in logstring.
 *
 * The inputs are the socket address of the requesting client
 * (sockaddr), the URI from the request (uri), and the size in bytes
 * of the response from the server (size).
 */

void format_log_entry(char *logstring, struct sockaddr_in *sockaddr, char *uri, int size)
{
    time_t now;
    char time_str[MAXLINE];
    unsigned long host;
    unsigned char a, b, c, d;

    /* Get a formatted time string */
    now = time(NULL);
    strftime(time_str, MAXLINE, "%a %d %b %Y %H:%M:%S %Z", localtime(&now));

    /*
     * Convert the IP address in network byte order to dotted decimal
     * form. Note that we could have used inet_ntoa, but chose not to
     * because inet_ntoa is a Class 3 thread unsafe function that
     * returns a pointer to a static variable (Ch 13, CS:APP).
     */
    host = ntohl(sockaddr->sin_addr.s_addr);
    a = host >> 24;
    b = (host >> 16) & 0xff;
    c = (host >> 8) & 0xff;
    d = host & 0xff;

    /* Return the formatted log entry string */
    sprintf(logstring, "%s: %d.%d.%d.%d %s %d", time_str, a, b, c, d, uri, size);
}

/* pump

DESCRIPTION
Transfer all data available from 'from' file descriptor to 'to' file descriptor.

RETURN VALUE
On success, the number of bytes transfered is returned.
-1 is returned when primitive library call failure occured.
*/

int pump(int from, int to)
{
    char buf[BUFSIZE];
    int readResult;
    int total;

    total = 0;
    while ((readResult = readAll(from, buf, sizeof(buf))) == -2)
    {
        if (writeAll(to, buf, sizeof(buf)) == -1)
        {
            return -1;
        }
        total += readResult;
    }
    if (readResult == -1)
    {
        return -1;
    }
    if (writeAll(to, buf, readResult) == -1)
    {
        return -1;
    }
    total += readResult;
    return total;
}

/* readAll

DESCRIPTION
Tries to read all the data currently available from file descriptor fd,
but truncates data to fit in size of count.
Remaining data can be readed by calling read(2) or readAll again.

ARGUMENTS
int fd
    File descriptor from which target data is available.
void *buf
    Buffer to save data
const size_t count
    Maximum size of data, possible the size of buffer.

RETURN VALUE
On success without truncation, the number of bytes readed is returned.
-1 is returned when read(2) failure occured.
-2 is returned when truncation is occured.
*/

int readAll(int fd, void *buf, const size_t count)
{
    char *cursor;
    int readResult;
    char * const endOfBuffer = buf + count;
    cursor = buf;

    *cursor = '\0';
    while (cursor < endOfBuffer)
    {
        if ((readResult = read(fd, cursor, (endOfBuffer - cursor))) == -1)
        {
            error("read");
            return -1;
        }
        if (readResult == 0)
        {
            return (cursor - (char*)buf);
        }
        cursor += readResult;
    }

    return -2;
}

/* readUntil

DESCRIPTION
Tries to read the data currently available from file descriptor fd,
stops when pattern is found in data.

ARGUMENTS
int fd
    File descriptor from which target data is available.
void *buf
    Buffer to save data
const size_t count
    Maximum size of data, possible the size of buffer.
const char *pattern
    Character pattern to determine stop condition of reading.

RETURN VALUE
On success without truncation, the number of bytes readed is returned.
-1 is returned when read(2) failure occured.
-2 is returned when truncation is occured.
*/

int readUntil(int fd, void *buf, const size_t count, const char *pattern)
{
    char *cursor;
    char *endOfBuffer;
    int readResult;
    char *substr;
    cursor = buf;
    endOfBuffer = buf + count;

    *cursor = '\0';
    while (cursor < endOfBuffer)
    {
        if ((readResult = read(fd, cursor, (endOfBuffer - cursor))) == -1)
        {
            error("read");
            return -1;
        }
        if (readResult == 0)
        {
            return (cursor - (char*)buf);
        }
        cursor += readResult;
        if (cursor >= endOfBuffer)
        {
            return -2;
        }
        *cursor = '\0';
        if ((substr = strstr(buf, pattern)) != NULL)
        {
            return (cursor - (char*)buf);
        }
    }

    return -2;
}

/* writeAll

DESCRIPTION
Tries to write all the data currently available to file descriptor fd.

ARGUMENTS
int fd
    This function writes to fd.
void *buf
    Character buffer from which target data is available.
const size_t count
    Size of data.

RETURN VALUE
On success, 0 is returned.
-1 is returned when write(2) failure occured.
*/

int writeAll(int fd, const void *buf, const size_t count)
{
    char *cursor;
    char *endOfData;
    int writeResult;
    cursor = (char*)buf;
    endOfData = (char*)buf + count;

    while (cursor < endOfData)
    {
        if ((writeResult = write(fd, buf, endOfData - cursor)) == -1)
        {
            error("write");
            return -1;
        }
        cursor += writeResult;
    }

    return 0;
}

/* fatal

DESCRIPTION
Print error message to stdout and terminate the program, based on errno.

ARGUMENTS
char *message
    Printed error message is form of 'message: ERROR_STR'

SIDE EFFECTS
This function prints string to STDOUT.
This function can causes termination of the entire program.
*/

void fatal(char *message)
{
    error(message);
    exit(EXIT_FAILURE);
}

/* error

DESCRIPTION
Print error message to stdout, based on errno.

ARGUMENTS
char *message
    Printed error message is form of 'message: ERROR_STR'

SIDE EFFECTS
This function prints string to STDOUT.
*/

void error(char *message)
{
    START_ERROR;
    perror(message);
    END_MESSAGE;
}
