#ifndef FS_TEST_PID_TYPES_H
#define FS_TEST_PID_TYPES_H

/* Host-test stand-in for the PID type: Core/Inc/pid.h is skipped by defining
 * its include guard (INC_PID_H) on the command line, so the HAL is not needed. */
typedef struct {
    float unused;
} PID_TypeDef;

#endif /* FS_TEST_PID_TYPES_H */
