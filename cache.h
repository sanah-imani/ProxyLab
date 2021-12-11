/*
 * @file: cache.h
 * @brief: this is the header file for the cache implementation for a web
 * multi-threaded proxy. The cache is implemented with LRU principles and allows
 * for adding unique URL-response objects with MAX_OBJECT_SIZE. The web-cache
 * struct contains a singly linked list, a size variable (MAX_CACHE_SIZE is the
 * limit), and a counter variable (LRU). The struct of a web object consists of
 * a key-value pair (urlKey and object), a timestamp of use/creation and a
 * referenceCnt to ensure that an object is not being freed while in use on two
 * or more threads.
 *
 * @Author Sanah Imani <simani@unix.andrew.cmu.edu>
 */

#include <stdbool.h>

// important size-limit constant definitions
#define MAX_CACHE_SIZE (1024 * 1024) // max size of the cache in bytes
#define MAX_OBJECT_SIZE                                                        \
    (100 * 1024) // max size of a response object being stored in the cache in
                 // bytes.

typedef struct web_object_t {
    char *urlKey;     // the url serves as an identifier for the response object
    char *object;     // the response object from the server
    int objSize;      // the length of the response
    int tStamp;       // when the object has been last used / first created
    int referenceCnt; // a value to check how many threads using the object.
                      // Only when zero, then the object can be freed during
                      // eviction
    struct web_object_t *next; // the pointer to the next web object if any
} web_object_t;

typedef struct web_cache {
    struct web_object_t
        *start; // the pointer to the first web_object_t of the linked list
    int size;   // current size of the cache that only includes the response
                // object sizes
    int LRU; // a variable counter to assign tStamps and implemented LRU-cache.
} web_cache_t;

// a global, external pointer to a heap-allocated web_cache object that forms
// the basis of the cache.
extern web_cache_t *web_cache;

/* initialising the web cache by mallocing a block that web_cache variables
 * points to */
void init_web_cache();

/* initialising pthread mutual exclusion lock is required. This allows for cache
 * access synchronization */
void init_cache_lock();
/*a method to safely free a web_object_t pointer.
 *
 * Needed during eviction where a web_object_t is removed from the linked list
 * to acoomodate newer objects.
 *
 * params[in] obj a pointer to the web_object_t struct being freed
 *
 * @pre obj != NULL
 */
void freeWebObj(web_object_t *obj);

/*if an HTTP response object linked to the URL cache_key is present, this
 * function writes to connfd, serving the client directly.
 *
 * No need to send a request to the server again.
 *
 * @params[out] fd this is the connection file descriptor returned when we
 * accept a client request.
 * @params[in] cache_key a const char pointer to the URL key required to search
 * and find object.
 *
 * @returns a boolean indicating if cache could serve the client (true = yes
 * client was written to with the object in the cache, false otherwise).
 * @pre cache_key != NULL and fd > 0.
 */
bool serve_cache(int fd, const char *cache_key);

/* adding a web response object to the cache. The web response object can be
 * thought of as a block of memory with the content supplied in cacheBuf along
 * with its key and other parameters mentioned before. Each object has the
 * response encapsulated in a buffer char pointer, the key associated with it
 * (the request URL) and the its size.
 *
 * @params[in] cache_key the request URL that is associated with the response
 * object being stored in the cache.
 * @params[in] cacheBuf a char pointer to the response from the server.
 * @params[in] size the number of bytes represented by cacheBuf/read from the
 * server
 *
 * @return a boolean indicated if adding to cache happened error free.
 * @pre no NULL inputs and size > 0.
 */
bool add_to_cache(char const *cache_key, char *cacheBuf, int size);
