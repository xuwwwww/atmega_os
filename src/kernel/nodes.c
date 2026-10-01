#include "nodes.h"
#include <avr/io.h>
#include <avr/interrupt.h>
#include <util/atomic.h>
#include <util/twi.h>
#include <string.h>
#include "../tasks/rv_demo_task.h"

#if NODE_ROLE
#if !LGT_SAFE_MODE
#error "Node milestone currently supports the single-native-stack profile only"
#endif
#if F_CPU != 16000000UL
#error "Node Timer0 and TWI configuration currently requires 16 MHz"
#endif
static volatile uint32_t clock_ms;
static job_info_t current_job;
#if NODE_ROLE == 1
static node_info_t remote;
static node_program_t programs[RV_GUEST_SLOT_COUNT];
static uint8_t program_count;
#endif
ISR(TIMER0_COMPA_vect) { clock_ms++; }
static uint32_t now(void)
{
    uint32_t result;
    ATOMIC_BLOCK(ATOMIC_RESTORESTATE) { result = clock_ms; }
    return result;
}

#if NODE_ROLE == 2
static uint8_t reply[JOB_REPORT_SIZE], reply_index;
static uint8_t paused;
static volatile uint8_t inbox[JOB_COMMAND_SIZE], inbox_ready;
static uint8_t inbox_index, inbox_drop;
static char job_name[12];
static uint8_t job_priority;
static uint16_t accepted_crc;
static uint16_t run_count;
static int8_t transfer_slot = -1;
static uint8_t transfer_size;
static uint16_t transfer_crc;
static uint8_t directory[22];
static volatile uint8_t directory_cursor;
static uint8_t published_slot = 0xff, published_tick;
ISR(TWI_vect)
{
    switch (TW_STATUS) {
    case TW_ST_SLA_ACK:
    case TW_ST_ARB_LOST_SLA_ACK:
        job_report(reply, (uint16_t)clock_ms, current_job);
        memcpy(reply + 12, directory, sizeof(directory));
        uint16_t report_crc = job_crc(reply, JOB_REPORT_SIZE - 2u);
        reply[34] = (uint8_t)report_crc;
        reply[35] = (uint8_t)(report_crc >> 8u);
        if (++directory_cursor >= rv_demo_slot_count()) directory_cursor = 0;
        reply_index = 0;
        /* fall through */
    case TW_ST_DATA_ACK:
        TWDR = reply_index < JOB_REPORT_SIZE ? reply[reply_index++] : 0xff;
        break;
    case TW_SR_SLA_ACK:
    case TW_SR_ARB_LOST_SLA_ACK:
        inbox_index = 0;
        inbox_drop = inbox_ready;
        break;
    case TW_SR_DATA_ACK:
        if (!inbox_drop && inbox_index < JOB_COMMAND_SIZE)
            inbox[inbox_index++] = TWDR;
        else inbox_drop = 1;
        break;
    case TW_SR_STOP:
        if (!inbox_drop && inbox_index == JOB_COMMAND_SIZE) inbox_ready = 1;
        break;
    case TW_BUS_ERROR:
        TWCR = (1u << TWINT) | (1u << TWSTO) | (1u << TWEN) |
               (1u << TWEA) | (1u << TWIE);
        return;
    default: break;
    }
    TWCR = (1u << TWINT) | (1u << TWEN) | (1u << TWEA) | (1u << TWIE);
}
#else
static uint8_t phase, index, frame[JOB_REPORT_SIZE], have_seen;
static uint8_t command[JOB_COMMAND_SIZE], sending, attempts, acknowledged;
static uint8_t cancel_pending;
static uint8_t deploy_active, deploy_stage, deploy_slot, deploy_priority;
static uint8_t deploy_pending, deploy_offset, deploy_size;
static uint16_t deploy_crc;
static uint32_t deploy_last_action;
static char deploy_name[12];
static uint8_t resend;
static uint16_t sequence;
static uint32_t submitted_at, last_send;
static uint32_t deadline, next_probe, last_change;
static void stop_probe(void)
{
    TWCR = (1u << TWINT) | (1u << TWEN) | (1u << TWSTO);
    phase = 4; /* STOP itself is also bounded, never busy-wait. */
    deadline = now() + 50u;
}
static void failed(uint8_t status)
{
    if (remote.failures != UINT16_MAX) remote.failures++;
    remote.status = status;
    remote.state = have_seen && (uint32_t)(now() - last_change) < 2000u ? 1 : 0;
    stop_probe();
}
#endif
#endif

