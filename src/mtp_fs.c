#include <bsp/board_api.h>
#include <tusb.h>

#include "mtp_logo_png.h"
#include "config.h"
#include "memory_card.h"
#include "led.h"

#define FS_MAX_FILE_COUNT NUM_MEMORY_CARDS
#define FS_MAX_FILENAME_LEN 16
#define FS_MAX_CAPACITY_BYTES (NUM_MEMORY_CARDS * MC_SIZE)
#define FS_ROOT_PARENT 0
static memory_card_t mc;
bool deleted_mcs[FS_MAX_FILE_COUNT] = { false };
extern const char* mc_filenames[];

//--------------------------------------------------------------------+
// Dataset
//--------------------------------------------------------------------+

//------------- storage info -------------//
#define STORAGE_DESCRIPTION { 'R', 'a', 'i', 'n', 'b', 'o', 'w', 'M', 'C', 0 }
#define VOLUME_IDENTIFIER { 'R', 'a', 'i', 'n', 'b', 'o', 'w', 'M', 'C', 0 }

enum {
  STORAGE_DESC_LEN = TU_ARRAY_SIZE((uint16_t[]) STORAGE_DESCRIPTION),
  VOLUME_ID_LEN = TU_ARRAY_SIZE((uint16_t[])VOLUME_IDENTIFIER)
};

typedef MTP_STORAGE_INFO_STRUCT(STORAGE_DESC_LEN, VOLUME_ID_LEN) storage_info_t;

storage_info_t storage_info = {

  .storage_type = MTP_STORAGE_TYPE_FIXED_RAM,
  .filesystem_type = MTP_FILESYSTEM_TYPE_GENERIC_FLAT,
  .access_capability = MTP_ACCESS_CAPABILITY_READ_WRITE,
  .max_capacity_in_bytes = FS_MAX_CAPACITY_BYTES,
  .free_space_in_bytes = 0,
  .free_space_in_objects = 0,
  .storage_description = {
    .count = (TU_FIELD_SIZE(storage_info_t, storage_description)-1) / sizeof(uint16_t),
    .utf16 = STORAGE_DESCRIPTION
  },
  .volume_identifier = {
    .count = (TU_FIELD_SIZE(storage_info_t, volume_identifier)-1) / sizeof(uint16_t),
    .utf16 = VOLUME_IDENTIFIER
  }
};

//--------------------------------------------------------------------+
// MTP FILESYSTEM
//--------------------------------------------------------------------+
#define FS_FIXED_DATETIME "20260427T101000.0" // "YYYYMMDDTHHMMSS.s"

enum {
  SUPPORTED_STORAGE_ID = 0x00010001u // physical = 1, logical = 1
};

static int32_t fs_get_device_info(tud_mtp_cb_data_t* cb_data);
static int32_t fs_open_close_session(tud_mtp_cb_data_t* cb_data);
static int32_t fs_get_storage_ids(tud_mtp_cb_data_t* cb_data);
static int32_t fs_get_storage_info(tud_mtp_cb_data_t* cb_data);
static int32_t fs_get_device_properties(tud_mtp_cb_data_t* cb_data);
static int32_t fs_get_object_handles(tud_mtp_cb_data_t* cb_data);
static int32_t fs_get_object_info(tud_mtp_cb_data_t* cb_data);
static int32_t fs_get_object(tud_mtp_cb_data_t* cb_data);
static int32_t fs_delete_object(tud_mtp_cb_data_t* cb_data);
static int32_t fs_send_object_info(tud_mtp_cb_data_t* cb_data);
static int32_t fs_send_object(tud_mtp_cb_data_t* cb_data);

typedef int32_t (*fs_op_handler_t)(tud_mtp_cb_data_t* cb_data);
typedef struct {
  uint32_t op_code;
  fs_op_handler_t handler;
}fs_op_handler_dict_t;

