/*
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *
 *  Copyright (C) 2002-2020  The DOSBox Team
 *  Copyright (C) 2020-2020  The dosbox-staging team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */

#include <cassert>
#include <new>

#include "mem_unaligned.h"
#include "paging.h"
#include "types.h"

class CodePageHandler;

// basic cache block representation
class CacheBlock {
public:
	void Clear();

	// link this cache block to another block, index specifies the code
	// path (always zero for unconditional links, 0/1 for conditional ones
	void LinkTo(Bitu index, CacheBlock *toblock)
	{
		assert(toblock);
		link[index].to=toblock;
		link[index].next = toblock->link[index].from; // set target block
		toblock->link[index].from = this; // remember who links me
	}

	struct {
		uint16_t start, end; // where in the page is the original code
		CodePageHandler *handler; // page containing this code
	} page;

	struct {
		uint8_t *start; // where in the cache are we
		Bitu size;
		CacheBlock *next;
		// writemap masking maskpointer/start/length to allow holes in
		// the writemap
		uint8_t *wmapmask;
		uint16_t maskstart;
		uint16_t masklen;
	} cache;

	struct {
		Bitu index;
		CacheBlock *next;
	} hash;

	struct {
		CacheBlock *to; // this block can transfer control to the to-block
		CacheBlock *next;
		CacheBlock *from; // the from-block can transfer control
		                  // to this block
	} link[2];                // maximum two links (conditional jumps)

	CacheBlock *crossblock;
};

static struct {
	struct {
		CacheBlock *first;   // the first cache block in the list
		CacheBlock *active;  // the current cache block
		CacheBlock *free;    // pointer to the free list
		CacheBlock *running; // the last block that was entered for
		                     // execution
	} block;
	uint8_t *pos;                // position in the cache block
	CodePageHandler *free_pages; // pointer to the free list
	CodePageHandler *used_pages; // pointer to the list of used pages
	CodePageHandler *last_page;  // the last used page
} cache;

// cache memory pointers, to be malloc'd later
static uint8_t *cache_code_start_ptr = nullptr;
static uint8_t *cache_code = nullptr;
static uint8_t *cache_code_link_blocks = nullptr;

static CacheBlock *cache_blocks = nullptr;
static CacheBlock link_blocks[2]; // default linking (specially marked)

// the CodePageHandler class provides access to the contained
// cache blocks and intercepts writes to the code for special treatment
class CodePageHandler : public PageHandler {
public:
	CodePageHandler() = default;

	CodePageHandler(const CodePageHandler &) = delete; // prevent copying
	CodePageHandler &operator=(const CodePageHandler &) = delete; // prevent assignment

	void SetupAt(Bitu _phys_page,PageHandler * _old_pagehandler) {
		// initialize this codepage handler
		phys_page=_phys_page;
		// save the old pagehandler to provide direct read access to the
		// memory, and to be able to restore it later on
		old_pagehandler=_old_pagehandler;

		// adjust flags
		flags=old_pagehandler->flags|(cpu.code.big ? PFLAG_HASCODE32:PFLAG_HASCODE16);
		flags&=~PFLAG_WRITEABLE;

		active_blocks=0;
		active_count=16;

		// initialize the maps with zero (no cache blocks as well as
		// code present)
		memset(&hash_map,0,sizeof(hash_map));
		memset(&write_map,0,sizeof(write_map));
		if (invalidation_map) {
			delete [] invalidation_map;
			invalidation_map = nullptr;
		}
	}

	// clear out blocks that contain code which has been modified
	bool InvalidateRange(Bitu start, Bitu end)
	{
		Bits index=1+(end>>DYN_HASH_SHIFT);
		bool is_current_block = false; // if the current block is
		                               // modified, it has to be exited
		                               // as soon as possible

		Bit32u ip_point=SegPhys(cs)+reg_eip;
		ip_point=(PAGING_GetPhysicalPage(ip_point)-(phys_page<<12))+(ip_point&0xfff);
		while (index>=0) {
			Bitu map=0;
			// see if there is still some code in the range
			for (Bitu count=start;count<=end;count++) map+=write_map[count];
			if (!map)
				return is_current_block; // no more code, finished

			CacheBlock *block = hash_map[index];
			while (block) {
				CacheBlock *nextblock = block->hash.next;
				// test if this block is in the range
				if (start<=block->page.end && end>=block->page.start) {
					if (ip_point<=block->page.end && ip_point>=block->page.start) is_current_block=true;
					block->Clear(); // clear the block,
					                // decrements the
					                // write_map accordingly
				}
				block=nextblock;
			}
			index--;
		}
		return is_current_block;
	}

