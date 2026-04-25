#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <pico/stdlib.h>
/* Time and Timestamps */
#include <pico/time.h>
/* TinyUSB */
#include <bsp/board.h>
#include <tusb.h>
/* LFS */
#include <lfs.h>
/* FAT Simulation */
#include "mimic_fat.h"
/* Memcard Simulation */
#include "memcard_simulator.h"
/* LED control */
#include "led.h"
/* Global Configuration */
#include "config.h"
#include "tusb_config.h"

extern const struct lfs_config lfs_pico_flash_config;  // littlefs_driver.c
lfs_t fs;
void cdc_task(void);

/*
 * Format the file system if it does not exist
 */
static void test_filesystem_and_format_if_necessary(bool force_format) {
   if (force_format || (lfs_mount(&fs, &lfs_pico_flash_config) != 0)) {
        printf("Format the onboard flash memory with littlefs\n");

        lfs_format(&fs, &lfs_pico_flash_config);
        lfs_mount(&fs, &lfs_pico_flash_config);

		/*
        lfs_file_t f;
        lfs_file_open(&fs, &f, "README.TXT", LFS_O_RDWR|LFS_O_CREAT);
        lfs_file_write(&fs, &f, README_TXT, strlen(README_TXT));
        lfs_file_close(&fs, &f);
		*/

        if (mimic_fat_usb_device_is_enabled()) {
            mimic_fat_create_cache();
        }
    }
}

/*------------- MAIN -------------*/
int main(void) {
	
	board_init();
	tud_init(BOARD_TUD_RHPORT);
	stdio_init_all();
	led_init();

	/* Initialize LittleFs */
	test_filesystem_and_format_if_necessary(false);

	while (1) {
		/* Pico connected to PC, initialize USB transfer mode */
		tud_task(); // tinyusb device task
		// cdc_task();

		if(to_ms_since_boot(get_absolute_time()) > TUD_MOUNT_TIMEOUT && !mimic_fat_usb_device_is_enabled())
			break;
	}
	lfs_unmount(&fs); // Memcard currently assumes it isn't already mounted
	/* Pico powered by PSX, initialize memory card simulation */
	simulate_memory_card();	

	return 0;
}

//--------------------------------------------------------------------+
// USB CDC
//--------------------------------------------------------------------+
void cdc_task(void) {
	// connected() check for DTR bit
	// Most but not all terminal client set this when making connection
	// if ( tud_cdc_connected() )
	{
		// connected and there are data available
		if ( tud_cdc_available() )
		{
			// read datas
			char buf[64];
			uint32_t count = tud_cdc_read(buf, sizeof(buf));
			(void) count;

			// Echo back
			// Note: Skip echo by commenting out write() and write_flush()
			// for throughput test e.g
			//    $ dd if=/dev/zero of=/dev/ttyACM0 count=10000
			tud_cdc_write(buf, count);
			tud_cdc_write_flush();
		}
	}
}

// Invoked when cdc when line state changed e.g connected/disconnected
void tud_cdc_line_state_cb(uint8_t itf, bool dtr, bool rts) {
	(void) itf;
	(void) rts;
}

// Invoked when CDC interface received data from host
void tud_cdc_rx_cb(uint8_t itf) {
	(void) itf;
}