fs_op_handler_dict_t fs_op_handler_dict[] = {
  { MTP_OP_GET_DEVICE_INFO,       fs_get_device_info    },
  { MTP_OP_OPEN_SESSION,          fs_open_close_session },
  { MTP_OP_CLOSE_SESSION,         fs_open_close_session },
  { MTP_OP_GET_STORAGE_IDS,       fs_get_storage_ids       },
  { MTP_OP_GET_STORAGE_INFO,      fs_get_storage_info      },
  { MTP_OP_GET_DEVICE_PROP_DESC,  fs_get_device_properties  },
  { MTP_OP_GET_DEVICE_PROP_VALUE, fs_get_device_properties },
  { MTP_OP_GET_OBJECT_HANDLES,    fs_get_object_handles    },
  { MTP_OP_GET_OBJECT_INFO,       fs_get_object_info       },
  { MTP_OP_GET_OBJECT,            fs_get_object            },
  { MTP_OP_DELETE_OBJECT,         fs_delete_object         },
  { MTP_OP_SEND_OBJECT_INFO,      fs_send_object_info      },
  { MTP_OP_SEND_OBJECT,           fs_send_object           },
};

static bool is_session_opened = false;
static uint32_t transfer_mc_index = MC_IMAGE_INDEX_NONE;

//--------------------------------------------------------------------+
// Control Request callback
//--------------------------------------------------------------------+
bool tud_mtp_request_cancel_cb(tud_mtp_request_cb_data_t* cb_data) {
  mtp_request_reset_cancel_data_t cancel_data;
  memcpy(&cancel_data, cb_data->buf, sizeof(cancel_data));
  (void) cancel_data.code;
  (void ) cancel_data.transaction_id;
  return true;
}

// Invoked when received Device Reset request
// return false to stall the request
bool tud_mtp_request_device_reset_cb(tud_mtp_request_cb_data_t* cb_data) {
  (void) cb_data;
  return true;
}

// Invoked when received Get Extended Event request. Application fill callback data's buffer for response
// return negative to stall the request
int32_t tud_mtp_request_get_extended_event_cb(tud_mtp_request_cb_data_t* cb_data) {
  (void) cb_data;
  return false; // not implemented yet
}

// Invoked when received Get DeviceStatus request. Application fill callback data's buffer for response
// return negative to stall the request
int32_t tud_mtp_request_get_device_status_cb(tud_mtp_request_cb_data_t* cb_data) {
  uint16_t* buf16 = (uint16_t*)(uintptr_t) cb_data->buf;
  buf16[0] = 4; // length
  buf16[1] = MTP_RESP_OK; // status
  return 4;
}

//--------------------------------------------------------------------+
// Bulk Only Protocol
//--------------------------------------------------------------------+
int32_t tud_mtp_command_received_cb(tud_mtp_cb_data_t* cb_data) {
  const mtp_container_command_t* command = cb_data->command_container;
  mtp_container_info_t* io_container = &cb_data->io_container;
  fs_op_handler_t handler = NULL;
  for (size_t i = 0; i < TU_ARRAY_SIZE(fs_op_handler_dict); i++) {
    if (fs_op_handler_dict[i].op_code == command->header.code) {
      handler = fs_op_handler_dict[i].handler;
      break;
    }
  }

  int32_t resp_code;
  if (handler == NULL) {
    resp_code = MTP_RESP_OPERATION_NOT_SUPPORTED;
  } else {
    resp_code = handler(cb_data);
  }
  if (resp_code > MTP_RESP_UNDEFINED) {
    // send response if needed
    io_container->header->code = (uint16_t)resp_code;
    tud_mtp_response_send(io_container);
  }

  return resp_code;
}

int32_t tud_mtp_data_xfer_cb(tud_mtp_cb_data_t* cb_data) {
  const mtp_container_command_t* command = cb_data->command_container;
  mtp_container_info_t* io_container = &cb_data->io_container;

  fs_op_handler_t handler = NULL;
  for (size_t i = 0; i < TU_ARRAY_SIZE(fs_op_handler_dict); i++) {
    if (fs_op_handler_dict[i].op_code == command->header.code) {
      handler = fs_op_handler_dict[i].handler;
      break;
    }
  }

  int32_t resp_code;
  if (handler == NULL) {
    resp_code = MTP_RESP_OPERATION_NOT_SUPPORTED;
  } else {
    resp_code = handler(cb_data);
  }
  if (resp_code > MTP_RESP_UNDEFINED) {
    // send response if needed
    io_container->header->code = (uint16_t)resp_code;
    tud_mtp_response_send(io_container);
  }

  return 0;
}

