/**
 * @file mm.c
 * @brief A 64-bit struct-based implicit free list memory allocator
 *
 *
 * INTRODUCTION
 * This lab implements a memory allocator using implicit free list with 
 * segregated free list. The allocator supports malloc, realloc, calloc, 
 * and free functions.
 *
 *
 * DESIGN CHOICE
 * 
 * Store extra information in the header -> Create helpers to extract
 * information and fast check if needed
 *
 *************************************************************************
 * @author Tram Tran <tntran@andrew.cmu.edu>
 */

#include <assert.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "memlib.h"
#include "mm.h"

/* Do not change the following! */

#ifdef DRIVER
/* create aliases for driver tests */
#define malloc mm_malloc
#define free mm_free
#define realloc mm_realloc
#define calloc mm_calloc
#define memset mem_memset
#define memcpy mem_memcpy
#endif /* def DRIVER */

/* You can change anything from here onward */

/*
*****************************************************************************
* If DEBUG is defined (such as when running mdriver-dbg), these macros      *
* are enabled. You can use them to print debugging output and to check      *
* contracts only in debug mode.                                             *
*                                                                           *
* Only debugging macros with names beginning "dbg_" are allowed.            *
* You may not define any other macros having arguments.                     *
*****************************************************************************
*/
#ifdef DEBUG
/* When DEBUG is defined, these form aliases to useful functions */
#define dbg_requires(expr) assert(expr)
#define dbg_assert(expr) assert(expr)
#define dbg_ensures(expr) assert(expr)
#define dbg_printf(...) ((void)printf(__VA_ARGS__))
#define dbg_printheap(...) print_heap(__VA_ARGS__)
#else
/* When DEBUG is not defined, these should emit no code whatsoever,
 * not even from evaluation of argument expressions.  However,
 * argument expressions should still be syntax-checked and should
 * count as uses of any variables involved.  This used to use a
 * straightforward hack involving sizeof(), but that can sometimes
 * provoke warnings about misuse of sizeof().  I _hope_ that this
 * newer, less straightforward hack will be more robust.
 * Hat tip to Stack Overflow poster chqrlie (see
 * https://stackoverflow.com/questions/72647780).
 */
#define dbg_discard_expr_(...) ((void)((0) && printf(__VA_ARGS__)))
#define dbg_requires(expr) dbg_discard_expr_("%d", !(expr))
#define dbg_assert(expr) dbg_discard_expr_("%d", !(expr))
#define dbg_ensures(expr) dbg_discard_expr_("%d", !(expr))
#define dbg_printf(...) dbg_discard_expr_(__VA_ARGS__)
#define dbg_printheap(...) ((void)((0) && print_heap(__VA_ARGS__)))
#endif

/* Basic constants */
#define SEG_LENGTH 15
typedef uint64_t word_t;

/** @brief Word and header size (bytes) */
static const size_t wsize = sizeof(word_t);

/** @brief Double word size (bytes) */
static const size_t dsize = 2 * wsize;

/** @brief Minimum block size (bytes) */
static const size_t min_block_size = dsize;

/**
 * @brief Memory that the allocator requests from the system when it needs to
 *        expand the heap
 * (Must be divisible by dsize)
 */
static const size_t chunksize = (1 << 12);

/**
 * @brief Bit mask to isolate allocation status
 */
static const word_t alloc_mask = 0x1;

/**
 * @brief Bit mask to isolate the allocation status of previous block
 */
static const word_t prev_alloc_mask = 0x2;

/**
 * @brief Bit mask to isolate the flag if the previous block is 
 * minimum block size
 */
static const word_t prev_min_tag_mask = 0x4;

/**
 * @brief Bit mask to isolate size of block
 */
static const word_t size_mask = ~(word_t)0xF;

/** @brief Represents the header and payload of one block in the heap */
typedef struct block block_t;

typedef struct block {
    word_t header;
    union payload_info {
        struct {
            struct block *next;
            struct block *prev;
        } linkList;
        char data[0];
    } payLoad;
} block_t;

/* Global variables */
/** @brief Array of 13 class sizes */
static block_t *seg_list[SEG_LENGTH];