	uint8_t *alloc_invalidation_map() const
	{
		constexpr size_t map_size = 4096;
		uint8_t *map = new (std::nothrow) uint8_t[map_size];
		if (GCC_UNLIKELY(!map))
			E_Exit("failed to allocate invalidation_map");
		memset(map, 0, map_size);
		return map;
	}

	// the following functions will clean all cache blocks that are invalid
	// now due to the write

	void writeb(PhysPt addr, Bitu val) override
	{
		assert((old_pagehandler->flags & PFLAG_HASROM) == 0x0);
		assert(old_pagehandler->flags & PFLAG_READABLE);

		addr&=4095;
		if (host_readb(hostmem+addr)==(Bit8u)val) return;
		host_writeb(hostmem+addr,val);
		// see if there's code where we are writing to
		if (!write_map[addr]) {
			if (active_blocks)
				return; // still some blocks in this page
			active_count--;
			if (!active_count)
				Release(); // delay page releasing until
				           // active_count is zero
			return;
		} else if (!invalidation_map) {
			invalidation_map = alloc_invalidation_map();
		}
		invalidation_map[addr]++;
		InvalidateRange(addr,addr);
	}

	void writew(PhysPt addr, Bitu val) override
	{
		assert((old_pagehandler->flags & PFLAG_HASROM) == 0x0);
		assert(old_pagehandler->flags & PFLAG_READABLE);

		addr&=4095;
		if (host_readw(hostmem+addr)==(Bit16u)val) return;
		host_writew(hostmem+addr,val);
		// see if there's code where we are writing to
		if (!read_unaligned_uint16(&write_map[addr])) {
			if (active_blocks)
				return; // still some blocks in this page
			active_count--;
			if (!active_count)
				Release(); // delay page releasing until
				           // active_count is zero
			return;
		} else if (!invalidation_map) {
			invalidation_map = alloc_invalidation_map();
		}
		host_addw(&invalidation_map[addr], 0x0101);
		InvalidateRange(addr,addr+1);
	}

	void writed(PhysPt addr, Bitu val) override
	{
		assert((old_pagehandler->flags & PFLAG_HASROM) == 0x0);
		assert(old_pagehandler->flags & PFLAG_READABLE);

		addr&=4095;
		if (host_readd(hostmem+addr)==(Bit32u)val) return;
		host_writed(hostmem+addr,val);
		// see if there's code where we are writing to
		if (!read_unaligned_uint32(&write_map[addr])) {
			if (active_blocks)
				return; // still some blocks in this page
			active_count--;
			if (!active_count)
				Release(); // delay page releasing until
				           // active_count is zero
			return;
		} else if (!invalidation_map) {
			invalidation_map = alloc_invalidation_map();
		}
		host_addd(&invalidation_map[addr], 0x01010101);
		InvalidateRange(addr,addr+3);
	}

	bool writeb_checked(PhysPt addr, Bitu val) override
	{
		assert((old_pagehandler->flags & PFLAG_HASROM) == 0x0);
		assert(old_pagehandler->flags & PFLAG_READABLE);

		addr&=4095;
		if (host_readb(hostmem+addr)==(Bit8u)val) return false;
		// see if there's code where we are writing to
		if (!write_map[addr]) {
			if (!active_blocks) {
				// no blocks left in this page, still delay
				// the page releasing a bit
				active_count--;
				if (!active_count) Release();
			}
		} else {
			if (!invalidation_map)
				invalidation_map = alloc_invalidation_map();

			invalidation_map[addr]++;
			if (InvalidateRange(addr,addr)) {
				cpu.exception.which=SMC_CURRENT_BLOCK;
				return true;
			}
		}
		host_writeb(hostmem+addr,val);
		return false;
	}

