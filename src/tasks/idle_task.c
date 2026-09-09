#include "idle_task.h"

void idle_task(void)
{
    for (;;)
    {
        __asm__ __volatile__("nop");
    }
}
