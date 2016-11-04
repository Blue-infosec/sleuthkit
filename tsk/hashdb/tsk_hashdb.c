/*
* The Sleuth Kit
*
* Brian Carrier [carrier <at> sleuthkit [dot] org]
* Copyright (c) 2003-2014 Brian Carrier.  All rights reserved
*
*
* This software is distributed under the Common Public License 1.0
*/

#include "tsk_hashdb_i.h"

#ifdef TSK_WIN32
#include <share.h>
#endif

/**
* \file tsk_hashdb.c
* Contains the code to open and close all supported hash database types.
*/

static FILE *
    hdb_open_file(TSK_TCHAR *file_path)
{
    FILE *file = NULL;

#ifdef TSK_WIN32
    int fd = 0;
    if (_wsopen_s(&fd, file_path, _O_RDONLY | _O_BINARY, _SH_DENYNO, 0) == 0) {
        file = _wfdopen(fd, L"rb");
    }
#else
    file = fopen(file_path, "rb");
#endif

    return file;
}

static TSK_HDB_DBTYPE_ENUM
    hdb_determine_db_type(FILE *hDb, const TSK_TCHAR *db_path)
{
    TSK_HDB_DBTYPE_ENUM db_type = TSK_HDB_DBTYPE_INVALID_ID;

    if (sqlite_hdb_is_sqlite_file(hDb)) {
        fseeko(hDb, 0, SEEK_SET);
        return TSK_HDB_DBTYPE_SQLITE_ID;
    }

    // Try each supported text-format database type to ensure a confident
    // identification. Only one of the tests should succeed. 
    fseeko(hDb, 0, SEEK_SET);
    if (nsrl_test(hDb)) {
        db_type = TSK_HDB_DBTYPE_NSRL_ID;
    }

    fseeko(hDb, 0, SEEK_SET);
    if (md5sum_test(hDb)) {
        if (db_type != TSK_HDB_DBTYPE_INVALID_ID) {
            fseeko(hDb, 0, SEEK_SET);
            return TSK_HDB_DBTYPE_INVALID_ID;
        }
        db_type = TSK_HDB_DBTYPE_MD5SUM_ID;
    }

    fseeko(hDb, 0, SEEK_SET);
    if (encase_test(hDb)) {
        if (db_type != TSK_HDB_DBTYPE_INVALID_ID) {
            fseeko(hDb, 0, SEEK_SET);
            return TSK_HDB_DBTYPE_INVALID_ID;
        }
        db_type = TSK_HDB_DBTYPE_ENCASE_ID;
    }

    fseeko(hDb, 0, SEEK_SET);
    if (hk_test(hDb)) {
        if (db_type != TSK_HDB_DBTYPE_INVALID_ID) {
            fseeko(hDb, 0, SEEK_SET);
            return TSK_HDB_DBTYPE_INVALID_ID;
        }
        db_type = TSK_HDB_DBTYPE_HK_ID;
    }

    fseeko(hDb, 0, SEEK_SET);
    return db_type;
}

/**
* \ingroup hashdblib
* Creates a new hash database. 
* @param file_path Path for database to create.
* @return 0 on success, 1 otherwise
*/
uint8_t 
    tsk_hdb_create(TSK_TCHAR *file_path)
{
    TSK_TCHAR *ext = NULL; 

    if (NULL ==  file_path) {
        tsk_error_reset();
        tsk_error_set_errno(TSK_ERR_HDB_ARG);
        tsk_error_set_errstr("tsk_hdb_create: NULL file path");
        return 1;
    }

    ext = TSTRRCHR(file_path, _TSK_T('.'));    
    if ((NULL != ext) && (TSTRLEN(ext) >= 4) && (TSTRCMP(ext, _TSK_T(".kdb")) == 0)) {
        return sqlite_hdb_create_db(file_path); 
    }
    else {
        tsk_error_reset();
        tsk_error_set_errno(TSK_ERR_HDB_ARG);
        tsk_error_set_errstr("tsk_hdb_create: path must end in .kdb extension");
        return 1;
    }
}