void nodes_init(void)
{
#if NODE_ROLE
    DDRC &= ~((1u << DDC4) | (1u << DDC5));
    PORTC |= (1u << PORTC4) | (1u << PORTC5);
    /* Genuine 16 MHz ATmega328P profile: Timer0 supplies node wall time.
     * Timer1 remains VM slice timer; Timer2 remains native kernel timer. */
    TCCR0A = (1u << WGM01);
    TCCR0B = (1u << CS01) | (1u << CS00);
    OCR0A = 249;
    TCNT0 = 0;
    TIFR0 = (1u << OCF0A);
    TIMSK0 = (1u << OCIE0A);
#if NODE_ROLE == 2
    TWAR = 0x12u << 1u;
    TWCR = (1u << TWINT) | (1u << TWEN) | (1u << TWEA) | (1u << TWIE);
#else
    TWSR = (1u << TWPS0);
    TWBR = 198; /* 10 kHz, /4 */
    TWCR = (1u << TWEN);
#endif
    sei();
#endif
}

void nodes_poll(void)
{
#if NODE_ROLE == 1
    uint32_t time = now();
    if ((current_job.state == JOB_QUEUED || current_job.state == JOB_RUNNING ||
         current_job.state == JOB_COPYING) &&
        (uint32_t)(time - submitted_at) >= 120000UL)
        current_job.state = JOB_UNKNOWN;
    if (have_seen && (uint32_t)(time - last_change) >= 2000u) {
        remote.state = 0;
        if (current_job.state == JOB_QUEUED || current_job.state == JOB_RUNNING ||
            current_job.state == JOB_COPYING)
            current_job.state = JOB_UNKNOWN;
    }
    if (phase == 0) {
        if ((int32_t)(time - next_probe) < 0) return;
        next_probe = time + (deploy_active ? 1u : 500u);
        if (deploy_active && !deploy_pending &&
            (uint32_t)(time - deploy_last_action) >= 1000u)
            deploy_pending = 1; /* Re-send an unacknowledged absolute chunk. */
        sending = deploy_pending || resend || (cancel_pending && attempts < 3 &&
                  (!attempts || (uint32_t)(time-last_send) >= 1000u)) ||
                  (current_job.state == JOB_QUEUED && !acknowledged &&
                  attempts < 3 && (!attempts || (uint32_t)(time-last_send) >= 1000u));
        if (sending) { if (attempts < 3) attempts++; last_send = time; resend = 0; }
        TWCR = (1u << TWINT) | (1u << TWEN) | (1u << TWSTA);
        deadline = time + 50u;
        phase = 1;
        return;
    }
    if (phase == 4) {
        if (!(TWCR & (1u << TWSTO)) || (int32_t)(time - deadline) >= 0) {
            TWCR = 0;
            TWCR = (1u << TWEN);
            phase = 0;
        }
        return;
    }
    if ((int32_t)(time - deadline) >= 0) { failed(255); return; }
    if (!(TWCR & (1u << TWINT))) return;
    uint8_t status = TW_STATUS;
    if (phase == 1 && status == TW_START) {
        TWDR = (0x12u << 1u) | (sending ? 0u : 1u);
        TWCR = (1u << TWINT) | (1u << TWEN);
        phase = 2;
    } else if (phase == 2 && sending && status == TW_MT_SLA_ACK) {
        index = 0;
        TWDR = command[index++];
        TWCR = (1u << TWINT) | (1u << TWEN);
        phase = 5;
    } else if (phase == 5 && status == TW_MT_DATA_ACK) {
        if (index == JOB_COMMAND_SIZE) {
            if (deploy_active && deploy_pending) {
                deploy_pending = 0;
                deploy_last_action = time;
            }
            stop_probe();
        }
        else {
            TWDR = command[index++];
            TWCR = (1u << TWINT) | (1u << TWEN);
        }
    } else if (phase == 2 && !sending && status == TW_MR_SLA_ACK) {
        index = 0;
        TWCR = (1u << TWINT) | (1u << TWEN) | (1u << TWEA);
        phase = 3;
    } else if (phase == 3 && status ==
               (index < JOB_REPORT_SIZE-1u ? TW_MR_DATA_ACK : TW_MR_DATA_NACK)) {
        frame[index++] = TWDR;
        if (index == JOB_REPORT_SIZE) {
            if (!job_valid_report(frame)) { failed(254); return; }
            uint16_t beat = frame[1] | ((uint16_t)frame[2] << 8u);
            if (!have_seen || beat != remote.heartbeat) {
                have_seen = 1;
                last_change = time;
                remote.state = 2;
            } else if (remote.state != 0) remote.state = 1;
            remote.heartbeat = beat;
            remote.status = 0;
            if (frame[13] && frame[13] <= RV_GUEST_SLOT_COUNT) {
                program_count = frame[13];
                node_program_t *p = &programs[frame[12]];
                memcpy(p->name, frame + 15, sizeof(p->name));
                p->size = frame[27] | ((uint16_t)frame[28] << 8u);
                p->crc = frame[29] | ((uint16_t)frame[30] << 8u);
                p->priority = frame[31];
                p->entry = frame[32] | ((uint16_t)frame[33] << 8u);
                p->loaded = frame[14];
                p->seen = 1;
            }
            uint16_t id = frame[3] | ((uint16_t)frame[4] << 8u);
            if (id > sequence) sequence = id;
            if (!id && acknowledged &&
                (current_job.state == JOB_RUNNING || current_job.state == JOB_QUEUED))
                current_job.state = JOB_UNKNOWN; /* Worker reboot lost record. */
            if (id && id == current_job.id) {
                acknowledged = 1;
                current_job.state = frame[5];
                if ((current_job.state == JOB_QUEUED || current_job.state == JOB_RUNNING) &&
                    (uint32_t)(time - submitted_at) >= 120000UL)
                    current_job.state = JOB_UNKNOWN;
                current_job.detail = frame[6];
                current_job.result = 0;
                for (uint8_t b = 0; b < 4; b++)
                    current_job.result |= (uint32_t)frame[7u+b] << (b*8u);
                if (current_job.state == JOB_DONE ||
                    current_job.state == JOB_FAULT ||
                    current_job.state == JOB_REJECTED ||
                    current_job.state == JOB_CANCELLED) cancel_pending = 0;
                if (deploy_active) {
                    if (current_job.state == JOB_REJECTED ||
                        current_job.state == JOB_FAULT) {
                        deploy_active = deploy_pending = 0;
                    } else if (deploy_stage == 1 &&
                               current_job.state == JOB_COPYING) {
                        deploy_stage = 2;
                        deploy_offset = (uint8_t)current_job.result;
                        uint8_t length = (uint8_t)(deploy_size - deploy_offset);
                        if (length > 11u) length = 11u;
                        uint8_t payload[11];
                        for (uint8_t b = 0; b < length; b++)
                            payload[b] = rv_demo_slot_image_byte(
                                deploy_slot, (uint16_t)deploy_offset + b);
                        job_deploy_data(command, current_job.id, deploy_offset,
                                        payload, length);
                        deploy_pending = 1;
                        next_probe = time;
                    } else if (deploy_stage == 2 &&
                               current_job.state == JOB_COPYING &&
                               current_job.result > deploy_offset) {
                        deploy_offset = (uint8_t)current_job.result;
                        if (deploy_offset < deploy_size) {
                            uint8_t length = (uint8_t)(deploy_size - deploy_offset);
                            if (length > 11u) length = 11u;
                            uint8_t payload[11];
                            for (uint8_t b = 0; b < length; b++)
                                payload[b] = rv_demo_slot_image_byte(
                                    deploy_slot, (uint16_t)deploy_offset + b);
                            job_deploy_data(command, current_job.id, deploy_offset,
                                            payload, length);
                        } else {
                            deploy_stage = 3;
                            job_deploy_commit(command, current_job.id, deploy_name,
                                              deploy_priority, deploy_crc);
                        }
                        deploy_pending = 1;
                        next_probe = time;
                    } else if (deploy_stage == 3 &&
                               current_job.state == JOB_DEPLOYED) {
                        deploy_active = deploy_pending = 0;
                        memset(&current_job, 0, sizeof(current_job));
                        (void)nodes_submit(deploy_name, deploy_priority, deploy_crc);
                    }
                }
            }
            stop_probe();
        } else TWCR = (1u << TWINT) | (1u << TWEN) |
                      (index < JOB_REPORT_SIZE-1u ? (1u << TWEA) : 0u);
    } else { failed(status); return; }
#elif NODE_ROLE == 2
    /* Publish metadata from main, not from interrupted loader updates in ISR. */
    uint8_t tick = (uint8_t)clock_ms;
    if (published_slot != directory_cursor || (uint8_t)(tick - published_tick) >= 128u) {
        uint8_t page[22] = {0};
        uint8_t slot = directory_cursor;
        page[0] = slot;
        page[1] = rv_demo_slot_count();
        page[2] = rv_demo_slot_loaded(slot);
        if (page[2]) {
            memcpy(page + 3, rv_demo_slot_name(slot), 12);
            uint16_t size = rv_demo_slot_image_size(slot);
            uint16_t crc = rv_demo_slot_image_crc(slot);
            uint16_t entry = rv_demo_slot_entry_point(slot);
            page[15] = (uint8_t)size; page[16] = (uint8_t)(size >> 8u);
            page[17] = (uint8_t)crc; page[18] = (uint8_t)(crc >> 8u);
            page[19] = rv_demo_slot_priority(slot);
            page[20] = (uint8_t)entry; page[21] = (uint8_t)(entry >> 8u);
        }
        ATOMIC_BLOCK(ATOMIC_RESTORESTATE) { memcpy(directory, page, sizeof(page)); }
        published_slot = slot;
        published_tick = tick;
    }
    if (!inbox_ready) return;
    uint8_t packet[JOB_COMMAND_SIZE];
    ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {
        for (uint8_t i = 0; i < JOB_COMMAND_SIZE; i++) packet[i] = inbox[i];
        inbox_ready = 0;
    }
    if (!job_valid_command(packet)) return;
    uint16_t id = packet[1] | ((uint16_t)packet[2] << 8u);
    uint16_t packet_crc = packet[19] | ((uint16_t)packet[20] << 8u);
    if (packet[18] == 4) {
        if (id == current_job.id && current_job.state == JOB_COPYING) return;
        if (id <= current_job.id || rv_demo_is_active() ||
            current_job.state == JOB_QUEUED || current_job.state == JOB_RUNNING) return;
        if (current_job.state == JOB_COPYING && transfer_slot >= 0)
            rv_demo_upload_cancel_slot((uint8_t)transfer_slot);
        transfer_slot = rv_demo_upload_begin_cached((const char *)&packet[4], packet[3]);
        memcpy(job_name, &packet[4], sizeof(job_name));
        transfer_size = packet[3];
        transfer_crc = packet[16] | ((uint16_t)packet[17] << 8u);
        current_job.id = id; current_job.result = 0;
        current_job.detail = transfer_slot < 0 ? 4u : 0u;
        current_job.state = transfer_slot < 0 ? JOB_REJECTED : JOB_COPYING;
        return;
    }
    if (packet[18] == 5) {
        if (id != current_job.id || current_job.state != JOB_COPYING ||
            transfer_slot < 0) return;
        uint8_t offset = packet[3], length = packet[4];
        if (offset < current_job.result) return; /* Absolute retry already stored. */
        if (offset != current_job.result || (uint16_t)offset + length > transfer_size) {
            current_job.detail = 5u; current_job.state = JOB_REJECTED; return;
        }
        for (uint8_t b = 0; b < length; b++)
            if (!rv_demo_upload_write_slot((uint8_t)transfer_slot,
                                           (uint16_t)offset + b, packet[5u+b])) {
                current_job.detail = 5u; current_job.state = JOB_REJECTED; return;
            }
        current_job.result += length;
        return;
    }
    if (packet[18] == 6) {
        if (id == current_job.id && current_job.state == JOB_DEPLOYED) return;
        if (id != current_job.id || current_job.state != JOB_COPYING ||
            transfer_slot < 0 || current_job.result != transfer_size ||
            rv_demo_upload_crc16_slot((uint8_t)transfer_slot, transfer_size) != transfer_crc ||
            !rv_demo_upload_commit_metadata_slot((uint8_t)transfer_slot, transfer_crc, 0u)) {
            current_job.detail = 6u; current_job.state = JOB_REJECTED; return;
        }
        (void)rv_demo_set_priority(job_name, packet[3]);
        current_job.detail = 0; current_job.state = JOB_DEPLOYED;
        return;
    }
    if (packet[18] == 3) {
        if (id == current_job.id) {
            if (current_job.state == JOB_RUNNING) rv_demo_request_cancel();
            else if (current_job.state == JOB_QUEUED)
                current_job.state = JOB_CANCELLED;
            else if (current_job.state == JOB_COPYING) {
                if (transfer_slot >= 0) rv_demo_upload_cancel_slot((uint8_t)transfer_slot);
                current_job.state = JOB_CANCELLED;
            }
        } else if (id > current_job.id && current_job.state != JOB_RUNNING &&
                   current_job.state != JOB_QUEUED) {
            /* A fast cancel may overtake the original JOB_RUN.  Retain a
             * terminal tombstone so a delayed/retried RUN with this ID can
             * never start the guest. */
            memcpy(job_name, &packet[4], sizeof(job_name));
            accepted_crc = packet_crc;
            current_job.id = id;
            current_job.result = 0;
            current_job.detail = 0;
            current_job.state = JOB_CANCELLED;
        }
        return;
    }
    if (id == current_job.id) {
        /* Same identity must mean same payload. Never rerun an accepted job.
         * A conflicting duplicate is ignored, retaining the original result. */
        if (packet_crc != accepted_crc) return;
        return;
    }
    if (id < current_job.id || current_job.state == JOB_QUEUED ||
        current_job.state == JOB_RUNNING) return;
    uint8_t detail = rv_demo_is_active() ? 3u : 1u;
    for (uint8_t slot = 0; slot < rv_demo_slot_count(); slot++) {
        if (rv_demo_slot_loaded(slot) &&
            strcmp(rv_demo_slot_name(slot), (const char *)&packet[4]) == 0) {
            uint16_t crc = packet[16] | ((uint16_t)packet[17] << 8u);
            detail = rv_demo_is_active() ? 3u :
                     (packet[18] == 2 || rv_demo_slot_image_crc(slot) == crc) ? 0u : 2u;
            break;
        }
    }
    memcpy(job_name, &packet[4], sizeof(job_name));
    job_priority = packet[3];
    accepted_crc = packet_crc;
    ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {
        current_job.id = id;
        current_job.result = 0;
        current_job.detail = detail;
        current_job.state = detail ? JOB_REJECTED : JOB_QUEUED;
    }
#endif
}