int32_t tud_mtp_data_complete_cb(tud_mtp_cb_data_t* cb_data) {
  const mtp_container_command_t* command = cb_data->command_container;
  mtp_container_info_t* resp = &cb_data->io_container;
  switch (command->header.code) {
    case MTP_OP_SEND_OBJECT_INFO: {
      if (transfer_mc_index > FS_MAX_FILE_COUNT) {
        resp->header->code = MTP_RESP_GENERAL_ERROR;
        break;
      }
      // parameter is: storage id, parent handle, new handle
      (void) mtp_container_add_uint32(resp, SUPPORTED_STORAGE_ID);
      (void) mtp_container_add_uint32(resp, 0);
      (void) mtp_container_add_uint32(resp, transfer_mc_index + 1);
      resp->header->code = MTP_RESP_OK;
      break;
    }

    default:
      resp->header->code = (cb_data->xfer_result == XFER_RESULT_SUCCESS) ? MTP_RESP_OK : MTP_RESP_GENERAL_ERROR;
      break;
  }

  tud_mtp_response_send(resp);

  // Synchronize MC with LFS
  uint32_t status;
  if (resp->header->code == MTP_RESP_OK && mc.out_of_sync) {
    status = memory_card_sync(&mc);
    if (status != MC_OK) {
        while (true) {
            led_blink_error(status);
		    sleep_ms(2000);
        }
    }
  }
  return 0;
}

int32_t tud_mtp_response_complete_cb(tud_mtp_cb_data_t* cb_data) {
  (void) cb_data;
  return 0; // nothing to do
}

//--------------------------------------------------------------------+
// File System Handlers
//--------------------------------------------------------------------+
static int32_t fs_get_device_info(tud_mtp_cb_data_t* cb_data) {
  // Device info is already prepared up to playback formats. Application only need to add string fields
  int32_t resp_code = 0;
  mtp_container_info_t* io_container = &cb_data->io_container;
  (void) mtp_container_add_cstring(io_container, DEV_INFO_MANUFACTURER);
  (void) mtp_container_add_cstring(io_container, DEV_INFO_MODEL);
  (void) mtp_container_add_cstring(io_container, DEV_INFO_VERSION);

  enum { MAX_SERIAL_NCHARS = 32 };
  uint16_t serial_utf16[MAX_SERIAL_NCHARS+1];
  size_t nchars = board_usb_get_serial(serial_utf16, MAX_SERIAL_NCHARS);
  serial_utf16[tu_min32(nchars, MAX_SERIAL_NCHARS)] = 0; // ensure null termination
  (void) mtp_container_add_string(io_container, serial_utf16);

  if (!tud_mtp_data_send(io_container)) {
    resp_code = MTP_RESP_DEVICE_BUSY;
  }
  return resp_code;
}

static int32_t fs_open_close_session(tud_mtp_cb_data_t* cb_data) {
  const mtp_container_command_t* command = cb_data->command_container;
  if (command->header.code == MTP_OP_OPEN_SESSION) {
    if (is_session_opened) {
      return MTP_RESP_SESSION_ALREADY_OPEN;
    }
    else {
        // Initialize virtual memory card and deleted objects array
        memory_card_init(&mc);
        memset(deleted_mcs, 0, sizeof(deleted_mcs));
        is_session_opened = true;
    }
  } else { // close session
    if (!is_session_opened) {
      return MTP_RESP_SESSION_NOT_OPEN;
    }
    is_session_opened = false;
  }
  return MTP_RESP_OK;
}

static int32_t fs_get_storage_ids(tud_mtp_cb_data_t* cb_data) {
  mtp_container_info_t* io_container = &cb_data->io_container;
  uint32_t storage_ids [] = { SUPPORTED_STORAGE_ID };
  (void) mtp_container_add_auint32(io_container, 1, storage_ids);
  tud_mtp_data_send(io_container);
  return 0;
}

