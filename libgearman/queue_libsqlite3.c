/* Gearman server and library
 * Copyright (C) 2008 Brian Aker, Eric Day
 * All rights reserved.
 *
 * Use and distribution licensed under the BSD license.  See
 * the COPYING file in the parent directory for full text.
 */

/**
 * @file
 * @brief Sqlite Queue Storage Definitions
 */

#include "common.h"

#include <libgearman/queue_libsqlite3.h>
#include <sqlite3.h>

/**
 * @addtogroup gearman_queue_sqlite sqlite Queue Storage Functions
 * @ingroup gearman_queue
 * @{
 */

/**
 * Default values.
 */
#define GEARMAN_QUEUE_SQLITE_DEFAULT_TABLE "gearman_queue"
#define GEARMAN_QUEUE_QUERY_BUFFER 256

/*
 * Private declarations
 */

#define SQLITE_MAX_TABLE_SIZE 256

/**
 * Structure for sqlite specific data.
 */
typedef struct
{
  sqlite3 * db;
  char table[SQLITE_MAX_TABLE_SIZE];
  char *query;
  size_t query_size;
  int in_trans;
} gearman_queue_sqlite_st;

/**
 * Query error handling function.
 */
static int _sqlite_query(gearman_st *gearman,
                         gearman_queue_sqlite_st *queue,
                         const char *query, size_t query_size,
                         sqlite3_stmt ** sth);
static int _sqlite_lock(gearman_st *gearman,
                        gearman_queue_sqlite_st *queue);
static int _sqlite_commit(gearman_st *gearman,
                          gearman_queue_sqlite_st *queue);
#if 0
static int _sqlite_rollback(gearman_st *gearman,
                            gearman_queue_sqlite_st *queue);
#endif

/* Queue callback functions. */
static gearman_return_t _sqlite_add(gearman_st *gearman, void *fn_arg,
                                        const void *unique, size_t unique_size,
                                        const void *function_name,
                                        size_t function_name_size,
                                        const void *data, size_t data_size,
                                        gearman_job_priority_t priority);
static gearman_return_t _sqlite_flush(gearman_st *gearman, void *fn_arg);
static gearman_return_t _sqlite_done(gearman_st *gearman, void *fn_arg,
                                         const void *unique,
                                         size_t unique_size,
                                         const void *function_name,
                                         size_t function_name_size);
static gearman_return_t _sqlite_replay(gearman_st *gearman, void *fn_arg,
                                           gearman_queue_add_fn *add_fn,
                                           void *add_fn_arg);

/*
 * Public definitions
 */

gearman_return_t gearman_queue_libsqlite3_conf(gearman_conf_st *conf)
{
  gearman_conf_module_st *module;

  module= gearman_conf_module_create(conf, NULL, "libsqlite3");
  if (module == NULL)
    return GEARMAN_MEMORY_ALLOCATION_FAILURE;

#define MCO(__name, __value, __help) \
  gearman_conf_module_add_option(module, __name, 0, __value, __help);

  MCO("db", "DB", "Database file to use.")
  MCO("table", "TABLE", "Table to use.")

  return gearman_conf_return(conf);
}