/** @brief Pointer to first block in the heap */
static block_t *heap_start = NULL;

/************* FUNCTION LISTS *************/

void *malloc(size_t size);
void free(void *bp);
void *realloc(void *ptr, size_t size);
void *calloc(size_t elements, size_t size);
bool mm_init(void);
bool mm_checkheap(int line);
static bool in_heap(const void *p);
static block_t *extend_heap(size_t size);
static block_t *coalesce_block(block_t *block);
static block_t *find_fit(size_t asize);
static void split_block(block_t *block, size_t asize);

static size_t max(size_t x, size_t y);
static size_t round_up(size_t size, size_t n);

static word_t pack(size_t size, bool alloc, bool prev_alloc, bool prev_min_alloc);
static size_t extract_size(word_t word);
static size_t get_size(block_t *block);
static size_t get_payload_size(block_t *block);
static block_t *payload_to_header(void *bp);
static void *header_to_payload(block_t *block);
static word_t *header_to_footer(block_t *block);
static block_t *footer_to_header(word_t *footer);

static bool extract_alloc(word_t word);
static bool get_alloc(block_t *block);
static bool get_prev_alloc(word_t header);
static bool get_prev_min_tag(word_t header);
static void write_epilogue(block_t *block);
static void write_block(block_t *block, size_t size, bool alloc);
static block_t *find_next(block_t *block);
static word_t *find_prev_footer(block_t *block);
static block_t *find_prev(block_t *block);

static void add_node(block_t *block);
static void delete_node(block_t *block);
static size_t get_seg_index(size_t size);

/*
*****************************************************************************
* The functions below are short wrapper functions to perform                *
* bit manipulation, pointer arithmetic, and other helper operations.        *
*                                                                           *
* We've given you the function header comments for the functions below      *
* to help you understand how this baseline code works.                      *
*                                                                           *
* Note that these function header comments are short since the functions    *
* they are describing are short as well; you will need to provide           *
* adequate details for the functions that you write yourself!               *
*****************************************************************************
*/

/*
 * ---------------------------------------------------------------------------
 *                        BEGIN SHORT HELPER FUNCTIONS
 * ---------------------------------------------------------------------------
 */

/**
 * @brief Returns the maximum of two integers.
 * @param[in] x
 * @param[in] y
 * @return `x` if `x > y`, and `y` otherwise.
 */
static size_t max(size_t x, size_t y) {
    return (x > y) ? x : y;
}

/**
 * @brief Rounds `size` up to next multiple of n
 * @param[in] size
 * @param[in] n
 * @return The size after rounding up
 */
static size_t round_up(size_t size, size_t n) {
    return n * ((size + (n - 1)) / n);
}

/**
 * @brief Packs the `size` and `alloc` of a block into a word suitable for
 *        use as a packed value.
 *
 * Packed values are used for both headers and footers.
 *
 * The allocation status is packed into the lowest bit of the word.
 *
 * @param[in] size The size of the block being represented
 * @param[in] alloc True if the block is allocated
 * @return The packed value
 */
static word_t pack(size_t size, bool alloc, bool prev_alloc, bool prev_min_alloc) {
    word_t word = size;
    if (alloc) {
        word |= alloc_mask;
    }

    if (prev_alloc) {word |= prev_alloc_mask;}
    
    if (prev_min_alloc) {word |= prev_min_tag_mask;}
    
    return word;
}

/**
 * @brief Extracts the size represented in a packed word.
 *
 * This function simply clears the lowest 4 bits of the word, as the heap
 * is 16-byte aligned.
 *
 * @param[in] word
 * @return The size of the block represented by the word
 */
static size_t extract_size(word_t word) {
    return (word & size_mask);
}

/**
 * @brief Extracts the size of a block from its header.
 * @param[in] block
 * @return The size of the block
 */
static size_t get_size(block_t *block) {
    return extract_size(block->header);
}

/**
 * @brief Given a payload pointer, returns a pointer to the corresponding
 *        block.
 * @param[in] bp A pointer to a block's payload
 * @return The corresponding block
 */
