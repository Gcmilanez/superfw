/*
 * Copyright (C) 2025 David Guillen Fandos <david@davidgf.net>
 *
 * This program is free software: you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.	 See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see
 * <http://www.gnu.org/licenses/>.
 */

#include <stdint.h>
#include <stdbool.h>

#include "directsave.h"
#include "supercard_driver.h"

#ifndef SRAM_BASE
  #define SRAM_BASE          0x0E000000
#endif

uint32_t base_sector(void);
uint32_t get_memory_size(void);

static inline uint32_t min32(uint32_t a, uint32_t b) {
  return (a < b) ? a : b;
}

// Check that the config on SRAM is valid.
bool validate_config() {
  union {
    t_dirsave_config cfg;
    uint8_t bytes[0];
    uint32_t words[0];
  } u;
  volatile uint8_t *sramcfg = (uint8_t*)(SRAM_BASE + 0x10000 - sizeof(t_dirsave_config));

  for (unsigned i = 0; i < sizeof(t_dirsave_config); i++)
    u.bytes[i] = sramcfg[i];

  if (u.cfg.magic != DIRSAV_CFG_MAGIC)
    return false;

  // Clear mutex variable to calculate the checksum.
  u.cfg.sd_mutex = 0;

  uint32_t crc = 0;
  for (unsigned i = 0; i < sizeof(t_dirsave_config) / sizeof(uint32_t); i++)
    crc ^= u.words[i];

  return crc == 0;
}

// EEPROM handlers
// SRAM and EEPROM save modes always load savegame data to SRAM, regardless
// of the savepatch used. This allows us to read straight from SRAM and just
// flush on writes.

// Reads EEPROM data, directly from SRAM (cached)
int ds_read_eeprom(uint32_t block_num, uint8_t *buf) {
  if (!validate_config())
    return -1;

  // Check memory size to avoid overflow
  if (block_num * 8 >= get_memory_size())
    return -1;

  // We read straight from RAM.
  const volatile uint8_t *sram = (uint8_t*)(SRAM_BASE);
  for (unsigned i = 0; i < 8; i++)
    buf[i] = sram[block_num * 8 + 7 - i];

  return 0;
}

// Writes EEPROM data, updates SRAM buffer and flushes to SD card.
int ds_write_eeprom(uint32_t block_num, const uint8_t *buf) {
  if (!validate_config())
    return -1;

  // Check memory size to avoid overflow
  if (block_num * 8 >= get_memory_size())
    return -1;

  const unsigned sram_off = block_num * 8;
  const unsigned sram_sdoff = sram_off & ~511;

  // Update the data on SRAM too for faster reads.
  volatile uint8_t *sram = (uint8_t*)(SRAM_BASE);
  for (unsigned i = 0; i < 8; i++)
    sram[sram_off + 7 - i] = buf[i];

  // We flush the updated SD sector.
  const uint32_t sdblocknum = block_num / (512 / 8) + base_sector();
  unsigned ret = sdcard_write_blocks((uint8_t*)&sram[sram_sdoff], sdblocknum, 1);
  return ret ? -1 : 0;
}

// FLASH handlers
// Read and write data from/to SD card. Reads tend to be large, but smaller
// reads are usually not very efficient. Writes are easy, since we always
// write full 4KiB blocks. Except for byte writes, more on that later :)
// The SRAM is used as scratch/buffer. The first 32KiB are a scratch buffer,
// the last few bytes, store the DirSav config. We also have a cache for 
// Byte-write cache : reading a block back from the SD card
// right after writing it can return shifted data on real hardware, so recently
// erased/written sectors are kept in SRAM (0x8000-0xEFFF) and byte writes are
// written through to the SD card without reading it.

#define FLASH_CACHE_MAGIC    0xCAC4ED68

#define SRAM_CACHE_DATA      (SRAM_BASE + 0x8000)   // 7 sectors (28KiB) of cached data
#define SRAM_CACHE_METADATA  (SRAM_BASE + DIRSAV_CACHE_META_OFF)
#define CACHE_SLOTS          7     // Pokemon touches 5 sectors per link/frontier save
#define NO_SECTOR            0xFF  // Marks an unused cache slot
#define NO_ADDR              0xFFFFFFFFU  // last_addr value when no byte was written yet