/**
* \ingroup hashdblib
*
* Opens an existing hash database. 
* @param file_path Path to database or database index file.
* @param flags Flags for opening the database.  
* @return Pointer to a struct representing the hash database or NULL on error.
*/
TSK_HDB_INFO *
    tsk_hdb_open(TSK_TCHAR *file_path, TSK_HDB_OPEN_ENUM flags)
{
    const char *func_name = "tsk_hdb_open";
    uint8_t file_path_is_idx_path = 0;
    TSK_TCHAR *db_path = NULL;
    TSK_TCHAR *ext = NULL; 
    FILE *hDb = NULL;
    FILE *hIdx = NULL;
    TSK_HDB_DBTYPE_ENUM db_type = TSK_HDB_DBTYPE_INVALID_ID;
    TSK_HDB_INFO *hdb_info = NULL;

    if (NULL == file_path) {
        tsk_error_reset();
        tsk_error_set_errno(TSK_ERR_HDB_ARG);
        tsk_error_set_errstr("%s: NULL file path", func_name);
        return NULL;
    }

    // Determine the hash database path using the given file path. Note that
    // direct use of an external index file for a text-format hash database for 
    // simple yes/no lookups is both explicitly and implicitly supported. For 
    // such "index only" databases, the path to where the hash database is 
    // normally required to be is still needed because of the way the code for
    // text-format hash databases has been written.
    db_path = (TSK_TCHAR*)tsk_malloc((TSTRLEN(file_path) + 1) * sizeof(TSK_TCHAR));
    if (NULL == db_path) {
        return NULL;
    }

    ext = TSTRRCHR(file_path, _TSK_T('-'));    
    if ((NULL != ext) && 
        (TSTRLEN(ext) == 8 || TSTRLEN(ext) == 9) && 
        ((TSTRCMP(ext, _TSK_T("-md5.idx")) == 0) || TSTRCMP(ext, _TSK_T("-sha1.idx")) == 0)) {
            // The file path extension suggests the path is for an external index
            // file generated by TSK for a text-format hash database. In this case, 
            // the database path should be the given file path sans the extension 
            // because the hash database, if it is available for lookups, is 
            // required to be be in the same directory as the external index file.
            file_path_is_idx_path = 1;
            TSTRNCPY(db_path, file_path, (ext - file_path));
    }
    else {
        TSTRNCPY(db_path, file_path, TSTRLEN(file_path));
    }

    // Determine the database type.
    if ((flags & TSK_HDB_OPEN_IDXONLY) == 0) {
        hDb = hdb_open_file(db_path);
        if (NULL != hDb) {
            db_type = hdb_determine_db_type(hDb, db_path);
            if (TSK_HDB_DBTYPE_INVALID_ID == db_type) {
                tsk_error_reset();
                tsk_error_set_errno(TSK_ERR_HDB_UNKTYPE);
                tsk_error_set_errstr("%s: error determining hash database type of %" PRIttocTSK, func_name, db_path);
                free(db_path);
                return NULL;
            }
        }
        else {
            if (file_path_is_idx_path) {
                db_type = TSK_HDB_DBTYPE_IDXONLY_ID;
            }
            else {
                tsk_error_reset();
                tsk_error_set_errno(TSK_ERR_HDB_OPEN);
                tsk_error_set_errstr("%s: failed to open %" PRIttocTSK, func_name, db_path);
                free(db_path);
                return NULL;
            }
        }
    }
    else {
        db_type = TSK_HDB_DBTYPE_IDXONLY_ID;
    }

    switch (db_type) {
    case TSK_HDB_DBTYPE_NSRL_ID:
        hdb_info = nsrl_open(hDb, db_path);
        break;
    case TSK_HDB_DBTYPE_MD5SUM_ID:
        hdb_info = md5sum_open(hDb, db_path);
        break;
    case TSK_HDB_DBTYPE_ENCASE_ID:
        hdb_info = encase_open(hDb, db_path);
        break;
    case TSK_HDB_DBTYPE_HK_ID:
        hdb_info = hk_open(hDb, db_path);
        break;
    case TSK_HDB_DBTYPE_IDXONLY_ID:
        hIdx = hdb_open_file(file_path);
        if (NULL == hIdx) {
            tsk_error_reset();
            tsk_error_set_errno(TSK_ERR_HDB_OPEN);
            tsk_error_set_errstr("%s: database is index only, failed to open index %" PRIttocTSK, func_name, db_path);
            free(db_path);
            return NULL;
        } 
        else {
            fclose(hIdx);
        }
        hdb_info = idxonly_open(db_path);
        break;
    case TSK_HDB_DBTYPE_SQLITE_ID: 
        if (NULL != hDb) {
            fclose(hDb);
        }
        hdb_info = sqlite_hdb_open(db_path);
        break;

        // included to prevent compiler warnings that it is not used
    case TSK_HDB_DBTYPE_INVALID_ID:
        break;
    }

    if (NULL != db_path) {
        free(db_path);
    }

    return hdb_info;
}