	bool writew_checked(PhysPt addr, Bitu val) override
	{
		assert((old_pagehandler->flags & PFLAG_HASROM) == 0x0);
		assert(old_pagehandler->flags & PFLAG_READABLE);

		addr&=4095;
		if (host_readw(hostmem+addr)==(Bit16u)val) return false;
		// see if there's code where we are writing to
		if (!read_unaligned_uint16(&write_map[addr])) {
			if (!active_blocks) {
				// no blocks left in this page, still delay
				// the page releasing a bit
				active_count--;
				if (!active_count) Release();
			}
		} else {
			if (!invalidation_map)
				invalidation_map = alloc_invalidation_map();

			host_addw(&invalidation_map[addr], 0x0101);
			if (InvalidateRange(addr,addr+1)) {
				cpu.exception.which=SMC_CURRENT_BLOCK;
				return true;
			}
		}
		host_writew(hostmem+addr,val);
		return false;
	}

	bool writed_checked(PhysPt addr, Bitu val) override
	{
		assert((old_pagehandler->flags & PFLAG_HASROM) == 0x0);
		assert(old_pagehandler->flags & PFLAG_READABLE);

		addr&=4095;
		if (host_readd(hostmem+addr)==(Bit32u)val) return false;
		// see if there's code where we are writing to
		if (!read_unaligned_uint32(&write_map[addr])) {
			if (!active_blocks) {
				// no blocks left in this page, still delay
				// the page releasing a bit
				active_count--;
				if (!active_count) Release();
			}
		} else {
			if (!invalidation_map)
				invalidation_map = alloc_invalidation_map();

			host_addd(&invalidation_map[addr], 0x01010101);
			if (InvalidateRange(addr,addr+3)) {
				cpu.exception.which=SMC_CURRENT_BLOCK;
				return true;
			}
		}
		host_writed(hostmem+addr,val);
		return false;
	}

	// add a cache block to this page and note it in the hash map
	void AddCacheBlock(CacheBlock *block)
	{
		Bitu index=1+(block->page.start>>DYN_HASH_SHIFT);
		block->hash.next = hash_map[index]; // link to old block at
		                                    // index from the new block
		block->hash.index=index;
		hash_map[index] = block; // put new block at hash position
		block->page.handler=this;
		active_blocks++;
	}

	// there's a block whose code started in a different page
	void AddCrossBlock(CacheBlock *block)
	{
		block->hash.next=hash_map[0];
		block->hash.index=0;
		hash_map[0]=block;
		block->page.handler=this;
		active_blocks++;
	}

	// remove a cache block
	void DelCacheBlock(CacheBlock *block)
	{
		active_blocks--;
		active_count=16;
		CacheBlock **where = &hash_map[block->hash.index];
		while (*where != block) {
			where = &((*where)->hash.next);
			// Will crash if a block isn't found, which should never
			// happen.
		}
		*where = block->hash.next;

		// remove the cleared block from the write map
		if (GCC_UNLIKELY(block->cache.wmapmask!=NULL)) {
			// first part is not influenced by the mask
			for (Bitu i=block->page.start;i<block->cache.maskstart;i++) {
				if (write_map[i]) write_map[i]--;
			}
			Bitu maskct=0;
			// last part sticks to the writemap mask
			for (Bitu i=block->cache.maskstart;i<=block->page.end;i++,maskct++) {
				if (write_map[i]) {
					// only adjust writemap if it isn't masked
					if ((maskct>=block->cache.masklen) || (!block->cache.wmapmask[maskct])) write_map[i]--;
				}
			}
			free(block->cache.wmapmask);
			block->cache.wmapmask=NULL;
		} else {
			for (Bitu i=block->page.start;i<=block->page.end;i++) {
				if (write_map[i]) write_map[i]--;
			}
		}
	}

	void Release()
	{
		// revert to old handler
		MEM_SetPageHandler(phys_page,1,old_pagehandler);
		PAGING_ClearTLB();

		// remove page from the lists
		if (prev) prev->next=next;
		else cache.used_pages=next;
		if (next) next->prev=prev;
		else cache.last_page=prev;
		next=cache.free_pages;
		cache.free_pages=this;
		prev=0;
	}

	void ClearRelease()
	{
		// clear out all cache blocks in this page
		for (Bitu index=0;index<(1+DYN_PAGE_HASH);index++) {
			CacheBlock *block = hash_map[index];
			while (block) {
				CacheBlock *nextblock = block->hash.next;
				block->page.handler = 0; // no need, full clear
				block->Clear();
				block=nextblock;
			}
		}
		Release(); // now can release this page
	}

