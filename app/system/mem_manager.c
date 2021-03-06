/*
    FreeRTOS V7.5.2 - Copyright (C) 2013 Real Time Engineers Ltd.

    VISIT http://www.FreeRTOS.org TO ENSURE YOU ARE USING THE LATEST VERSION.

    ***************************************************************************
     *                                                                       *
     *    FreeRTOS provides completely free yet professionally developed,    *
     *    robust, strictly quality controlled, supported, and cross          *
     *    platform software that has become a de facto standard.             *
     *                                                                       *
     *    Help yourself get started quickly and support the FreeRTOS         *
     *    project by purchasing a FreeRTOS tutorial book, reference          *
     *    manual, or both from: http://www.FreeRTOS.org/Documentation        *
     *                                                                       *
     *    Thank you!                                                         *
     *                                                                       *
    ***************************************************************************

    This file is part of the FreeRTOS distribution.

    FreeRTOS is free software; you can redistribute it and/or modify it under
    the terms of the GNU General Public License (version 2) as published by the
    Free Software Foundation >>!AND MODIFIED BY!<< the FreeRTOS exception.

    >>! NOTE: The modification to the GPL is included to allow you to distribute
    >>! a combined work that includes FreeRTOS without being obliged to provide
    >>! the source code for proprietary components outside of the FreeRTOS
    >>! kernel.

    FreeRTOS is distributed in the hope that it will be useful, but WITHOUT ANY
    WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
    FOR A PARTICULAR PURPOSE.  Full license text is available from the following
    link: http://www.freertos.org/a00114.html

    1 tab == 4 spaces!

    ***************************************************************************
     *                                                                       *
     *    Having a problem?  Start by reading the FAQ "My application does   *
     *    not run, what could be wrong?"                                     *
     *                                                                       *
     *    http://www.FreeRTOS.org/FAQHelp.html                               *
     *                                                                       *
    ***************************************************************************

    http://www.FreeRTOS.org - Documentation, books, training, latest versions,
    license and Real Time Engineers Ltd. contact details.

    http://www.FreeRTOS.org/plus - A selection of FreeRTOS ecosystem products,
    including FreeRTOS+Trace - an indispensable productivity tool, a DOS
    compatible FAT file system, and our tiny thread aware UDP/IP stack.

    http://www.OpenRTOS.com - Real Time Engineers ltd license FreeRTOS to High
    Integrity Systems to sell under the OpenRTOS brand.  Low cost OpenRTOS
    licenses offer ticketed support, indemnification and middleware.

    http://www.SafeRTOS.com - High Integrity Systems also provide a safety
    engineered and independently SIL3 certified version for use in safety and
    mission critical applications that require provable dependability.

    1 tab == 4 spaces!
*/

/*
 * A sample implementation of pvPortMalloc() and vPortFree() that combines 
 * (coalescences) adjacent memory blocks as they are freed, and in so doing 
 * limits memory fragmentation.
 *
 * See heap_1.c, heap_2.c and heap_3.c for alternative implementations, and the 
 * memory management pages of http://www.FreeRTOS.org for more information.
 */
#if 1
#include <stdlib.h>
#include <string.h>

#include "bios.h"
#include "mem_manager.h"

/* Architecture specifics. */
#define portBYTE_ALIGNMENT			8
#define portBYTE_ALIGNMENT_MASK	(7)
#ifndef portPOINTER_SIZE_TYPE
	#define portPOINTER_SIZE_TYPE unsigned long
#endif
#ifndef configUSE_MALLOC_FAILED_HOOK
	#define configUSE_MALLOC_FAILED_HOOK 0
#endif
#ifndef configASSERT
	#define configASSERT( x )
	#define configASSERT_DEFINED 0
#else
	#define configASSERT_DEFINED 1
#endif


extern char _heap_start;

#define configTOTAL_HEAP_SIZE			( ( size_t ) ( 0x3fffc000 - (uint32)&_heap_start ) )

/* Block sizes must not get too small. */
#define heapMINIMUM_BLOCK_SIZE	( ( size_t ) ( heapSTRUCT_SIZE * 2 ) )

/* Assumes 8bit bytes! */
#define heapBITS_PER_BYTE		( ( size_t ) 8 )

/* A few bytes might be lost to byte aligning the heap start address. */
#define heapADJUSTED_HEAP_SIZE	( configTOTAL_HEAP_SIZE - portBYTE_ALIGNMENT )

/* Allocate the memory for the heap. */
//static unsigned char ucHeap[ configTOTAL_HEAP_SIZE ];
//static unsigned char *ucHeap;

