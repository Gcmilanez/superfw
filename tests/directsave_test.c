#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdbool.h>

#include "directsave_test_sram.h"
#include "directsave.h"

#define SAVE_SIZE        (128 * 1024)
#define SECTOR           4096
#define SIGNATURE_OFF    0xFF8
#define SAVE_BASE_LBA    100

int ds_read_flash(uint8_t *buf, uint32_t offset, uint32_t bytecount);
int ds_write_sector_flash(const uint8_t *buf, uint32_t sectnum);
int ds_erase_chip_flash(void);
int ds_erase_sector_flash(uint32_t sectnum);
int ds_write_byte_flash(uint32_t offset, uint8_t value);

uint8_t fake_sram[0x10000];
static uint8_t fake_sd[SAVE_SIZE];
static unsigned verified_reads;
static bool verified_reads_fail;
static bool writes_fail;

uint32_t base_sector(void) {
  return SAVE_BASE_LBA;
}

uint32_t get_memory_size(void) {
  return SAVE_SIZE;
}

static uint8_t *sd_block(uint32_t blocknum) {
  assert(blocknum >= SAVE_BASE_LBA);
  assert((blocknum - SAVE_BASE_LBA) * 512 < SAVE_SIZE);
  return &fake_sd[(blocknum - SAVE_BASE_LBA) * 512];
}

unsigned sdcard_read_blocks(uint8_t *buffer, uint32_t blocknum, unsigned blkcnt) {
  for (unsigned i = 0; i < blkcnt; i++)
    memcpy(&buffer[i * 512], sd_block(blocknum + i), 512);
  return 0;
}

unsigned sdcard_write_blocks(const uint8_t *buffer, uint32_t blocknum, unsigned blkcnt) {
  if (writes_fail)
    return 1;
  for (unsigned i = 0; i < blkcnt; i++)
    memcpy(sd_block(blocknum + i), &buffer[i * 512], 512);
  return 0;
}

unsigned sdcard_read_block_verified(uint8_t *buffer, uint32_t blocknum) {
  verified_reads++;
  if (verified_reads_fail)
    return 1;
  memcpy(buffer, sd_block(blocknum), 512);
  return 0;
}

static void boot_with_save(const uint8_t *save) {
  memset(fake_sram, 0x5A, sizeof(fake_sram));
  memset(&fake_sram[DIRSAV_CACHE_META_OFF], 0xFF, DIRSAV_CACHE_META_SIZE);

  t_dirsave_config cfg = {
    .magic = DIRSAV_CFG_MAGIC,
    .nrandom = 0x1234,
    .memory_size = SAVE_SIZE,
    .base_sector = SAVE_BASE_LBA,
  };
  uint32_t words[DIRSAV_CFG_SIZE / 4];
  memcpy(words, &cfg, sizeof(cfg));
  for (unsigned i = 0; i < DIRSAV_CFG_SIZE / 4; i++)
    cfg.checksum ^= words[i];
  memcpy(&fake_sram[sizeof(fake_sram) - sizeof(cfg)], &cfg, sizeof(cfg));

  memcpy(fake_sd, save, SAVE_SIZE);
  verified_reads = 0;
  verified_reads_fail = false;
  writes_fail = false;
}

static void random_bytes(uint8_t *buf, unsigned len) {
  for (unsigned i = 0; i < len; i++)
    buf[i] = rand();
}

static void emerald_link_save(const unsigned *sectors, unsigned count, uint8_t contents[][SECTOR]) {
  for (unsigned s = 0; s < count; s++) {
    assert(!ds_erase_sector_flash(sectors[s]));
    for (unsigned i = 0; i < SIGNATURE_OFF; i++)
      assert(!ds_write_byte_flash(sectors[s] * SECTOR + i, contents[s][i]));
    for (unsigned i = SIGNATURE_OFF + 1; i < SECTOR; i++)
      assert(!ds_write_byte_flash(sectors[s] * SECTOR + i, contents[s][i]));
  }
  for (unsigned s = 0; s < count; s++)
    assert(!ds_write_byte_flash(sectors[s] * SECTOR + SIGNATURE_OFF, contents[s][SIGNATURE_OFF]));
}

