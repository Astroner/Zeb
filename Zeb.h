#if !defined(ZEB_H)
#define ZEB_H

#include <stddef.h>

#define ZEB_BLOCK_MIN_SIZE sizeof(void*)

#define Zeb_roundBlockSize(blockSize) (blockSize < ZEB_BLOCK_MIN_SIZE ? ZEB_BLOCK_MIN_SIZE : blockSize)
#define Zeb_getTotalMemorySize(blockSize, blocksNumber) (Zeb_roundBlockSize(blockSize) * blocksNumber)

typedef struct Zeb {
    size_t blockSize;
    size_t blocksNumber;
    char* buffer;
    char** cursor;
} Zeb;

#define Zeb_define(NAME, BLOCK_SIZE, BLOCKS_NUMBER)\
    char NAME##__buffer[Zeb_getTotalMemorySize(BLOCK_SIZE, BLOCKS_NUMBER)] = {\
        [0] = 0,\
        [1] = 0,\
        [2] = 0,\
        [3] = 0,\
        [4] = 0,\
        [5] = 0,\
        [6] = 0,\
        [7] = 0,\
        [8] = 0,\
    };\
    Zeb NAME##__data = {\
        .blockSize = Zeb_roundBlockSize(BLOCK_SIZE),\
        .blocksNumber = BLOCKS_NUMBER,\
        .buffer = NAME##__buffer,\
        .cursor = (char**)NAME##__buffer,\
    };\
    Zeb* NAME = &NAME##__data;\

#define Zeb_defineStatic(NAME, BLOCK_SIZE, BLOCKS_NUMBER)\
    static char NAME##__buffer[Zeb_getTotalMemorySize(BLOCK_SIZE, BLOCKS_NUMBER)] = {\
        [0] = 0,\
        [1] = 0,\
        [2] = 0,\
        [3] = 0,\
        [4] = 0,\
        [5] = 0,\
        [6] = 0,\
        [7] = 0,\
        [8] = 0,\
    };\
    static Zeb NAME##__data = {\
        .blockSize = Zeb_roundBlockSize(BLOCK_SIZE),\
        .blocksNumber = BLOCKS_NUMBER,\
        .buffer = NAME##__buffer,\
        .cursor = (char**)NAME##__buffer,\
    };\
    static Zeb* NAME = &NAME##__data;\

Zeb* Zeb_init(Zeb* zeb, char* buffer, size_t bufferLength, size_t blockSize);
Zeb* Zeb_create(size_t blockSize, size_t blocksNumber);
void Zeb_destroy(Zeb* zeb);

void* Zeb_alloc(Zeb* zeb);
void Zeb_free(Zeb* zeb, void* block);
void Zeb_clear(Zeb* zeb);

void Zeb_print(const Zeb* zeb);

#define ZEB_ITERATE(ZEB, INFO_NAME, TYPE, VARIABLE)\
    ZebIterator INFO_NAME;\
    ZebIterator_init(&INFO_NAME, ZEB);\
    TYPE VARIABLE;\
    while((VARIABLE = ZebIterator_next(&INFO_NAME)))

typedef struct ZebIterator {
    struct {
        const Zeb* zeb;
        const char* end;
    } __internal;

    size_t index;
    int isFree;
    void* current;
} ZebIterator;

void ZebIterator_init(ZebIterator* iterator, const Zeb* zeb);
void* ZebIterator_next(ZebIterator* iterator);
void ZebIterator_reset(ZebIterator* iterator);

#endif // ZEB_H
#if defined(ZEB_IMPLEMENTATION)


#if !defined(ZEB_STD_MALLOC)
    #include <stdlib.h>
    #define ZEB_STD_MALLOC malloc
#endif // ZEB_STD_MALLOC

#if !defined(ZEB_STD_FREE)
    #include <stdlib.h>
    #define ZEB_STD_FREE free
#endif // ZEB_STD_FREE

#if !defined(ZEB_STD_PRINT)
    #include <stdio.h>
    #define ZEB_STD_PRINT printf
#endif // ZEB_STD_PRINT