uint8_t nodes_submit(const char *name, uint8_t priority, uint16_t image_crc)
{
#if NODE_ROLE == 1
    if (remote.state != 2 || priority > 3 || !*name || strlen(name) > 11 ||
        current_job.state == JOB_QUEUED || current_job.state == JOB_RUNNING ||
        current_job.state == JOB_UNKNOWN || sequence == UINT16_MAX) return 0;
    job_command(command, ++sequence, name, priority, image_crc);
    if (!job_valid_command(command)) { sequence--; return 0; }
    /* command[] is reused for deploy DATA and CANCEL frames.  Keep the UI/job
     * identity in stable storage instead of treating a wire buffer as a TCB. */
    if (name != deploy_name) {
        memset(deploy_name, 0, sizeof(deploy_name));
        strncpy(deploy_name, name, sizeof(deploy_name) - 1u);
    }
    current_job.id = sequence;
    current_job.state = JOB_QUEUED;
    current_job.detail = 0;
    current_job.result = 0;
    submitted_at = now();
    attempts = acknowledged = 0;
    next_probe = submitted_at;
    return 1;
#else
    (void)name; (void)priority; (void)image_crc;
    return 0;
#endif
}

uint8_t nodes_submit_named(const char *name, uint8_t priority)
{
#if NODE_ROLE == 1
    if (!nodes_submit(name, priority, 0)) return 0;
    /* Version 2 selects the worker's current directory entry by name.
     * Packet CRC and job-ID dedup remain mandatory; version 1 pins image CRC. */
    command[18] = 2;
    uint16_t crc = job_crc(command, JOB_COMMAND_SIZE - 2u);
    command[19] = (uint8_t)crc;
    command[20] = (uint8_t)(crc >> 8u);
    return 1;
#else
    (void)name; (void)priority;
    return 0;
#endif
}