static void test_battle_tent_save_writes_every_byte_without_reading_sd(void) {
  static uint8_t save[SAVE_SIZE], expected[SAVE_SIZE];
  static uint8_t contents[5][SECTOR];
  const unsigned sectors[5] = { 26, 27, 14, 15, 16 };

  random_bytes(save, sizeof(save));
  random_bytes(&contents[0][0], sizeof(contents));
  boot_with_save(save);

  emerald_link_save(sectors, 5, contents);

  memcpy(expected, save, sizeof(expected));
  for (unsigned s = 0; s < 5; s++)
    memcpy(&expected[sectors[s] * SECTOR], contents[s], SECTOR);
  assert(!memcmp(fake_sd, expected, SAVE_SIZE));
  assert(verified_reads == 0);
}

static void test_second_battle_tent_save_in_same_session_is_also_correct(void) {
  static uint8_t save[SAVE_SIZE], expected[SAVE_SIZE];
  static uint8_t first[5][SECTOR], second[5][SECTOR];
  const unsigned first_sectors[5] = { 26, 27, 14, 15, 16 };
  const unsigned second_sectors[5] = { 3, 4, 5, 6, 7 };

  random_bytes(save, sizeof(save));
  random_bytes(&first[0][0], sizeof(first));
  random_bytes(&second[0][0], sizeof(second));
  boot_with_save(save);

  emerald_link_save(first_sectors, 5, first);
  emerald_link_save(second_sectors, 5, second);

  memcpy(expected, save, sizeof(expected));
  for (unsigned s = 0; s < 5; s++) {
    memcpy(&expected[first_sectors[s] * SECTOR], first[s], SECTOR);
    memcpy(&expected[second_sectors[s] * SECTOR], second[s], SECTOR);
  }
  assert(!memcmp(fake_sd, expected, SAVE_SIZE));
  assert(verified_reads == 0);
}

static void test_signature_byte_on_sector_from_previous_session_reads_its_block_once(void) {
  static uint8_t save[SAVE_SIZE], expected[SAVE_SIZE];
  random_bytes(save, sizeof(save));
  boot_with_save(save);

  assert(!ds_write_byte_flash(9 * SECTOR + SIGNATURE_OFF, 0x25));

  memcpy(expected, save, sizeof(expected));
  expected[9 * SECTOR + SIGNATURE_OFF] = 0x25;
  assert(!memcmp(fake_sd, expected, SAVE_SIZE));
  assert(verified_reads == 1);
}

static void test_failed_sd_read_reports_error_and_keeps_sd_untouched(void) {
  static uint8_t save[SAVE_SIZE];
  random_bytes(save, sizeof(save));
  boot_with_save(save);
  verified_reads_fail = true;

  assert(ds_write_byte_flash(9 * SECTOR + SIGNATURE_OFF, 0x25) == -1);

  assert(!memcmp(fake_sd, save, SAVE_SIZE));
}

static void test_failed_sd_read_is_retried_on_next_write(void) {
  static uint8_t save[SAVE_SIZE], expected[SAVE_SIZE];
  random_bytes(save, sizeof(save));
  boot_with_save(save);
  verified_reads_fail = true;
  assert(ds_write_byte_flash(9 * SECTOR + 10, 0x11) == -1);

  verified_reads_fail = false;
  assert(!ds_write_byte_flash(9 * SECTOR + 20, 0x22));

  memcpy(expected, save, sizeof(expected));
  expected[9 * SECTOR + 20] = 0x22;
  assert(!memcmp(fake_sd, expected, SAVE_SIZE));
}