static int32_t fs_get_storage_info(tud_mtp_cb_data_t* cb_data) {
  const mtp_container_command_t* command = cb_data->command_container;
  mtp_container_info_t* io_container = &cb_data->io_container;
  const uint32_t storage_id = command->params[0];
  TU_VERIFY(SUPPORTED_STORAGE_ID == storage_id, -1);
  // update storage info with current free space
  // storage_info.max_capacity_in_bytes = sizeof(README_TXT_CONTENT) + LOGO_LEN + FS_MAX_CAPACITY_BYTES;
  // storage_info.free_space_in_objects = FS_MAX_FILE_COUNT - fs_get_file_count();
  // storage_info.free_space_in_bytes = storage_info.free_space_in_objects ? FS_MAX_CAPACITY_BYTES : 0;
  
  // Count deleted MCs
  uint32_t num_deleted = 0;
  for (size_t obj_index = 0; obj_index < NUM_MEMORY_CARDS; ++obj_index) {
    if (deleted_mcs[obj_index]) ++num_deleted;
  }
  storage_info.max_capacity_in_bytes = FS_MAX_CAPACITY_BYTES;
  storage_info.free_space_in_objects = num_deleted;
  storage_info.free_space_in_bytes = num_deleted * MC_SIZE;
  
  (void) mtp_container_add_raw(io_container, &storage_info, sizeof(storage_info));
  tud_mtp_data_send(io_container);
  return 0;
}

static int32_t fs_get_device_properties(tud_mtp_cb_data_t* cb_data) {
  const mtp_container_command_t* command = cb_data->command_container;
  mtp_container_info_t* io_container = &cb_data->io_container;
  const uint16_t dev_prop_code = (uint16_t) command->params[0];

  if (command->header.code == MTP_OP_GET_DEVICE_PROP_DESC) {
    // get describing dataset
    mtp_device_prop_desc_header_t device_prop_header;
    device_prop_header.device_property_code = dev_prop_code;
    switch (dev_prop_code) {
      case MTP_DEV_PROP_DEVICE_FRIENDLY_NAME:
        device_prop_header.datatype = MTP_DATA_TYPE_STR;
        device_prop_header.get_set = MTP_MODE_GET;
        (void) mtp_container_add_raw(io_container, &device_prop_header, sizeof(device_prop_header));
        (void) mtp_container_add_cstring(io_container, DEV_PROP_FRIENDLY_NAME); // factory
        (void) mtp_container_add_cstring(io_container, DEV_PROP_FRIENDLY_NAME); // current
        (void) mtp_container_add_uint8(io_container, 0); // no form
        tud_mtp_data_send(io_container);
        break;

      default:
        return MTP_RESP_PARAMETER_NOT_SUPPORTED;
    }
  } else {
    // get value
    switch (dev_prop_code) {
      case MTP_DEV_PROP_DEVICE_FRIENDLY_NAME:
        (void) mtp_container_add_cstring(io_container, DEV_PROP_FRIENDLY_NAME);
        tud_mtp_data_send(io_container);
        break;

      default:
        return MTP_RESP_PARAMETER_NOT_SUPPORTED;
    }
  }
  return 0;
}

static int32_t fs_get_object_handles(tud_mtp_cb_data_t* cb_data) {
  const mtp_container_command_t* command = cb_data->command_container;
  mtp_container_info_t* io_container = &cb_data->io_container;

  const uint32_t storage_id = command->params[0];
  const uint32_t obj_format = command->params[1]; // optional
  const uint32_t parent_handle = command->params[2]; // folder handle, 0xFFFFFFFF is root
  (void)obj_format;

  if (storage_id != 0xFFFFFFFFu && storage_id != SUPPORTED_STORAGE_ID) {
    return MTP_RESP_INVALID_STORAGE_ID;
  }

  uint32_t handles[FS_MAX_FILE_COUNT] = { 0 };
  uint32_t count = 0u;
  for (uint8_t i = 0u; i < FS_MAX_FILE_COUNT; i++) {
    // Only return handles for root directory (parent is 0xFFFFFFFFu) and file hasn't been deleted
    // TODO: What if parent is specified as 0?
    if (parent_handle == 0xFFFFFFFFu & !deleted_mcs[i]) {
      handles[count++] = (uint32_t) i + 1u; // handle is index + 1
    }
  }
  (void) mtp_container_add_auint32(io_container, count, handles);
  tud_mtp_data_send(io_container);

  return 0;
}

