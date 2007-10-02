/*
 * poolid_private.h
 * 
 */

#ifndef POOLID_PRIVATE_H
#define POOLID_PRIVATE_H

// the size of all buffers is incremented in blocks
// these are the block values (increment values) for the
// string hashtable, rel hashtable, stringspace buffer and idarray
// 
#define STRING_BLOCK      2047          // hashtable for strings
#define REL_BLOCK         1023          // hashtable for relations
#define STRINGSPACE_BLOCK 65535         // string buffer

#endif /* POOLID_PRIVATE_H */
