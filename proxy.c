/*
 * proxy.c - CS:APP Web proxy
 *
 * Student Information:
 *     JangHo Seo<jangho.se@snu.ac.kr>, 2014-18790
 *
 * IMPORTANT: Give a high level description of your code here. You
 * must also provide a header comment at the beginning of each
 * function that describes what that function does.
 */

#include "csapp.h"

#define BUFSIZE         1024*1024

#define START_INFO      {printf("\033[36m"); fflush(stdout);}
#define START_SUCCESS   {printf("\033[32m"); fflush(stdout);}
#define START_NOTICE    {printf("\033[33m"); fflush(stdout);}
#define START_ERROR     {printf("\033[31m"); fflush(stdout);}
#define START_QUOTE     {printf("\033[35m"); fflush(stdout);}
#define END_MESSAGE     {printf("\033[0m"); fflush(stdout);}

/*
 * Function prototypes
 */
void handleClientRequest(int clientFD);
int parse_uri(char *uri, char *target_addr, in_port_t *port);
void format_log_entry(char *logstring, struct sockaddr_in *sockaddr, char *uri, int size);
int readAll(int fd, void *buf, const size_t count);
int readUntil(int fd, void *buf, const size_t count, const char *pattern);
int writeAll(int fd, const void *buf, const size_t count);
int pump(int from, int to);
void fatal(char *message);

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
        int clientFD;
        struct sockaddr_in clientAddr;
        socklen_t clientAddr_len;

        /* waiting message */
        START_INFO;
        printf("Listening on %s:%u\n", inet_ntoa(listenAddr.sin_addr), ntohs(listenAddr.sin_port));
        END_MESSAGE;

        /* accept */
        clientAddr_len = sizeof(clientAddr);
        clientFD = accept(listenFD, (struct sockaddr*) &clientAddr, &clientAddr_len);
        START_SUCCESS;
        printf("Client connection from %s:%u\n", inet_ntoa(clientAddr.sin_addr), ntohs(clientAddr.sin_port));
        END_MESSAGE;

        /* handle */
        handleClientRequest(clientFD);

        /* close */
        close(clientFD);
        START_SUCCESS;
        printf("Closed connection to client\n");
        END_MESSAGE;
    }

    return 0;
}

void handleClientRequest(int clientFD)
{
    const char *headerDelimiter = "\r\n\r\n";
    char buf[BUFSIZE];
    int readResult;
    char *http, *request_host;
    in_port_t request_port;
    struct addrinfo *serverAddrInfo;
    int getaddrinfoResult;
    struct sockaddr_in serverAddr;
    int serverFD;

    /* read HTTP header */
    if ((readResult = readUntil(clientFD, buf, sizeof(buf), headerDelimiter)) == -1)
    {
        START_ERROR;
        printf("Buffer full\n");
        END_MESSAGE;
        return;
    }

    START_NOTICE;
    printf("Client request:\n");
    END_MESSAGE;
    START_QUOTE;
    writeAll(STDOUT_FILENO, buf, strstr(buf, headerDelimiter) - buf);
    putchar('\n');
    END_MESSAGE;

    /* analyze the request */
    request_host = malloc(MAXLINE);
    http = strstr(buf, "http://");

    if (http == NULL || parse_uri(http, request_host, &request_port) == -1)
    {
        START_ERROR;
        printf("Cannot parse client request\n");
        END_MESSAGE;
        return;
    }
    START_INFO;
    printf("host:\t%s\nport:\t%d\n", request_host, request_port);
    END_MESSAGE;

    /* dns lookup */
    if ((getaddrinfoResult = getaddrinfo(request_host, NULL, NULL, &serverAddrInfo)) != 0)
    {
        START_ERROR;
        printf("DNS lookup failure: %s\n", gai_strerror(getaddrinfoResult));
        END_MESSAGE;
        return;
    }
    else
    {
        memcpy(&serverAddr, serverAddrInfo->ai_addr, sizeof(struct sockaddr));
        freeaddrinfo(serverAddrInfo);
        START_INFO;
        printf("resolv:\t%s\n", inet_ntoa(serverAddr.sin_addr));
        END_MESSAGE;
    }

    /* prepare serverAddr */
    serverAddr.sin_port = htons(request_port);

    /* connect to end server */
    if ((serverFD = socket(serverAddr.sin_family, SOCK_STREAM, 0)) == -1)
    {
        fatal("socket");
    }

    START_SUCCESS;
    printf("Connected to %s\n", inet_ntoa(serverAddr.sin_addr));
    END_MESSAGE;

    if (connect(serverFD, (struct sockaddr*)(&serverAddr), sizeof(serverAddr)) == -1)
    {
        START_ERROR;
        perror("connect");
        END_MESSAGE;
        return;
    }

    /* forward request */
    writeAll(serverFD, buf, readResult);
    writeAll(serverFD, headerDelimiter, sizeof(headerDelimiter));
    START_SUCCESS;
    printf("Forwarded request to server\n");
    END_MESSAGE;

    /* forward response */
    pump(serverFD, clientFD);
    START_SUCCESS;
    printf("Forwarded response to client\n");
    END_MESSAGE;

    close(serverFD);
    START_SUCCESS;
    printf("Closed connection to server\n");
    END_MESSAGE;
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
    sprintf(logstring, "%s: %d.%d.%d.%d %s", time_str, a, b, c, d, uri);
}

int pump(int from, int to)
{
    char buf[BUFSIZE];
    int readResult;
    int total;

    total = 0;
    while ((readResult = readAll(from, buf, sizeof(buf))) == -1)
    {
        writeAll(to, buf, sizeof(buf));
        total += readResult;
    }
    writeAll(to, buf, readResult);
    total += readResult;
    return total;
}

int readAll(int fd, void *buf, const size_t count)
{
    void *cursor;
    int readResult;
    void * const endOfBuffer = buf + count;
    cursor = buf;

    while (cursor < endOfBuffer)
    {
        if ((readResult = read(fd, cursor, (endOfBuffer - cursor))) == -1)
        {
            fatal("read");
        }
        if (readResult == 0)
        {
            return (cursor - buf);
        }
        cursor += readResult;
    }

    return -1;
}

int readUntil(int fd, void *buf, const size_t count, const char *pattern)
{
    char *cursor;
    char *endOfBuffer;
    int readResult;
    char *substr;
    cursor = buf;
    endOfBuffer = buf + count;

    while (cursor < endOfBuffer)
    {
        if ((readResult = read(fd, cursor, (endOfBuffer - cursor))) == -1)
        {
            fatal("read");
        }
        if (readResult == 0)
        {
            return (cursor - (char*)buf);
        }
        cursor += readResult;
        if (cursor >= endOfBuffer)
        {
            return -1;
        }
        *cursor = '\0';
        if ((substr = strstr(buf, pattern)) != NULL)
        {
            return (cursor - (char*)buf);
        }
    }

    return -1;
}

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
            fatal("write");
        }
        cursor += writeResult;
    }

    return count;
}

void fatal(char *message)
{
    START_ERROR;
    perror(message);
    END_MESSAGE;
    exit(EXIT_FAILURE);
}
