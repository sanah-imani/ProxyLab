/*
 * @file: cache.c
 * @brief: this is the C code for the web multi-threaded proxy cache
 * implementation for the signature in cache.h. The cache is implemented with
 * LRU principles and allows for adding unique URL-response objects with
 * MAX_OBJECT_SIZE. The web-cache struct contains a singly linked list, a size
 * variable (MAX_CACHE_SIZE is the limit), and a counter variable (LRU). The
 * struct of a web object consists of a key-value pair (urlKey and object), a
 * timestamp of use/creation and a referenceCnt to ensure that an object is not
 * being freed while in use on two or more threads.
 *
 * @Author Sanah Imani <simani@unix.andrew.cmu.edu>
 */

#include <csapp.h>
#include <ctype.h>
#include <inttypes.h>
#include <limits.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define MAX_CACHE_SIZE (1024 * 1024)
#define MAX_OBJECT_SIZE (100 * 1024)

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
web_cache_t *web_cache = NULL;

// a mutual exclusion lock used synchrony of cache accesses for the
// multi-threaded proxy.
static pthread_mutex_t cache_lock;

/*a method to safely free a web_object_t pointer.
 *
 * Needed during eviction where a web_object_t is removed from the linked list
 * to acoomodate newer objects.
 *
 * params[in] obj a pointer to the web_object_t struct being freed
 *
 * @pre obj != NULL
 */

void freeWebObj(web_object_t *obj) {
    if (obj->next != NULL) {
        free(obj->next);
    }
    free(obj->urlKey);
    free(obj->object);
    free(obj);
}

/*a helper method to fetch the web object linked to the supplied key arg
 *contained in the singly linked list in web_cache.
 *
 *
 * params[in] key the url key which we are searching the cache for
 *
 *@return a web object pointer to with urlKey = key
 * @post @return != NULL
 */
static web_object_t *get_obj_with_key(char const *key) {
    web_object_t *currObj = web_cache->start;
    while (currObj != NULL) {
        if (strlen(key) == strlen(currObj->urlKey) &&
            (strncmp(currObj->urlKey, key, strlen(key)) == 0)) {
            return currObj;
        }
        currObj = currObj->next;
    }
    return NULL;
}

/*a method to evict one or more  web object(s) when our cache has hit peak
 *capacity and needs room to insert a new response object.
 *
 * params[in] size the size of the new response object to be added.
 *
 *@return a web object pointer to with urlKey = key
 * @post @return != NULL
 */
static void cache_eviction(int size) {

    // evict objects until size constraint is satisfied.
    while (web_cache->size + size > MAX_CACHE_SIZE) {
        web_object_t *currObj = web_cache->start;
        web_object_t *prevObj = NULL;
        // a struct to hold important pointers to faciliate removal of object in
        // linked list (prev and curr objs).
        web_object_t *toEvict[2];
        int minStamp = INT_MAX;
        while (currObj != NULL) {
            // finding min tStamp.
            if (currObj->tStamp < minStamp) {
                minStamp = currObj->tStamp;
                toEvict[0] = currObj;
                toEvict[1] = prevObj;
            }
            // updating the two pointers
            prevObj = currObj;
            currObj = currObj->next;
        }

        // doing the removal
        if (toEvict[1] == NULL) {
            web_cache->start = (toEvict[0]->next);
            toEvict[0]->next = NULL;
        } else {
            toEvict[1]->next = (toEvict[0]->next);
            toEvict[0]->next = NULL;
        }
        // upating cache size
        web_cache->size = web_cache->size - (toEvict[0]->objSize);
        // removal decreases ref count.
        toEvict[0]->referenceCnt--;
        // only free when we have that no threads are not using the obj
        while (toEvict[0]->referenceCnt != 0)
            ;
        freeWebObj(toEvict[0]);
    }
}