uint8_t nodes_deploy_run(const char *name, uint8_t priority)
{
#if NODE_ROLE == 1
    if (remote.state != 2 || priority > 3 || sequence == UINT16_MAX ||
        current_job.state == JOB_QUEUED || current_job.state == JOB_RUNNING ||
        current_job.state == JOB_COPYING || current_job.state == JOB_UNKNOWN) return 0;
    int8_t local = -1;
    for (uint8_t slot = 0; slot < rv_demo_slot_count(); slot++)
        if (rv_demo_slot_loaded(slot) && strcmp(rv_demo_slot_name(slot), name) == 0)
            local = (int8_t)slot;
    if (local < 0) return 0;
    uint16_t size = rv_demo_slot_image_size((uint8_t)local);
    uint16_t crc = rv_demo_slot_image_crc((uint8_t)local);
    if (rv_demo_slot_entry_point((uint8_t)local) != 0u) return 0;
    for (uint8_t slot = 0; slot < program_count; slot++)
        if (programs[slot].seen && programs[slot].loaded &&
            strcmp(programs[slot].name, name) == 0 &&
            programs[slot].size == size && programs[slot].crc == crc)
            return nodes_submit(name, priority, crc);
    if (size == 0 || size > 192u) return 0;
    deploy_active = deploy_pending = deploy_stage = 1;
    deploy_slot = (uint8_t)local; deploy_priority = priority;
    deploy_offset = 0; deploy_size = (uint8_t)size; deploy_crc = crc;
    memset(deploy_name, 0, sizeof(deploy_name));
    strncpy(deploy_name, name, sizeof(deploy_name)-1u);
    current_job.id = ++sequence; current_job.state = JOB_COPYING;
    current_job.result = 0; current_job.detail = 0;
    job_deploy_begin(command, current_job.id, deploy_name, deploy_size, deploy_crc);
    submitted_at = deploy_last_action = now();
    attempts = acknowledged = 0; next_probe = submitted_at;
    return 1;
#else
    (void)name; (void)priority;
    return 0;
#endif
}