static block_t *payload_to_header(void *bp) {
    return (block_t *)((char *)bp - offsetof(block_t, payLoad));
}

/**
 * @brief Given a block pointer, returns a pointer to the corresponding
 *        payload.
 * @param[in] block
 * @return A pointer to the block's payload
 * @pre The block must be a valid block, not a boundary tag.
 */
static void *header_to_payload(block_t *block) {
    dbg_requires(get_size(block) != 0);
    return (void *)(block->payLoad.data);    
}

/**
 * @brief Given a block pointer, returns a pointer to the corresponding
 *        footer.
 * @param[in] block   
 * @return A pointer to the block's footer
 * @pre The block must be a valid block, not a boundary tag.
 */
static word_t *header_to_footer(block_t *block) {
    dbg_requires(get_size(block) != 0 &&
                 "Called header_to_footer on the epilogue block");
    return (word_t *)(block->payLoad.data + get_size(block) - dsize);
}                        

/**
 * @brief Given a block footer, returns a pointer to the corresponding
 *        header.
 *
 * The header is found by subtracting the block size from
 * the footer and adding back wsize.
 *
 * If the prologue is given, then the footer is return as the block.
 *
 * @param[in] footer A pointer to the block's footer
 * @return A pointer to the start of the block
 */
static block_t *footer_to_header(word_t *footer) {
    size_t size = extract_size(*footer);
    if (size == 0) {
        return (block_t *)footer;
    }
    return (block_t *)((char *)footer + wsize - size);
}

/**
 * @brief Returns the payload size of a given block.
 *
 * The payload size is equal to the entire block size minus the sizes of the
 * block's header and footer.
 *
 * @param[in] block
 * @return The size of the block's payload
 */
static size_t get_payload_size(block_t *block) {
    size_t asize = get_size(block);
    size_t res = asize;

    if (get_alloc(block)) res -= wsize;
    else res -= dsize;
    return res;
}

/**
 * @brief Returns the allocation status of a given header value.
 *
 * This is based on the lowest bit of the header value.
 *
 * @param[in] word
 * @return The allocation status correpsonding to the word
 */
static bool extract_alloc(word_t word) {
    return (bool)(word & alloc_mask);
}

/**
 * @brief Returns the allocation status of a block, based on its header.
 * @param[in] block
 * @return The allocation status of the block
 */
static bool get_alloc(block_t *block) {
    return extract_alloc(block->header);
}

/**
 * @brief Returns min previous tag based on the size
 * @return The allocation status of the previous block
 */
static bool get_prev_min_tag(word_t header) {
    return (header & prev_min_tag_mask);
}

/**
 * @brief Writes an epilogue header at the given address.
 *
 * The epilogue header has size 0, and is marked as allocated.
 *
 * @param[out] block The location to write the epilogue header
 */
static void write_epilogue(block_t *block) {
    dbg_requires(block != NULL);
    dbg_requires((char *)block == (char *)mem_heap_hi() - 7);
    block->header = pack(0, true, get_prev_alloc(block->header), get_prev_min_tag(block->header));
}

/**
 * @brief Writes a block starting at the given address.
 *
 * This function writes both a header and footer, where the location of the
 * footer is computed in relation to the header.
 *
 * @pre block must not be NULL
 * @pre size is greater than 0
 *
 * @post block will have its header (and footer if necessary) updated
 * @post if the block is free, its footer will be written correctly
 * 
 * @param[out] block The location to begin writing the block header
 * @param[in] size The size of the new block
 * @param[in] alloc The allocation status of the new block
 */
static void write_block(block_t *block, size_t size, bool alloc) {
    dbg_requires(block != NULL);
    dbg_requires(size > 0);


    block->header = pack(size, alloc, get_prev_alloc(block->header), get_prev_min_tag(block->header));

    /* Block if free, we need footer */
    if (!alloc && get_size(block) > min_block_size) {
        word_t *footerp = header_to_footer(block);
        *footerp = pack(size, alloc, get_prev_alloc(block->header), get_prev_min_tag(block->header));
    }

    /* Update the flag of next block */
    block_t* block_next = find_next(block);
    block_next->header = pack(get_size(block_next), get_alloc(block_next), alloc, get_size(block) == min_block_size);
    
}

