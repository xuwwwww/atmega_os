#ifndef RV_DEMO_TASK_H
#define RV_DEMO_TASK_H

#ifndef RV_DEMO_STACK_SIZE
#define RV_DEMO_STACK_SIZE 128u
#endif

#include <stdint.h>

#define RV_DEMO_NAME_CAPACITY 12u

void rv_demo_task(void);
uint8_t rv_demo_run(void);
uint8_t rv_demo_run_all(void);
uint8_t rv_demo_run_name(const char *name);
uint8_t rv_demo_set_priority(const char *name, uint8_t priority);
void rv_demo_set_output_prefix(uint8_t enabled);
uint8_t rv_demo_output_prefix_enabled(void);
void rv_demo_set_switch_trace(uint8_t enabled);
uint8_t rv_demo_switch_trace_enabled(void);
uint8_t rv_demo_slot_count(void);
uint8_t rv_demo_slot_loaded(uint8_t slot);
int8_t rv_demo_find_name(const char *name);
uint16_t rv_demo_storage_bytes(void);
const char *rv_demo_slot_name(uint8_t slot);
uint8_t rv_demo_slot_priority(uint8_t slot);
uint16_t rv_demo_slot_image_size(uint8_t slot);
uint16_t rv_demo_slot_image_crc(uint8_t slot);
uint16_t rv_demo_slot_entry_point(uint8_t slot);
uint8_t rv_demo_slot_status(uint8_t slot);
uint32_t rv_demo_slot_exit(uint8_t slot);
uint8_t rv_demo_slot_image_byte(uint8_t slot, uint16_t offset);
void rv_demo_register_builtin(void);
uint8_t rv_demo_is_active(void);
void rv_demo_request_cancel(void);
uint8_t rv_demo_was_cancelled(void);
uint8_t rv_demo_status(void);
uint16_t rv_demo_image_size(void);
uint16_t rv_demo_memory_size(void);
uint32_t rv_demo_exit_code(void);
uint32_t rv_demo_pc(void);
uint32_t rv_demo_register(uint8_t index);
uint8_t rv_demo_upload_begin(uint16_t image_size);
uint8_t rv_demo_upload_write(uint16_t offset, uint8_t value);
uint16_t rv_demo_upload_crc16(uint16_t length);
uint8_t rv_demo_upload_commit(void);
void rv_demo_upload_cancel(void);
void rv_demo_use_builtin(void);
uint8_t rv_demo_image_is_uploaded(void);
uint8_t rv_demo_upload_begin_slot(uint8_t slot, uint16_t image_size);
uint8_t rv_demo_upload_write_slot(uint8_t slot, uint16_t offset,
                                  uint8_t value);
uint16_t rv_demo_upload_crc16_slot(uint8_t slot, uint16_t length);
uint8_t rv_demo_upload_commit_slot(uint8_t slot);
uint8_t rv_demo_upload_commit_metadata_slot(uint8_t slot,
                                            uint16_t image_crc,
                                            uint16_t entry_point);
void rv_demo_upload_cancel_slot(uint8_t slot);
int8_t rv_demo_upload_begin_name(const char *name, uint16_t image_size);
int8_t rv_demo_upload_begin_cached(const char *name, uint16_t image_size);

#endif