/* Define the linked list structure.  This is used to link free blocks in order
of their memory address. */
typedef struct A_BLOCK_LINK
{
	struct A_BLOCK_LINK *pxNextFreeBlock;	/*<< The next free block in the list. */
	size_t xBlockSize;						/*<< The size of the free block. */
} xBlockLink;

/*-----------------------------------------------------------*/

/*
 * Inserts a block of memory that is being freed into the correct position in 
 * the list of free memory blocks.  The block being freed will be merged with
 * the block in front it and/or the block behind it if the memory blocks are
 * adjacent to each other.
 */
static void prvInsertBlockIntoFreeList( xBlockLink *pxBlockToInsert );

/*
 * Called automatically to setup the required heap structures the first time
 * pvPortMalloc() is called.
 */
//void prvHeapInit( void );

/*-----------------------------------------------------------*/

/* The size of the structure placed at the beginning of each allocated memory
block must by correctly byte aligned. */
#define heapSTRUCT_SIZE	( ( sizeof ( xBlockLink ) + ( portBYTE_ALIGNMENT - 1 ) ) & ~portBYTE_ALIGNMENT_MASK )

/* Ensure the pxEnd pointer will end up on the correct byte alignment. */
//static const size_t xTotalHeapSize = ( ( size_t ) heapADJUSTED_HEAP_SIZE ) & ( ( size_t ) ~portBYTE_ALIGNMENT_MASK );
//static size_t xTotalHeapSize;

/* Create a couple of list links to mark the start and end of the list. */
static xBlockLink xStart, *pxEnd DATA_IRAM_ATTR; // = NULL;

/* Keeps track of the number of free bytes remaining, but says nothing about
fragmentation. */
//static size_t xFreeBytesRemaining = ( ( size_t ) heapADJUSTED_HEAP_SIZE ) & ( ( size_t ) ~portBYTE_ALIGNMENT_MASK );
static size_t xFreeBytesRemaining DATA_IRAM_ATTR;

/* Gets set to the top bit of an size_t type.  When this bit in the xBlockSize 
member of an xBlockLink structure is set then the block belongs to the 
application.  When the bit is free the block is still part of the free heap
space. */
static size_t xBlockAllocatedBit DATA_IRAM_ATTR;// = 0;

/*-----------------------------------------------------------*/

void *pvPortMalloc( size_t xWantedSize )
{
xBlockLink *pxBlock, *pxPreviousBlock, *pxNewBlockLink;
void *pvReturn = NULL;

//    os_printf("%s %d %d\n", __func__, xWantedSize, xFreeBytesRemaining);

	ets_intr_lock();
	{
		/* If this is the first call to malloc then the heap will require
		initialisation to setup the list of free blocks. */
#if 0
		if( pxEnd == NULL )
		{
			prvHeapInit();
		}
#endif
		/* Check the requested block size is not so large that the top bit is
		set.  The top bit of the block size member of the xBlockLink structure 
		is used to determine who owns the block - the application or the
		kernel, so it must be free. */
		if( ( xWantedSize & xBlockAllocatedBit ) == 0 )
		{
			/* The wanted size is increased so it can contain a xBlockLink
			structure in addition to the requested amount of bytes. */
			if( xWantedSize > 0 )
			{
				xWantedSize += heapSTRUCT_SIZE;

				/* Ensure that blocks are always aligned to the required number 
				of bytes. */
				if( ( xWantedSize & portBYTE_ALIGNMENT_MASK ) != 0x00 )
				{
					/* Byte alignment required. */
					xWantedSize += ( portBYTE_ALIGNMENT - ( xWantedSize & portBYTE_ALIGNMENT_MASK ) );
				}
			}

			if( ( xWantedSize > 0 ) && ( xWantedSize <= xFreeBytesRemaining ) )
			{
				/* Traverse the list from the start	(lowest address) block until 
				one	of adequate size is found. */
				pxPreviousBlock = &xStart;
				pxBlock = xStart.pxNextFreeBlock;
				while( ( pxBlock->xBlockSize < xWantedSize ) && ( pxBlock->pxNextFreeBlock != NULL ) )
				{
					pxPreviousBlock = pxBlock;
					pxBlock = pxBlock->pxNextFreeBlock;
				}

				/* If the end marker was reached then a block of adequate size 
				was	not found. */
				if( pxBlock != pxEnd )
				{
					/* Return the memory space pointed to - jumping over the 
					xBlockLink structure at its start. */
					pvReturn = ( void * ) ( ( ( unsigned char * ) pxPreviousBlock->pxNextFreeBlock ) + heapSTRUCT_SIZE );

					/* This block is being returned for use so must be taken out 
					of the list of free blocks. */
					pxPreviousBlock->pxNextFreeBlock = pxBlock->pxNextFreeBlock;

					/* If the block is larger than required it can be split into 
					two. */
					if( ( pxBlock->xBlockSize - xWantedSize ) > heapMINIMUM_BLOCK_SIZE )
					{
						/* This block is to be split into two.  Create a new 
						block following the number of bytes requested. The void 
						cast is used to prevent byte alignment warnings from the 
						compiler. */
						pxNewBlockLink = ( void * ) ( ( ( unsigned char * ) pxBlock ) + xWantedSize );

						/* Calculate the sizes of two blocks split from the 
						single block. */
						pxNewBlockLink->xBlockSize = pxBlock->xBlockSize - xWantedSize;
						pxBlock->xBlockSize = xWantedSize;

						/* Insert the new block into the list of free blocks. */
						prvInsertBlockIntoFreeList( ( pxNewBlockLink ) );
					}

					xFreeBytesRemaining -= pxBlock->xBlockSize;

					/* The block is being returned - it is allocated and owned
					by the application and has no "next" block. */
					pxBlock->xBlockSize |= xBlockAllocatedBit;
					pxBlock->pxNextFreeBlock = NULL;
				}
			}
		}
	}
	ets_intr_unlock();

	#if( configUSE_MALLOC_FAILED_HOOK == 1 )
	{
		if( pvReturn == NULL )
		{
			extern void vApplicationMallocFailedHook( void );
			vApplicationMallocFailedHook();
		}
	}
	#endif

//    os_printf("%s %x %x\n", __func__, pvReturn, pxBlock);
	return pvReturn;
}