static void test_read_flash_sees_bytes_still_pending_in_cache(void) {
  static uint8_t save[SAVE_SIZE];
  random_bytes(save, sizeof(save));
  boot_with_save(save);

  assert(!ds_erase_sector_flash(5));
  for (unsigned i = 0; i < 10; i++)
    assert(!ds_write_byte_flash(5 * SECTOR + i, 0xA0 + i));

  uint8_t got[16];
  assert(!ds_read_flash(got, 5 * SECTOR, sizeof(got)));
  for (unsigned i = 0; i < 10; i++)
    assert(got[i] == 0xA0 + i);
  for (unsigned i = 10; i < sizeof(got); i++)
    assert(got[i] == 0xFF);
}

static void test_sector_write_replaces_cached_copy_of_that_sector(void) {
  static uint8_t save[SAVE_SIZE], newdata[SECTOR];
  random_bytes(save, sizeof(save));
  random_bytes(newdata, sizeof(newdata));
  boot_with_save(save);

  assert(!ds_erase_sector_flash(5));
  assert(!ds_write_sector_flash(newdata, 5));
  assert(!ds_write_byte_flash(5 * SECTOR + 100, 0xAB));

  newdata[100] = 0xAB;
  assert(!memcmp(&fake_sd[5 * SECTOR], newdata, SECTOR));
  assert(verified_reads == 1);
}

static void test_chip_erase_forgets_cached_sectors(void) {
  static uint8_t save[SAVE_SIZE];
  random_bytes(save, sizeof(save));
  boot_with_save(save);

  assert(!ds_erase_sector_flash(5));
  assert(!ds_erase_chip_flash());
  memset(&fake_sd[5 * SECTOR], 0x33, SECTOR);
  assert(!ds_write_byte_flash(5 * SECTOR + 100, 0xAB));

  assert(fake_sd[5 * SECTOR + 99] == 0x33);
  assert(fake_sd[5 * SECTOR + 100] == 0xAB);
  assert(verified_reads == 1);
}

static void test_corrupted_cache_metadata_is_discarded(void) {
  static uint8_t save[SAVE_SIZE];
  random_bytes(save, sizeof(save));
  boot_with_save(save);

  assert(!ds_erase_sector_flash(5));
  fake_sram[DIRSAV_CACHE_META_OFF + 12] ^= 0x01;
  assert(!ds_write_byte_flash(5 * SECTOR + 100, 0xAB));

  assert(fake_sd[5 * SECTOR + 99] == 0xFF);
  assert(verified_reads == 1);
}

static void test_reading_the_save_between_phases_keeps_cached_sectors(void) {
  static uint8_t save[SAVE_SIZE];
  random_bytes(save, sizeof(save));
  boot_with_save(save);

  for (unsigned s = 3; s < 8; s++) {
    assert(!ds_erase_sector_flash(s));
    for (unsigned i = 0; i < SIGNATURE_OFF; i++)
      assert(!ds_write_byte_flash(s * SECTOR + i, 0x11));
  }
  uint8_t got[SECTOR];
  assert(!ds_read_flash(got, 3 * SECTOR, sizeof(got)));
  for (unsigned s = 3; s < 8; s++)
    assert(!ds_write_byte_flash(s * SECTOR + SIGNATURE_OFF, 0x25));

  assert(got[0] == 0x11 && got[SIGNATURE_OFF] == 0xFF);
  assert(verified_reads == 0);
  for (unsigned s = 3; s < 8; s++)
    assert(fake_sd[s * SECTOR + SIGNATURE_OFF] == 0x25);
}

static void test_sector_write_keeps_other_cached_sectors(void) {
  static uint8_t save[SAVE_SIZE], newdata[SECTOR];
  random_bytes(save, sizeof(save));
  random_bytes(newdata, sizeof(newdata));
  boot_with_save(save);

  assert(!ds_erase_sector_flash(5));
  assert(!ds_erase_sector_flash(6));
  assert(!ds_write_sector_flash(newdata, 5));
  assert(!ds_write_byte_flash(6 * SECTOR + 100, 0xAB));

  assert(fake_sd[6 * SECTOR + 99] == 0xFF);
  assert(fake_sd[6 * SECTOR + 100] == 0xAB);
  assert(verified_reads == 0);
}

