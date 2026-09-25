#ifndef BASE_H
#define BASE_H

#include "intellisense.h"

#include <gamemath.h>
#include <stdint.h>
#include <yaul.h>

extern void satAssert(const char *filename, int line, const char *msg);

// #define HAS_DEBUG_REQUIRE_FUNCTIONS
#ifdef HAS_DEBUG_REQUIRE_FUNCTIONS
#define __STRINGIFY(x) #x
#define __TOSTRING(x) __STRINGIFY(x)
#define REQUIRE_BODY_OP(A, B, _OP_)                                            \
  do {                                                                         \
    if (!((A)_OP_(B))) {                                                       \
      satAssert(__FILE__, __LINE__,                                            \
        "Error (" #A " " #_OP_ " " #B ") at " __FILE__                         \
        ":" __TOSTRING(__LINE__));                                             \
    }                                                                          \
  } while (0)

#define DEBUG_REQUIRE_EQ(A, B) REQUIRE_BODY_OP(A, B, ==)
#define DEBUG_REQUIRE_NE(A, B) REQUIRE_BODY_OP(A, B, !=)
#define DEBUG_REQUIRE_LE(A, B) REQUIRE_BODY_OP(A, B, <=)
#define DEBUG_REQUIRE_LT(A, B) REQUIRE_BODY_OP(A, B, <)
#define DEBUG_REQUIRE_GE(A, B) REQUIRE_BODY_OP(A, B, >=)
#define DEBUG_REQUIRE_GT(A, B) REQUIRE_BODY_OP(A, B, >)
#define DEBUG_REQUIRE(A)                                                       \
  do {                                                                         \
    if (!(A)) {                                                                \
      satAssert(__FILE__, __LINE__,                                            \
        "Error (" #A " is false) at " __FILE__ ":" __TOSTRING(__LINE__));      \
    }                                                                          \
  } while (0)

#define VDP_INFLOOP()                                                          \
  while (1) {                                                                  \
    dbgio_flush();                                                             \
    vdp2_sync();                                                               \
    vdp2_sync_wait();                                                          \
  }

#define logError(__FMT__, ...)                                                                     \
  do {                                                                                             \
    clearLog();                                                                                    \
    dbgio_printf(__FMT__, __VA_ARGS__);                                                            \
    dbgio_flush();                                                                                 \
    vdp2_sync();                                                                                   \
    vdp2_sync_wait();                                                                              \
  } while (true)

#define logMessage(__FMT__, ...)                                                                   \
  do {                                                                                             \
    dbgio_printf(__FMT__, __VA_ARGS__);                                                            \
    dbgio_flush();                                                                                 \
  } while (false)

#else // HAS_DEBUG_REQUIRE_FUNCTIONS
#define DEBUG_REQUIRE_EQ(A, B)
#define DEBUG_REQUIRE_NE(A, B)
#define DEBUG_REQUIRE_LE(A, B)
#define DEBUG_REQUIRE_LT(A, B)
#define DEBUG_REQUIRE_GE(A, B)
#define DEBUG_REQUIRE_GT(A, B)
#define DEBUG_REQUIRE(A)
#define VDP_INFLOOP() \
    do {              \
    } while (false)

#define logError(__FMT__, ...) \
    do {                       \
    } while (true)

#define logMessage(__FMT__, ...) \
    do {                         \
    } while (false)

#endif // HAS_DEBUG_REQUIRE_FUNCTIONS

static __unused void clearLog() { dbgio_printf("[H[2J"); }

#endif // BASE_H
