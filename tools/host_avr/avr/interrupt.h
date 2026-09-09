#ifndef HOST_AVR_INTERRUPT_H
#define HOST_AVR_INTERRUPT_H

void host_cli(void);
#define cli() host_cli()

#endif