//void *malloc(size_t nbytes) __attribute__((alias("pvPortMalloc")));

/*-----------------------------------------------------------*/

void vPortFree( void *pv )
{
unsigned char *puc = ( unsigned char * ) pv;
xBlockLink *pxLink;

//    os_printf("%s\n", __func__);
    
	if( pv != NULL )
	{
		/* The memory being freed will have an xBlockLink structure immediately
		before it. */
		puc -= heapSTRUCT_SIZE;

		/* This casting is to keep the compiler from issuing warnings. */
		pxLink = ( void * ) puc;

		/* Check the block is actually allocated. */
		configASSERT( ( pxLink->xBlockSize & xBlockAllocatedBit ) != 0 );
		configASSERT( pxLink->pxNextFreeBlock == NULL );
		
		if( ( pxLink->xBlockSize & xBlockAllocatedBit ) != 0 )
		{
			if( pxLink->pxNextFreeBlock == NULL )
			{
				/* The block is being returned to the heap - it is no longer
				allocated. */
				pxLink->xBlockSize &= ~xBlockAllocatedBit;

				ets_intr_lock();
				{
					/* Add this block to the list of free blocks. */
					xFreeBytesRemaining += pxLink->xBlockSize;
					prvInsertBlockIntoFreeList( ( ( xBlockLink * ) pxLink ) );
				}
				ets_intr_unlock();
			}
		}
	}

//	os_printf("%s %x %d\n", __func__, pv, xFreeBytesRemaining);
}

//void free(void *ptr) __attribute__((alias("vPortFree")));

/*-----------------------------------------------------------*/

void *pvPortCalloc(size_t count, size_t size)
{
  void *p;

  /* allocate 'count' objects of size 'size' */
  p = pvPortMalloc(count * size);
  if (p) {
    /* zero the memory */
	ets_memset(p, 0, count * size);
  }
  return p;
}

//void *calloc(size_t count, size_t nbytes) __attribute__((alias("pvPortCalloc")));

/*-----------------------------------------------------------*/

void *pvPortZalloc(size_t size)
{
     return pvPortCalloc(1, size);	
}

//void *zalloc(size_t nbytes) __attribute__((alias("pvPortZalloc")));

/*-----------------------------------------------------------*/

void *pvPortRealloc(void *mem, size_t newsize)
{
     void *p;  	
     p = pvPortMalloc(newsize);
     if (p) {
       /* zero the memory */
    	 ets_memcpy(p, mem, newsize);
    	 vPortFree(mem);
     }
     return p;     
}

//void *realloc(void *ptr, size_t nbytes) __attribute__((alias("pvPortRealloc")));

/*-----------------------------------------------------------*/

size_t xPortGetFreeHeapSize( void )
{
	return xFreeBytesRemaining;
}
/*-----------------------------------------------------------*/

void vPortInitialiseBlocks( void )
{
	/* This just exists to keep the linker quiet. */
}
/*-----------------------------------------------------------*/