/**
 * @brief Finds the next consecutive block on the heap.
 *
 * This function accesses the next block in the "implicit list" of the heap
 * by adding the size of the block.
 *
 * @param[in] block A block in the heap
 * @return The next consecutive block on the heap
 * @pre The block is not the epilogue
 */
static block_t *find_next(block_t *block) {
    dbg_requires(block != NULL);
    dbg_requires(get_size(block) != 0 ||
                 (bool)"Called find_next on the last block in the heap");
    return (block_t *)((char *)block + get_size(block));
}

/**
 * @brief Finds the footer of the previous block on the heap.
 * @param[in] block A block in the heap
 * @return The location of the previous block's footer
 */
static word_t *find_prev_footer(block_t *block) {
    // Compute previous footer position as one word before the header
    return &(block->header) - 1;
}

/**
 * @brief Finds the previous consecutive block on the heap.
 *
 * This is the previous block in the "implicit list" of the heap.
 *
 * The position of the previous block is found by reading the previous
 * block's footer to determine its size, then calculating the start of the
 * previous block based on its size.
 *
 * @param[in] block A block in the heap
 * @return The previous consecutive block in the heap.
 * @pre The block is not the prologue
 */
static block_t *find_prev(block_t *block) {
    dbg_requires(block != NULL);
    dbg_requires(get_size(block) != 0 ||
                 (bool)"Called find_prev on the first block in the heap");

    /* BLock has min block size */
    if (get_prev_min_tag(block->header)) {
        return (block_t*)((char*)block - 16);
    }
    
    word_t *footerp = find_prev_footer(block);
    return footer_to_header(footerp);
}

/**
 * @brief Returns the allocation status of the previous block based on size
 * @param[in] size
 * @return The allocation status of previous block
 */
static bool get_prev_alloc(word_t header) {
    return (header & prev_alloc_mask);
}

/**
 * @brief Initiliaze the segregated list
 */
static void init_seg_list() {
    for (int i = 0; i < SEG_LENGTH; i++) {
        seg_list[i] = NULL;
    }
}

/**
 * @brief Return the index of current position in the segregated list
 *
 * @param[in] size The size of the current block we are checking
 * @return The index based on the size
 */
static size_t get_seg_index(size_t size) {
    size_t sizes[] = {min_block_size, 32, 48, 64, 80, 96, 112, 128};

    size_t num = sizeof(sizes) / sizeof(sizes[0]);

    for (size_t i = 0; i < num; i++) {
        if (size == sizes[i]) {
            return i;
        }
    }

    size_t idx = num;
    size >>= 5;
    
    while (size > 1 && idx < SEG_LENGTH - 1) {
        size >>= 1;
        idx++;
    }
    return idx;
}


/**
 * @brief Insert a new block to the segregated free list using LIFO approach
 *
 * @pre block must not be NULL
 * 
 * @param[in] block A pointer to the current block added
 */
static void add_node(block_t *block) {
    dbg_requires(block != NULL);

    size_t size = get_size(block);
    size_t idx = get_seg_index(size);

    block_t* start = seg_list[idx];

    /* Block of min block size */
    if ((int)idx == 0) {
        block->payLoad.linkList.next = start;
        seg_list[idx] = block;
        return;
    }

    if (block != start) block->payLoad.linkList.next = start;
    
    block->payLoad.linkList.prev = NULL;

    if (start != NULL && block != start) {
        start->payLoad.linkList.prev = block;
    }

    seg_list[idx] = block;
}

/**
 * @brief Delete the new from the segregated free list
 */
