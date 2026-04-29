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
/* Memcard Simulation */
#include "memcard_simulator.h"
/* LED control */
#include "led.h"
/* Global Configuration */
#include "config.h"
#include "tusb_config.h"

extern const struct lfs_config lfs_pico_flash_config;  // littlefs_driver.c
lfs_t fs;
bool is_mounted = false;
void cdc_task(void);

/*
 * Format the file system if it does not exist
 */
static void test_filesystem_and_format_if_necessary(bool force_format) {
   if (force_format || (lfs_mount(&fs, &lfs_pico_flash_config) != 0)) {
        printf("Format the onboard flash memory with littlefs\n");

        lfs_format(&fs, &lfs_pico_flash_config);
		lfs_mount(&fs, &lfs_pico_flash_config);
    }
	lfs_unmount(&fs);
}

/*------------- MAIN -------------*/
int main(void) {
	
	board_init();
  	tusb_rhport_init_t dev_init = {
    	.role = TUSB_ROLE_DEVICE,
    	.speed = TUSB_SPEED_AUTO
  	};
  	tusb_init(BOARD_TUD_RHPORT, &dev_init);
  	board_init_after_tusb();
	// stdio_init_all();
	led_init();

	/* Initialize LittleFs */
	test_filesystem_and_format_if_necessary(false);

	while (1) {
		/* Pico connected to PC, initialize USB transfer mode */
		tud_task(); // tinyusb device task
		// cdc_task();

		if(to_ms_since_boot(get_absolute_time()) > TUD_MOUNT_TIMEOUT && !is_mounted)
			break;
	}

	/* Pico powered by PSX, initialize memory card simulation */
	simulate_memory_card();	

	return 0;
}

//--------------------------------------------------------------------+
// Device callbacks
//--------------------------------------------------------------------+

// Invoked when device is mounted
void tud_mount_cb(void) {
  // blink_interval_ms = BLINK_MOUNTED;
  is_mounted = true;
}

// Invoked when device is unmounted
void tud_umount_cb(void) {
  // blink_interval_ms = BLINK_NOT_MOUNTED;
	is_mounted = false;
}

// Invoked when usb bus is suspended
// remote_wakeup_en : if host allow us  to perform remote wakeup
// Within 7ms, device must draw an average of current less than 2.5 mA from bus
void tud_suspend_cb(bool remote_wakeup_en) {
  (void) remote_wakeup_en;
  // blink_interval_ms = BLINK_SUSPENDED;
}

// Invoked when usb bus is resumed
void tud_resume_cb(void) {
  // blink_interval_ms = tud_mounted() ? BLINK_MOUNTED : BLINK_NOT_MOUNTED;
}