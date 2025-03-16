/**
 * @file mm.c
 * @brief A 64-bit struct-based implicit free list memory allocator
 *
 * 15-213: Introduction to Computer Systems
 *
 * TODO: insert your documentation here. :)
 *
 *************************************************************************
 *
 * ADVICE FOR STUDENTS.
 * - Step 0: Please read the writeup!
 * - Step 1: Write your heap checker.
 * - Step 2: Write contracts / debugging assert statements.
 * - Good luck, and have fun!
 *
 *************************************************************************
 *
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
#define UINT_MAX ((size_t)-1)
#define ALIGNMENT 16
typedef uint64_t word_t;

/** @brief Word and header size (bytes) */
static const size_t wsize = sizeof(word_t);
 
/** @brief Double word size (bytes) */
static const size_t dsize = 2 * wsize;

/** @brief Minimum block size (bytes) */
static const size_t min_block_size = 2 * dsize;

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
 * @brief Bit mask to isolate size of SOMETHINGGGGGGGGGG
 */
static const word_t size_mask = ~(word_t)0xF;

/** @brief Represents the header and payload of one block in the heap */
typedef struct block block_t;

struct block {
    word_t header; /* Header contains size + allocation flag */
    union {
        struct {
            block_t* prev;
            block_t* next;
        };
        char payload[0]; /* A pointer to the block payload */
    };
};

/* Global variables */
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
static word_t pack(size_t size, bool alloc);
static size_t extract_size(word_t word);
static size_t get_size(block_t *block);
static size_t get_payload_size(block_t *block);
static block_t *payload_to_header(void *bp);
static void *header_to_payload(block_t *block);
static word_t *header_to_footer(block_t *block);
static block_t *footer_to_header(word_t *footer);
static bool extract_alloc(word_t word);
static bool get_alloc(block_t *block);
static bool extract_prev_alloc(word_t word);
static bool get_prev_alloc(block_t *block);
static void write_epilogue(block_t *block);
static void write_block(block_t *block, size_t size, bool alloc);
static void write_footer(block_t *block, size_t size, bool alloc);
static block_t *find_next(block_t *block);
static word_t *find_prev_footer(block_t *block);
static block_t *find_prev(block_t *block);


