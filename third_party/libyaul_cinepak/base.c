#include "base.h"

void satAssert(const char *filename, int line, const char *msg) {
  cpu_intc_mask_set(15);

  if (msg != NULL) {
    dbgio_printf("%s\n\n", msg);
  } else {
    dbgio_printf("Assertion failed at %s:%d\n\n", filename, line);
  }

  dbgio_flush();
  vdp2_sync();
  vdp2_sync_wait();

  __asm__ volatile("sleep\n");

  while (true) {
  }
}