// Cache metadata, stored in SRAM (accessed byte by byte, since SRAM is 8 bit).
typedef union {
  struct {
    uint32_t magic;            // Cache magic number
    uint32_t last_addr;        // Last flash addr written by the WriteByte function.
    uint32_t dirty;            // Whether the block holding last_addr is not yet on the SD card.
    uint8_t sector[CACHE_SLOTS];  // Flash sector number held by each slot (or NO_SECTOR)
    uint8_t valid[CACHE_SLOTS];   // Bitmask of the 512 byte blocks (of 8) that hold valid data
    uint8_t victim;            // Next slot to replace (round robin)
    uint8_t unused;
    uint32_t checksum;         // XOR checksum of this structure.
  };
  uint32_t words[DIRSAV_CACHE_META_SIZE / 4];
  uint8_t bytes[DIRSAV_CACHE_META_SIZE];
} t_cache_meta;

_Static_assert(sizeof(t_cache_meta) == DIRSAV_CACHE_META_SIZE, "t_cache_meta size mismatch");
_Static_assert(0x8000 + CACHE_SLOTS * 4096 <= DIRSAV_CACHE_META_OFF, "flash cache overlaps its metadata");

static uint32_t cache_checksum(const t_cache_meta *cache) {
  uint32_t res = 0;
  for (unsigned i = 0; i < DIRSAV_CACHE_META_SIZE / 4 - 1; i++)
    res ^= cache->words[i];
  return res;
}

// Drops all cached data. Callers must flush the dirty block first.
static void empty_cache(t_cache_meta *cache) {
  cache->last_addr = NO_ADDR;
  cache->dirty = 0;
  for (unsigned i = 0; i < CACHE_SLOTS; i++) {
    cache->sector[i] = NO_SECTOR;
    cache->valid[i] = 0;
  }
  cache->victim = 0;
  cache->unused = 0;
}

// Load the cache from SRAM, check that the struct is valid.
// Invalid/corrupted metadata results in an empty cache (data is read from SD).
static void load_cache_metadata(t_cache_meta *cache) {
  volatile uint8_t *metablk = (uint8_t*)SRAM_CACHE_METADATA;
  for (unsigned i = 0; i < sizeof(t_cache_meta); i++)
    cache->bytes[i] = metablk[i];

  if (cache->magic != FLASH_CACHE_MAGIC || cache->checksum != cache_checksum(cache))
    empty_cache(cache);
}

// Writes the metadata back into SRAM.
static void save_cache_metadata(t_cache_meta *cache) {
  cache->magic = FLASH_CACHE_MAGIC;
  cache->checksum = cache_checksum(cache);

  volatile uint8_t *metablk = (uint8_t*)SRAM_CACHE_METADATA;
  for (unsigned i = 0; i < sizeof(t_cache_meta); i++)
    metablk[i] = cache->bytes[i];
}

// Returns the SRAM address of the 512 byte block containing flash address addr.
static uint8_t *cached_block(unsigned slot, uint32_t addr) {
  return (uint8_t*)(SRAM_CACHE_DATA + slot * 4096U + (addr & 4095U & ~511U));
}

// Returns the slot holding the flash sector, or -1 if it is not cached.
static int find_slot(const t_cache_meta *cache, uint32_t sectnum) {
  for (unsigned i = 0; i < CACHE_SLOTS; i++)
    if (cache->sector[i] == sectnum)
      return i;
  return -1;
}

// Drops the cached copy of a sector (if any).
static void forget_sector(t_cache_meta *cache, uint32_t sectnum) {
  const int slot = find_slot(cache, sectnum);
  if (slot >= 0) {
    cache->sector[slot] = NO_SECTOR;
    cache->valid[slot] = 0;
  }
}