/*
    General algorithm:
    Let's say, we have 3 blocks with size equal to 8 bytes and lets assume that the size of a pointer is exact 8 bytes.
    Empty allocator looks like this:
    buffer = | NULL | rand | rand |
    3 initial blocks, first holds NULL pointer and the rest contains random bytes(from malloc() for example).

    At the beginning Zeb.cursor will point to the first element;
    cursor = 1

    In general, block can contain 2 things:
     - pointer to the next free cell, therefore it is a free block
     - data
    
    Block is free to be allocated if it is pointed by cursor or any other free block, otherwise this block contains data. 
    If block contains pointer to itself then this block has no further blocks.
    If block contains NULL then it is a head of the growing sequence of allocated blocks.

    In our example first block contains NULL so it is a head of the growing sequence, therefore all the blocks after this one are free.

    When user calls Zeb_alloc() at first we check the cursor. If it points to NULL, then we have no free space left.
    In our example the cursor points to the first block, so this is the block we will return to the user.
    Then we need to calculate the next value of the cursor.
     - If cursored block contains NULL, then we need to check amount of free space after it, and we have some then we just set cursor to the next free block
       and fill it with NULL else we set cursor to NULL(we have no space left)
     - If cursored block contains pointer to itself, then it is the last free block, therefore we set cursor to NULL
     - In all other cases cursored pointer contains pointer to the next free block which is the next cursor
    
    After first Zeb_alloc() call the structure will look like this:
    buffer = | data | NULL | rand |
    cursor = 2

    If user calls Zeb_alloc() again the process will be the same but with the cursor pointing the second block.
    After the second call the structure will look like this:
    buffer = | data | data | NULL |
    cursor = 3

    When user calls Zeb_free() with specified pointer, we need to check that the provided pointer is in the buffer boundaries 
    and then we need to check the cursor.
     - If the cursor points to the NULL, then we just set cursor to the freed block and fill it with pointer to itself, 
       indicating that this is the last free block
     - In other case we still set cursor to the freed block, but this time we fill it with the previous cursor, making unidirectional linked list.
    
    So in our case after Zeb_free() call with first allocated block the structure will look like this:
    buffer = | 3 | data | NULL |
    cursor = 1

    After freeing the second block the structure will look like this:
    buffer = | 3 | 1 | NULL |
    cursor = 2

    And after Zeb_alloc() calls we will allocate previously freed blocks one by one from the linked list:
    2 -> 1 -> 3

*/
void* Zeb_alloc(Zeb* zeb) {
    if(zeb->cursor == NULL) return NULL;

    void* result = zeb->cursor;

    if(*zeb->cursor == NULL) { // head of the growing sequence
        if((char*)zeb->cursor < zeb->buffer + zeb->blockSize * (zeb->blocksNumber - 1)) { // have more free blocks further
            zeb->cursor = (char**)((char*)zeb->cursor + zeb->blockSize);
            *zeb->cursor = NULL;
        } else { // No free blocks left
            zeb->cursor = NULL;
        }
    } else if (*zeb->cursor == (char*)zeb->cursor) { // Last element in sequence
        zeb->cursor = NULL;
    } else { // sequence continues
        zeb->cursor = (char**)*zeb->cursor;
    }

    return result;
}

void Zeb_free(Zeb* zeb, void* block) {
    if((char*)block < zeb->buffer || (char*)block > zeb->buffer + zeb->blockSize * zeb->blocksNumber) return;

    if(zeb->cursor) {
        char** ptr = (char**)block;
        *ptr = (char*)zeb->cursor;
        zeb->cursor = ptr;
    } else {
        zeb->cursor = block;
        *zeb->cursor = (char*)zeb->cursor;
    }
}

void Zeb_clear(Zeb* zeb) {
    char** first = (char**)zeb->buffer;
    *first = NULL;
    zeb->cursor = first;
}

Zeb* Zeb_create(size_t blockSize, size_t blocksNumber) {
    char* mem = ZEB_STD_MALLOC(sizeof(Zeb) + Zeb_getTotalMemorySize(blockSize, blocksNumber));

    if(mem == NULL) return NULL;

    Zeb* zeb = (Zeb*)mem;
    char* buffer = mem + sizeof(Zeb);
    char** first = (char**)buffer;
    *first = NULL;

    zeb->blockSize = Zeb_roundBlockSize(blockSize);
    zeb->blocksNumber = blocksNumber;
    zeb->cursor = first;
    zeb->buffer = buffer;

    return zeb;
}

void Zeb_destroy(Zeb* zeb) {
    ZEB_STD_FREE(zeb);
}

Zeb* Zeb_init(Zeb* zeb, char* buffer, size_t bufferLength, size_t blockSize) {
    size_t realBlockSize = Zeb_roundBlockSize(blockSize);
    size_t blocksNumber = bufferLength / realBlockSize;
    if(blocksNumber == 0) return NULL;
    
    zeb->blockSize = realBlockSize;
    zeb->blocksNumber = blocksNumber;
    zeb->buffer = buffer;

    char** first = (char**)buffer;
    *first = NULL;

    zeb->cursor = first;

    return zeb;
}

void ZebIterator_init(ZebIterator* iterator, const Zeb* zeb) {
    iterator->__internal.zeb = zeb;
    iterator->__internal.end = zeb->buffer + zeb->blockSize * zeb->blocksNumber;
    iterator->current = NULL;
}

static int Zeb__internal__isInSequence(const Zeb* zeb, const void* searching) {
    char** ptr = zeb->cursor;
    while(1) {
        if(ptr == searching) {
            return 1;
        }

        if(*ptr == NULL) {
            if(searching > (void*)ptr) {
                return 1;
            } else {
                return 0;
            }
        }

        if(*ptr == (char*)ptr) {
            return 0;
        }

        ptr = (char**)*ptr;
    }
}

void* ZebIterator_next(ZebIterator* iterator) {
    if((char*)iterator->current + iterator->__internal.zeb->blockSize == iterator->__internal.end) {
        return NULL;
    }

    if(iterator->current == NULL) {
        iterator->current = iterator->__internal.zeb->buffer;
        iterator->index = 0;
    } else {
        iterator->index += 1;
        iterator->current = (char*)iterator->current + iterator->__internal.zeb->blockSize;
    }

    iterator->isFree = Zeb__internal__isInSequence(iterator->__internal.zeb, iterator->current);
    return iterator->current;
}

void ZebIterator_reset(ZebIterator* iterator) {
    iterator->current = NULL;
}

#if defined(ZEB_DEBUG)
void Zeb_print(const Zeb* zeb) {
    ZEB_ITERATE(zeb, info, void*, el) {
        ZEB_STD_PRINT("[%.2zu] %s\n", info.index, info.isFree ? "FREE" : "BUSY");
    }
}
#endif // ZEB_DEBUG

#endif // ZEB_IMPLEMENTATION