job_info_t nodes_job(void)
{
    job_info_t result = {0};
#if NODE_ROLE
    ATOMIC_BLOCK(ATOMIC_RESTORESTATE) { result = current_job; }
#endif
    return result;
}

void nodes_service(void)
{
#if NODE_ROLE == 2
    nodes_poll();
    if (current_job.state != JOB_QUEUED || rv_demo_is_active()) return;
    ATOMIC_BLOCK(ATOMIC_RESTORESTATE) { current_job.state = JOB_RUNNING; }
    if (run_count != UINT16_MAX) run_count++;
    (void)rv_demo_set_priority(job_name, job_priority);
    uint8_t ok = rv_demo_run_name(job_name);
    uint32_t result = 0;
    uint8_t status = 7;
    for (uint8_t slot = 0; slot < rv_demo_slot_count(); slot++) {
        if (strcmp(rv_demo_slot_name(slot), job_name) == 0) {
            status = rv_demo_slot_status(slot);
            result = rv_demo_slot_exit(slot);
            break;
        }
    }
    ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {
        current_job.result = result;
        current_job.detail = status;
        current_job.state = rv_demo_was_cancelled() ? JOB_CANCELLED :
                            ok && status == 2 ? JOB_DONE : JOB_FAULT;
    }
#endif
}

