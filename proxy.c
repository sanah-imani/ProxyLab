/*
 * @file: proxy.c
 * @brief: Implementation of a concurrent multi-threaded proxy with a cache. A
 * web proxy is a special type of proxy server whose clients are typically web
 * browsers and whose servers are web servers providing web content. When a web
 * browser uses a proxy, it contacts the proxy instead of communicating directly
 * with the web server. The proxy forwards the client's request to the web
 * server, reads the server's response, then forwards the response to the
 * client.
 *
 * @author: Sanah Imani <simani@unix.andrew.cmu.edu>
 */
#include "csapp.h"

#include <assert.h>
#include <cache.h>
#include <ctype.h>
#include <http_parser.h>
#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>

/*
 * Max cache and object sizes
 * You might want to move these to the file containing your cache implementation
 */
#define MAX_CACHE_SIZE (1024 * 1024)
#define MAX_OBJECT_SIZE (100 * 1024)
#define HOSTLEN 256
#define SERVLEN 8

/* for convenience */
typedef struct sockaddr SA;
/* Information about a connected client. */
typedef struct {
    struct sockaddr_in addr; // Socket address
    socklen_t addrlen;       // Socket address length
    int connfd;              // Client connection file descriptor
    char host[HOSTLEN];      // Client host
    char serv[SERVLEN];      // Client service (port)
} client_info;

/*
 * String to use for the User-Agent header.
 * Don't forget to terminate with \r\n
 */
static const char *header_user_agent = "Mozilla/5.0"
                                       " (X11; Linux x86_64; rv:3.10.0)"
                                       " Gecko/20210731 Firefox/63.0.1";

// freeing the parsing and closing necessary file descriptors.
static void cleanup(int fd, int fd2, parser_t *p) {
    if (fd2 != -1) {
        close(fd2);
    }
    if (fd != -1) {
        close(fd);
    }
    if (p != NULL) {
        parser_free(p);
    }
}

// a formal way to handle errors by writing back to the client (modified from
// tiny.c)
static void clienterror(int fd, const char *errnum, const char *shortmsg,
                        const char *longmsg) {
    char buf[MAXLINE];
    char body[MAXBUF];
    size_t buflen;
    size_t bodylen;

    /* Build the HTTP response body */
    bodylen = snprintf(body, MAXBUF,
                       "<!DOCTYPE html>\r\n"
                       "<html>\r\n"
                       "<head><title>Server Error</title></head>\r\n"
                       "<body bgcolor=\"ffffff\">\r\n"
                       "<h1>%s: %s</h1>\r\n"
                       "<p>%s</p>\r\n"
                       "</body></html>\r\n",
                       errnum, shortmsg, longmsg);
    if (bodylen >= MAXBUF) {
        return; // Overflow!
    }

    /* Build the HTTP response headers */
    buflen = snprintf(buf, MAXLINE,
                      "HTTP/1.0 %s %s\r\n"
                      "Content-Type: text/html\r\n"
                      "Content-Length: %zu\r\n\r\n",
                      errnum, shortmsg, bodylen);
    if (buflen >= MAXLINE) {
        return; // Overflow!
    }

    /* Write the headers */
    if (rio_writen(fd, buf, buflen) < 0) {
        fprintf(stderr, "Error writing error response headers to client\n");
        return;
    }

    /* Write the body */
    if (rio_writen(fd, body, bodylen) < 0) {
        fprintf(stderr, "Error writing error response body to client\n");
        return;
    }
}

/*
 * read_requestline - using the rio package to read the client requestline of
 * the entire request by client. Performs necessary bad request error handling.
 *
 * @params[in] connfd the file descriptor with the client request
 * @params[in] rp a pointer to the rio_t object required for buffered reading of
 * lines.
 * @params[out] pars a pointer to the parser that stores and interprets each
 * line of the HTTP request.
 * @params[out] fBuf a final buffer containing the entire prxy-modified request.
 *
 * @return -1 is error occured.
 */
