/*
 * proxy.c - A Basic Web proxy
 *
 * 
 * IMPORTANT: Give a high level description of your code here. You
 * must also provide a header comment at the beginning of each
 * function that describes what that function does.
 */ 

#include "csapp.h"

/* The name of the proxy's log file */
#define PROXY_LOG "proxy.log"

/* Undefine this if you don't want debugging output */
#define DEBUG

/*
 * Globals
 */ 
FILE *log_file; /* Log file with one line per HTTP request */
pthread_mutex_t loglock;
pthread_mutex_t clientfdlock;

/*
 * Functions not provided to the students
 */
int open_clientfd(char *hostname, int port); 
ssize_t Rio_readn_w(int fd, void *ptr, size_t nbytes);
ssize_t Rio_readlineb_w(rio_t *rp, void *usrbuf, size_t maxlen); 
void Rio_writen_w(int fd, void *usrbuf, size_t n);

/*
 * Function prototypes
 */
int parse_uri(char *uri, char *target_addr, char *path, int  *port);
void format_log_entry(char *logstring, struct sockaddr_in *sockaddr, char *uri, int size);
void* thread(void* arg);

/* 
 * Structures
 */
struct thread_data{
  int thread_id;
  int connfd;
  struct sockaddr_in clientaddr;
};

/* 
 * main - Main routine for the proxy program 
 */
int main(int argc, char **argv)
{
    int listenfd;             /* The proxy's listening descriptor */
    int port;                 /* The port the proxy is listening on */
    int clientlen;            /* Size in bytes of the client socket address */
    struct sockaddr_in clientaddr;  /* Client address structure*/  

    /* Check arguments */
    if (argc != 2) {
	fprintf(stderr, "Usage: %s <port number>\n", argv[0]);
	exit(0);
    }

    /* 
     * Ignore any SIGPIPE signals elicited by writing to a connection
     * that has already been closed by the peer process.
     */
    signal(SIGPIPE, SIG_IGN);

    /* Create a listening descriptor */
    port = atoi(argv[1]);
    listenfd = Open_listenfd(port);

    /* Inititialize */
    log_file = Fopen(PROXY_LOG, "a");

    /* Initialize mutex */
    pthread_mutex_init(&loglock, NULL);
    pthread_mutex_init(&clientfdlock, NULL);
 
    int thread_count = 0; // number of threads 
  
    /* Wait for and process client connections */
    /* Create thread */
    while (1) { 
      pthread_t tid;
      struct thread_data *td;
      
      /* Allocate memory to struct */
      td = malloc(sizeof(struct thread_data));
      
      /* accept */ 
      clientlen = sizeof(clientaddr);  
      td->connfd = Accept(listenfd, (SA *)&clientaddr, &clientlen);
      
      /* thread id */
      td->thread_id = thread_count++;
      td->clientaddr = clientaddr;
      
      pthread_create(&tid, NULL, thread, td);
    }
    
    fflush(log_file);
    fclose(log_file);
      
    /* Control never reaches here */
    exit(0);
}


/*
 * Rio_readn_w - A wrapper function for rio_readn (csapp.c) that
 * prints a warning message when a read fails instead of terminating
 * the process.
 */
ssize_t Rio_readn_w(int fd, void *ptr, size_t nbytes) 
{
    ssize_t n;
  
    if ((n = rio_readn(fd, ptr, nbytes)) < 0) {
	printf("Warning: rio_readn failed\n");
	return 0;
    }    
    return n;
}

/*
 * Rio_readlineb_w - A wrapper for rio_readlineb (csapp.c) that
 * prints a warning when a read fails instead of terminating 
 * the process.
 */
ssize_t Rio_readlineb_w(rio_t *rp, void *usrbuf, size_t maxlen) 
{
    ssize_t rc;

    if ((rc = rio_readlineb(rp, usrbuf, maxlen)) < 0) {
	printf("Warning: rio_readlineb failed\n");
	return 0;
    }
    return rc;
} 