uint8_t nodes_resend(void)
{
#if NODE_ROLE == 1
    /* Never manually replay an uncertain request after worker state loss. */
    if (!current_job.id || remote.state != 2 || current_job.state == JOB_UNKNOWN)
        return 0;
    resend = 1;
    next_probe = now();
    return 1;
#else
    return 0;
#endif
}
uint8_t nodes_cancel(void)
{
#if NODE_ROLE == 1
    if (remote.state != 2 || (current_job.state != JOB_QUEUED &&
                             current_job.state != JOB_RUNNING &&
                             current_job.state != JOB_COPYING)) return 0;
    job_cancel_command(command, current_job.id, deploy_name);
    deploy_active = deploy_pending = 0;
    cancel_pending = 1;
    attempts = 0;
    next_probe = now();
    return 1;
#else
    return 0;
#endif
}
uint8_t nodes_discard(void)
{
#if NODE_ROLE == 1
    if (current_job.state != JOB_UNKNOWN) return 0;
    memset(&current_job, 0, sizeof(current_job));
    cancel_pending = resend = 0;
    return 1;
#else
    return 0;
#endif
}
uint8_t nodes_retry(void)
{
#if NODE_ROLE == 1
    if (current_job.state != JOB_UNKNOWN || remote.state != 2 ||
        (command[18] != 1 && command[18] != 2)) return 0;
    char name[12];
    memcpy(name, command + 4, sizeof(name));
    uint8_t priority = command[3], version = command[18];
    uint16_t image_crc = command[16] | ((uint16_t)command[17] << 8u);
    memset(&current_job, 0, sizeof(current_job));
    return version == 2 ? nodes_submit_named(name, priority) :
                          nodes_submit(name, priority, image_crc);
#else
    return 0;
#endif
}
uint16_t nodes_run_count(void)
{
#if NODE_ROLE == 2
    return run_count;
#else
    return 0;
#endif
}

