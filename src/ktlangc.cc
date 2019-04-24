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
 *        kt_fdw/src/ktlang.h
 *
 *-------------------------------------------------------------------------
 */

#include <ktremotedb.h>
#include "ktlangc.h"

using namespace kyototycoon;

extern "C" {
#include "postgres.h"

/**
 * Create a database object.
 */
KTDB* ktdbnew(void) {
    _assert_(true);
    return (KTDB*)new RemoteDB;
}


/**
 * Destroy a database object.
 */
void ktdbdel(KTDB* db) {
    _assert_(db);
    RemoteDB* pdb = (RemoteDB*)db;
    delete pdb;
}

/**
 * Open a database file.
 */
bool ktdbopen(KTDB* db, const char* host, int32_t port, double timeout) {
    _assert_(db && host && port && timeout);
    RemoteDB* pdb = (RemoteDB*)db;
    return pdb->open(host, port, timeout);
}

/**
 * Close the database file.
 */
bool ktdbclose(KTDB* db) {
    _assert_(db);
    RemoteDB* pdb = (RemoteDB*)db;
    return pdb->close();
}

/*
 * get a count of the number of keys
 */
int64_t ktdbcount(KTDB* db) {
    _assert_(db);
    RemoteDB* pdb = (RemoteDB*)db;
    return pdb->count();
}

KTCUR* get_cursor(KTDB* db) {
    _assert_(db);
    RemoteDB* pdb = (RemoteDB*)db;
    RemoteDB::Cursor *cur = pdb->cursor();
    cur->jump();
    return (KTCUR*) cur;
}

void ktcurdel(KTCUR* cur) {
    _assert_(cur);
    RemoteDB::Cursor *rcur = (RemoteDB::Cursor *) cur;
    delete rcur;
}

bool next(KTDB* db, KTCUR* cur, char **key, char **value)
{
    std::string skey;
    std::string sval;

    RemoteDB::Cursor *rcur = (RemoteDB::Cursor *) cur;
    bool res = rcur->get(&skey, &sval, NULL, true);
    if(!res)
        return false;

    *key = (char *) palloc(sizeof(char)*(skey.length()+1));
    *value = (char *) palloc(sizeof(char)*(sval.length()+1));

    std::strcpy(*key, skey.c_str());
    std::strcpy(*value, sval.c_str());
    return true;
}

bool nextl(KTDB* db, KTCUR* cur, char **key, size_t *key_len, char **value, size_t *val_len)
{
    std::string skey;
    std::string sval;

    RemoteDB::Cursor *rcur = (RemoteDB::Cursor *) cur;
    bool res = rcur->get(&skey, &sval, NULL, true);
    if(!res)
        return false;

    *key = (char *) palloc(sizeof(char)*(skey.length() + 1));
    *value = (char *) palloc(sizeof(char)*(sval.length() + 1));
    *key_len = skey.length();
    *val_len = sval.length();

    memcpy(*key, skey.c_str(), skey.length());
    memcpy(*value, sval.c_str(), sval.length());
    *(*key + skey.length()) = '\0';
    *(*value + sval.length()) = '\0';

    return true;
}

bool ktget(KTDB* db, char *key, char **value){
    _assert_(db && key);

    std::string skey(key);
    std::string sval;
    RemoteDB* pdb = (RemoteDB*)db;

    if(!pdb->get(skey, &sval))
        return false;

    *value = (char *) palloc(sizeof(char)*(sval.length()+1));
    std::strcpy(*value, sval.c_str());

    return true;
}

bool ktgetl(KTDB* db, char *key, size_t *key_len, char **value, size_t *val_len){
    _assert_(db && key);

    std::string skey(key);
    std::string sval;
    RemoteDB* pdb = (RemoteDB*)db;

    if(!pdb->get(skey, &sval))
        return false;

    *value = (char *) palloc(sizeof(char)*(sval.length()));
    *val_len = sval.length();
    memcpy(*value, sval.c_str(), sval.length());

    return true;
}

bool ktadd(KTDB*db, const char * key, const char * value)
{
    _assert_(db);
    RemoteDB* pdb = (RemoteDB*)db;

    std::string skey(key);
    std::string sval(value);

    return pdb->add(skey, sval);
}

bool ktaddl(KTDB*db, const char * key, size_t keylen, const char * value, size_t vallen)
{
    _assert_(db);
    RemoteDB* pdb = (RemoteDB*)db;

    return pdb->add(key, keylen, value, vallen);
}

bool ktreplace(KTDB*db, const char * key, const char * value)
{
    _assert_(db);
    RemoteDB* pdb = (RemoteDB*)db;

    std::string skey(key);
    std::string sval(value);

    return pdb->replace(skey, sval);
}

bool ktreplacel(KTDB*db, const char * key, size_t keylen,
                         const char * value, size_t vallen)
{
    _assert_(db);
    RemoteDB* pdb = (RemoteDB*)db;

    return pdb->replace(key, keylen, value, vallen);
}

bool ktremove(KTDB*db, const char * key)
{
    _assert_(db);
    RemoteDB* pdb = (RemoteDB*)db;

    std::string skey(key);

    return pdb->remove(skey);
}

bool ktremovel(KTDB*db, const char * key, size_t keylen)
{
    _assert_(db);
    RemoteDB* pdb = (RemoteDB*)db;

    return pdb->remove(key, keylen);
}

const char *ktgeterror(KTDB* db)
{
    _assert_(db);
    RemoteDB* pdb = (RemoteDB*)db;
    return pdb->error().name();
}

const char *ktgeterrormsg(KTDB* db)
{
    _assert_(db);
    RemoteDB* pdb = (RemoteDB*)db;
    return pdb->error().message();
}

bool ktbegin_transaction(KTDB *db)
{
    _assert_(db);
    RemoteDB* pdb = (RemoteDB*)db;
    std::map<std::string, std::string> in;
    std::map<std::string, std::string> out;
    return pdb->play_script("ktfdwbegintransaction", in, &out);
}

bool ktcommit(KTDB *db)
{
    _assert_(db);
    RemoteDB* pdb = (RemoteDB*)db;
    std::map<std::string, std::string> in;
    std::map<std::string, std::string> out;
    return pdb->play_script("ktfdwcommit", in, &out);
}

bool ktabort(KTDB *db)
{
    _assert_(db);
    RemoteDB* pdb = (RemoteDB*)db;
    std::map<std::string, std::string> in;
    std::map<std::string, std::string> out;
    return pdb->play_script("ktfdwabort", in, &out);
}

}

// END OF FILE