/*a helper function for add_to_cache that specifically adds to the linked list
 * of web response objects by adding to the beginning of the list.
 *
 * Also, identifies if eviction needs to occur before insertion.
 *
 * @params[in] webObj a pointer to the allocated web_object_t block on the heap.
 *
 * @pre no NULL input.
 */
static void insert_into_cache(web_object_t *webObj) {
    if (web_cache->start == NULL) {
        web_cache->start = webObj;
        return;
    }

    // acounting for the eviction case.
    int objSize = webObj->objSize;
    if (web_cache->size + objSize >= MAX_CACHE_SIZE) {
        cache_eviction(objSize);
    }
    // adding to the head
    web_object_t *temp = web_cache->start;
    web_cache->start = webObj;
    webObj->next = temp;
}

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
bool add_to_cache(char const *cache_key, char *cacheBuf, int size) {
    // checking if the key already exists as we need unique keys in cache.
    if (get_obj_with_key(cache_key) != NULL) {
        return false;
    }
    // locks because we are adding to cache and dynamic memory is shared.
    pthread_mutex_lock(&cache_lock);
    // dynamic allocation of a web_object_t to store the web response object
    // with other important params.
    web_object_t *webObj = (web_object_t *)malloc(sizeof(web_object_t));
    if (webObj == NULL) {
        pthread_mutex_unlock(&cache_lock);
        return true;
    }

    // only allocated the required size.
    char *dest = (char *)malloc(size);
    // memcpy as response may contain non-ASCII chars
    dest = memcpy(dest, cacheBuf, size);

    if (dest == NULL) {
        freeWebObj(webObj);
        pthread_mutex_unlock(&cache_lock);
        return true;
    }

    webObj->object = dest;

    // need to create a key copy as the key will be freed in the proxy code.
    char *keyCopy = (char *)malloc((strlen(cache_key) + 1));
    keyCopy = strcpy(keyCopy, cache_key);
    if (keyCopy == NULL) {
        freeWebObj(webObj);
        pthread_mutex_unlock(&cache_lock);
        return true;
    }

    // basic initialization.
    webObj->urlKey = keyCopy;
    webObj->objSize = size;
    webObj->tStamp = web_cache->LRU;
    webObj->next = NULL;
    webObj->referenceCnt = 1;
    insert_into_cache(webObj);
    web_cache->size += size;
    pthread_mutex_unlock(&cache_lock);
    return false;
}

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
bool serve_cache(int fd, const char *cache_key) {
    // single mutex lock while accessing global variable web_cache.
    //
    pthread_mutex_lock(&cache_lock);
    web_object_t *cacheObj = NULL;
    // update LRU var
    web_cache->LRU++;
    // finding matching object associated with cache_key
    if ((cacheObj = get_obj_with_key(cache_key)) != NULL) {
        // updated object struct to show that it is being used and for the LRU
        // implementation.
        cacheObj->tStamp = web_cache->LRU;
        cacheObj->referenceCnt++;
        pthread_mutex_unlock(&cache_lock);
        // writing back to the client directly.
        if (rio_writen(fd, cacheObj->object, cacheObj->objSize) <= 0) {
            fprintf(stderr, "Could not write response to client");
        }
        // done with the use.
        cacheObj->referenceCnt--;
        return true;
    }
    pthread_mutex_unlock(&cache_lock);
    return false;
}

/* initialising the web cache by mallocing a block that web_cache variables
 * points to */
void init_web_cache() {
    web_cache = (web_cache_t *)malloc(sizeof(web_cache_t));
    if (web_cache == NULL) {
        fprintf(stderr, "unable to create cache\n");
        return;
    }
    if (web_cache != NULL) {
        web_cache->size = 0;
        web_cache->LRU = 0;
        web_cache->start = NULL;
    }
}

/* initialising pthread mutual exclusion lock is required. This allows for cache
 * access synchronization */
void init_cache_lock() {
    pthread_mutex_init(&cache_lock, NULL);
}