void ICACHE_FLASH_ATTR prvHeapInit( void )
{
xBlockLink *pxFirstFreeBlock;
unsigned char *pucHeapEnd, *pucAlignedHeap;

	xFreeBytesRemaining = ( ( size_t ) heapADJUSTED_HEAP_SIZE ) & ( ( size_t ) ~portBYTE_ALIGNMENT_MASK );
	unsigned char *ucHeap = &_heap_start;

	/* Ensure the heap starts on a correctly aligned boundary. */
	pucAlignedHeap = ( unsigned char * ) ( ( ( portPOINTER_SIZE_TYPE ) &ucHeap[ portBYTE_ALIGNMENT ] ) & ( ( portPOINTER_SIZE_TYPE ) ~portBYTE_ALIGNMENT_MASK ) );

	/* xStart is used to hold a pointer to the first item in the list of free
	blocks.  The void cast is used to prevent compiler warnings. */
	xStart.pxNextFreeBlock = ( void * ) pucAlignedHeap;
	xStart.xBlockSize = ( size_t ) 0;

	/* pxEnd is used to mark the end of the list of free blocks and is inserted
	at the end of the heap space. */
	pucHeapEnd = pucAlignedHeap + xFreeBytesRemaining;
	pucHeapEnd -= heapSTRUCT_SIZE;
	pxEnd = ( void * ) pucHeapEnd;
	configASSERT( ( ( ( unsigned long ) pxEnd ) & ( ( unsigned long ) portBYTE_ALIGNMENT_MASK ) ) == 0UL );
	pxEnd->xBlockSize = 0;
	pxEnd->pxNextFreeBlock = NULL;

	/* To start with there is a single free block that is sized to take up the
	entire heap space, minus the space taken by pxEnd. */
	pxFirstFreeBlock = ( void * ) pucAlignedHeap;
	pxFirstFreeBlock->xBlockSize = xFreeBytesRemaining - heapSTRUCT_SIZE;
	pxFirstFreeBlock->pxNextFreeBlock = pxEnd;

	/* The heap now contains pxEnd. */
	xFreeBytesRemaining -= heapSTRUCT_SIZE;

	/* Work out the position of the top bit in a size_t variable. */
	xBlockAllocatedBit = ( ( size_t ) 1 ) << ( ( sizeof( size_t ) * heapBITS_PER_BYTE ) - 1 );
}
/*-----------------------------------------------------------*/

static void prvInsertBlockIntoFreeList( xBlockLink *pxBlockToInsert )
{
xBlockLink *pxIterator;
unsigned char *puc;

	/* Iterate through the list until a block is found that has a higher address than the block being inserted. */
	for( pxIterator = &xStart; pxIterator->pxNextFreeBlock < pxBlockToInsert; pxIterator = pxIterator->pxNextFreeBlock )
	{
		/* Nothing to do here, just iterate to the right position. */
	}

	/* Do the block being inserted, and the block it is being inserted after
	make a contiguous block of memory? */	
	puc = ( unsigned char * ) pxIterator;
	if( ( puc + pxIterator->xBlockSize ) == ( unsigned char * ) pxBlockToInsert )
	{
		pxIterator->xBlockSize += pxBlockToInsert->xBlockSize;
		pxBlockToInsert = pxIterator;
	}

	/* Do the block being inserted, and the block it is being inserted before
	make a contiguous block of memory? */
	puc = ( unsigned char * ) pxBlockToInsert;
	if( ( puc + pxBlockToInsert->xBlockSize ) == ( unsigned char * ) pxIterator->pxNextFreeBlock )
	{
		if( pxIterator->pxNextFreeBlock != pxEnd )
		{
			/* Form one big block from the two blocks. */
			pxBlockToInsert->xBlockSize += pxIterator->pxNextFreeBlock->xBlockSize;
			pxBlockToInsert->pxNextFreeBlock = pxIterator->pxNextFreeBlock->pxNextFreeBlock;
		}
		else
		{
			pxBlockToInsert->pxNextFreeBlock = pxEnd;
		}
	}
	else
	{
		pxBlockToInsert->pxNextFreeBlock = pxIterator->pxNextFreeBlock;		
	}

	/* If the block being inserted plugged a gab, so was merged with the block
	before and the block after, then it's pxNextFreeBlock pointer will have
	already been set, and should not be set here as that would make it point
	to itself. */
	if( pxIterator != pxBlockToInsert )
	{
		pxIterator->pxNextFreeBlock = pxBlockToInsert;
	}
}

// use os_printf_plus() SDK 1.1.2
size_t xPortWantedSizeAlign(size_t xSize)
{
	xSize += heapSTRUCT_SIZE; // + 0x10 SDK 1.1.2
	if((xSize & 7) != 0) {
		xSize &= ~7;
		xSize += 8;
	}
	return xSize;
}

#endif