uint8_t nodes_program_count(void)
{
#if NODE_ROLE == 1
    return program_count;
#else
    return 0;
#endif
}
const node_program_t *nodes_program(uint8_t slot)
{
#if NODE_ROLE == 1
    if (slot < program_count) return &programs[slot];
#else
    (void)slot;
#endif
    return 0;
}
const char *nodes_job_name(void)
{
#if NODE_ROLE == 1
    return deploy_name;
#elif NODE_ROLE == 2
    return job_name;
#else
    return "";
#endif
}

node_info_t nodes_info(void)
{
    node_info_t result = {0};
#if NODE_ROLE == 1
    nodes_poll();
    result = remote;
#elif NODE_ROLE == 2
    result.heartbeat = (uint16_t)now();
    result.state = paused ? 0 : 2;
#endif
    return result;
}

/* Worker-only fault injection: stop answering without disabling its clock,
 * UART shell or VM. The master must not assume heartbeat equals job health. */
void nodes_pause(void)
{
#if NODE_ROLE == 2
    ATOMIC_BLOCK(ATOMIC_RESTORESTATE) { TWCR = 0; paused = 1; }
#endif
}
void nodes_resume(void)
{
#if NODE_ROLE == 2
    ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {
        paused = 0;
        TWCR = (1u << TWINT) | (1u << TWEN) | (1u << TWEA) | (1u << TWIE);
    }
#endif
}
