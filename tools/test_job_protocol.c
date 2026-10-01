#include "job_protocol.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
int main(void)
{
    assert(job_crc((const uint8_t *)"123456789", 9) == 0x29b1);
    uint8_t packet[JOB_COMMAND_SIZE], changed[JOB_COMMAND_SIZE];
    job_command(packet, 0x1234, "checksum", 2, 0xd8ca);
    assert(job_valid_command(packet));
    assert(packet[1] == 0x34 && packet[2] == 0x12);
    assert(!strcmp((const char *)&packet[4], "checksum"));
    assert(packet[16] == 0xca && packet[17] == 0xd8);
    for (unsigned i = 0; i < sizeof(packet); i++) {
        memcpy(changed, packet, sizeof(packet));
        changed[i] ^= 1;
        assert(!job_valid_command(changed));
    }
    job_command(packet, 0, "checksum", 2, 0);
    assert(!job_valid_command(packet));
    job_command(packet, 1, "checksum", 2, 0);
    packet[18] = 2; /* Name-based selection, still sealed and validated. */
    uint16_t crc = job_crc(packet, JOB_COMMAND_SIZE - 2u);
    packet[19] = (uint8_t)crc;
    packet[20] = (uint8_t)(crc >> 8u);
    assert(job_valid_command(packet));
    for (unsigned i = 0; i < sizeof(packet); i++) {
        memcpy(changed, packet, sizeof(packet));
        changed[i] ^= 1;
        assert(!job_valid_command(changed));
    }
    packet[18] = 3;
    crc = job_crc(packet, JOB_COMMAND_SIZE - 2u);
    packet[19] = (uint8_t)crc;
    packet[20] = (uint8_t)(crc >> 8u);
    assert(!job_valid_command(packet));
    job_cancel_command(packet, 7, "checksum");
    assert(job_valid_command(packet));
    assert(packet[1] == 7 && packet[2] == 0 && packet[3] == 0 && packet[18] == 3);
    for (unsigned i = 0; i < sizeof(packet); i++) {
        memcpy(changed, packet, sizeof(packet));
        changed[i] ^= 1;
        assert(!job_valid_command(changed));
    }
    job_deploy_begin(packet, 8, "fibonacci", 155, 0xea5e);
    assert(job_valid_command(packet) && packet[18] == 4 && packet[3] == 155);
    const uint8_t payload[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
    job_deploy_data(packet, 8, 22, payload, sizeof(payload));
    assert(job_valid_command(packet) && packet[18] == 5 && packet[3] == 22 &&
           packet[4] == sizeof(payload) && !memcmp(packet + 5, payload, sizeof(payload)));
    for (unsigned i = 0; i < sizeof(packet); i++) {
        memcpy(changed, packet, sizeof(packet));
        changed[i] ^= 1;
        assert(!job_valid_command(changed));
    }
    packet[4] = 12;
    crc = job_crc(packet, JOB_COMMAND_SIZE - 2u);
    packet[19] = (uint8_t)crc; packet[20] = (uint8_t)(crc >> 8u);
    assert(!job_valid_command(packet));
    job_deploy_commit(packet, 8, "fibonacci", 3, 0xea5e);
    assert(job_valid_command(packet) && packet[18] == 6 && packet[3] == 3);
    job_command(packet, 1, "checksum", 4, 0);
    assert(!job_valid_command(packet));
    job_command(packet, 1, "bad name", 2, 0);
    assert(!job_valid_command(packet));
    uint8_t report[JOB_REPORT_SIZE];
    job_info_t job = {.id=0x1234, .result=0x12345678, .state=JOB_DONE, .detail=2};
    job_report(report, 0xabcd, job);
    assert(job_valid_report(report));
    assert(report[1] == 0xcd && report[2] == 0xab);
    assert(report[7] == 0x78 && report[10] == 0x12);
    report[12] = 1; report[13] = 3; report[14] = 1;
    memcpy(report + 15, "checksum", 9);
    report[27] = 155; report[29] = 0xca; report[30] = 0xd8;
    report[31] = 2;
    crc = job_crc(report, JOB_REPORT_SIZE - 2u);
    report[34] = (uint8_t)crc; report[35] = (uint8_t)(crc >> 8u);
    assert(job_valid_report(report));
    for (unsigned i = 0; i < sizeof(report); i++) {
        report[i] ^= 1;
        assert(!job_valid_report(report));
        report[i] ^= 1;
    }
    report[12] = 3; /* Out-of-range page even with a valid packet CRC. */
    crc = job_crc(report, JOB_REPORT_SIZE - 2u);
    report[34] = (uint8_t)crc; report[35] = (uint8_t)(crc >> 8u);
    assert(!job_valid_report(report));
    puts("job protocol CRC + packet validation tests passed");
}