/* thread routine */ 
void *thread(void *arg){
  int error = 0;                  /* error detection */
  int thread_id;                  /* thread ID */ 
  int request_count = 0;          /* Number of requests received so far */
  int connfd;                     /* socket desciptor for talkign wiht client*/ 
  int serverfd;                   /* Socket descriptor for talking with end server */
  char *request;                  /* HTTP request from client */
  char *request_uri;              /* Start of URI in first HTTP request header line */
  char *request_uri_end;          /* End of URI in first HTTP request header line */
  char *rest_of_request;          /* Beginning of second HTTP request header line */
  int request_len;                /* Total size of HTTP request */
  int response_len;               /* Total size in bytes of response from end server */
  int i, n;                       /* General index and counting variables */
  int realloc_factor;             /* Used to increase size of request buffer if necessary */  

  char hostname[MAXLINE];         /* Hostname extracted from request URI */
  char pathname[MAXLINE];         /* Content pathname extracted from request URI */
  int serverport;                 /* Port number extracted from request URI (default 80) */
  char log_entry[MAXLINE];        /* Formatted log entry */

  rio_t rio;                      /* Rio buffer for calls to buffered rio_readlineb routine */
  char buf[MAXLINE];              /* General I/O buffer */
  struct thread_data *td;

  /* get from struct */
  td = ((struct thread_data *) arg);
  connfd = td->connfd;
  thread_id = td->thread_id;

  /* detach thread */
  pthread_detach(pthread_self());
  
  while(1){
    error = 0; // bugfix
    /* 
     * Read the entire HTTP request into the request buffer, one line
     * at a time.
     */
    request = (char *)Malloc(MAXLINE);
    request[0] = '\0';
    realloc_factor = 2;
    request_len = 0;
    Rio_readinitb(&rio, connfd);
    
    while (1) {
	if ((n = Rio_readlineb_w(&rio, buf, MAXLINE)) <= 0) {
	  error = 1; //bugfix
	    printf("process_request: client issued a bad request (1).\n");
	    close(connfd);
	    free(request);
            break;
	}

	/* If not enough room in request buffer, make more room */
	if (request_len + n + 1 > MAXLINE)
	    Realloc(request, MAXLINE*realloc_factor++);

	strcat(request, buf);
	request_len += n;

	/* An HTTP requests is always terminated by a blank line */
	if (strcmp(buf, "\r\n") == 0)
	    break;
    }
 
    /*
     * Bug fix - start over on bad request
     */
    if (error)
      continue;
 
    /* 
     * Make sure that this is indeed a GET request
     */
    if (strncmp(request, "GET ", strlen("GET "))) {
	printf("process_request: Received non-GET request\n");
	close(connfd);
	free(request);
	break;
    }
  
  request_uri = request + 4;
    /* 
     * Extract the URI from the request
     */
    request_uri_end = NULL;
    for (i = 0; i < request_len; i++) {
	if (request_uri[i] == ' ') {
	    request_uri[i] = '\0';
	    request_uri_end = &request_uri[i];
	    break;
	}
    }

    /* 
     * If we hit the end of the request without encountering a
     * terminating blank, then there is something screwy with the
     * request
     */
    if ( i == request_len ) {
	printf("process_request: Couldn't find the end of the URI\n");
	close(connfd);
	free(request);
	break;
    } 
    
    /*
     * Make sure that the HTTP version field follows the URI 
     */
    if (strncmp(request_uri_end + 1, "HTTP/1.0\r\n", strlen("HTTP/1.0\r\n")) &&
	strncmp(request_uri_end + 1, "HTTP/1.1\r\n", strlen("HTTP/1.1\r\n"))) {
	printf("process_request: client issued a bad request (4).\n");
	close(connfd);
	free(request);
	break;
    }

    /*
     * We'll be forwarding the remaining lines in the request
     * to the end server without modification
     */
    rest_of_request = request_uri_end + strlen("HTTP/1.0\r\n") + 1;

    /* 
     * Parse the URI into its hostname, pathname, and port components.
     * Since the recipient is a proxy, the browser will always send
     * a URI consisting of a full URL "http://hostname:port/pathname"
     */  
    if (parse_uri(request_uri, hostname, pathname, &serverport) < 0) {
	printf("process_request: cannot parse uri\n");
	close(connfd);
	free(request);
	break;
    }    

    /*
     * Forward the request to the end server
     */ 
    
    // lock 
    pthread_mutex_lock(&clientfdlock);
    if ((serverfd = open_clientfd(hostname, serverport)) < 0) {
	printf("process_request: Unable to connect to end server.\n");
	free(request);
	break;
    }
    // unlock
    pthread_mutex_unlock(&clientfdlock);
    
    Rio_writen_w(serverfd, "GET /", strlen("GET /"));
    Rio_writen_w(serverfd, pathname, strlen(pathname));
    Rio_writen_w(serverfd, " HTTP/1.0\r\n", strlen(" HTTP/1.0\r\n"));
    Rio_writen_w(serverfd, rest_of_request, strlen(rest_of_request));
  
    /*
     * Receive reply from server and forward on to client
     */
    Rio_readinitb(&rio, serverfd);
    response_len = 0;
    while( (n = Rio_readn_w(serverfd, buf, MAXLINE)) > 0 ) {
	response_len += n;
	Rio_writen_w(connfd, buf, n);
        #if defined(DEBUG)	
	printf("Forwarded %d bytes from end server to client\n", n);
	fflush(stdout);
        #endif
	bzero(buf, MAXLINE);
    }

    /*
     * Log the request to disk
     */
    format_log_entry(log_entry, &td, request_uri, response_len);  
    
    /* lock log_file */
    pthread_mutex_lock(&loglock);
    fprintf(log_file, "%s %d\n", log_entry, response_len);

    /* unlock log_file */
    pthread_mutex_unlock(&loglock);

    /* Clean up to avoid memory leaks and then return */
    close(connfd);
    close(serverfd);
    free(request);
    
    free(arg);
    break;
  }
    return NULL;
    
}


/*
 * Rio_writen_w - A wrapper function for rio_writen (csapp.c) that
 * prints a warning when a write fails, instead of terminating the
 * process.
 */
void Rio_writen_w(int fd, void *usrbuf, size_t n) 
{
    if (rio_writen(fd, usrbuf, n) != n) {
	printf("Warning: rio_writen failed.\n");
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
    int len;

    if (strncasecmp(uri, "http://", 7) != 0) {
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
	*port = atoi(hostend + 1);
    
    /* Extract the path */
    pathbegin = strchr(hostbegin, '/');
    if (pathbegin == NULL) {
	pathname[0] = '\0';
    }
    else {
	pathbegin++;	
	strcpy(pathname, pathbegin);
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
void format_log_entry(char *logstring, struct sockaddr_in *sockaddr, 
		      char *uri, int size)
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


