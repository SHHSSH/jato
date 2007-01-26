/*
 * Copyright (C) 2006  Pekka Enberg
 *
 * This file is released under the GPL version 2. Please refer to the file
 * LICENSE for details.
 */

#include <vm/bitset.h>
#include <vm/system.h>

#include <stdlib.h>
#include <string.h>

#define BITS_PER_BYTE 8

/**
 *	bitset_alloc - Allocate a new bit set
 *	@size: Number of elements in the set.
 *
 *	This function creates a bit set that is large enough to represent
 *	bits with indices zero to @size-1.
 */
struct bitset *alloc_bitset(unsigned long nr_bits)
{
	struct bitset *bitset;
	unsigned long size;

	nr_bits = ALIGN(nr_bits, BITS_PER_LONG);
	size = sizeof(struct bitset) + (nr_bits / BITS_PER_BYTE);
	bitset = malloc(size);
	if (bitset) {
		memset(bitset, 0, size);
		bitset->nr_bits = nr_bits;
	}
	return bitset;
}

static inline unsigned long *addr_of(unsigned long *bitset, unsigned long bit)
{
	return bitset + (bit / BITS_PER_LONG);
}

static inline unsigned long bit_mask(unsigned long bit)
{
	return 1UL << (bit & (BITS_PER_LONG-1));
}

int test_bit(unsigned long *bitset, unsigned long bit)
{
	unsigned long *addr, mask;

	addr = addr_of(bitset, bit);
	mask = bit_mask(bit);

	return ((*addr & mask) != 0);
}

void set_bit(unsigned long *bitset, unsigned long bit)
{
	unsigned long *addr, mask;
	
	addr = addr_of(bitset, bit); 
	mask = bit_mask(bit);

	*addr |= mask;
}