gearman_return_t gearman_queue_libsqlite3_init(gearman_st *gearman,
                                               gearman_conf_st *conf)
{
  gearman_queue_sqlite_st *queue;
  gearman_conf_module_st *module;
  const char *name;
  const char *value;
  char *table = NULL;
  sqlite3_stmt * sth;
  char create[1024];

  GEARMAN_INFO(gearman, "Initializing libsqlite3 module")

  queue= malloc(sizeof(gearman_queue_sqlite_st));
  if (queue == NULL)
  {
    GEARMAN_ERROR_SET(gearman, "gearman_queue_libsqlite3_init", "malloc")
    return GEARMAN_MEMORY_ALLOCATION_FAILURE;
  }

  memset(queue, 0, sizeof(gearman_queue_sqlite_st));
  snprintf(queue->table, SQLITE_MAX_TABLE_SIZE,
           GEARMAN_QUEUE_SQLITE_DEFAULT_TABLE);

  /* Get module and parse the option values that were given. */
  module= gearman_conf_module_find(conf, "libsqlite3");
  if (module == NULL)
  {
    GEARMAN_ERROR_SET(gearman, "gearman_queue_libsqlite3_init",
                      "gearman_conf_module_find:NULL");
    free(queue);
    return GEARMAN_QUEUE_ERROR;
  }

  gearman_set_queue_fn_arg(gearman, queue);

  while (gearman_conf_module_value(module, &name, &value))
  {
    if (!strcmp(name, "db"))
    {
      if (sqlite3_open(value, &(queue->db)) != SQLITE_OK)
      {
        gearman_queue_libsqlite3_deinit(gearman);        
        GEARMAN_ERROR_SET(gearman, "gearman_queue_libsqlite3_init",
                          "Can't open database: %s\n", sqlite3_errmsg(queue->db));
        free(queue);
        return GEARMAN_QUEUE_ERROR;
      }
    }
    else if (!strcmp(name, "table"))
      snprintf(queue->table, SQLITE_MAX_TABLE_SIZE, "%s", value);
    else
    {
      gearman_queue_libsqlite3_deinit(gearman);
      GEARMAN_ERROR_SET(gearman, "gearman_queue_libsqlite3_init",
                        "Unknown argument: %s", name);
      return GEARMAN_QUEUE_ERROR;
    }
  }

  if (!queue->db)
  {
    gearman_queue_libsqlite3_deinit(gearman);
    GEARMAN_ERROR_SET(gearman, "gearman_queue_libsqlite3_init",
                      "missing required --sqlite-db=<dbfile> argument");
    return GEARMAN_QUEUE_ERROR;
  }    

  if (_sqlite_query(gearman, queue, "SELECT name FROM sqlite_master WHERE type='table'", -1, &sth) != SQLITE_OK)
  {
    gearman_queue_libsqlite3_deinit(gearman);
    return GEARMAN_QUEUE_ERROR;
  }

  while (sqlite3_step(sth) == SQLITE_ROW)
  {
    table = (char*)sqlite3_column_text(sth, 0);
    if (!strcasecmp(queue->table, table))
    {
      GEARMAN_INFO(gearman, "sqlite module using table '%s'", table);
      break;
    }
  }

  if (sqlite3_finalize(sth) != SQLITE_OK)
  {
    GEARMAN_ERROR_SET(gearman, "gearman_queue_libsqlite3_init", "sqlite_finalize:%s", 
                      sqlite3_errmsg(queue->db));
    gearman_queue_libsqlite3_deinit(gearman);
    return GEARMAN_QUEUE_ERROR;
  }

  if (table == NULL)
  {
    snprintf(create, 1024,
             "CREATE TABLE %s"
             "("
               "unique_key TEXT PRIMARY KEY,"
               "function_name TEXT,"
               "priority INTEGER,"
               "data BLOB"
             ")",
             queue->table);

    GEARMAN_INFO(gearman, "sqlite module creating table '%s'", queue->table);

    if (_sqlite_query(gearman, queue, create, strlen(create), &sth)
        != SQLITE_OK)
    {
      gearman_queue_libsqlite3_deinit(gearman);
      return GEARMAN_QUEUE_ERROR;
    }

    if (sqlite3_step(sth) != SQLITE_DONE)
    {
      GEARMAN_ERROR_SET(gearman, "gearman_queue_libsqlite3_init", "create table error: %s", sqlite3_errmsg(queue->db));
      sqlite3_finalize(sth);
      return GEARMAN_QUEUE_ERROR;
    }
    
    if (sqlite3_finalize(sth) != SQLITE_OK)
    {
      GEARMAN_ERROR_SET(gearman, "gearman_queue_libsqlite3_init", "sqlite_finalize:%s", 
                        sqlite3_errmsg(queue->db));
      gearman_queue_libsqlite3_deinit(gearman);
      return GEARMAN_QUEUE_ERROR;
    }
  }

  gearman_set_queue_add(gearman, _sqlite_add);
  gearman_set_queue_flush(gearman, _sqlite_flush);
  gearman_set_queue_done(gearman, _sqlite_done);
  gearman_set_queue_replay(gearman, _sqlite_replay);

  return GEARMAN_SUCCESS;
}

