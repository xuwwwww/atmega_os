#ifndef NODES_H
#define NODES_H
#include <stdint.h>
#include "job_protocol.h"
#ifndef NODE_ROLE
#define NODE_ROLE 0
#endif
#if NODE_ROLE < 0 || NODE_ROLE > 2
#error "NODE_ROLE must be 0 (disabled), 1 (coordinator), or 2 (worker)"
#endif
typedef struct {
    uint16_t heartbeat;
    uint16_t failures;
    uint8_t state; /* 0 OFFLINE, 1 SUSPECT, 2 ONLINE */
    uint8_t status;
} node_info_t;
typedef struct {
    char name[12];
    uint16_t size, crc, entry;
    uint8_t priority, loaded, seen;
} node_program_t;
uint8_t nodes_program_count(void);
const node_program_t *nodes_program(uint8_t slot);
const char *nodes_job_name(void);
void nodes_init(void);
void nodes_poll(void);
node_info_t nodes_info(void);
void nodes_pause(void);
void nodes_resume(void);
uint8_t nodes_submit(const char *name, uint8_t priority, uint16_t image_crc);
uint8_t nodes_submit_named(const char *name, uint8_t priority);
uint8_t nodes_deploy_run(const char *name, uint8_t priority);
job_info_t nodes_job(void);
void nodes_service(void); /* shell safe point only; never called from ISR/VM */
uint8_t nodes_resend(void);
uint8_t nodes_cancel(void);
uint8_t nodes_discard(void);
uint8_t nodes_retry(void);
uint16_t nodes_run_count(void);
#endif
