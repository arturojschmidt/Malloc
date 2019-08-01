#define SUPER_BLOCK_SIZE 4096
#define SUPER_BLOCK_MASK (~(SUPER_BLOCK_SIZE-1))
#define MIN_ALLOC 32 /* Smallest real allocation.  Round smaller mallocs up */
#define MAX_ALLOC 2048 /* Fail if anything bigger is attempted.  
                        * Challenge: handle big allocations */
#define RESERVE_SUPERBLOCK_THRESHOLD 2

#define FREE_POISON 0xab
#define ALLOC_POISON 0xcd


#include <stdint.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <sys/mman.h>
#include <math.h>
#include <string.h>
 
#define assert(cond) if (!(cond)) __asm__ __volatile__ ("int $3")

/* Object: One return from malloc/input to free. */
struct __attribute__((packed)) object {
    union {
        struct object *next; // For free list (when not in use)
        // pointer to next object, which in itself is has an object and a pointer to data
        char * raw; // Actual data
    };
};

/* Super block bookeeping; one per superblock.  "steal" the first
 * object to store this structure
 */
struct __attribute__((packed)) superblock_bookkeeping {
    struct superblock_bookkeeping * next; // next super block
    struct object *free_list;
    // Free count in this superblock
    uint8_t free_count; // Max objects per superblock is 128-1, so a byte is sufficient
    uint8_t level;
    uint8_t used;
};
  
/* Superblock: a chunk of contiguous virtual memory.
 * Subdivide into allocations of same power-of-two size. */
struct __attribute__((packed)) superblock {
    struct superblock_bookkeeping bkeep;
    void *raw;  // Actual data here
};


/* The structure for one pool of superblocks.  
 * One of these per power-of-two */
struct superblock_pool {
    struct superblock_bookkeeping *next;
    uint64_t free_objects; // Total number of free objects across all superblocks
    uint64_t whole_superblocks; // Superblocks with all entries free
};

// 10^5 -- 10^11 == 7 levels
#define LEVELS 7
static struct superblock_pool levels[LEVELS] = {{NULL, 0, 0},
                                                {NULL, 0, 0},
                                                {NULL, 0, 0},
                                                {NULL, 0, 0},
                                                {NULL, 0, 0},
                                                {NULL, 0, 0},
                                                {NULL, 0, 0}};

static inline int size2level (ssize_t size) {
    if(size > 2048){
        errno = -EINVAL;
    }else if(size > 1024){
        size = 6;
    }else if(size > 512){
        size = 5;
    }else if(size > 256){
        size = 4;
    }else if(size> 128){
        size = 3;
    }else if(size> 64){
        size = 2;
    }else if(size > 32){
        size = 1;
    }else if(size > 0){
        size = 0;
    }else {
       errno = -EINVAL;
        //handle edge case
    }
    return size;
}

static inline
struct superblock_bookkeeping * alloc_super (int power) {
    char *here = "here";
    

    
    void *page;
    struct superblock* sb;
    struct superblock_bookkeeping *next;
    int free_objects = 0, bytes_per_object = 0;
    char *cursor;
    int size = 4096;
   
    
    page = mmap(NULL, size, PROT_READ|PROT_WRITE, MAP_ANON|MAP_PRIVATE, -1, 0);
   
    if((void *) -1 == page){
        printf("couldnt map memory : %s\n", strerror(errno));
        return NULL;
    }

    sb                          = (struct superblock*) page;
    if(levels[power].next == NULL){
         levels[power].next = &sb->bkeep;

    }else{
        next = levels[power].next;
        while(next->next!=NULL){
        next = next->next;
        }
        next->next              = &sb->bkeep;
    }
   
    free_objects                = 4096/(pow((double) 2, (power+5))); //correct
    bytes_per_object            = 4096/free_objects; //correct
    sb->bkeep.free_count        = --free_objects; //correct
    sb->bkeep.next              = NULL; //correct
    sb->bkeep.level             = power;
    sb->bkeep.used              = 0;
    levels[power].free_objects += free_objects;
    levels[power].whole_superblocks++;
   