const TSK_TCHAR *
    tsk_hdb_get_db_path(TSK_HDB_INFO * hdb_info)
{
    if (!hdb_info) {
        tsk_error_reset();
        tsk_error_set_errno(TSK_ERR_HDB_ARG);
        tsk_error_set_errstr("tsk_hdb_get_db_path: NULL hdb_info");
        return 0;
    }

    return hdb_info->get_db_path(hdb_info);
}

const char *
    tsk_hdb_get_display_name(TSK_HDB_INFO * hdb_info)
{
    if (!hdb_info) {
        tsk_error_reset();
        tsk_error_set_errno(TSK_ERR_HDB_ARG);
        tsk_error_set_errstr("tsk_hdb_get_display_name: NULL hdb_info");
        return 0;
    }

    return hdb_info->get_display_name(hdb_info);
}

uint8_t 
    tsk_hdb_uses_external_indexes(TSK_HDB_INFO *hdb_info)
{
    if (!hdb_info) {
        tsk_error_reset();
        tsk_error_set_errno(TSK_ERR_HDB_ARG);
        tsk_error_set_errstr("tsk_hdb_get_display_name: NULL hdb_info");
        return 0;
    }

    return hdb_info->uses_external_indexes();
}

const TSK_TCHAR *
    tsk_hdb_get_idx_path(TSK_HDB_INFO * hdb_info, TSK_HDB_HTYPE_ENUM htype)
{
    if (!hdb_info) {
        tsk_error_reset();
        tsk_error_set_errno(TSK_ERR_HDB_ARG);
        tsk_error_set_errstr("tsk_hdb_get_idx_path: NULL hdb_info");
        return 0;
    }

    return hdb_info->get_index_path(hdb_info, htype);
}

uint8_t tsk_hdb_open_idx(TSK_HDB_INFO *hdb_info, TSK_HDB_HTYPE_ENUM htype)
{
    if (!hdb_info) {
        tsk_error_reset();
        tsk_error_set_errno(TSK_ERR_HDB_ARG);
        tsk_error_set_errstr("tsk_hdb_open_idx: NULL hdb_info");
        return 0;
    }

    return hdb_info->open_index(hdb_info, htype);
}

/**
* \ingroup hashdblib
* Determine if the open hash database has an index.
*
* @param hdb_info Hash database to consider
* @param htype Hash type that index should be of
*
* @return 1 if index exists and 0 if not
*/
uint8_t
    tsk_hdb_has_idx(TSK_HDB_INFO * hdb_info, TSK_HDB_HTYPE_ENUM htype)
{
    if (!hdb_info) {
        tsk_error_reset();
        tsk_error_set_errno(TSK_ERR_HDB_ARG);
        tsk_error_set_errstr("tsk_hdb_has_idx: NULL hdb_info");
        return 0;
    }

    return (hdb_info->open_index(hdb_info, htype) == 0) ? 1 : 0;
}

