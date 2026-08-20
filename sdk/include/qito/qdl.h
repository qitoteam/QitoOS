/* QitoOS SDK - QDL */
#ifndef QITO_SDK_QDL_H
#define QITO_SDK_QDL_H

/* QDL is same header as QTX with format 'D', library flag, export table */

#define QDL_PATH "/lib/"

void *qdl_resolve(const char *symbol); /* resolve symbol from loaded QDLs */

#endif