    cursor = (char *) sb; //why is this a char pointer? 4 bytes instead of 8

    // skip the first object
    for (cursor += bytes_per_object; free_objects--; cursor += bytes_per_object) {
        // Place the object on the free list
        struct object* tmp = (struct object *) cursor;
        tmp->next = sb->bkeep.free_list;
        sb->bkeep.free_list = tmp;
    }
    //sb->bkeep.free_list         = (struct object *)&sb->bkeep + bytes_per_object;
    return &sb->bkeep;
}

void *malloc(size_t size) {
   
    struct superblock_pool *pool;
    struct superblock_bookkeeping *bkeep;
    struct superblock_bookkeeping *tmp;
    struct object *cursor;
    int power      = size2level(size);
    int sizeObject = pow((double) 2, (power+5));

    if (size > MAX_ALLOC) {
        errno = -ENOMEM;
        return NULL;
    }
    pool = &levels[power];
    if (!pool->free_objects) {
        bkeep = alloc_super(power);
    } else{
         bkeep = pool->next;
    }

    while (bkeep != NULL) {
        if (bkeep->free_count>0) {
           cursor = (struct object *) bkeep->free_list;

          if(bkeep->used == 0){
              pool->whole_superblocks--;
          }
            /* Remove an object from the free list. */
            bkeep->free_list = cursor->next;  
            if(bkeep->free_count == 0){
               bkeep->free_list   = NULL;
           }  
            bkeep->free_count--;
            pool->free_objects--;
            bkeep->used = 1;
            break;
        }else{
            bkeep = bkeep->next;
        }
    }         
        return cursor;
}
static inline
struct superblock_bookkeeping * obj2bkeep (void *ptr) {
    uint64_t addr = (uint64_t) ptr;
    addr &= SUPER_BLOCK_MASK;
    return (struct superblock_bookkeeping *) addr;
}

void free(void *ptr) {
    
    struct superblock_bookkeeping *bkeep;
    struct superblock_pool *pool;
    struct object *next;
    struct object * pointer;
    uint8_t objects; 
    int bytes_per_object;           
    uint8_t power;
    uint64_t offset; 

    bkeep   =  obj2bkeep(ptr); 
    pointer = (struct object *) ptr;
    power   = bkeep->level;
    pool    = &levels[power];
     
    objects          = 4096/(pow((double) 2, (power+5)));
   
    bytes_per_object = 4096/objects;

    //walks free list (if not null) and replaces last null free with newly freed
    if (bkeep->free_list == NULL){
        bkeep->free_list = pointer;
    }else{
        struct object * tmp;
        tmp                    = bkeep->free_list;
        pointer->next          =tmp;
        bkeep->free_list       = pointer;

    }
    bkeep->free_count++;
    pool->free_objects++;
    //checks if freed was the only occupied object in sb, decrements whole superblocks
    // calls releaseSuperblocks to check if there is 
    if (bkeep->free_count == objects-1){
        pool->whole_superblocks++;
    }
    memset(pointer+1, FREE_POISON, bytes_per_object-(sizeof(struct object)));
    
    if (levels[bkeep->level].whole_superblocks > RESERVE_SUPERBLOCK_THRESHOLD) {
            struct superblock_bookkeeping *tmp;
            if(bkeep->free_count == objects-1){
                if(pool->next==bkeep){
                    pool->next = bkeep->next;
                }else{
                    tmp = pool->next;
                    while(tmp->next!=bkeep){
                    tmp = tmp->next;
                    }
                    if(bkeep->next!=NULL){
                    tmp->next = bkeep->next;
                    }else{
                    tmp->next = NULL;
                    }
                }
            }
            pool->whole_superblocks--;
            pool->free_objects = (pool->free_objects-(objects-1));
            int size = 4092*sizeof(char);
            munmap((void *)bkeep, size);
        }
}

int pthread_create(void __attribute__((unused)) *x, ...) {
    exit(-ENOSYS);
}