	CacheBlock *FindCacheBlock(Bitu start)
	{
		CacheBlock *block = hash_map[1 + (start >> DYN_HASH_SHIFT)];
		// see if there's a cache block present at the start address
		while (block) {
			if (block->page.start == start)
				return block; // found
			block=block->hash.next;
		}
		return 0; // none found
	}

	HostPt GetHostReadPt(Bitu phys_page) override
	{
		hostmem = old_pagehandler->GetHostReadPt(phys_page);
		return hostmem;
	}

	HostPt GetHostWritePt(Bitu phys_page) override
	{
		return GetHostReadPt(phys_page);
	}

public:
	// the write map, there are write_map[i] cache blocks that cover
	// the byte at address i
	uint8_t write_map[4096] = {};
	uint8_t *invalidation_map = nullptr;

	CodePageHandler *prev = nullptr;
	CodePageHandler *next = nullptr;

private:
	PageHandler *old_pagehandler = nullptr;

	// hash map to quickly find the cache blocks in this page
	CacheBlock *hash_map[1 + DYN_PAGE_HASH] = {};

	Bitu active_blocks = 0; // the number of cache blocks in this page
	Bitu active_count = 0;  // delaying parameter to not immediately release
	                        // a page
	HostPt hostmem = nullptr;
	Bitu phys_page = 0;
};

static inline void cache_add_unused_block(CacheBlock *block)
{
	// block has become unused, add it to the freelist
	block->cache.next = cache.block.free;
	cache.block.free = block;
}

static CacheBlock *cache_getblock()
{
	// get a free cache block and advance the free pointer
	CacheBlock *ret = cache.block.free;
	if (!ret)
		E_Exit("Ran out of CacheBlocks");
	cache.block.free=ret->cache.next;
	ret->cache.next=0;
	return ret;
}

void CacheBlock::Clear()
{
	Bitu ind;
	// check if this is not a cross page block
	if (hash.index) for (ind=0;ind<2;ind++) {
		CacheBlock * fromlink=link[ind].from;
		link[ind].from=0;
		while (fromlink) {
			CacheBlock * nextlink=fromlink->link[ind].next;
			// clear the next-link and let the block point to the
			// standard linkcode
			fromlink->link[ind].next=0;
			fromlink->link[ind].to=&link_blocks[ind];

			fromlink=nextlink;
		}
		if (link[ind].to!=&link_blocks[ind]) {
			// not linked to the standard linkcode, find the block
			// that links to this block
			CacheBlock **wherelink = &link[ind].to->link[ind].from;
			while (*wherelink != this && *wherelink) {
				wherelink = &(*wherelink)->link[ind].next;
			}
			// now remove the link
			if (*wherelink)
				*wherelink = (*wherelink)->link[ind].next;
			else
				LOG(LOG_CPU, LOG_ERROR)("Cache anomaly. please investigate");
		}
	} else {
		cache_add_unused_block(this);
	}
	if (crossblock) {
		// clear out the crossblock (in the page before) as well
		crossblock->crossblock=0;
		crossblock->Clear();
		crossblock=0;
	}
	if (page.handler) {
		// clear out the code page handler
		page.handler->DelCacheBlock(this);
		page.handler=0;
	}
	if (cache.wmapmask){
		free(cache.wmapmask);
		cache.wmapmask=NULL;
	}
}

static CacheBlock *cache_openblock()
{
	CacheBlock *block = cache.block.active;
	// check for enough space in this block
	Bitu size=block->cache.size;
	CacheBlock *nextblock = block->cache.next;
	if (block->page.handler)
		block->Clear();
	// block size must be at least CACHE_MAXSIZE
	while (size<CACHE_MAXSIZE) {
		if (!nextblock)
			goto skipresize;
		// merge blocks
		size+=nextblock->cache.size;
		CacheBlock *tempblock = nextblock->cache.next;
		if (nextblock->page.handler)
			nextblock->Clear();
		// block is free now
		cache_add_unused_block(nextblock);
		nextblock=tempblock;
	}
skipresize:
	// adjust parameters and open this block
	block->cache.size=size;
	block->cache.next=nextblock;
	cache.pos=block->cache.start;
	return block;
}

