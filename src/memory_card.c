#include "memory_card.h"
#include <lfs.h>
#include <pico/malloc.h>
#include <pico/time.h>
#include <pico/multicore.h>
#include "led.h"
#include "config.h"

extern const struct lfs_config lfs_pico_flash_config;  // littlefs_driver.c
uint8_t mc_buffer[MC_SIZE] __attribute__ ((section (".uninitialized_data")));
const char *mc_filenames[] = { "WHITE.MCR", "RED.MCR", "ORANGE.MCR", "YELLOW.MCR", "GREEN.MCR", "BLUE.MCR", "INDIGO.MCR", "PURPLE.MCR"};

uint32_t memory_card_init(memory_card_t* mc) {
	if(!mc)
		return MC_NO_INIT;
	mc->data = mc_buffer;
	mc->flag_byte = MC_FLAG_BYTE_DEF;
	mc->out_of_sync = false;
	mc->last_operation_timestamp = 0;
	mc->index = MC_IMAGE_INDEX_NONE;
	return MC_OK;
}

void memory_card_format(memory_card_t* mc) {

	uint8_t *buffer = mc->data;
	memset(buffer,0,MC_SIZE);

	// Sector 1 (of 1024)
	/* header frame (block 0, sec 0) */
	buffer[0] = 'M';
	buffer[1] = 'C';
	buffer[MC_SEC_SIZE - 1] = 0x0E;

	/* directory frames (block 0, sec 1..15) */
	uint32_t offset = MC_SEC_SIZE;
	buffer[offset] = 0xA0;	// free block
	buffer[offset + 8] = buffer[offset + 9] = 0xFF;	// no next block
	buffer[offset + MC_SEC_SIZE - 1] = 0xA0;
	// Copy block 0, sec 1 fourteen times
	for(uint32_t i = 2; i < 16; i++) {
		memcpy(buffer + (MC_SEC_SIZE * i), buffer + offset, MC_SEC_SIZE);
	}

	/* broken sector list (block 0, sec 16..35) */
	offset = MC_SEC_SIZE * 16;
	buffer[offset] = buffer[offset + 1] = buffer[offset + 2] = buffer[offset + 3] = 0xff;	// no broken sector
	buffer[offset + 4] = buffer[offset + 5] = buffer[offset + 6] = buffer[offset + 7] = 0x00;	// 0 fill
	buffer[offset + 8] = buffer[offset + 9] = 0xff;	// 1 fill
	// buffer[offset + MC_SEC_SIZE - 1] = xor;
	// Copy block 0, sec 16 nineteen times
	for(uint32_t i = 17; i < 36; i++) {
		memcpy(buffer + (MC_SEC_SIZE * i), buffer + offset, MC_SEC_SIZE);
	}

	/* broken sector replacement data (block 0, sec 36..55) and unused frames (block 0, sec 56..62) */

	/* test write sector (block 0, sec 63) */
	offset = MC_SEC_SIZE * 63;
	buffer[offset + 0] = 'M';
	buffer[offset + 1] = 'C';
	buffer[offset + MC_SEC_SIZE - 1] = 0x0E;

	/* fill remaining 15 blocks with zeros */
}

uint32_t memory_card_import(memory_card_t* mc, uint32_t index) {
	uint32_t status = MC_OK;
	if (mc) {
		if (index < NUM_MEMORY_CARDS) {
			lfs_t lfs;
			lfs_file_t memcard;
			int lfs_err;
			if (LFS_ERR_OK == lfs_mount(&lfs, &lfs_pico_flash_config)) {
				lfs_err = lfs_file_open(&lfs, &memcard, mc_filenames[index], LFS_O_RDONLY);
				if (lfs_err == LFS_ERR_OK) {
					lfs_ssize_t size = lfs_file_read(&lfs, &memcard, mc->data, MC_SIZE);
					if (size < 0) {
						status = MC_FILE_READ_ERR;
					} else if (size != MC_SIZE) {
						status = MC_FILE_SIZE_ERR;
					}
					lfs_file_close(&lfs, &memcard);
				} else if (lfs_err == LFS_ERR_NOENT) {
					// If file is not found, intialize a new card
					memory_card_format(mc);
				} else status = MC_FILE_OPEN_ERR;
				lfs_unmount(&lfs);
			} else status = MC_MOUNT_ERR;
		} else status = MC_FILE_OPEN_ERR;
	} else status = MC_NO_INIT;

	if (status == MC_OK) {
		// If successfullly loaded, reset fields
		memory_card_set_sync(mc, false);
		mc->index = index;
		mc->flag_byte = MC_FLAG_BYTE_DEF;
		mc->last_operation_timestamp = 0;
	}

	return status;
}

bool memory_card_is_sector_valid(memory_card_t* mc, uint32_t sector) {
	(void) mc;
	if(sector < 0 || sector >= MC_SEC_COUNT)
		return false;
	return true;
}

uint8_t* memory_card_get_sector_ptr(memory_card_t* mc, uint32_t sector) {
	if(mc) {
		return &mc->data[sector * MC_SEC_SIZE];
	}
	return NULL;
}

void memory_card_set_sync(memory_card_t* mc, bool out_of_sync) {
	if(mc) {
		led_output_sync_status(out_of_sync);
		mc->out_of_sync = out_of_sync;
	}
}

bool memory_card_get_sync(memory_card_t* mc) {
	if(mc) {

		return mc->out_of_sync;
	}
}

void memory_card_update_timestamp(memory_card_t* mc) {
	if(mc) {
		mc->last_operation_timestamp = to_ms_since_boot(get_absolute_time());
	}
}

uint32_t memory_card_sync(memory_card_t* mc) {
	uint32_t status = MC_OK;
	if(mc) {
		if (mc->index < NUM_MEMORY_CARDS) {
			lfs_t lfs;
			lfs_file_t memcard;
			if(LFS_ERR_OK == lfs_mount(&lfs, &lfs_pico_flash_config)) {
				if(LFS_ERR_OK == lfs_file_open(&lfs, &memcard, mc_filenames[mc->index], LFS_O_WRONLY | LFS_O_CREAT)) {
					if (MC_SIZE == lfs_file_write(&lfs, &memcard, mc->data, MC_SIZE)) {
						memory_card_set_sync(mc, false);
					} else {
						status = MC_FILE_WRITE_ERR;
					}
					lfs_file_close(&lfs, &memcard);
				} else  {
					status = MC_FILE_OPEN_ERR;
				}
				lfs_unmount(&lfs);
			} else {
				status = MC_MOUNT_ERR;
			}
		} else {
			status = MC_INDEX_OOB_ERR;
		}
	} else {
		status = MC_NO_INIT;
	}
	
	return status;
}

void memory_card_reset_seen_flag(memory_card_t* mc) {
	if(mc) {
		mc->flag_byte &= ~(1 << 3);
	}
}