/**
* \ingroup hashdblib
* Test for index only (legacy)
* Assumes that the db was opened using the TSK_HDB_OPEN_TRY option.
*
* @param hdb_info Hash database to consider
*
* @return 1 if there is only a legacy index AND no db, 0 otherwise
*/
uint8_t
    tsk_hdb_is_idx_only(TSK_HDB_INFO *hdb_info)
{
    if (!hdb_info) {
        tsk_error_reset();
        tsk_error_set_errno(TSK_ERR_HDB_ARG);
        tsk_error_set_errstr("tsk_hdb_is_idx_only: NULL hdb_info");
        return 0;
    }

    return (hdb_info->db_type == TSK_HDB_DBTYPE_IDXONLY_ID);
}

/**
* \ingroup hashdblib
* Create an index for an open hash database.
* @param hdb_info Open hash database to index
* @param type Text of hash database type
* @returns 1 on error
*/
uint8_t
    tsk_hdb_make_index(TSK_HDB_INFO *hdb_info, TSK_TCHAR *type)
{
    if (!hdb_info) {
        tsk_error_reset();
        tsk_error_set_errno(TSK_ERR_HDB_ARG);
        tsk_error_set_errstr("tsk_hdb_make_index: NULL hdb_info");
        return 1;
    }

    return hdb_info->make_index(hdb_info, type);
}

/**
* \ingroup hashdblib
* Searches a hash database for a text/ASCII hash value.
* @param hdb_info Struct representing an open hash database.
* @param hash Hash value to search for (NULL terminated string).
* @param flags Flags to control behavior of the lookup.
* @param action Callback function to call for each entry in the hash 
* database that matches the hash value argument (not called if QUICK 
* flag is given).
* @param ptr Pointer to data to pass to each invocation of the callback.
* @return -1 on error, 0 if hash value not found, and 1 if value was found.
*/
int8_t
    tsk_hdb_lookup_str(TSK_HDB_INFO *hdb_info, const char *hash,
    TSK_HDB_FLAG_ENUM flags, TSK_HDB_LOOKUP_FN action,
    void *ptr)
{
    if (!hdb_info) {
        tsk_error_reset();
        tsk_error_set_errno(TSK_ERR_HDB_ARG);
        tsk_error_set_errstr("tsk_hdb_lookup_str: NULL hdb_info");
        return -1;
    }

    return hdb_info->lookup_str(hdb_info, hash, flags, action, ptr);
}

/**
* \ingroup hashdblib
* Search the index for the given hash value given (in binary form).
*
* @param hdb_info Open hash database (with index)
* @param hash Array with binary hash value to search for
* @param len Number of bytes in binary hash value
* @param flags Flags to use in lookup
* @param action Callback function to call for each hash db entry 
* (not called if QUICK flag is given)
* @param ptr Pointer to data to pass to each callback
*
* @return -1 on error, 0 if hash value not found, and 1 if value was found.
*/
int8_t
    tsk_hdb_lookup_raw(TSK_HDB_INFO * hdb_info, uint8_t * hash, uint8_t len,
    TSK_HDB_FLAG_ENUM flags,
    TSK_HDB_LOOKUP_FN action, void *ptr)
{
    if (!hdb_info) {
        tsk_error_reset();
        tsk_error_set_errno(TSK_ERR_HDB_ARG);
        tsk_error_set_errstr("tsk_hdb_lookup_raw: NULL hdb_info");
        return -1;
    }

    return hdb_info->lookup_raw(hdb_info, hash, len, flags, action, ptr);
}

int8_t
    tsk_hdb_lookup_verbose_str(TSK_HDB_INFO *hdb_info, const char *hash, void *result)
{
    if (!hdb_info) {
        tsk_error_reset();
        tsk_error_set_errno(TSK_ERR_HDB_ARG);
        tsk_error_set_errstr("tsk_hdb_lookup_verbose_str: NULL hdb_info");
        return -1;
    }

    if (!hash) {
        tsk_error_reset();
        tsk_error_set_errno(TSK_ERR_HDB_ARG);
        tsk_error_set_errstr("tsk_hdb_lookup_verbose_str: NULL hash");
        return -1;
    }

    if (!result) {
        tsk_error_reset();
        tsk_error_set_errno(TSK_ERR_HDB_ARG);
        tsk_error_set_errstr("tsk_hdb_lookup_verbose_str: NULL result");
        return -1;
    }

    return hdb_info->lookup_verbose_str(hdb_info, hash, result);
}