static void cache_closeblock()
{
	CacheBlock *block = cache.block.active;
	// links point to the default linking code
	block->link[0].to=&link_blocks[0];
	block->link[1].to=&link_blocks[1];
	block->link[0].from=0;
	block->link[1].from=0;
	block->link[0].next=0;
	block->link[1].next=0;
	// close the block with correct alignment
	Bitu written = (Bitu)(cache.pos - block->cache.start);
	if (written>block->cache.size) {
		if (!block->cache.next) {
			if (written > block->cache.size + CACHE_MAXSIZE)
				E_Exit("CacheBlock overrun 1 %" PRIuPTR,
				       written - block->cache.size);
		} else {
			E_Exit("CacheBlock overrun 2 written %" PRIuPTR " size %" PRIuPTR,
			       written, block->cache.size);
		}
	} else {
		Bitu new_size;
		Bitu left=block->cache.size-written;
		// smaller than cache align then don't bother to resize
		if (left>CACHE_ALIGN) {
			new_size=((written-1)|(CACHE_ALIGN-1))+1;
			CacheBlock *newblock = cache_getblock();
			// align block now to CACHE_ALIGN
			newblock->cache.start=block->cache.start+new_size;
			newblock->cache.size=block->cache.size-new_size;
			newblock->cache.next=block->cache.next;
			block->cache.next=newblock;
			block->cache.size=new_size;
		}
	}
	// advance the active block pointer
#if (C_DYNAMIC_X86)
	const bool cache_is_full = !block->cache.next;
#elif (C_DYNREC)
	const uint8_t *limit = (cache_code_start_ptr + CACHE_TOTAL - CACHE_MAXSIZE);
	const bool cache_is_full = (!block->cache.next ||
	                            (block->cache.next->cache.start > limit));
#endif
	if (cache_is_full) {
		// DEBUG_LOG_MSG("Cache full; restarting");
		cache.block.active=cache.block.first;
	} else {
		cache.block.active=block->cache.next;
	}
}

// place an 8bit value into the cache
static inline void cache_addb(uint8_t val)
{
	*cache.pos = val;
	cache.pos += sizeof(uint8_t);
}

// place a 16bit value into the cache
static inline void cache_addw(uint16_t val)
{
	write_unaligned_uint16(cache.pos, val);
	cache.pos += sizeof(uint16_t);
}

// place a 32bit value into the cache
static inline void cache_addd(uint32_t val)
{
	write_unaligned_uint32(cache.pos, val);
	cache.pos += sizeof(uint32_t);
}

// place a 64bit value into the cache
static inline void cache_addq(uint64_t val)
{
	write_unaligned_uint64(cache.pos, val);
	cache.pos += sizeof(uint64_t);
}

#if (C_DYNAMIC_X86)
static void gen_return(BlockReturn retcode);
#elif (C_DYNREC)
static void dyn_return(BlockReturn retcode, bool ret_exception);
static void dyn_run_code();
static void cache_block_before_close();
static void cache_block_closing(uint8_t *block_start, Bitu block_size);
#endif

/* Define temporary pagesize so the MPROTECT case and the regular case share as much code as possible */
#if (C_HAVE_MPROTECT)
#define PAGESIZE_TEMP PAGESIZE
#else
#define PAGESIZE_TEMP 4096
#endif

static bool cache_initialized = false;