static void delete_node(block_t *block) {
    dbg_requires(block != NULL);
    int idx = (int)get_seg_index(get_size(block));

    dbg_assert(idx >= 0 && idx <= 14);

    if (idx == 0) {
        block_t* curr = seg_list[idx];

        if (curr == block) {
            seg_list[idx] = curr->payLoad.linkList.next;
            return;
        }

        while (curr != NULL) {
            if (curr->payLoad.linkList.next == block) {
                curr->payLoad.linkList.next = block->payLoad.linkList.next;
                break;
            }
            curr = curr->payLoad.linkList.next;
        }
        return;
    }
    
    if (block->payLoad.linkList.prev) {
        block->payLoad.linkList.prev->payLoad.linkList.next =
        block->payLoad.linkList.next;
    }
    else {seg_list[idx] = block->payLoad.linkList.next;}

    /* deleted block is not the last node */
    if (block->payLoad.linkList.next) {
        block->payLoad.linkList.next->payLoad.linkList.prev = block->payLoad.linkList.prev;
    }
    block->payLoad.linkList.next = NULL;
    block->payLoad.linkList.prev = NULL;
}

/*
 * ---------------------------------------------------------------------------
 *                        END SHORT HELPER FUNCTIONS
 * ---------------------------------------------------------------------------
 */

/******** The remaining content below are helper and debug routines ********/

/**
 * @brief Coalesce the current block with the adjacent free block, if possible.
 * This will help reduce fragmentation in the heap by making larger contiguous
 * free blocks, and this benefits future allocations. <Are there any
 * preconditions or postconditions?>
 *
 * @param[in] block A pointer to the current block to be coalesced
 * @return A pointer to the coalesced block
 */
static block_t *coalesce_block(block_t *block) {
    dbg_requires(block != NULL);
    dbg_requires(in_heap(block));

    block_t *block_prev = NULL;
    block_t *block_next = find_next(block);

    dbg_assert(block_next != NULL);

    size_t newSize = get_size(block);
    size_t block_prev_size = 0;
    size_t block_next_size = get_size(block_next);

    bool block_prev_alloc = get_prev_alloc(block->header);
    bool block_next_alloc = get_alloc(block_next);

    /* Free block has footer -> easily get previous's block information */
    if (!block_prev_alloc) {
        block_prev = find_prev(block);
        block_prev_size = get_size(block_prev);
        dbg_assert(block_prev != NULL);
    }

    /* Case 1: prev alloc and next alloc */
    if (block_prev_alloc && block_next_alloc) {
        dbg_assert(in_heap(block));
        add_node(block);
        return block;
    }

    /* Case 2: prev alloc but next free */
    else if (block_prev_alloc && !block_next_alloc) {
        newSize += block_next_size;
        dbg_assert(in_heap(block));
        delete_node(block_next);
        dbg_assert(in_heap(block));
        write_block(block, newSize, false);
        dbg_assert(in_heap(block));
        add_node(block); 
        return block;
    }
    /* Case 3: prev free but next alloc */
    else if (!block_prev_alloc && block_next_alloc) {
        newSize += block_prev_size;
        delete_node(block_prev);
        write_block(block_prev, newSize, false);
        dbg_assert(in_heap(block));
        add_node(block_prev);
        return block_prev;
    }

    /* Case 4: prev and next free */
    else {
        newSize += block_prev_size + block_next_size;
        delete_node(block_prev);
        delete_node(block_next);
        write_block(block_prev, newSize, false);
        dbg_assert(in_heap(block));
        add_node(block_prev);
        return block_prev;
    }
}

/**
 * @brief
 *
 * <What does this function do?>
 * <What are the function's arguments?>
 * <What is the function's return value?>
 * <Are there any preconditions or postconditions?>
 *
 * @param[in] size
 * @return
 */
static block_t *extend_heap(size_t size) {
    void *bp;

    // Allocate an even number of words to maintain alignment
    size = round_up(size, dsize);
    if ((bp = mem_sbrk((intptr_t)size)) == (void *)-1) {
        return NULL;
    }

    // Initialize free block header/footer
    block_t *block = payload_to_header(bp);
    write_block(block, size, false);

    // Create new epilogue header
    block_t *block_next = find_next(block);
    write_epilogue(block_next);

    // Coalesce in case the previous block was free
    block = coalesce_block(block);

    return block;
}