static int32_t fs_get_object_info(tud_mtp_cb_data_t* cb_data) {
  size_t i;
  uint16_t mtp_filename[FS_MAX_FILENAME_LEN + 1];
  const mtp_container_command_t* command = cb_data->command_container;
  mtp_container_info_t* io_container = &cb_data->io_container;
  const uint32_t obj_handle = command->params[0];
  if (obj_handle == 0 || obj_handle > FS_MAX_FILE_COUNT || deleted_mcs[obj_handle - 1]) {
    return MTP_RESP_INVALID_OBJECT_HANDLE;
  }
  mtp_object_info_header_t obj_info_header = {
    .storage_id = SUPPORTED_STORAGE_ID,
    .object_format = MTP_OBJ_FORMAT_UNDEFINED_FIRMWARE,
    .protection_status =  MTP_PROTECTION_STATUS_NO_PROTECTION,
    .object_compressed_size = MC_SIZE,
    .thumb_format = MTP_OBJ_FORMAT_UNDEFINED,
    .thumb_compressed_size = 0,
    .thumb_pix_width = 0,
    .thumb_pix_height = 0,
    .image_pix_width = 0,
    .image_pix_height = 0,
    .image_bit_depth = 0,
    .parent_object = 0,
    .association_type = MTP_ASSOCIATION_UNDEFINED,
    .association_desc = 0,
    .sequence_number = 0
  };

    // Convert ASCII MC filename to UTF-16 string
    size_t filename_len = strnlen(mc_filenames[obj_handle - 1],FS_MAX_FILENAME_LEN);
    for (i = 0; i < filename_len; ++i ) {
        mtp_filename[i] = mc_filenames[obj_handle - 1][i];
    }
    mtp_filename[i] = 0; // Explicitly null terminate

  (void) mtp_container_add_raw(io_container, &obj_info_header, sizeof(obj_info_header));
  (void) mtp_container_add_string(io_container, mtp_filename);
  (void) mtp_container_add_cstring(io_container, FS_FIXED_DATETIME);
  (void) mtp_container_add_cstring(io_container, FS_FIXED_DATETIME);
  (void) mtp_container_add_cstring(io_container, ""); // keywords, not used
  tud_mtp_data_send(io_container);

  return 0;
}

static int32_t fs_get_object(tud_mtp_cb_data_t* cb_data) {
  const mtp_container_command_t* command = cb_data->command_container;
  mtp_container_info_t* io_container = &cb_data->io_container;
  const uint32_t obj_handle = command->params[0];
  if (obj_handle == 0 || obj_handle > FS_MAX_FILE_COUNT || deleted_mcs[obj_handle - 1]) {
    return MTP_RESP_INVALID_OBJECT_HANDLE;
  }

  // Load memcard from LFS if necessary
  uint32_t requested_mc_image_index = obj_handle - 1;
  if (requested_mc_image_index != mc.index) {
    if (memory_card_import(&mc,requested_mc_image_index) != MC_OK) {
        return MTP_RESP_STORE_NOT_AVAILABLE;
    }
  }

  if (cb_data->phase == MTP_PHASE_COMMAND) {
    // If file contents is larger than CFG_TUD_MTP_EP_BUFSIZE, data may only partially is added here
    // the rest will be sent in tud_mtp_data_more_cb
    (void) mtp_container_add_raw(io_container, mc.data, MC_SIZE);
    tud_mtp_data_send(io_container);
  } else if (cb_data->phase == MTP_PHASE_DATA) {
    // continue sending remaining data: file contents offset is xferred byte minus header size
    const uint32_t offset = cb_data->total_xferred_bytes - sizeof(mtp_container_header_t);
    const uint32_t xact_len = tu_min32(MC_SIZE - offset, io_container->payload_bytes);
    if (xact_len > 0) {
      memcpy(io_container->payload, mc.data + offset, xact_len);
      tud_mtp_data_send(io_container);
    }
  } else {
    // nothing to do
  }

  return 0;
}

