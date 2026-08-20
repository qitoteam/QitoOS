/* QitoOS SDK - QTX format */
#ifndef QITO_SDK_QTX_H
#define QITO_SDK_QTX_H

#include <stdint.h>

#define QX_SIGNATURE "QX"
#define QTX_FORMAT_EXEC 'X'
#define QTX_FORMAT_LIB 'D'
#define QTX_VERSION 1
#define QTX_MAX_SECTIONS 16
#define QTX_MAX_IMPORTS 64
#define QTX_MAX_SYMBOLS 128
#define QTX_NAME_MAX 24
#define QTX_MACHINE_X86_64 0x8664

#define QTX_FLAG_EXECUTABLE 0x0001
#define QTX_FLAG_LIBRARY 0x0002

typedef enum {
    QTX_SECTION_CODE=1,
    QTX_SECTION_DATA=2,
    QTX_SECTION_RODATA=3,
    QTX_SECTION_BSS=4,
    QTX_SECTION_RESOURCE=5
} qtx_section_kind_t;

#define QTX_SEC_READ 0x01
#define QTX_SEC_WRITE 0x02
#define QTX_SEC_EXEC 0x04

/* For user programs, imports are resolved by kernel at load time */
#define QTX_IMPORT(name) extern void *name

#endif