/**
* \ingroup hashdblib
* Indicates whether a hash database accepts updates.
* @param hdb_info The hash database object
* @return 1 if hash database accepts updates, 0 if it does not
*/
uint8_t 
    tsk_hdb_accepts_updates(TSK_HDB_INFO *hdb_info)
{
    const char *func_name = "tsk_hdb_accepts_updates";

    if (!hdb_info) {
        tsk_error_reset();
        tsk_error_set_errno(TSK_ERR_HDB_ARG);
        tsk_error_set_errstr("%s: NULL hdb_info", func_name);
        return 0;
    }

    if (!hdb_info->accepts_updates) {
        tsk_error_reset();
        tsk_error_set_errno(TSK_ERR_HDB_ARG);
        tsk_error_set_errstr("%s: NULL add_entry function ptr", func_name);
        return 0;
    }

    return hdb_info->accepts_updates();
}

/**
* \ingroup hashdblib
* Adds a new entry to a hash database.
* @param hdb_info The hash database object
* @param filename Name of the file that was hashed (can be NULL)
* @param md5 Text representation of MD5 hash (can be NULL)
* @param sha1 Text representation of SHA1 hash (can be NULL)
* @param sha256 Text representation of SHA256 hash (can be NULL)
* @param comment A comment to asociate with the hash (can be NULL)
* @return 1 on error, 0 on success
*/
uint8_t
    tsk_hdb_add_entry(TSK_HDB_INFO *hdb_info, const char *filename, const char *md5, 
    const char *sha1, const char *sha256, const char *comment)
{
    const char *func_name = "tsk_hdb_add_entry";

    if (!hdb_info) {
        tsk_error_reset();
        tsk_error_set_errno(TSK_ERR_HDB_ARG);
        tsk_error_set_errstr("%s: NULL hdb_info", func_name);
        return 1;
    }

    if (!hdb_info->add_entry) {
        tsk_error_reset();
        tsk_error_set_errno(TSK_ERR_HDB_ARG);
        tsk_error_set_errstr("%s: NULL add_entry function ptr", func_name);
        return 1;
    }

    if (hdb_info->accepts_updates()) {
        return hdb_info->add_entry(hdb_info, filename, md5, sha1, sha256, comment);
    }
    else {
        tsk_error_reset();
        tsk_error_set_errno(TSK_ERR_HDB_PROC);
        tsk_error_set_errstr("%s: operation not supported for this database type (=%u)", func_name, hdb_info->db_type);
        return 1;
    }
}

/**
* \ingroup hashdblib
* Begins a transaction on a hash database.
* @param hdb_info A hash database info object
* @return 1 on error, 0 on success
*/
uint8_t 
    tsk_hdb_begin_transaction(TSK_HDB_INFO *hdb_info)
{
    const char *func_name = "tsk_hdb_begin_transaction";

    if (!hdb_info) {
        tsk_error_reset();
        tsk_error_set_errno(TSK_ERR_HDB_ARG);
        tsk_error_set_errstr("%s: NULL hdb_info", func_name);
        return 1;
    }

    if (!hdb_info->begin_transaction) {
        tsk_error_reset();
        tsk_error_set_errno(TSK_ERR_HDB_ARG);
        tsk_error_set_errstr("%s: NULL begin_transaction function ptr", func_name);
        return 1;
    }

    if (hdb_info->accepts_updates()) {
        if (!hdb_info->transaction_in_progress) {
            if (hdb_info->begin_transaction(hdb_info)) {
                return 1;
            }
            else {
                hdb_info->transaction_in_progress = 1;
                return 0;
            }
        }
        else {
            tsk_error_reset();
            tsk_error_set_errno(TSK_ERR_HDB_PROC);
            tsk_error_set_errstr("%s: transaction already begun", func_name);
            return 1;
        }
    }
    else {
        tsk_error_reset();
        tsk_error_set_errno(TSK_ERR_HDB_PROC);
        tsk_error_set_errstr("%s: operation not supported for this database type (=%u)", func_name, hdb_info->db_type);
        return 1;
    }
}