/**
 * @brief
 *
 * <What does this function do?>
 * <What are the function's arguments?>
 * <What is the function's return value?>
 * <Are there any preconditions or postconditions?>
 *
 * @param[in] block
 * @param[in] asize
 */
static void split_block(block_t *block, size_t asize) {
    dbg_requires(get_alloc(block));
    dbg_requires(get_size(block) > asize);

    size_t block_size = get_size(block);

    if ((block_size - asize) >= min_block_size) {
        block_t *block_next;
        write_block(block, asize, true);

        block_next = find_next(block);
        write_block(block_next, block_size - asize, false);
        add_node(block_next);
    }
    else {write_block(block, block_size, true);}

    dbg_ensures(get_alloc(block));
}

/**
 * @brief First fit
 *
 * <What does this function do?>
 * <What are the function's arguments?>
 * <What is the function's return value?>
 * <Are there any preconditions or postconditions?>
 *
 * @param[in] asize
 * @return
 */
static block_t *find_fit(size_t asize) {
    int class_idx = (int)get_seg_index(asize);
    dbg_assert(class_idx >= 0 && class_idx <= 14);
    block_t *block;
    block_t *start;

    /* First-fit*/
    if (class_idx >= 0 && class_idx <= 4) {
        for (int i = class_idx; i < 5; i++) {
            start = seg_list[i];
            for (block = start; block != NULL; block = block->payLoad.linkList.next) {
                if (asize <= get_size(block)) {
                    return block;
                }
            }
        }
    }

    block_t *good_block = NULL;
    size_t good_size = SIZE_MAX;
    int MAX_TRIES = 5;

    for (int i = class_idx; i < SEG_LENGTH; i++) {
        start = seg_list[i];
        int tries = 0;

        for (block = start; block != NULL; block = block->payLoad.linkList.next) {
            size_t block_size = get_size(block);

            if (asize == block_size) {
                good_block = block;
                good_size = block_size;
                break;
            }
            else if (asize < block_size) {
                if (good_block == NULL) {
                    good_block = block;
                    good_size = block_size;
                    ++tries;
                }
                else {
                    if (tries < MAX_TRIES) {
                        if (block_size < good_size) {
                            good_block = block;
                            good_size = block_size;
                            ++tries;
                        }
                    }
                    else break;
                }
            }
            
        }
        if (good_block != NULL) return good_block;
    }

    return NULL; // no fit found
}

/**
 * Check pointer consistency, pointer in bound,
 */
static bool is_valid_segregated_list(int line) {
    for (int i = 0; i < SEG_LENGTH; i++) {
        for (block_t *curr = seg_list[i]; curr != NULL; curr = curr->payLoad.linkList.next) {
            block_t *next = curr->payLoad.linkList.next;

            /* Check pointer consistency */
            if (next != NULL && next->payLoad.linkList.prev != curr) {
                dbg_printf("Error on pointer consistency at line %d\n", line);
                return false;
            }

            /* Check pointer in boundary or not */
            if (curr > (block_t *)mem_heap_hi() ||
                curr < (block_t *)mem_heap_lo()) {
                dbg_printf("Error on pointer boundaries at line %d\n", line);
            }
        }
    }
    return true;
}

/**
 *
 */
static bool in_heap(const void *p) {
    return p <= mem_heap_hi() && p >= mem_heap_lo();
}

/**
 */
static bool is_valid_heap_boundaries(int line) {
    block_t *base = (block_t *)mem_heap_lo(); /* Get the prologue */
    block_t *top = (block_t *)((char *)mem_heap_hi() - sizeof(block_t) +
                               1); /* Get the epilogue*/

    /* Check heap starts */
    if ((char *)heap_start != (char *)(base) + 8) {
        dbg_printf("Error: heap start error at line %d\n", line);
        return false;
    }

    /* Check epilogue prologue */
    if (get_size(top) == 0 && get_size(base) == 0 && get_alloc(top) &&
        get_alloc(base))
        return true;

    else {
        dbg_printf("Error: epilogue and prologue: %d\n", line);
        return false;
    }

    /* Check heap boundaries */
    block_t *curr_block = heap_start;
    while (curr_block <= (block_t *)mem_heap_hi() &&
           get_size(curr_block) != 0) {
        block_t *next_block = find_next(curr_block);
        if (next_block > (block_t *)mem_heap_hi()) {
            dbg_printf("Heap boundaries: %d\n", line);
            return false;
        }
        curr_block = next_block;
    }

    /* Check block header footer */
    curr_block = heap_start;
    while (curr_block <= (block_t *)mem_heap_hi() &&
           get_size(curr_block) != 0) {
        word_t header = curr_block->header;
        word_t footer = *(header_to_footer(curr_block));
        if (header != footer) {
            dbg_printf("Head and footer at line %d\n", line);
            return false;
        }
    }
    return true;
}