static int read_requestline(int connfd, rio_t *rp, parser_t *pars,
                            char *buffer) {
    // read the line
    if (rio_readlineb(rp, buffer, MAXLINE) <= 0)
        return -1;
    // parse the line
    parser_state mPs = parser_parse_line(pars, buffer);
    if (mPs != REQUEST) {
        return -1;
    }
    // checking if request is well-formed
    char method[MAXLINE];
    char uri[MAXLINE];
    char version;

    if (sscanf(buffer, "%s %s HTTP/1.%c\r\n", method, uri, &version) != 3 ||
        (version != '0' && version != '1')) {
        return -1;
    }

    // only GET requests implemented
    if (strcmp(method, "GET") != 0) {
        clienterror(connfd, "501", "Not Implemented",
                    "Server couldn't find this file");
        return -1;
    }

    // ensure that there is a resource path
    const char *mPath;
    if (parser_retrieve(pars, PATH, &mPath) != 0) {
        return -1;
    }
    // only doing HTTP 1.0
    int n = snprintf(buffer, MAXLINE, "%s %s HTTP/1.0\r\n", method, mPath);
    return n;
}
/*
 * read_request - using the rio package to read the client request line by line
 * and adding them to a buffer to be written to the server. The details of the
 * modifications and conditioning done is as per the proxylab requirements.
 *
 * @params[in] connfd the file descriptor with the client request
 * @params[in] rp a pointer to the rio_t object required for buffered reading of
 * lines.
 * @params[out] pars a pointer to the parser that stores and interprets each
 * line of the HTTP request.
 * @params[out] fBuf a final buffer containing the entire prxy-modified request.
 *
 * @return true if an error occurred, or false otherwise.
 */
static bool read_request(int connfd, rio_t *rp, parser_t *pars, char *fBuf) {
    char buf[MAXLINE];
    header_t *header;
    int n;
    if ((n = read_requestline(connfd, rp, pars, fBuf)) <= 0) {
        return true;
    }
    while (true) {
        if ((n = rio_readlineb(rp, buf, sizeof(buf))) <= 0) {
            return true;
        }

        /* Check for end of request headers */
        if (strcmp(buf, "\r\n") == 0) {
            // adding the four required headers
            if ((header = parser_lookup_header(pars, "Host")) == NULL) {
                const char *mHost;
                const char *mPort;
                if (parser_retrieve(pars, HOST, &mHost) != 0) {
                    return true;
                }

                if (parser_retrieve(pars, PORT, &mPort) != 0) {
                    return true;
                }
                sprintf(buf, "Host: %s:%s\r\n", mHost, mPort);
                fBuf = strncat(fBuf, buf, MAXLINE);
            }
            sprintf(buf, "User-Agent: %s\r\n", header_user_agent);
            fBuf = strncat(fBuf, buf, MAXLINE);
            fBuf = strncat(fBuf, "Connection: close\r\n", MAXLINE);
            fBuf = strncat(fBuf, "Proxy-Connection: close\r\n", MAXLINE);
            fBuf = strncat(fBuf, "\r\n", MAXLINE);
            return false;
        }
        parser_state ps = parser_parse_line(pars, buf);

        // the request line has already be read. Only headers to be considered
        // now.
        if (ps == ERROR || ps == REQUEST) {
            return true;
        }
        if (ps == HEADER) {
            if ((header = parser_retrieve_next_header(pars)) != NULL) {
                const char *name = header->name;
                // forwarding the other headers as is.
                if (strcmp(name, "Proxy-Connection") != 0 &&
                    strcmp(name, "Connection") != 0 &&
                    strcmp(name, "User-Agent") != 0) {
                    fBuf = strncat(fBuf, buf, n);
                }
            }
        }
    }
}