static void cache_init(bool enable) {
	Bits i;
	if (enable) {
		// see if cache is already initialized
		if (cache_initialized) return;
		cache_initialized = true;
		if (cache_blocks == NULL) {
			// allocate the cache blocks memory
			cache_blocks = (CacheBlock *)malloc(CACHE_BLOCKS *
			                                    sizeof(CacheBlock));
			if (!cache_blocks)
				E_Exit("Allocating cache_blocks has failed");
			memset(cache_blocks, 0, sizeof(CacheBlock) * CACHE_BLOCKS);
			cache.block.free=&cache_blocks[0];
			// initialize the cache blocks
			for (i=0;i<CACHE_BLOCKS-1;i++) {
				cache_blocks[i].link[0].to = (CacheBlock *)1;
				cache_blocks[i].link[1].to = (CacheBlock *)1;
				cache_blocks[i].cache.next = &cache_blocks[i + 1];
			}
		}
		if (cache_code_start_ptr==NULL) {
			// allocate the code cache memory
#if defined (WIN32)
			cache_code_start_ptr=(Bit8u*)VirtualAlloc(0,CACHE_TOTAL+CACHE_MAXSIZE+PAGESIZE_TEMP-1+PAGESIZE_TEMP,
				MEM_COMMIT,PAGE_EXECUTE_READWRITE);
			if (!cache_code_start_ptr)
				cache_code_start_ptr=(Bit8u*)malloc(CACHE_TOTAL+CACHE_MAXSIZE+PAGESIZE_TEMP-1+PAGESIZE_TEMP);
#else
			cache_code_start_ptr=(Bit8u*)malloc(CACHE_TOTAL+CACHE_MAXSIZE+PAGESIZE_TEMP-1+PAGESIZE_TEMP);
#endif
			if (!cache_code_start_ptr)
				E_Exit("Allocating dynamic core cache memory failed");

			// align the cache at a page boundary
			cache_code = (Bit8u *)(((Bitu)cache_code_start_ptr +
			                        PAGESIZE_TEMP - 1) &
			                       ~(PAGESIZE_TEMP - 1)); // Bitu is
			                                              // same size
			                                              // as a
			                                              // pointer.

			cache_code_link_blocks=cache_code;
			cache_code += PAGESIZE_TEMP;

#if (C_HAVE_MPROTECT)
			if(mprotect(cache_code_link_blocks,CACHE_TOTAL+CACHE_MAXSIZE+PAGESIZE_TEMP,PROT_WRITE|PROT_READ|PROT_EXEC))
				LOG_MSG("Setting execute permission on the code cache has failed");
#endif
			CacheBlock *block = cache_getblock();
			cache.block.first=block;
			cache.block.active=block;
			block->cache.start=&cache_code[0];
			block->cache.size=CACHE_TOTAL;
			block->cache.next = 0; // last block in the list
		}
		// setup the default blocks for block linkage returns
		cache.pos=&cache_code_link_blocks[0];
#if (C_DYNAMIC_X86)
		link_blocks[0].cache.start=cache.pos;
		gen_return(BR_Link1);
		cache.pos=&cache_code_link_blocks[32];
		link_blocks[1].cache.start=cache.pos;
		gen_return(BR_Link2);
#elif (C_DYNREC)
		core_dynrec.runcode = (BlockReturn(*)(uint8_t *))cache.pos;
		// can use op to PAGESIZE_TEMP-64 bytes
		dyn_run_code();
		cache_block_before_close();
		cache_block_closing(cache_code_link_blocks,
		                    cache.pos - cache_code_link_blocks);

		cache.pos = &cache_code_link_blocks[PAGESIZE_TEMP - 64];
		link_blocks[0].cache.start = cache.pos;
		// link code that returns with a special return code
		// must be less than 32 bytes
		dyn_return(BR_Link1, false);
		cache_block_before_close();
		cache_block_closing(link_blocks[0].cache.start,
		                    cache.pos - link_blocks[0].cache.start);

		cache.pos = &cache_code_link_blocks[PAGESIZE_TEMP - 32];
		link_blocks[1].cache.start = cache.pos;
		// link code that returns with a special return code
		// must be less than 32 bytes
		dyn_return(BR_Link2, false);
		cache_block_before_close();
		cache_block_closing(link_blocks[1].cache.start,
		                    cache.pos - link_blocks[1].cache.start);
#endif

		cache.free_pages=0;
		cache.last_page=0;
		cache.used_pages=0;
		// setup the code pages
		for (i=0;i<CACHE_PAGES;i++) {
			CodePageHandler *newpage = new CodePageHandler();
			newpage->next=cache.free_pages;
			cache.free_pages=newpage;
		}
	}
}

static void cache_close(void) {
/*	for (;;) {
		if (cache.used_pages) {
			CodePageHandler * cpage=cache.used_pages;
			CodePageHandler * npage=cache.used_pages->next;
			cpage->ClearRelease();
			delete cpage;
			cache.used_pages=npage;
		} else break;
	}
	if (cache_blocks != NULL) {
		free(cache_blocks);
		cache_blocks = NULL;
	}
	if (cache_code_start_ptr != NULL) {
		### care: under windows VirtualFree() has to be used if
		###       VirtualAlloc was used for memory allocation
		free(cache_code_start_ptr);
		cache_code_start_ptr = NULL;
	}
	cache_code = NULL;
	cache_code_link_blocks = NULL;
	cache_initialized = false; */
}