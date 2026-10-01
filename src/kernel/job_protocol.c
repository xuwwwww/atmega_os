#include "job_protocol.h"
#include <string.h>

uint16_t job_crc(const uint8_t *data, uint8_t length)
{
    uint16_t crc = 0xffff;
    for (uint8_t i = 0; i < length; i++) {
        crc ^= (uint16_t)data[i] << 8u;
        for (uint8_t bit = 0; bit < 8; bit++)
            crc = crc & 0x8000u ? (uint16_t)((crc << 1u) ^ 0x1021u) :
                                 (uint16_t)(crc << 1u);
    }
    return crc;
}
static void seal(uint8_t *data, uint8_t size)
{
    uint16_t crc = job_crc(data, size - 2u);
    data[size - 2u] = (uint8_t)crc;
    data[size - 1u] = (uint8_t)(crc >> 8u);
}
static uint8_t valid(const uint8_t *data, uint8_t size)
{
    return job_crc(data, size - 2u) ==
           (uint16_t)(data[size - 2u] | ((uint16_t)data[size - 1u] << 8u));
}
void job_command(uint8_t data[JOB_COMMAND_SIZE], uint16_t id,
                 const char *name, uint8_t priority, uint16_t image_crc)
{
    memset(data, 0, JOB_COMMAND_SIZE);
    data[0] = 0x4a;
    data[1] = (uint8_t)id;
    data[2] = (uint8_t)(id >> 8u);
    data[3] = priority;
    for (uint8_t i = 0; i < 11 && name[i]; i++) data[4u + i] = (uint8_t)name[i];
    data[16] = (uint8_t)image_crc;
    data[17] = (uint8_t)(image_crc >> 8u);
    data[18] = 1; /* Protocol version. */
    seal(data, JOB_COMMAND_SIZE);
}
void job_cancel_command(uint8_t data[JOB_COMMAND_SIZE], uint16_t id,
                        const char *name)
{
    job_command(data, id, name, 0, 0);
    data[18] = 3; /* Control packet: cancel this exact job ID. */
    seal(data, JOB_COMMAND_SIZE);
}
void job_deploy_begin(uint8_t data[JOB_COMMAND_SIZE], uint16_t id,
                      const char *name, uint8_t size, uint16_t image_crc)
{
    job_command(data, id, name, size, image_crc);
    data[18] = 4;
    seal(data, JOB_COMMAND_SIZE);
}
void job_deploy_data(uint8_t data[JOB_COMMAND_SIZE], uint16_t id,
                     uint8_t offset, const uint8_t *payload, uint8_t length)
{
    memset(data, 0, JOB_COMMAND_SIZE);
    data[0] = 0x4a; data[1] = (uint8_t)id; data[2] = (uint8_t)(id >> 8u);
    data[3] = offset; data[4] = length;
    if (length <= 11u) memcpy(data + 5, payload, length);
    data[18] = 5;
    seal(data, JOB_COMMAND_SIZE);
}
void job_deploy_commit(uint8_t data[JOB_COMMAND_SIZE], uint16_t id,
                       const char *name, uint8_t priority, uint16_t image_crc)
{
    job_command(data, id, name, priority, image_crc);
    data[18] = 6;
    seal(data, JOB_COMMAND_SIZE);
}
uint8_t job_valid_command(const uint8_t data[JOB_COMMAND_SIZE])
{
    uint8_t version = data[18];
    if (data[0] != 0x4a || version < 1 || version > 6 ||
        !(data[1] | data[2]) || !valid(data, JOB_COMMAND_SIZE)) return 0;
    if (version == 5)
        return data[4] >= 1u && data[4] <= 11u &&
               (uint16_t)data[3] + data[4] <= 192u;
    if (!data[4] || data[15]) return 0;
    if ((version <= 3 || version == 6) && data[3] > 3) return 0;
    if (version == 3 && data[3] != 0) return 0;
    if (version == 4 && (data[3] == 0 || data[3] > 192u)) return 0;
    for (uint8_t i = 4; i < 15 && data[i]; i++) {
        uint8_t c = data[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
              c == '_' || c == '-')) return 0;
    }
    return 1;
}
void job_report(uint8_t data[JOB_REPORT_SIZE], uint16_t heartbeat,
                job_info_t job)
{
    memset(data, 0, JOB_REPORT_SIZE);
    data[0] = 0x12;
    data[1] = (uint8_t)heartbeat;
    data[2] = (uint8_t)(heartbeat >> 8u);
    data[3] = (uint8_t)job.id;
    data[4] = (uint8_t)(job.id >> 8u);
    data[5] = job.state;
    data[6] = job.detail;
    for (uint8_t i = 0; i < 4; i++) data[7u+i] = (uint8_t)(job.result >> (i*8u));
    data[11] = 2;
    seal(data, JOB_REPORT_SIZE);
}
uint8_t job_valid_report(const uint8_t data[JOB_REPORT_SIZE])
{
    return data[0] == 0x12 && data[11] == 2 && data[5] <= JOB_DEPLOYED &&
           (!data[13] || data[12] < data[13]) && data[14] <= 1 &&
           (!data[14] || (data[15] && !data[26] && data[31] <= 3)) &&
           valid(data, JOB_REPORT_SIZE);
}