gearman_return_t gearman_queue_libsqlite3_deinit(gearman_st *gearman)
{
  gearman_queue_sqlite_st *queue;

  GEARMAN_INFO(gearman, "Shutting down sqlite queue module")

  queue= (gearman_queue_sqlite_st *)gearman_queue_fn_arg(gearman);
  gearman_set_queue_fn_arg(gearman, NULL);
  sqlite3_close(queue->db);
  free(queue);

  return GEARMAN_SUCCESS;
}

gearman_return_t gearmand_queue_libsqlite3_init(gearmand_st *gearmand,
                                                gearman_conf_st *conf)
{
  return gearman_queue_libsqlite3_init(gearmand->server.gearman, conf);
}

gearman_return_t gearmand_queue_libsqlite3_deinit(gearmand_st *gearmand)
{
  return gearman_queue_libsqlite3_deinit(gearmand->server.gearman);
}

/*
 * Private definitions
 */

int _sqlite_query(gearman_st *gearman,
                  gearman_queue_sqlite_st *queue,
                  const char *query, size_t query_size,
                  sqlite3_stmt ** sth)
{
  int ret;
  GEARMAN_CRAZY(gearman, "sqlite query: %s", query);
  ret = sqlite3_prepare(queue->db, query, query_size, sth, NULL);
  if (ret  != SQLITE_OK)
  {
    if (*sth)
      sqlite3_finalize(*sth);
    *sth = NULL;
    GEARMAN_ERROR_SET(gearman, "_sqlite_query", "sqlite_prepare:%s", 
                      sqlite3_errmsg(queue->db));
  }

  return ret;
}

int _sqlite_lock(gearman_st *gearman,
                 gearman_queue_sqlite_st *queue) {
  sqlite3_stmt * sth;
  int ret;
  if (queue->in_trans)
  {
    /* already in transaction */
    return SQLITE_OK;
  }

  ret = _sqlite_query(gearman, queue, "BEGIN TRANSACTION", -1, &sth);
  if (ret != SQLITE_OK)
  {
    GEARMAN_ERROR_SET(gearman, "_sqlite_lock", "failed to begin transaction: %s", 
                      sqlite3_errmsg(queue->db));
    if(sth)
      sqlite3_finalize(sth);
      
    return ret;
  }

  ret = sqlite3_step(sth);
  if (ret != SQLITE_DONE)
  {
    GEARMAN_ERROR_SET(gearman, "_sqlite_lock", "lock error: %s", sqlite3_errmsg(queue->db));
    sqlite3_finalize(sth);
    return ret;
  }

  sqlite3_finalize(sth);
  queue->in_trans++;
  return SQLITE_OK;
}

int _sqlite_commit(gearman_st *gearman,
                 gearman_queue_sqlite_st *queue) {
  sqlite3_stmt * sth;
  int ret;
  if (! queue->in_trans)
  {
    /* not in transaction */
    return SQLITE_OK;
  }

  ret = _sqlite_query(gearman, queue, "COMMIT", -1, &sth);
  if (ret != SQLITE_OK)
  {
    GEARMAN_ERROR_SET(gearman, "_sqlite_commit", "failed to commit transaction: %s", 
                      sqlite3_errmsg(queue->db));
    if(sth)
      sqlite3_finalize(sth);
    return ret;
  }
  ret = sqlite3_step(sth);
  if (ret != SQLITE_DONE)
  {
    GEARMAN_ERROR_SET(gearman, "_sqlite_commit", "commit error: %s", sqlite3_errmsg(queue->db));
    sqlite3_finalize(sth);
    return ret;
  }
  sqlite3_finalize(sth);
  queue->in_trans = 0;
  return SQLITE_OK;
}

