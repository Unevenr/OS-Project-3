#include "io_helper.h"
#include "request.h"


#define MAXBUF (8192)

// below default values are defined in 'request.h'
int num_threads = DEFAULT_THREADS;
int buffer_max_size = DEFAULT_BUFFER_SIZE;
int scheduling_algo = DEFAULT_SCHED_ALGO;	

//
//	TODO: add code to create and manage the shared global buffer of requests
//	HINT: You will need synchronization primitives.
//		pthread_mutuex_t lock_var is a viable option.
typedef struct 
{
    int fd;
    char filename[MAXBUF];
    int filesize;
    int counter;
} request_t;

static request_t *buffer;
static int buffer_head = 0;
static int buffer_tail = 0;
static int buffer_count = 0;

static pthread_mutex_t buffer_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t buffer_not_empty = PTHREAD_COND_INITIALIZER;
static pthread_cond_t buffer_not_full = PTHREAD_COND_INITIALIZER;

int not_ethan_grabber()
{

    int index = buffer_head;
    //FIFO
    if (scheduling_algo == 0)
    {
        return buffer_head;
    }

    //SFF
    if(scheduling_algo == 1)
    {
  
        int min_size = 1000000000;
        
        for (int i = 0; i < buffer_count; i++)
        {
           int position = (buffer_head +i) % buffer_max_size; 
           buffer[position].counter++;
           if (buffer[position].counter >= 20) return position;
           if(buffer[position].filesize < min_size)
           {
                min_size = buffer[position].filesize;
                index = position;
           }
        }
    }
    else if (scheduling_algo == 2)
    {
            int offset = rand() % buffer_count;
            index = (buffer_head + offset) % buffer_max_size;
    }

    return index;
}

void buffer_add(int fd, char *filename, int filesize)
{
    pthread_mutex_lock(&buffer_lock);

    while (buffer_count == buffer_max_size)
    {
        pthread_cond_wait(&buffer_not_full, &buffer_lock);
    }

    buffer[buffer_tail].fd = fd;
    strcpy(buffer[buffer_tail].filename, filename);
    buffer[buffer_tail].filesize = filesize;
    buffer[buffer_tail].counter = 0;

    printf("[+] Added request: %s (fd=%d, size=%d)\n", filename, fd, filesize);

    buffer_tail = (buffer_tail + 1) % buffer_max_size;
    buffer_count++;

    pthread_cond_signal(&buffer_not_empty);
    pthread_mutex_unlock(&buffer_lock);
}

request_t buffer_remove()
{
    pthread_mutex_lock(&buffer_lock);

    while (buffer_count == 0)
    {
        pthread_cond_wait(&buffer_not_empty, &buffer_lock);
    }

    int index = not_ethan_grabber();

    if (index != buffer_head)
    {
        request_t temporary = buffer[buffer_head];
        buffer[buffer_head] = buffer[index];
        buffer[index] = temporary;
    }

    request_t request = buffer[buffer_head];

    printf("[-] Removed request: %s (fd=%d, size=%d)\n", request.filename, request.fd, request.filesize);

    buffer_head = (buffer_head + 1) % buffer_max_size;
    buffer_count--;

    pthread_cond_signal(&buffer_not_full);
    pthread_mutex_unlock(&buffer_lock);

    return request;
}

//
// Sends out HTTP response in case of errors
//
void request_error(int fd, char *cause, char *errnum, char *shortmsg, char *longmsg) {
    char buf[MAXBUF], body[MAXBUF];
    
    // Create the body of error message first (have to know its length for header)
    sprintf(body, ""
	    "<!doctype html>\r\n"
	    "<head>\r\n"
	    "  <title>CYB-3053 WebServer Error</title>\r\n"
	    "</head>\r\n"
	    "<body>\r\n"
	    "  <h2>%s: %s</h2>\r\n" 
	    "  <p>%s: %s</p>\r\n"
	    "</body>\r\n"
	    "</html>\r\n", errnum, shortmsg, longmsg, cause);
    
    // Write out the header information for this response
    sprintf(buf, "HTTP/1.0 %s %s\r\n", errnum, shortmsg);
    write_or_die(fd, buf, strlen(buf));
    
    sprintf(buf, "Content-Type: text/html\r\n");
    write_or_die(fd, buf, strlen(buf));
    
    sprintf(buf, "Content-Length: %lu\r\n\r\n", strlen(body));
    write_or_die(fd, buf, strlen(buf));
    
    // Write out the body last
    write_or_die(fd, body, strlen(body));
    
    // close the socket connection
    close_or_die(fd);
}

//
// Reads and discards everything up to an empty text line
//
void request_read_headers(int fd) {
    char buf[MAXBUF];
    
    readline_or_die(fd, buf, MAXBUF);
    while (strcmp(buf, "\r\n")) {
	readline_or_die(fd, buf, MAXBUF);
    }
    return;
}