/**
 * @brief
 *
 * <What does this function do?>
 * <What are the function's arguments?>
 * <What is the function's return value?>
 * <Are there any preconditions or postconditions?>
 *
 * Check epilogue and prologue
 * Check address alignment
 * Check block in heap bounds
 * Check block's header and footer and consistency
 * Check coalescing
 * @param[in] line
 * @return
 */
bool mm_checkheap(int line) {
    block_t *curr = heap_start;

    /* Check validity on list level: check pointer consistency, pointer in bound
     */
    if (!is_valid_segregated_list(line)) {
        dbg_printf("Error: invalid segregated list at line %d\n", line);
        return false;
    }

    /* Check validity on heap level: check heap start, epilogue, prologue, heap
     * bounds, block header footer */
    if (!is_valid_heap_boundaries(line)) {
        dbg_printf("Error: invalid heap bound at line %d\n", line);
        return false;
    }

    // Iterate through all blocks in the heap
    while (curr <= (block_t *)mem_heap_hi() && get_size(curr) != 0) {
        // Check block in heap bound
        if (!in_heap(curr)) {
            dbg_printf("Error: Block is not in heap boundaries at line %d\n",
                       line);
            return false;
        }
        block_t *next = find_next(curr);

        /* Check coalescing */
        if (get_size(next) != 0) {
            if (!get_alloc(curr) && !get_alloc(next)) {
                dbg_printf("Coalescing error at line %d\n", line);
                return false;
            }
        }
        curr = next;
    }

    dbg_printf("Heap is consistent at line %d\n", line);
    return true;
}

/**
 * @brief
 *
 * <What does this function do?>
 * <What are the function's arguments?>
 * <What is the function's return value?>
 * <Are there any preconditions or postconditions?>
 *
 * @return
 */
bool mm_init(void) {
    // Create the initial empty heap
    word_t *start = (word_t *)(mem_sbrk(2 * wsize));

    if (start == (void *)-1) {
        return false;
    }

    start[0] = pack(0, true, false, false); // Heap prologue (block footer)
    start[1] = pack(0, true, true, false); // Heap epilogue (block header)

    // Heap starts with first "block header", currently the epilogue
    heap_start = (block_t *)&(start[1]);

    init_seg_list();

    // Extend the empty heap with a free block of chunksize bytes
    if (extend_heap(chunksize) == NULL) {
        return false;
    }

    return true;
}

/**
 * @brief
 *
 * <What does this function do?>
 * <What are the function's arguments?>
 * <What is the function's return value?>
 * <Are there any preconditions or postconditions?>
 *
 * @param[in] size
 * @return
 */
void *malloc(size_t size) {
    dbg_requires(mm_checkheap(__LINE__));

    size_t asize;      // Adjusted block size
    size_t extendsize; // Amount to extend heap if no fit is found
    block_t *block;
    void *bp = NULL;

    // Initialize heap if it isn't initialized
    if (heap_start == NULL) {
        if (!(mm_init())) {
            dbg_printf("Problem initializing heap. Likely due to sbrk");
            return NULL;
        }
    }

    // Ignore spurious request
    if (size == 0) {
        dbg_ensures(mm_checkheap(__LINE__));
        return bp;
    }

    // Adjust block size to include overhead and to meet alignment requirements
    asize = round_up(size + dsize, dsize);

    // Search the free list for a fit
    block = find_fit(asize);

    // If no fit is found, request more memory, and then and place the block
    if (block == NULL) {
        // Always request at least chunksize
        extendsize = max(asize, chunksize);
        block = extend_heap(extendsize);
        // extend_heap returns an error
        if (block == NULL) {
            return bp;
        }
    }

    // The block should be marked as free
    dbg_assert(!get_alloc(block));

    // Mark block as allocated
    size_t block_size = get_size(block);
    write_block(block, block_size, true);
    delete_node(block);

    // Try to split the block if too large
    split_block(block, asize);

    bp = header_to_payload(block);

    dbg_ensures(mm_checkheap(__LINE__));
    return bp;
}