// Returns the slot for a sector, replacing the oldest one if it is not cached.
// The replaced slot is never dirty, since callers flush the dirty block first.
static unsigned claim_slot(t_cache_meta *cache, uint32_t sectnum) {
  const int found = find_slot(cache, sectnum);
  if (found >= 0)
    return found;

  const unsigned slot = cache->victim;
  cache->victim = (slot + 1) % CACHE_SLOTS;
  cache->sector[slot] = sectnum;
  cache->valid[slot] = 0;
  return slot;
}

// Writes the pending (dirty) block to the SD card, if any.
// On a write error the block stays dirty, so that it is retried later. Callers
// must then abort, since a dirty slot must never be replaced.
static unsigned flush_dirty_block(t_cache_meta *cache) {
  if (!cache->dirty)
    return 0;

  const int slot = find_slot(cache, cache->last_addr / 4096U);
  if (slot < 0) {
    cache->dirty = 0;   // Should not happen: the data is gone, report it.
    return SD_ERR_BADWRITE;
  }
  if (sdcard_write_blocks(cached_block(slot, cache->last_addr), base_sector() + cache->last_addr / 512U, 1))
    return SD_ERR_BADWRITE;

  cache->dirty = 0;
  return 0;
}

// Writes the pending block (if any) to the SD card, so that the SD card holds
// the latest data. The cache stays valid, since its content matches the card.
static int flush_pending_block() {
  t_cache_meta cacheinfo;
  load_cache_metadata(&cacheinfo);
  const unsigned errs = flush_dirty_block(&cacheinfo);
  save_cache_metadata(&cacheinfo);
  return errs ? -1 : 0;
}

// Reads flash bytes (directly from SD card) into a user-defined buffer.
// Uses the first 32KiB of SRAM as scratch area.
int ds_read_flash(uint8_t *buf, uint32_t offset, uint32_t bytecount) {
  if (!validate_config())
    return -1;

  if (flush_pending_block())
    return -1;

  const uint32_t msize = get_memory_size();
  if (offset > msize || bytecount > msize || offset + bytecount > msize)
    return -1;

  // We must read 512byte aligned blocks from SD card, therefore we
  // start reading from the first sector, and keep doing that. We read up to
  // 64 blocks in one go.
  const uint32_t basen = base_sector();

  uint8_t *tmpbuf = (uint8_t*)SRAM_BASE;
  while (bytecount) {
    uint32_t start_blk = offset / 512U;
    uint32_t end_blk = (offset + bytecount - 1U) / 512U;
    unsigned bcnt = end_blk - start_blk + 1U;
    if (bcnt > 64U)
      bcnt = 64U;      // Limit to 32KiB

    unsigned ret = sdcard_read_blocks(tmpbuf, basen + start_blk, bcnt);
    if (ret)
      return -1;

    // Copy the data we read to the user buffer
    unsigned blkoff = offset & 511U;
    unsigned tocpy = min32(bytecount, bcnt * 512U - blkoff);        // Max data we can copy.

    // Copy data to user buffer, advance pointer.
    for (unsigned i = 0; i < tocpy; i++)
      *buf++ = tmpbuf[blkoff + i];
    // Adjust the offsets and byte counts.
    offset += tocpy;
    bytecount -= tocpy;
  }

  return 0;
}

// Writes a full sector (4KBytes) to the flash device. This was previously erased.
int ds_write_sector_flash(const uint8_t *buf, uint32_t sectnum) {
  const uint32_t blpersector = 4096 / 512;

  if (!validate_config())
    return -1;
  if (sectnum * 4096 >= get_memory_size())
    return -1;

  // Flush any pending data and drop the cached copy of this sector (only).
  t_cache_meta cacheinfo;
  load_cache_metadata(&cacheinfo);
  if (flush_dirty_block(&cacheinfo))
    return -1;
  forget_sector(&cacheinfo, sectnum);
  save_cache_metadata(&cacheinfo);

  if (sdcard_write_blocks(buf, base_sector() + sectnum * blpersector, blpersector))
    return -1;

  return 0;
}