static void add_node(block_t *block);
static void delete_node(block_t *block);
static size_t get_seg_index(size_t size);
static size_t get_seg_size(size_t size);

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
static word_t pack(size_t size, bool alloc) {
    word_t word = size;
    if (alloc) {
        word |= alloc_mask;
    }
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
    return (block_t *)((char *)bp - offsetof(block_t, payload));
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
    return (void *)(block->payload);
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
    return (word_t *)(block->payload + get_size(block) - dsize);
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
    return asize - dsize;
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
* @brief Writes an epilogue header at the given address.
*
* The epilogue header has size 0, and is marked as allocated.
*
* @param[out] block The location to write the epilogue header
*/
static void write_epilogue(block_t *block) {
    dbg_requires(block != NULL);
    dbg_requires((char *)block == (char *)mem_heap_hi() - 7);
    block->header = pack(0, true);
}

/**
  * @brief Writes a block starting at the given address.
  *
  * This function writes both a header and footer, where the location of the
  * footer is computed in relation to the header.
  *
  * TODO: Are there any preconditions or postconditions?
  *
  * @param[out] block The location to begin writing the block header
  * @param[in] size The size of the new block
  * @param[in] alloc The allocation status of the new block
  */
static void write_block(block_t *block, size_t size, bool alloc) {
    dbg_requires(block != NULL);
    dbg_requires(size > 0);
    block->header = pack(size, alloc);

    word_t *footerp = header_to_footer(block);
    *footerp = pack(size, alloc);
}


/**
* @brief Given a block, its size, and allocation status, write appropriate
* value to the footer
*/
static void write_footer(block_t *block, size_t size, bool alloc) {
    word_t *footerp = header_to_footer(block);
    *footerp = pack(size, alloc);
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
    word_t *footerp = find_prev_footer(block);
    return footer_to_header(footerp);
}

/**/
static bool extract_prev_alloc(word_t word) {
    return (bool)(word & 0x2);
}

/**/
static bool get_prev_alloc(block_t *block) {
    return extract_prev_alloc(block->header);
}

/**/
static void init_seg_list() {
    for (int i = 0; i < SEG_LENGTH; i++) {
        seg_list[i] = NULL;
    }
}

static size_t get_seg_index(size_t size) {
    size_t idx = 0;
    size >>= 5;
    while (size > 1 && idx < SEG_LENGTH - 1) {
        size >>= 1;
        idx++;
    }
    return idx;
}

/**/
static size_t get_seg_size(size_t idx) {
    if (idx == 0)
        return min_block_size;
    return 1 << (idx + 5);
}

/**
  * @brief Insert a new node to a doubled linked list using LIFO
  */
static void add_node(block_t *block) {

    size_t size = get_size(block);
    size_t idx = get_seg_index(size);

    /* Case 1: free list is empty */
    if (seg_list[idx] == NULL) {
        seg_list[idx] = block;
        block->prev = NULL;
        block->next = NULL;
    }

    /* Case 2: free list is non-empty */
    else {
        block->prev = NULL;
        block->next = seg_list[idx];
        seg_list[idx]->prev = block;
        seg_list[idx] = block;
    }
    return;
}

static void delete_node(block_t *block) {
    size_t idx = get_seg_index(get_size(block));

    /* Case 1: deleted node is not the first block */
    if (block->prev != NULL) { 
        block->prev->next = block->next;
    } 

    else {
        if (block != NULL)
            seg_list[idx] = block->next;
        else
            seg_list[idx] = NULL;
    }

    /* deleted block is not the last node */
    if (block->next != NULL) { 
        block->next->prev = block->prev;
    }
    block->next = NULL;
    block->prev = NULL;
}

/*
* ---------------------------------------------------------------------------
*                        END SHORT HELPER FUNCTIONS
* ---------------------------------------------------------------------------
*/

/******** The remaining content below are helper and debug routines ********/

/**
  * @brief
  *
  * <What does this function do?>
  * <What are the function's arguments?>
  * <What is the function's return value?>
  * <Are there any preconditions or postconditions?>
  *
  * @param[in] block
  * @return
  */
static block_t *coalesce_block(block_t *block) {
    dbg_requires(block != NULL);
    dbg_requires(in_heap(block));

    block_t *block_prev = find_prev(block);
    block_t *block_next = find_next(block);

    dbg_assert(block_prev != NULL);
    dbg_assert(block_next != NULL);

    size_t newSize = get_size(block);
    size_t block_prev_size = get_size(block_prev);
    size_t block_next_size = get_size(block_next);

    bool block_prev_alloc = get_alloc(block_prev);
    bool block_next_alloc = get_alloc(block_next);

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

    size_t block_size = get_size(block);

    if ((block_size - asize) >= min_block_size) {
        block_t *block_next;
        write_block(block, asize, true);

        block_next = find_next(block);
        write_block(block_next, block_size - asize, false);
        add_node(block_next);
    }

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
    size_t class_idx = get_seg_index(asize);
    dbg_assert(class_idx >= 0 && class_idx <= 14);

    for (size_t i = class_idx; i < SEG_LENGTH; i++) {
        block_t *ll_start = seg_list[i];
        block_t *block;
        for (block = ll_start; block != NULL; block = block->next) {
            if (asize <= get_size(block)) {
                return block;
            }
        }
    }

    return NULL; // no fit found
}

/**
*/
static bool is_acyclic(block_t *block) {
    block_t *h;
    block_t *t;

    if (block == NULL)
        return true;
    h = block->next;
    t = block;
    while (h != t) {
        if (h == NULL || h->next == NULL)
            return false;
        h = h->next->next;
        t = t->next;
    }
    return true;
}

/**
*/
static bool is_aligned(const void *p) {
    size_t ip = (size_t)p;

    size_t aligned_value =
        min_block_size * ((ip + min_block_size - 1) / min_block_size);

    // Check if already aligned
    return aligned_value == ip ? ip : aligned_value;
}

/**
 */
static bool is_valid_block(block_t *block) {
    word_t *footerp = (word_t *)((block->payload) + get_size(block) - dsize);

    /* Check payload alignment */
    if (!is_aligned(block->payload))
        return false;

    /* Check valid size */
    if (get_size(block) < min_block_size)
        return false;
    else if (get_size(block) >= min_block_size && !get_alloc(block)) {
        /* Header and footer matching */
        if (extract_size(*footerp) != get_size(block))
            return false;
        if (extract_alloc(*footerp) != get_alloc(block))
            return false;
    }
    return true;
}

/**
* @brief Check validity of list segment
*/
static bool is_segment(block_t *start, block_t *end, size_t size) {
    if (start == NULL || start == end)
        return true;
    if (!is_valid_block(start) || get_size(start) > size) {
        dbg_printf("Error: segment error at line \n");
        return false;
    }

    /* Previous/Next consistency */
    if (size < min_block_size)
        return false;
    else {
        if (start->next != NULL && start->next->prev != start) {
            dbg_printf("Error: next consistency error at line\n");
            return false;
        }

        if (start->prev != NULL && start->prev->next != start) {
            dbg_printf("Error: previous consistency error at line\n");
            return false;
        }
    }
    return is_segment(start->next, NULL, size);
}

/**
 */
static bool is_valid_list(block_t *block, size_t size) {
    if (block == NULL)
        return true;
    if (!is_segment(block, NULL, size)) {
        dbg_printf("Error: invalid list 1 error at line\n");
        return false;
    }

    if (!is_acyclic(block)) {
        dbg_printf("Error: acyclic error at line\n");
        return false;
    }

    return true;
}

/**
 */
static bool is_valid_segregated_list() {
    size_t size;

    for (int i = 0; i < SEG_LENGTH; i++) {
        size = get_seg_size((unsigned int)i);

        if (i == SEG_LENGTH - 1) {
            size = UINT_MAX;
        }

        if (!is_valid_list(seg_list[i], size)) {
            dbg_printf("Error: invalid list error at line\n");
            return false;
        }
    }
    return true;
}

/**
 */
static bool in_heap(const void *p) {
    return p <= mem_heap_hi() && p >= mem_heap_lo();
}

/**
 */
static bool is_valid_heap_boundaries(int line) {
    block_t *base = (block_t *)mem_heap_lo();
    block_t *top = (block_t *)((char *)mem_heap_hi() - 7); 

    /* Check prologue */
    if (!get_alloc(base) || get_size(base) != 0) {
        dbg_printf("Error: prologue error at line %d\n", line);
        return false;
    }

    /* Check heap starts */
    if ((char *)heap_start != (char *)(base) + 8) {
        dbg_printf("Error: heap start error at line %d\n", line);
        return false;
    }

    /* Check epilogue */
    if (!get_alloc(top) || get_size(top) != 0) {
        dbg_printf("Error: epilogue error at line %d\n", line);
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
  * @param[in] line
  * @return
  */
bool mm_checkheap(int line) {
    block_t *curr = heap_start;
    block_t *prev;

    /* Check validity on list level */
    if (!is_valid_segregated_list()) {
        dbg_printf("Error: invalid segregated list at line %d\n", line);
        return false;
    }

    /* Check validity on heap level */
    if (!is_valid_heap_boundaries(line)) {
        dbg_printf("Error: invalid heap bound at line %d\n", line);
        return false;
    }

    // Iterate through all blocks in the heap
    while (get_size(curr) != 0) {
        // Check block in heap bound
        if (!in_heap(curr)) {
            dbg_printf("Error: Block is not in heap boundaries at line %d\n",
                    line);
            return false;
        }

        prev = curr;
        curr = find_next(curr);

        /* Check adjacent free blocks */
        if (!get_alloc(prev) && !get_alloc(curr) && get_size(curr) != 0) {
            dbg_printf("Error: free block err at line %d\n", line);
            return false;
        }
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

    start[0] = pack(0, true); // Heap prologue (block footer)
    start[1] = pack(0, true); // Heap epilogue (block header)

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
    // add_node(block);

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
    // write_footer(block, size, false);

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