static void test_first_byte_at_flash_offset_zero_reaches_sd_immediately(void) {
  static uint8_t save[SAVE_SIZE];
  random_bytes(save, sizeof(save));
  boot_with_save(save);

  assert(!ds_erase_sector_flash(0));
  assert(!ds_write_byte_flash(0, 0x42));

  assert(fake_sd[0] == 0x42);
}

static void test_failed_sd_write_keeps_the_block_pending_until_it_succeeds(void) {
  static uint8_t save[SAVE_SIZE];
  random_bytes(save, sizeof(save));
  boot_with_save(save);

  assert(!ds_erase_sector_flash(5));
  for (unsigned i = 0; i < 10; i++)
    assert(!ds_write_byte_flash(5 * SECTOR + i, 0xA0 + i));
  writes_fail = true;
  assert(ds_write_byte_flash(5 * SECTOR + 600, 0x77) == -1);
  assert(ds_read_flash(&save[0], 5 * SECTOR, 10) == -1);

  writes_fail = false;
  uint8_t got[10];
  assert(!ds_read_flash(got, 5 * SECTOR, sizeof(got)));
  for (unsigned i = 0; i < 10; i++) {
    assert(got[i] == 0xA0 + i);
    assert(fake_sd[5 * SECTOR + i] == 0xA0 + i);
  }
}

static void test_failed_sd_write_aborts_erase_instead_of_replacing_the_pending_sector(void) {
  static uint8_t save[SAVE_SIZE];
  random_bytes(save, sizeof(save));
  boot_with_save(save);

  for (unsigned s = 0; s < 7; s++)
    assert(!ds_erase_sector_flash(s));
  for (unsigned i = 0; i < 10; i++)
    assert(!ds_write_byte_flash(i, 0xA0 + i));
  writes_fail = true;
  assert(ds_erase_sector_flash(7) == -1);

  writes_fail = false;
  uint8_t got[10];
  assert(!ds_read_flash(got, 0, sizeof(got)));
  for (unsigned i = 0; i < 10; i++) {
    assert(got[i] == 0xA0 + i);
    assert(fake_sd[i] == 0xA0 + i);
  }
}

static void test_failed_sd_read_does_not_evict_cached_sectors(void) {
  static uint8_t save[SAVE_SIZE];
  random_bytes(save, sizeof(save));
  boot_with_save(save);

  for (unsigned s = 0; s < 7; s++)
    assert(!ds_erase_sector_flash(s));
  verified_reads_fail = true;
  assert(ds_write_byte_flash(20 * SECTOR + SIGNATURE_OFF, 0x25) == -1);
  verified_reads_fail = false;
  verified_reads = 0;

  for (unsigned s = 0; s < 7; s++)
    assert(!ds_write_byte_flash(s * SECTOR + SIGNATURE_OFF, 0x25));

  assert(verified_reads == 0);
}

int main() {
  srand(85);
  test_battle_tent_save_writes_every_byte_without_reading_sd();
  test_second_battle_tent_save_in_same_session_is_also_correct();
  test_signature_byte_on_sector_from_previous_session_reads_its_block_once();
  test_failed_sd_read_reports_error_and_keeps_sd_untouched();
  test_failed_sd_read_is_retried_on_next_write();
  test_read_flash_sees_bytes_still_pending_in_cache();
  test_sector_write_replaces_cached_copy_of_that_sector();
  test_chip_erase_forgets_cached_sectors();
  test_corrupted_cache_metadata_is_discarded();
  test_reading_the_save_between_phases_keeps_cached_sectors();
  test_sector_write_keeps_other_cached_sectors();
  test_first_byte_at_flash_offset_zero_reaches_sd_immediately();
  test_failed_sd_write_keeps_the_block_pending_until_it_succeeds();
  test_failed_sd_write_aborts_erase_instead_of_replacing_the_pending_sector();
  test_failed_sd_read_does_not_evict_cached_sectors();
  printf("directsave tests OK\n");
  return 0;
}
