#ifndef CD_H
#define CD_H

#include "base.h"

#define HIRQ 0x0008UL
#define DRDY 0x0002 /* Data transfer preparations complete */
#define EHST 0x0080 /* Host I/O processing complete */

// Read 2 bytes from register
#define CD_BLOCK_DATA_2 0x25890000UL

static uint32_t numSectorsForSize(uint32_t size) {
  // Past size so we get the complete number of sectors for the whole data.
  return (size + (CDFS_SECTOR_SIZE - 1)) / CDFS_SECTOR_SIZE;
}

static void queueDiskRead(uint32_t fad, uint32_t size) {
  DEBUG_REQUIRE_NE(size, 0);
  DEBUG_REQUIRE_EQ(size % 4, 0);

  /* Saber Rider's stdio streamer may still have a long disk_play queued after
   * a short read.  Do what cd_sat.c does for a seek: reset selector 0 and
   * replace that stream immediately instead of waiting for PAUSE. */
  int status __unused = cd_block_cmd_selector_reset(0, 0);
  DEBUG_REQUIRE_EQ(status, 0);

  status = cd_block_cmd_cd_dev_connection_set(0);
  DEBUG_REQUIRE_EQ(status, 0);

  status = cd_block_cmd_disk_play(0, fad, numSectorsForSize(size));
  DEBUG_REQUIRE_EQ(status, 0);
}

static uint32_t getSectorsReady(uint32_t wantSectors) {
  int32_t sectorsReady;
  while (true) {
    sectorsReady = cd_block_cmd_sector_number_get(0);
    if (sectorsReady >= wantSectors) {
      break;
    } else {
      for (volatile int32_t i = 0; i < 1024; ++i) { cpu_instr_nop(); }
    }
  }

  return sectorsReady;
}

static void cdCmdDataTransferEnd() {
  cd_block_cmd_data_transfer_end();
}

static void waitUntilCdDataIsAvailable() {
  // Wait until data is available
  bool ready __unused = false;
  for (volatile int32_t i = 0; i < 240000; ++i) {
    if (MEMORY_READ(16, CD_BLOCK(HIRQ)) & DRDY) {
      ready = true;
      break;
    }
  }

  DEBUG_REQUIRE(ready);
}

#endif //CD_H