// Erases the full chip (the entire flash memory)
int ds_erase_chip_flash(void) {
  const uint32_t blrun = 32;   // Erase 32 Blocks in a row.
  if (!validate_config())
    return -1;

  // Everything is about to be erased, pending data included: drop the cache.
  t_cache_meta cacheinfo;
  load_cache_metadata(&cacheinfo);
  empty_cache(&cacheinfo);
  save_cache_metadata(&cacheinfo);

  // Clear buffer and write that to the SD card
  uint8_t *tmpbuf = (uint8_t*)SRAM_BASE;
  for (unsigned i = 0; i < blrun*512; i++)
    tmpbuf[i] = 0xff;

  // Erase in 32 block chunks (16KiB)
  const uint32_t memblks = get_memory_size() / 512U;
  for (uint32_t s = 0; s < memblks; s += blrun)
    if (sdcard_write_blocks(tmpbuf, base_sector() + s, min32(blrun, memblks - s)))
      return -1;

  return 0;
}

// Erases one flash sector (4KiB).
int ds_erase_sector_flash(uint32_t sectnum) {
  const uint32_t blpersector = 4096 / 512;

  if (!validate_config())
    return -1;
  if (sectnum * 4096 >= get_memory_size())
    return -1;

  t_cache_meta cacheinfo;
  load_cache_metadata(&cacheinfo);
  if (flush_dirty_block(&cacheinfo))
    return -1;

  // Use the sector's cache slot as the 0xFF buffer: after erasing, the cached
  // copy is valid and later byte writes to this sector need no SD reads.
  const unsigned slot = claim_slot(&cacheinfo, sectnum);
  uint8_t *sectdata = cached_block(slot, 0);
  for (unsigned i = 0; i < 4096; i++)
    sectdata[i] = 0xff;

  const bool erased = !sdcard_write_blocks(sectdata, base_sector() + sectnum * blpersector, blpersector);
  cacheinfo.valid[slot] = erased ? 0xFF : 0;   // All 8 blocks are valid after erasing
  save_cache_metadata(&cacheinfo);

  return erased ? 0 : -1;
}

// Writes a single byte to flash. This routine is not used by most games.
// So far only Pokemon seems to use it, to perform a 2-phase write.
int ds_write_byte_flash(uint32_t offset, uint8_t value) {
  if (!validate_config())
    return -1;
  if (offset >= get_memory_size())
    return -1;

  const uint32_t blkn = offset / 512U;
  const unsigned blkbit = 1U << (blkn % 8);

  t_cache_meta cacheinfo;
  load_cache_metadata(&cacheinfo);

  // Moving to another block: write back the pending one first.
  if (cacheinfo.last_addr / 512U != blkn && flush_dirty_block(&cacheinfo))
    return -1;

  // Only read from the SD card if this block is not cached (ie. the sector
  // was not erased/written in this session). The read is CRC checked, so a
  // bad read results in an error (the game reports it) instead of corruption.
  // It goes to the scratch area first, so a failed read evicts nothing.
  int slot = find_slot(&cacheinfo, offset / 4096U);
  if (slot < 0 || !(cacheinfo.valid[slot] & blkbit)) {
    uint8_t *readbuf = (uint8_t*)SRAM_BASE;
    if (sdcard_read_block_verified(readbuf, base_sector() + blkn)) {
      save_cache_metadata(&cacheinfo);
      return -1;
    }

    slot = claim_slot(&cacheinfo, offset / 4096U);
    uint8_t *blkdata = cached_block(slot, offset);
    for (unsigned i = 0; i < 512; i++)
      blkdata[i] = readbuf[i];
    cacheinfo.valid[slot] |= blkbit;
  }

  // Patch the byte in the cached block, it is flushed below or on the next call.
  cached_block(slot, offset)[offset % 512U] = value;
  const bool sequential = cacheinfo.last_addr != NO_ADDR && cacheinfo.last_addr + 1 == offset;
  cacheinfo.last_addr = offset;
  cacheinfo.dirty = 1;

  // Is this the end of the block, or was this non-sequential?
  unsigned errs = 0;
  if ((offset % 512U) == 512 - 1 || !sequential)
    errs = flush_dirty_block(&cacheinfo);

  save_cache_metadata(&cacheinfo);

  return errs ? -1 : 0;
}

