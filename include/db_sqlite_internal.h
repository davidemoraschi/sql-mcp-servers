/*
 * db_sqlite_internal.h - Private shared state between db_sqlite_core.c and db_sqlite_transfer.c
 *
 * Not part of the public API. Include only from SQLite translation units.
 */

#ifndef DB_SQLITE_INTERNAL_H
#define DB_SQLITE_INTERNAL_H

#include <stddef.h>

#if defined(__has_include)
  #if __has_include(<sqlite3.h>)
    #include <sqlite3.h>
    #define HAVE_SQLITE3 1
  #elif __has_include("sqlite3.h")
    #include "sqlite3.h"
    #define HAVE_SQLITE3 1
  #endif
#endif
#ifndef HAVE_SQLITE3
  #include "sqlite3.h"
  #define HAVE_SQLITE3 1
#endif

/* Defined in db_sqlite_core.c, used by db_sqlite_transfer.c */
#ifdef HAVE_SQLITE3
extern sqlite3 *g_db;
#endif

char *safe_strdup(const char *s);

#endif /* DB_SQLITE_INTERNAL_H */
