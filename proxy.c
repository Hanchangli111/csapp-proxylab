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

#define BUFSIZE         1024

#define START_INFO      {printf("\033[36m"); fflush(stdout);}
#define START_SUCCESS   {printf("\033[32m"); fflush(stdout);}
#define START_WARNING   {printf("\033[33m"); fflush(stdout);}
#define START_ERROR     {printf("\033[31m"); fflush(stdout);}
#define START_QUOTE     {printf("\033[35m"); fflush(stdout);}
#define END_MESSAGE     {printf("\033[0m"); fflush(stdout);}

/*
 * Function prototypes
 */
void handleClientRequest(int connFD, struct sockaddr_in *clientAddr, socklen_t clientAddr_len);
int parse_uri(char *uri, char *target_addr, char *path, int *port);
void format_log_entry(char *logstring, struct sockaddr_in *sockaddr, char *uri, int size);
int readAll(int fd, void *buf, size_t count);
int readUntil(int fd, void *buf, size_t count, char *pattern);
int writeAll(int fd, void *buf, size_t count);

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
        perror("socket");
        exit(EXIT_FAILURE);
    }

    /* option for listen socket */
    optval = 1;
    if (setsockopt(listenFD, SOL_SOCKET, SO_REUSEADDR,
                (const void*) &optval, sizeof(optval)) == -1)
    {
        perror("setsockopt");
        exit(EXIT_FAILURE);
    }

    /* prepare address */
    memset(&listenAddr, 0, sizeof(listenAddr));
    listenAddr.sin_family = AF_INET;
    listenAddr.sin_addr.s_addr = htonl(INADDR_ANY);
    listenAddr.sin_port = htons(listenPort);

    /* bind */
    if (bind(listenFD, (const struct sockaddr*) &listenAddr, sizeof(listenAddr)) == -1)
    {
        perror("bind");
        exit(EXIT_FAILURE);
    }

    /* listen */
    if (listen(listenFD, LISTENQ) == -1)
    {
        perror("listen");
        exit(EXIT_FAILURE);
    }
    START_INFO;
    printf("Listening on %s:%u\n", inet_ntoa(listenAddr.sin_addr), ntohs(listenAddr.sin_port));
    END_MESSAGE;

    for(;;)
    {
        int connFD;
        struct sockaddr_in clientAddr;
        socklen_t clientAddr_len;

        /* accept */
        clientAddr_len = sizeof(clientAddr);
        connFD = accept(listenFD, (struct sockaddr*) &clientAddr, &clientAddr_len);
        START_SUCCESS;
        printf("Connection from %s:%u\n", inet_ntoa(clientAddr.sin_addr), ntohs(clientAddr.sin_port));
        END_MESSAGE;

        /* handle */
        handleClientRequest(connFD, &clientAddr, clientAddr_len);

        /* close */
        close(connFD);
        START_SUCCESS;
        printf("Connection closed\n");
        END_MESSAGE;
    }

    return 0;
}

void handleClientRequest(int connFD, struct sockaddr_in *clientAddr, socklen_t clientAddr_len)
{
    char buf[BUFSIZE];
    int readResult;
    char *http, *request_host, *request_path;
    int request_port;
    struct addrinfo *serverAddrInfo;
    int getaddrinfoResult;

    /* read HTTP header */
    if ((readResult = readUntil(connFD, buf, sizeof(buf), "\r\n\r\n")) == -1)
    {
        START_ERROR;
        printf("Buffer full\n");
        END_MESSAGE;
        return;
    }

    /* forward to stdout */
    START_QUOTE;
    writeAll(STDOUT_FILENO, buf, readResult);
    putchar('\n');
    END_MESSAGE;

    /* analyze the request */
    request_host = malloc(MAXLINE);
    request_path = malloc(MAXLINE);
    http = strstr(buf, "http://");

    if (http == NULL || parse_uri(http, request_host, request_path, &request_port) == -1)
    {
        START_ERROR;
        printf("Parse error\n");
        END_MESSAGE;
        return;
    }
    START_INFO;
    printf("host:\t%s\npath:\t%s\nport:\t%d\n", request_host, request_path, request_port);
    END_MESSAGE;

    /* dns lookup */
    if ((getaddrinfoResult = getaddrinfo(request_host, NULL, NULL, &serverAddrInfo)) != 0)
    {
        START_ERROR;
        printf("DNS lookup failure: %s\n", gai_strerror(getaddrinfoResult));
        END_MESSAGE;
    }
    else
    {
        START_INFO;
        printf("resolv:\t%s\n", inet_ntoa(((struct sockaddr_in*)serverAddrInfo->ai_addr)->sin_addr));
        END_MESSAGE;
        /* finalize */
        freeaddrinfo(serverAddrInfo);
    }
}

/*
 * parse_uri - URI parser
 *
 * Given a URI from an HTTP proxy GET request (i.e., a URL), extract
 * the host name, path name, and port.  The memory for hostname and
 * pathname must already be allocated and should be at least MAXLINE
 * bytes. Return -1 if there are any problems.
 */
int parse_uri(char *uri, char *hostname, char *pathname, int *port)
{
    char *hostbegin;
    char *hostend;
    char *pathbegin;
    char *pathend;
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

    /* Extract the path */
    pathbegin = strchr(hostbegin, '/');
    if (pathbegin == NULL)
    {
        pathname[0] = '\0';
    }
    else
    {
        pathbegin++;
        pathend = strpbrk(pathbegin, " \r\n\0");
        strncpy(pathname, pathbegin, pathend - pathbegin);
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

int readAll(int fd, void *buf, size_t count)
{
    void *cursor;
    void *endOfBuffer;
    int readResult;
    cursor = buf;
    endOfBuffer = buf + count;

    while (cursor < endOfBuffer)
    {
        if ((readResult = read(fd, cursor, (endOfBuffer - cursor))) == -1)
        {
            perror("read");
            exit(EXIT_FAILURE);
        }
        if (readResult == 0)
        {
            return (cursor - buf);
        }
        cursor += readResult;
    }

    return -1;
}

int readUntil(int fd, void *buf, size_t count, char *pattern)
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
            perror("read");
            exit(EXIT_FAILURE);
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
            *substr = '\0';
            return (substr - (char*)buf);
        }
    }

    return -1;
}

int writeAll(int fd, void *buf, size_t count)
{
    char *cursor;
    char *endOfData;
    int writeResult;
    cursor = buf;
    endOfData = buf + count;

    while (cursor < endOfData)
    {
        if ((writeResult = write(fd, buf, endOfData - cursor)) == -1)
        {
            perror("write");
            exit(EXIT_FAILURE);
        }
        cursor += writeResult;
    }

    return count;
}