#if 0
int _sqlite_rollback(gearman_st *gearman,
                     gearman_queue_sqlite_st *queue) {
  sqlite3_stmt * sth;
  int ret;
  if (! queue->in_trans)
  {
    /* not in transaction */
    return SQLITE_OK;
  }

  ret = _sqlite_query(gearman, queue, "ROLLBACK", -1, &sth);
  if (ret != SQLITE_OK)
  {
    GEARMAN_ERROR_SET(gearman, "_sqlite_rollback", "failed to rollback transaction: %s", 
                      sqlite3_errmsg(queue->db));
    if(sth)
      sqlite3_finalize(sth);
    return ret;
  }
  ret = sqlite3_step(sth);
  if (ret != SQLITE_DONE)
  {
    GEARMAN_ERROR_SET(gearman, "_sqlite_rollback", "rollback error: %s", sqlite3_errmsg(queue->db));
    sqlite3_finalize(sth);
    return ret;
  }
  sqlite3_finalize(sth);
  queue->in_trans = 0;
  return SQLITE_OK;
}
#endif

static gearman_return_t _sqlite_add(gearman_st *gearman, void *fn_arg,
                                        const void *unique, size_t unique_size,
                                        const void *function_name,
                                        size_t function_name_size,
                                        const void *data, size_t data_size,
                                        gearman_job_priority_t priority)
{
  gearman_queue_sqlite_st *queue= (gearman_queue_sqlite_st *)fn_arg;
  char *query;
  size_t query_size;
  sqlite3_stmt * sth;

  GEARMAN_DEBUG(gearman, "sqlite add: %.*s", (uint32_t)unique_size,
                (char *)unique);

  if (_sqlite_lock(gearman, queue) !=  SQLITE_OK)
      return GEARMAN_QUEUE_ERROR;

  query_size= ((unique_size + function_name_size + data_size) * 2) +
              GEARMAN_QUEUE_QUERY_BUFFER;
  if (query_size > queue->query_size)
  {
    query= realloc(queue->query, query_size);
    if (query == NULL)
    {
      GEARMAN_ERROR_SET(gearman, "_sqlite_add", "realloc")
      return GEARMAN_MEMORY_ALLOCATION_FAILURE;
    }

    queue->query= query;
    queue->query_size= query_size;
  }
  else
    query= queue->query;

  query_size= (size_t)snprintf(query, query_size,
                               "INSERT INTO %s (priority,unique_key,function_name,data) VALUES (?,?,?,?)",
                               queue->table);

  if (_sqlite_query(gearman, queue, query, query_size, &sth) != SQLITE_OK)
    return GEARMAN_QUEUE_ERROR;

  sqlite3_bind_int(sth,  1, priority);
  sqlite3_bind_text(sth, 2, unique, unique_size, SQLITE_TRANSIENT);
  sqlite3_bind_text(sth, 3, function_name, function_name_size, SQLITE_TRANSIENT);
  sqlite3_bind_blob(sth, 4, data, data_size, SQLITE_TRANSIENT);

  if (sqlite3_step(sth) != SQLITE_DONE)
  {
    GEARMAN_ERROR_SET(gearman, "_sqlite_add", "insert error: %s", sqlite3_errmsg(queue->db));
    if (sqlite3_finalize(sth) != SQLITE_OK )
      GEARMAN_ERROR_SET(gearman, "_sqlite_add", "finalize error: %s", sqlite3_errmsg(queue->db));

    return GEARMAN_QUEUE_ERROR;
  }

  sqlite3_finalize(sth);

  if (_sqlite_commit(gearman, queue) !=  SQLITE_OK)
      return GEARMAN_QUEUE_ERROR;

  return GEARMAN_SUCCESS;
}

static gearman_return_t _sqlite_flush(gearman_st *gearman,
                                          void *fn_arg __attribute__((unused)))
{
  GEARMAN_DEBUG(gearman, "sqlite flush")

  return GEARMAN_SUCCESS;
}