void *threadRoutine(void *args) {
    // detach each new thread, so that spare resources are automatically reaped
    // upon thread exit.
    pthread_detach(pthread_self());
    // dereference
    int connfd = *((int *)args);
    free(args);

    int clientfd;
    char buffer[MAXBUF];
    char cacheBuf[MAX_OBJECT_SIZE];
    rio_t rio;

    // reading client request
    rio_readinitb(&rio, connfd);
    // http_parser creation
    parser_t *mParser = parser_new();

    // parse through the whole request
    if (read_request(connfd, &rio, mParser, buffer)) {
        clienterror(connfd, "400", "Bad Request",
                    "Received a malformed request");
        cleanup(connfd, -1, mParser);
        return NULL;
    }

    // host and port to open_clientfd and URI for the cache key
    const char *mHost;
    const char *mPort;
    const char *mURI;

    if (parser_retrieve(mParser, PORT, &mPort) != 0 ||
        parser_retrieve(mParser, HOST, &mHost) != 0 ||
        parser_retrieve(mParser, URI, &mURI) != 0) {
        clienterror(connfd, "400", "Bad Request",
                    "Received a malformed request");
        cleanup(connfd, -1, mParser);
        return NULL;
    }

    // if possible we need to try and serve the client by looking through the
    // cache and seeing if the request has already been cached.
    if (serve_cache(connfd, mURI)) {
        cleanup(connfd, -1, mParser);
        return NULL;
    }

    // otherwise, open connection to server
    clientfd = open_clientfd(mHost, mPort);
    if (clientfd < 0) {
        perror("connect");
        cleanup(connfd, clientfd, mParser);
        return NULL;
    }

    // writing to server with modified client request
    if (rio_writen(clientfd, buffer, MAXBUF) < 0) {
        clienterror(connfd, "500", "Server Error", "Cannot write to server");
        cleanup(connfd, clientfd, mParser);
        return NULL;
    }

    rio_t rio2;
    rio_readinitb(&rio2, clientfd);
    int bytesR = 0;
    int totalBytesR = 0;
    bool is_cacheable = true;
    int iter = 0;

    // server responsed and bytes need to be sent to the client via connfd.
    while ((bytesR = rio_readnb(&rio2, cacheBuf, MAX_OBJECT_SIZE)) > 0) {
        if (rio_writen(connfd, cacheBuf, bytesR) <= 0) {
            fprintf(stderr, "Could not write response to client");
        }
        totalBytesR += bytesR;
        // if too big we won't cache
        if (totalBytesR > MAX_OBJECT_SIZE) {
            is_cacheable = false;
        }
        iter += 1;
    }

    // if object is cacheable then add to cache.
    if (is_cacheable) {
        if (add_to_cache(mURI, cacheBuf, totalBytesR)) {
            fprintf(stderr, "Could not cache web object\n");
        }
    }

    // error handling (no response)
    if (iter == 0 && bytesR <= 0) {
        fprintf(stderr, "Could not read response from server\n");
    }

    // need to cleanup resources
    cleanup(connfd, clientfd, mParser);
    return NULL;
}
int main(int argc, char **argv) {
    int listenfd;
    // ignoring sigpipe signals
    signal(SIGPIPE, SIG_IGN);
    pthread_t mProxyT;
    /* Check command line args */
    if (argc != 2) {
        fprintf(stderr, "usage: %s <port>\n", argv[0]);
        exit(1);
    }
    // initialising the proxy cache
    init_web_cache();
    // initialsing the lock for the proxy.
    init_cache_lock();
    // listening to incoming requests from client
    listenfd = open_listenfd(argv[1]);
    // make sure its a valid file descriptor
    if (listenfd < 0) {
        fprintf(stderr, "Failed to listen on port: %s\n", argv[1]);
        exit(1);
    }
    // continously running and attending client requests
    while (true) {
        client_info client_data;
        client_info *client = &client_data;
        client->addrlen = sizeof(client->addr);
        // accepting request and getting the connection file descriptor
        int connfd = accept(listenfd, (SA *)&client->addr, &client->addrlen);
        client->connfd = connfd;
        if (client->connfd < 0) {
            perror("accept");
            close(client->connfd);
            continue;
        }
        // get extra info
        getnameinfo((SA *)&client->addr, client->addrlen, client->host, HOSTLEN,
                    client->serv, SERVLEN, 0);

        int *connfdP = malloc(sizeof(int));
        *connfdP = client->connfd;
        // spawn a pthread to handle every request till its termination.
        pthread_create(&mProxyT, NULL, threadRoutine, (void *)connfdP);
    }
    return 0;
}