static int32_t fs_send_object_info(tud_mtp_cb_data_t* cb_data) {
  size_t filename_index;
  const mtp_container_command_t* command = cb_data->command_container;
  mtp_container_info_t* io_container = &cb_data->io_container;
  const uint32_t storage_id = command->params[0];
  const uint32_t parent_handle = command->params[1]; // folder handle, 0xFFFFFFFF is root
  (void) parent_handle;

  if (!is_session_opened) {
    return MTP_RESP_SESSION_NOT_OPEN;
  }
  if (storage_id != 0xFFFFFFFFu && storage_id != SUPPORTED_STORAGE_ID) {
    return MTP_RESP_INVALID_STORAGE_ID;
  }

  if (cb_data->phase == MTP_PHASE_COMMAND) {
    (void) tud_mtp_data_receive(io_container);
  } 
  else if (cb_data->phase == MTP_PHASE_DATA) {
    mtp_object_info_header_t* obj_info = (mtp_object_info_header_t*) io_container->payload;
    if (obj_info->storage_id != 0 && obj_info->storage_id != SUPPORTED_STORAGE_ID) {
      return MTP_RESP_INVALID_STORAGE_ID;
    }

    if (obj_info->parent_object != 0 && obj_info->parent_object != 0xFFFFFFFFu) { // not root
        return MTP_RESP_INVALID_PARENT_OBJECT;
    }

    uint32_t i;
    uint8_t* buf = io_container->payload + sizeof(mtp_object_info_header_t);
    uint16_t new_filename_u16[FS_MAX_FILENAME_LEN + 1];
    char new_filename[FS_MAX_FILENAME_LEN + 1];
    uint32_t utf16_length = mtp_container_get_string(buf, new_filename_u16); // TODO: How do we prevent this from overflowing?
    uint32_t filename_len = (utf16_length - 1) / 2;
    if (filename_len > FS_MAX_FILENAME_LEN) {
        return MTP_RESP_INVALID_OBJECT_PROP_VALUE;
    }
    // Convert from UTF-16LE to ASCII
    for (i = 0; i < filename_len; ++i ) {
        new_filename[i] = new_filename_u16[i];
    }
    new_filename[i] = 0; // Explicitly null terminate

    // Only allow replacing a known virtual MC file
    for (filename_index = 0; filename_index < NUM_MEMORY_CARDS; ++filename_index) {
        if (strncmp(mc_filenames[filename_index],new_filename,FS_MAX_FILENAME_LEN) == 0) break;
    }
    if (filename_index >= NUM_MEMORY_CARDS) {
        return MTP_RESP_OBJECT_PROP_NOT_SUPPORTED; // Not a valid virtual MC name
    }
    if (obj_info->object_compressed_size != MC_SIZE) {
        return MTP_RESP_OBJECT_TOO_LARGE; // Not the correct size for a virtual MC
    }

    // If the virtual MC was deleted, reinstate it
    transfer_mc_index = filename_index;
    deleted_mcs[filename_index] = false;
  } else {
    // nothing to do
  }

  return 0;
}

static int32_t fs_send_object(tud_mtp_cb_data_t* cb_data) {
  mtp_container_info_t* io_container = &cb_data->io_container;
  if (transfer_mc_index > NUM_MEMORY_CARDS) {
    return MTP_RESP_INVALID_OBJECT_HANDLE;
  }

  if (cb_data->phase == MTP_PHASE_COMMAND) {
    io_container->header->len += MC_SIZE;
    tud_mtp_data_receive(io_container);
  } else {
    // file contents offset is total xferred minus header size minus last received chunk
    const uint32_t offset = cb_data->total_xferred_bytes - sizeof(mtp_container_header_t) - io_container->payload_bytes;
    memcpy(mc.data + offset, io_container->payload, io_container->payload_bytes);
    if (cb_data->total_xferred_bytes - sizeof(mtp_container_header_t) < MC_SIZE) {
      tud_mtp_data_receive(io_container);
    }
  }

  // Mark that MC needs to sync with LFS
  mc.index = transfer_mc_index;
  mc.out_of_sync = true;

  return 0;
}

static int32_t fs_delete_object(tud_mtp_cb_data_t* cb_data) {
  const mtp_container_command_t* command = cb_data->command_container;
  const uint32_t obj_handle = command->params[0];
  const uint32_t obj_format = command->params[1]; // optional
  (void) obj_format;

  if (!is_session_opened) {
    return MTP_RESP_SESSION_NOT_OPEN;
  }
  if (obj_handle == 0 || obj_handle > FS_MAX_FILE_COUNT) {
    return MTP_RESP_INVALID_OBJECT_HANDLE;
  }

  // Delete by adding to deletion array
  if (deleted_mcs[obj_handle - 1]) {
    return MTP_RESP_INVALID_OBJECT_HANDLE; // Already deleted
  }
  deleted_mcs[obj_handle - 1] = true;
  return MTP_RESP_OK;
}