static gearman_return_t _sqlite_done(gearman_st *gearman, void *fn_arg,
                                         const void *unique,
                                         size_t unique_size,
                                         const void *function_name __attribute__((unused)),
                                         size_t function_name_size __attribute__((unused)))
{
  gearman_queue_sqlite_st *queue= (gearman_queue_sqlite_st *)fn_arg;
  char *query;
  size_t query_size;
  sqlite3_stmt * sth;

  GEARMAN_DEBUG(gearman, "sqlite done: %.*s", (uint32_t)unique_size,
                (char *)unique)

  if (_sqlite_lock(gearman, queue) !=  SQLITE_OK)
      return GEARMAN_QUEUE_ERROR;

  query_size= (unique_size * 2) + GEARMAN_QUEUE_QUERY_BUFFER;
  if (query_size > queue->query_size)
  {
    query= realloc(queue->query, query_size);
    if (query == NULL)
    {
      GEARMAN_ERROR_SET(gearman, "_sqlite_add", "realloc")
      return GEARMAN_MEMORY_ALLOCATION_FAILURE;
    }

    queue->query= query;
    queue->query_size= query_size;
  }
  else
    query= queue->query;

  query_size= (size_t)snprintf(query, query_size,
                               "DELETE FROM %s WHERE unique_key=?",
                               queue->table);

  if (_sqlite_query(gearman, queue, query, query_size, &sth) != SQLITE_OK)
    return GEARMAN_QUEUE_ERROR;

  sqlite3_bind_text(sth, 1, unique, unique_size, SQLITE_TRANSIENT);
  
  if (sqlite3_step(sth) != SQLITE_DONE)
  {
    GEARMAN_ERROR_SET(gearman, "_sqlite_done", "delete error: %s", sqlite3_errmsg(queue->db));
    sqlite3_finalize(sth);
    return GEARMAN_QUEUE_ERROR;
  }

  sqlite3_finalize(sth);

  if (_sqlite_commit(gearman, queue) !=  SQLITE_OK)
      return GEARMAN_QUEUE_ERROR;

  return GEARMAN_SUCCESS;
}

static gearman_return_t _sqlite_replay(gearman_st *gearman, void *fn_arg,
                                           gearman_queue_add_fn *add_fn,
                                           void *add_fn_arg)
{
  gearman_queue_sqlite_st *queue= (gearman_queue_sqlite_st *)fn_arg;
  char *query;
  size_t query_size;
  sqlite3_stmt * sth;
  gearman_return_t gret;

  GEARMAN_INFO(gearman, "sqlite replay start")

  if (GEARMAN_QUEUE_QUERY_BUFFER > queue->query_size)
  {
    query= realloc(queue->query, GEARMAN_QUEUE_QUERY_BUFFER);
    if (query == NULL)
    {
      GEARMAN_ERROR_SET(gearman, "_sqlite_replay", "realloc")
      return GEARMAN_MEMORY_ALLOCATION_FAILURE;
    }

    queue->query= query;
    queue->query_size= GEARMAN_QUEUE_QUERY_BUFFER;
  }
  else
    query= queue->query;

  query_size= (size_t)snprintf(query, GEARMAN_QUEUE_QUERY_BUFFER,
                               "SELECT unique_key,function_name,priority,data "
                               "FROM %s",
                               queue->table);

  if (_sqlite_query(gearman, queue, query, query_size, &sth) != SQLITE_OK)
    return GEARMAN_QUEUE_ERROR;

  while (sqlite3_step(sth) == SQLITE_ROW)
  {
    const void *unique = sqlite3_column_text(sth,0);
    size_t unique_size = sqlite3_column_bytes(sth,0);

    const void *function_name = sqlite3_column_text(sth,1);
    size_t function_name_size = sqlite3_column_bytes(sth,1);

    gearman_job_priority_t priority = (double)sqlite3_column_int64(sth,2);
      
    size_t data_size = sqlite3_column_bytes(sth,3);
    /* need to make a copy here ... gearman_server_job_free will free it later */
    void *data = malloc(data_size);
    memcpy(data, sqlite3_column_blob(sth,3), data_size);

    GEARMAN_DEBUG(gearman, "sqlite replay: %s", (char*)function_name);


    gret= (*add_fn)(gearman, add_fn_arg,
                    unique, unique_size,
                    function_name, function_name_size,
                    data, data_size,
                    priority);
    if (gret != GEARMAN_SUCCESS)
    {
      sqlite3_finalize(sth);
      return gret;
    }
  }

  sqlite3_finalize(sth);

  return GEARMAN_SUCCESS;
}
