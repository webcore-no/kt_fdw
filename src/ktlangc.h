/*-------------------------------------------------------------------------
 *
 * Kyoto Tycoon Foreign Data Wrapper for PostgreSQL
 *
 * Copyright (c) 2013 CloudFlare
 *
 * This software is released under the MIT Licence
 *
 * Author: Matvey Arye <mat@cloudflare.com>
 *
 * IDENTIFICATION
 *        kt_fdw/src/ktlangc.h
 *
 *-------------------------------------------------------------------------
 */

/*
 * This is a C wrapper around KT's C++ interface. Needed for PostgreSQL integration
 *
 */


#ifndef _KTLANGC_H                       /* duplication check */
#define _KTLANGC_H

#if defined(__cplusplus)
extern "C" {
#endif

#if !defined(__STDC_LIMIT_MACROS)
#define __STDC_LIMIT_MACROS  1           /**< enable limit macros for C++ */
#endif

#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <float.h>
#include <limits.h>
#include <locale.h>
#include <math.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <string.h>
#include <time.h>
#include <stdint.h>

/**
 * C wrapper of polymorphic database.
 */
typedef struct {
  void* db;                              /**< dummy member */
} KTDB;

typedef struct {
  void* cur;                             /**< dummy member */
} KTCUR;

KTDB* ktdbnew(void);
void ktdbdel(KTDB* db);
bool ktdbopen(KTDB* db, const char* host, int32_t port, double timeout);
bool ktdbclose(KTDB* db);

int64_t ktdbcount(KTDB* db);

KTCUR* get_cursor(KTDB* DB);
void ktcurdel(KTCUR* cur);
bool next(KTDB* db, KTCUR* cur, char **key, char **value);
bool ktget(KTDB* db, char *key, char **value);

bool ktgetl(KTDB* db, char *key, size_t *key_len, char **value, size_t *val_len);
bool nextl(KTDB* db, KTCUR* cur, char **key, size_t *key_len, char **value, size_t *val_len);
bool ktadd(KTDB*db, const char * key, const char * value);
bool ktaddl(KTDB*db, const char * key, size_t keylen,  const char * value, size_t vallen);
bool ktreplace(KTDB*db, const char * key, const char * value);
bool ktreplacel(KTDB*db, const char * key, size_t keylen, const char * value, size_t vallen);
bool ktremove(KTDB*db, const char * key);
bool ktremovel(KTDB*db, const char * key, size_t keylen);

const char *ktgeterror(KTDB* db);
const char *ktgeterrormsg(KTDB* db);

bool ktbegin_transaction(KTDB *db);
bool ktcommit(KTDB *db);
bool ktabort(KTDB *db);

#if defined(__cplusplus)
}
#endif

#endif                                   /* duplication check */

/* END OF FILE */