/**
 * @brief
 *
 * <What does this function do?>
 * <What are the function's arguments?>
 * <What is the function's return value?>
 * <Are there any preconditions or postconditions?>
 *
 * @param[in] bp
 */
void free(void *bp) {
    dbg_requires(mm_checkheap(__LINE__));

    if (bp == NULL) {
        return;
    }

    block_t *block = payload_to_header(bp);
    size_t size = get_size(block);

    // The block should be marked as allocated
    dbg_assert(get_alloc(block));

    // Mark the block as free
    write_block(block, size, false);

    // Try to coalesce the block with its neighbors
    coalesce_block(block);

    dbg_ensures(mm_checkheap(__LINE__));
}

/**
 * @brief
 *
 * <What does this function do?>
 * <What are the function's arguments?>
 * <What is the function's return value?>
 * <Are there any preconditions or postconditions?>
 *
 * @param[in] ptr
 * @param[in] size
 * @return
 */
void *realloc(void *ptr, size_t size) {
    block_t *block = payload_to_header(ptr);
    size_t copysize;
    void *newptr;

    // If size == 0, then free block and return NULL
    if (size == 0) {
        free(ptr);
        return NULL;
    }

    // If ptr is NULL, then equivalent to malloc
    if (ptr == NULL) {
        return malloc(size);
    }

    // Otherwise, proceed with reallocation
    newptr = malloc(size);

    // If malloc fails, the original block is left untouched
    if (newptr == NULL) {
        return NULL;
    }

    // Copy the old data
    copysize = get_payload_size(block); // gets size of old payload
    if (size < copysize) {
        copysize = size;
    }
    memcpy(newptr, ptr, copysize);

    // Free the old block
    free(ptr);

    return newptr;
}

/**
 * @brief
 *
 * <What does this function do?>
 * <What are the function's arguments?>
 * <What is the function's return value?>
 * <Are there any preconditions or postconditions?>
 *
 * @param[in] elements
 * @param[in] size
 * @return
 */
void *calloc(size_t elements, size_t size) {
    void *bp;
    size_t asize = elements * size;

    if (elements == 0) {
        return NULL;
    }
    if (asize / elements != size) {
        // Multiplication overflowed
        return NULL;
    }

    bp = malloc(asize);
    if (bp == NULL) {
        return NULL;
    }

    // Initialize all bits to 0
    memset(bp, 0, asize);

    return bp;
}

/*
*****************************************************************************
* Do not delete the following super-secret(tm) lines!                       *
*                                                                           *
* 53 6f 20 79 6f 75 27 72 65 20 74 72 79 69 6e 67 20 74 6f 20               *
*                                                                           *
* 66 69 67 75 72 65 20 6f 75 74 20 77 68 61 74 20 74 68 65 20               *
* 68 65 78 61 64 65 63 69 6d 61 6c 20 64 69 67 69 74 73 20 64               *
* 6f 2e 2e 2e 20 68 61 68 61 68 61 21 20 41 53 43 49 49 20 69               *
*                                                                           *
* 73 6e 27 74 20 74 68 65 20 72 69 67 68 74 20 65 6e 63 6f 64               *
* 69 6e 67 21 20 4e 69 63 65 20 74 72 79 2c 20 74 68 6f 75 67               *
* 68 21 20 2d 44 72 2e 20 45 76 69 6c 0a c5 7c fc 80 6e 57 0a               *
*                                                                           *
*****************************************************************************
*/
