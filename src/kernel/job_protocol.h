#ifndef JOB_PROTOCOL_H
#define JOB_PROTOCOL_H
#include <stdint.h>
#define JOB_COMMAND_SIZE 21u
#define JOB_REPORT_SIZE 36u
enum { JOB_NONE, JOB_QUEUED, JOB_RUNNING, JOB_DONE, JOB_FAULT,
       JOB_REJECTED, JOB_UNKNOWN, JOB_CANCELLED, JOB_COPYING, JOB_DEPLOYED };
typedef struct {
    uint16_t id;
    uint32_t result;
    uint8_t state;
    uint8_t detail;
} job_info_t;
uint16_t job_crc(const uint8_t *data, uint8_t length);
void job_command(uint8_t data[JOB_COMMAND_SIZE], uint16_t id,
                 const char *name, uint8_t priority, uint16_t image_crc);
void job_cancel_command(uint8_t data[JOB_COMMAND_SIZE], uint16_t id,
                        const char *name);
void job_deploy_begin(uint8_t data[JOB_COMMAND_SIZE], uint16_t id,
                      const char *name, uint8_t size, uint16_t image_crc);
void job_deploy_data(uint8_t data[JOB_COMMAND_SIZE], uint16_t id,
                     uint8_t offset, const uint8_t *payload, uint8_t length);
void job_deploy_commit(uint8_t data[JOB_COMMAND_SIZE], uint16_t id,
                       const char *name, uint8_t priority, uint16_t image_crc);
uint8_t job_valid_command(const uint8_t data[JOB_COMMAND_SIZE]);
uint8_t job_valid_report(const uint8_t data[JOB_REPORT_SIZE]);
void job_report(uint8_t data[JOB_REPORT_SIZE], uint16_t heartbeat,
                job_info_t job);
#endif