//
// Return 1 if static, 0 if dynamic content (executable file)
// Calculates filename (and cgiargs, for dynamic) from uri
int request_parse_uri(char *uri, char *filename, char *cgiargs) {
    char *ptr;
    
    if (!strstr(uri, "cgi")) { 
	// static
	strcpy(cgiargs, "");
	sprintf(filename, ".%s", uri);
	if (uri[strlen(uri)-1] == '/') {
	    strcat(filename, "index.html");
	}
	return 1;
    } else { 
	// dynamic
	ptr = index(uri, '?');
	if (ptr) {
	    strcpy(cgiargs, ptr+1);
	    *ptr = '\0';
	} else {
	    strcpy(cgiargs, "");
	}
	sprintf(filename, ".%s", uri);
	return 0;
    }
}

//
// Fills in the filetype given the filename
//
void request_get_filetype(char *filename, char *filetype) {
    if (strstr(filename, ".html")) 
	strcpy(filetype, "text/html");
    else if (strstr(filename, ".gif")) 
	strcpy(filetype, "image/gif");
    else if (strstr(filename, ".jpg")) 
	strcpy(filetype, "image/jpeg");
    else 
	strcpy(filetype, "text/plain");
}

//
// Handles requests for static content
//
void request_serve_static(int fd, char *filename, int filesize) {
    int srcfd;
    char *srcp, filetype[MAXBUF], buf[MAXBUF];
    
    request_get_filetype(filename, filetype);
    srcfd = open_or_die(filename, O_RDONLY, 0);
    
    // Rather than call read() to read the file into memory, 
    // which would require that we allocate a buffer, we memory-map the file
    srcp = mmap_or_die(0, filesize, PROT_READ, MAP_PRIVATE, srcfd, 0);
    close_or_die(srcfd);
    
    // put together response
    sprintf(buf, ""
	    "HTTP/1.0 200 OK\r\n"
	    "Server: OSTEP WebServer\r\n"
	    "Content-Length: %d\r\n"
	    "Content-Type: %s\r\n\r\n", 
	    filesize, filetype);
       
    write_or_die(fd, buf, strlen(buf));
    
    //  Writes out to the client socket the memory-mapped file 
    write_or_die(fd, srcp, filesize);
    munmap_or_die(srcp, filesize);
}

//
// Fetches the requests from the buffer and handles them (thread logic)
//
void* thread_request_serve_static(void* arg)
{
    // TODO: write code to actualy respond to HTTP requests
    // Pull from global buffer of requests
    printf ("Thread %lu started.\n", pthread_self());

    //SFF test
    // pthread_mutex_lock(&buffer_lock);
    // while (buffer_count < 3) {
    //     // printf("Thread %lu waiting for buffer to fill...\n", pthread_self());
    //     pthread_mutex_unlock(&buffer_lock);
    //     sched_yield();  // Let other threads/clients run

    //     pthread_mutex_lock(&buffer_lock);
    // }
    printf("Thread %lu started work\n", pthread_self());
    // pthread_mutex_unlock(&buffer_lock);


    while (1)
    {
        request_t request = buffer_remove();

        printf ("Thread %lu handing request for %s\n", pthread_self(), request.filename);

        request_serve_static(request.fd, request.filename, request.filesize);
        close_or_die(request.fd);
    }
    return NULL;
}

//
// Initial handling of the request
//
void request_handle(int fd) {
    int is_static;
    struct stat sbuf;
    char buf[MAXBUF], method[MAXBUF], uri[MAXBUF], version[MAXBUF];
    char filename[MAXBUF], cgiargs[MAXBUF];

    if (buffer == NULL)
    {
        buffer = malloc(sizeof(request_t) * buffer_max_size);
    }
    
    // get the request type, file path and HTTP version
    readline_or_die(fd, buf, MAXBUF);
    sscanf(buf, "%s %s %s", method, uri, version);
    printf("method:%s uri:%s version:%s\n", method, uri, version);

    // verify if the request type is GET or not
    if (strcasecmp(method, "GET")) {
	request_error(fd, method, "501", "Not Implemented", "server does not implement this method");
	return;
    }
    request_read_headers(fd);
    
    // check requested content type (static/dynamic)
    is_static = request_parse_uri(uri, filename, cgiargs);
    
    printf("Looking for file: %s\n", filename);
    // get some data regarding the requested file, also check if requested file is present on server
    if (stat(filename, &sbuf) < 0) {
	request_error(fd, filename, "404", "Not found", "server could not find this file");
	return;
    }
    
    // verify if requested content is static
    if (is_static) 
    {
    
        // TODO: directory traversal mitigation	
        if (strstr(filename, "..") != NULL)
        {
            request_error(fd, filename, "403", "Forbidden", "Nice try lil bro (attempted directory traversal detected)");
            return;
        }    

        if (!(S_ISREG(sbuf.st_mode)) || !(S_IRUSR & sbuf.st_mode)) {
            request_error(fd, filename, "403", "Forbidden", "server could not read this file");
            return;
        }
        


	    // TODO: write code to add HTTP requests in the buffer
        buffer_add(fd, filename, sbuf.st_size);
    } 
    else 
    {
	request_error(fd, filename, "501", "Not Implemented", "server does not serve dynamic content request");
    }
}