/**
* \ingroup hashdblib
* Commits a transaction on a hash database.
* @param hdb_info A hash database info object
* @return 1 on error, 0 on success 
*/
uint8_t 
    tsk_hdb_commit_transaction(TSK_HDB_INFO *hdb_info)
{
    const char *func_name = "tsk_hdb_commit_transaction";

    if (!hdb_info) {
        tsk_error_reset();
        tsk_error_set_errno(TSK_ERR_HDB_ARG);
        tsk_error_set_errstr("%s: NULL hdb_info", func_name);
        return 1;
    }

    if (!hdb_info->commit_transaction) {
        tsk_error_reset();
        tsk_error_set_errno(TSK_ERR_HDB_ARG);
        tsk_error_set_errstr("%s: NULL commit_transaction function ptr", func_name);
        return 1;
    }

    if (hdb_info->accepts_updates()) {
        if (hdb_info->transaction_in_progress) {
            if (hdb_info->commit_transaction(hdb_info)) {
                return 1;
            }
            else {
                hdb_info->transaction_in_progress = 0;
                return 0;
            }
        }
        else {
            tsk_error_reset();
            tsk_error_set_errno(TSK_ERR_HDB_PROC);
            tsk_error_set_errstr("%s: transaction not begun", func_name);
            return 1;
        }
    }
    else {
        tsk_error_reset();
        tsk_error_set_errno(TSK_ERR_HDB_PROC);
        tsk_error_set_errstr("%s: operation not supported for this database type (=%u)", func_name, hdb_info->db_type);
        return 1;
    }
}

/**
* \ingroup hashdblib
* Rolls back a transaction on a hash database.
* @param hdb_info A hash database info object
* @return 1 on error, 0 on success 
*/
uint8_t 
    tsk_hdb_rollback_transaction(TSK_HDB_INFO *hdb_info)
{
    const char *func_name = "tsk_hdb_rollback_transaction";

    if (!hdb_info) {
        tsk_error_reset();
        tsk_error_set_errno(TSK_ERR_HDB_ARG);
        tsk_error_set_errstr("%s: NULL hdb_info", func_name);
        return 1;
    }

    if (!hdb_info->rollback_transaction) {
        tsk_error_reset();
        tsk_error_set_errno(TSK_ERR_HDB_ARG);
        tsk_error_set_errstr("%s: NULL rollback_transaction function ptr", func_name);
        return 1;
    }

    if (hdb_info->accepts_updates()) {
        if (hdb_info->transaction_in_progress) {
            if (hdb_info->commit_transaction(hdb_info)) {
                return 1;
            }
            else {
                hdb_info->transaction_in_progress = 0;
                return 0;
            }
        }
        else {
            tsk_error_reset();
            tsk_error_set_errno(TSK_ERR_HDB_PROC);
            tsk_error_set_errstr("%s: transaction not begun", func_name);
            return 1;
        }
    }
    else {
        tsk_error_reset();
        tsk_error_set_errno(TSK_ERR_HDB_PROC);
        tsk_error_set_errstr("%s: operation not supported for this database type (=%u)", func_name, hdb_info->db_type);
        return 1;
    }
}

/**
* \ingroup hashdblib
* Closes an open hash database.
* @param hdb_info The hash database object
*/
void
    tsk_hdb_close(TSK_HDB_INFO *hdb_info)
{
    if (!hdb_info) {
        tsk_error_reset();
        tsk_error_set_errno(TSK_ERR_HDB_ARG);
        tsk_error_set_errstr("tsk_hdb_close: NULL hdb_info");
        return;
    }

    hdb_info->close_db(hdb_info);
}
