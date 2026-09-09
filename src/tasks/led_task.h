#ifndef LED_TASK_H
#define LED_TASK_H

#include <stdint.h>

enum
{
    LED_MODE_BLINK = 0,
    LED_MODE_OFF,
    LED_MODE_ON,
};

void led_task_init(void);
void led_task(void);
void led_task_set_mode(uint8_t mode);
uint8_t led_task_get_mode(void);

#endif
