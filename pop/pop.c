/**
 * @file
 * POP network mailbox
 *
 * @authors
 * Copyright (C) 2000-2002 Vsevolod Volkov <vvv@mutt.org.ua>
 * Copyright (C) 2006-2007,2009 Rocco Rutte <pdmef@gmx.net>
 * Copyright (C) 2018 Richard Russon <rich@flatcap.org>
 *
 * @copyright
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 2 of the License, or (at your option) any later
 * version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/**
 * @page pop_pop POP network mailbox
 *
 * POP network mailbox
 */

#include "config.h"
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "pop_private.h"
#include "mutt/mutt.h"
#include "config/lib.h"
#include "email/lib.h"
#include "conn/conn.h"
#include "mutt.h"
#include "pop.h"
#include "account.h"
#include "bcache.h"
#include "context.h"
#include "globals.h"
#include "hook.h"
#include "mailbox.h"
#include "mutt_account.h"
#include "mutt_header.h"
#include "mutt_logging.h"
#include "mutt_socket.h"
#include "muttlib.h"
#include "mx.h"
#include "ncrypt/ncrypt.h"
#include "progress.h"
#include "hcache/hcache.h"
#ifdef ENABLE_NLS
#include <libintl.h>
#endif

struct BodyCache;

/* These Config Variables are only used in pop/pop.c */
short C_PopCheckinterval; ///< Config: (pop) Interval between checks for new mail
unsigned char C_PopDelete; ///< Config: (pop) After downloading POP messages, delete them on the server
char *C_PopHost; ///< Config: (pop) Url of the POP server
bool C_PopLast;  ///< Config: (pop) Use the 'LAST' command to fetch new mail

#define HC_FNAME "neomutt" /* filename for hcache as POP lacks paths */
#define HC_FEXT "hcache"   /* extension for hcache as POP lacks paths */

/**
 * cache_id - Make a message-cache-compatible id
 * @param id POP message id
 * @retval ptr Sanitised string
 *
 * The POP message id may contain '/' and other awkward characters.
 *
 * @note This function returns a pointer to a static buffer.
 */
static const char *cache_id(const char *id)
{
  static char clean[128];
  mutt_str_strfcpy(clean, id, sizeof(clean));
  mutt_file_sanitize_filename(clean, true);
  return clean;
}

/**
 * pop_adata_free - Free data attached to the Mailbox
 * @param[out] ptr POP data
 *
 * The PopAccountData struct stores global POP data, such as the connection to
 * the database.  This function will close the database, free the resources and
 * the struct itself.
 */
static void pop_adata_free(void **ptr)
{
  if (!ptr || !*ptr)
    return;

  // struct PopAccountData *adata = *ptr;
  // FREE(&adata->conn);
  FREE(ptr);
}

/**
 * pop_adata_new - Create a new PopAccountData object
 * @retval ptr New PopAccountData struct
 */
static struct PopAccountData *pop_adata_new(void)
{
  return mutt_mem_calloc(1, sizeof(struct PopAccountData));
}

/**
 * pop_edata_free - Free data attached to an Email
 * @param[out] ptr Email data
 *
 * Each email has an attached PopEmailData, which contains things like the tags
 * (labels).
 */
static void pop_edata_free(void **ptr)
{
  if (!ptr || !*ptr)
    return;

  struct PopEmailData *edata = *ptr;
  FREE(&edata->uid);
  FREE(ptr);
}

/**
 * pop_edata_new - Create a new PopEmailData for an email
 * @param uid Email UID
 * @retval ptr New PopEmailData struct
 */
static struct PopEmailData *pop_edata_new(const char *uid)
{
  struct PopEmailData *edata = mutt_mem_calloc(1, sizeof(struct PopEmailData));
  edata->uid = mutt_str_strdup(uid);
  return edata;
}

/**
 * fetch_message - write line to file - Implements ::pop_fetch_t
 * @param line String to write
 * @param data FILE pointer to write to
 * @retval  0 Success
 * @retval -1 Failure
 */
static int fetch_message(char *line, void *data)
{
  FILE *fp = data;

  fputs(line, fp);
  if (fputc('\n', fp) == EOF)
    return -1;

  return 0;
}

/**
 * pop_read_header - Read header
 * @param adata POP Account data
 * @param e     Email
 * @retval  0 Success
 * @retval -1 Connection lost
 * @retval -2 Invalid command or execution error
 * @retval -3 Error writing to tempfile
 */
static int pop_read_header(struct PopAccountData *adata, struct Email *e)
{
  FILE *fp = mutt_file_mkstemp();
  if (!fp)
  {
    mutt_perror(_("Can't create temporary file"));
    return -3;
  }

  int index = 0;
  size_t length = 0;
  char buf[1024];

  snprintf(buf, sizeof(buf), "LIST %d\r\n", e->refno);
  int rc = pop_query(adata, buf, sizeof(buf));
  if (rc == 0)
  {
    sscanf(buf, "+OK %d %zu", &index, &length);

    snprintf(buf, sizeof(buf), "TOP %d 0\r\n", e->refno);
    rc = pop_fetch_data(adata, buf, NULL, fetch_message, fp);

    if (adata->cmd_top == 2)
    {
      if (rc == 0)
      {
        adata->cmd_top = 1;

        mutt_debug(LL_DEBUG1, "set TOP capability\n");
      }

      if (rc == -2)
      {
        adata->cmd_top = 0;

        mutt_debug(LL_DEBUG1, "unset TOP capability\n");
        snprintf(adata->err_msg, sizeof(adata->err_msg), "%s",
                 _("Command TOP is not supported by server"));
      }
    }
  }

  switch (rc)
  {
    case 0:
    {
      rewind(fp);
      e->env = mutt_rfc822_read_header(fp, e, false, false);
      e->content->length = length - e->content->offset + 1;
      rewind(fp);
      while (!feof(fp))
      {
        e->content->length--;
        fgets(buf, sizeof(buf), fp);
      }
      break;
    }
    case -2:
    {
      mutt_error("%s", adata->err_msg);
      break;
    }
    case -3:
    {
      mutt_error(_("Can't write header to temporary file"));
      break;
    }
  }

  mutt_file_fclose(&fp);
  return rc;
}

/**
 * fetch_uidl - parse UIDL - Implements ::pop_fetch_t
 * @param line String to parse
 * @param data Mailbox
 * @retval  0 Success
 * @retval -1 Failure
 */
static int fetch_uidl(char *line, void *data)
{
  struct Mailbox *m = data;
  struct PopAccountData *adata = pop_adata_get(m);
  char *endp = NULL;

  errno = 0;
  int index = strtol(line, &endp, 10);
  if (errno)
    return -1;
  while (*endp == ' ')
    endp++;
  memmove(line, endp, strlen(endp) + 1);

  /* uid must be at least be 1 byte */
  if (strlen(line) == 0)
    return -1;

  int i;
  for (i = 0; i < m->msg_count; i++)
  {
    struct PopEmailData *edata = m->emails[i]->edata;
    if (mutt_str_strcmp(line, edata->uid) == 0)
      break;
  }

  if (i == m->msg_count)
  {
    mutt_debug(LL_DEBUG1, "new header %d %s\n", index, line);

    if (i >= m->email_max)
      mx_alloc_memory(m);

    m->msg_count++;
    m->emails[i] = mutt_email_new();

    m->emails[i]->edata = pop_edata_new(line);
    m->emails[i]->free_edata = pop_edata_free;
  }
  else if (m->emails[i]->index != index - 1)
    adata->clear_cache = true;

  m->emails[i]->refno = index;
  m->emails[i]->index = index - 1;

  return 0;
}

/**
 * msg_cache_check - Check the Body Cache for an ID - Implements ::bcache_list_t
 */
static int msg_cache_check(const char *id, struct BodyCache *bcache, void *data)
{
  struct Mailbox *m = data;
  if (!m)
    return -1;

  struct PopAccountData *adata = pop_adata_get(m);
  if (!adata)
    return -1;

#ifdef USE_HCACHE
  /* keep hcache file if hcache == bcache */
  if (strcmp(HC_FNAME "." HC_FEXT, id) == 0)
    return 0;
#endif

  for (int i = 0; i < m->msg_count; i++)
  {
    struct PopEmailData *edata = m->emails[i]->edata;
    /* if the id we get is known for a header: done (i.e. keep in cache) */
    if (edata->uid && (mutt_str_strcmp(edata->uid, id) == 0))
      return 0;
  }

  /* message not found in context -> remove it from cache
   * return the result of bcache, so we stop upon its first error */
  return mutt_bcache_del(bcache, cache_id(id));
}

#ifdef USE_HCACHE
/**
 * pop_hcache_namer - Create a header cache filename for a POP mailbox - Implements ::hcache_namer_t
 */
static int pop_hcache_namer(const char *path, char *dest, size_t destlen)
{
  return snprintf(dest, destlen, "%s." HC_FEXT, path);
}

/**
 * pop_hcache_open - Open the header cache
 * @param adata POP Account data
 * @param path  Path to the mailbox
 * @retval ptr Header cache
 */
static header_cache_t *pop_hcache_open(struct PopAccountData *adata, const char *path)
{
  if (!adata || !adata->conn)
    return mutt_hcache_open(C_HeaderCache, path, NULL);

  struct Url url;
  char p[1024];

  mutt_account_tourl(&adata->conn->account, &url);
  url.path = HC_FNAME;
  url_tostring(&url, p, sizeof(p), U_PATH);
  return mutt_hcache_open(C_HeaderCache, p, pop_hcache_namer);
}
#endif

/**
 * pop_fetch_headers - Read headers
 * @param m Mailbox
 * @retval  0 Success
 * @retval -1 Connection lost
 * @retval -2 Invalid command or execution error
 * @retval -3 Error writing to tempfile
 */
static int pop_fetch_headers(struct Mailbox *m)
{
  if (!m)
    return -1;

  struct PopAccountData *adata = pop_adata_get(m);
  struct Progress progress;

#ifdef USE_HCACHE
  header_cache_t *hc = pop_hcache_open(adata, m->path);
#endif

  time(&adata->check_time);
  adata->clear_cache = false;

  if (!m->emails)
  {
    /* Allocate some memory to get started */
    m->email_max = m->msg_count;
    m->msg_count = 0;
    m->msg_unread = 0;
    m->vcount = 0;
    mx_alloc_memory(m);
  }

  for (int i = 0; i < m->msg_count; i++)
    m->emails[i]->refno = -1;

  const int old_count = m->msg_count;
  int rc = pop_fetch_data(adata, "UIDL\r\n", NULL, fetch_uidl, m);
  const int new_count = m->msg_count;
  m->msg_count = old_count;

  if (adata->cmd_uidl == 2)
  {
    if (rc == 0)
    {
      adata->cmd_uidl = 1;

      mutt_debug(LL_DEBUG1, "set UIDL capability\n");
    }

    if ((rc == -2) && (adata->cmd_uidl == 2))
    {
      adata->cmd_uidl = 0;

      mutt_debug(LL_DEBUG1, "unset UIDL capability\n");
      snprintf(adata->err_msg, sizeof(adata->err_msg), "%s",
               _("Command UIDL is not supported by server"));
    }
  }

  if (!m->quiet)
  {
    mutt_progress_init(&progress, _("Fetching message headers..."),
                       MUTT_PROGRESS_MSG, C_ReadInc, new_count - old_count);
  }

  if (rc == 0)
  {
    int i, deleted;
    for (i = 0, deleted = 0; i < old_count; i++)
    {
      if (m->emails[i]->refno == -1)
      {
        m->emails[i]->deleted = true;
        deleted++;
      }
    }
    if (deleted > 0)
    {
      mutt_error(
          ngettext("%d message has been lost. Try reopening the mailbox.",
                   "%d messages have been lost. Try reopening the mailbox.", deleted),
          deleted);
    }

    bool hcached = false;
    for (i = old_count; i < new_count; i++)
    {
      if (!m->quiet)
        mutt_progress_update(&progress, i + 1 - old_count, -1);
      struct PopEmailData *edata = m->emails[i]->edata;
#ifdef USE_HCACHE
      void *data = mutt_hcache_fetch(hc, edata->uid, strlen(edata->uid));
      if (data)
      {
        /* Detach the private data */
        m->emails[i]->edata = NULL;

        int refno = m->emails[i]->refno;
        int index = m->emails[i]->index;
        /* - POP dynamically numbers headers and relies on e->refno
         *   to map messages; so restore header and overwrite restored
         *   refno with current refno, same for index
         * - e->data needs to a separate pointer as it's driver-specific
         *   data freed separately elsewhere
         *   (the old e->data should point inside a malloc'd block from
         *   hcache so there shouldn't be a memleak here) */
        struct Email *e = mutt_hcache_restore((unsigned char *) data);
        mutt_hcache_free(hc, &data);
        mutt_email_free(&m->emails[i]);
        m->emails[i] = e;
        m->emails[i]->refno = refno;
        m->emails[i]->index = index;

        /* Reattach the private data */
        m->emails[i]->edata = edata;
        m->emails[i]->free_edata = pop_edata_free;
        rc = 0;
        hcached = true;
      }
      else
#endif
          if ((rc = pop_read_header(adata, m->emails[i])) < 0)
        break;
#ifdef USE_HCACHE
      else
      {
        mutt_hcache_store(hc, edata->uid, strlen(edata->uid), m->emails[i], 0);
      }
#endif

      /* faked support for flags works like this:
       * - if 'hcached' is true, we have the message in our hcache:
       *        - if we also have a body: read
       *        - if we don't have a body: old
       *          (if $mark_old is set which is maybe wrong as
       *          $mark_old should be considered for syncing the
       *          folder and not when opening it XXX)
       * - if 'hcached' is false, we don't have the message in our hcache:
       *        - if we also have a body: read
       *        - if we don't have a body: new */
      const bool bcached =
          (mutt_bcache_exists(adata->bcache, cache_id(edata->uid)) == 0);
      m->emails[i]->old = false;
      m->emails[i]->read = false;
      if (hcached)
      {
        if (bcached)
          m->emails[i]->read = true;
        else if (C_MarkOld)
          m->emails[i]->old = true;
      }
      else
      {
        if (bcached)
          m->emails[i]->read = true;
      }

      m->msg_count++;
    }
  }

#ifdef USE_HCACHE
  mutt_hcache_close(hc);
#endif

  if (rc < 0)
  {
    for (int i = m->msg_count; i < new_count; i++)
      mutt_email_free(&m->emails[i]);
    return rc;
  }

  /* after putting the result into our structures,
   * clean up cache, i.e. wipe messages deleted outside
   * the availability of our cache */
  if (C_MessageCacheClean)
    mutt_bcache_list(adata->bcache, msg_cache_check, m);

  mutt_clear_error();
  return new_count - old_count;
}

/**
 * pop_clear_cache - delete all cached messages
 * @param adata POP Account data
 */
static void pop_clear_cache(struct PopAccountData *adata)
{
  if (!adata->clear_cache)
    return;

  mutt_debug(LL_DEBUG1, "delete cached messages\n");

  for (int i = 0; i < POP_CACHE_LEN; i++)
  {
    if (adata->cache[i].path)
    {
      unlink(adata->cache[i].path);
      FREE(&adata->cache[i].path);
    }
  }
}

/**
 * pop_fetch_mail - Fetch messages and save them in $spoolfile
 */
void pop_fetch_mail(void)
{
  if (!C_PopHost)
  {
    mutt_error(_("POP host is not defined"));
    return;
  }

  char buf[1024];
  char msgbuf[128];
  int last = 0, msgs, bytes, rset = 0, ret;
  struct ConnAccount acct;

  char *p = mutt_mem_calloc(strlen(C_PopHost) + 7, sizeof(char));
  char *url = p;
  if (url_check_scheme(C_PopHost) == U_UNKNOWN)
  {
    strcpy(url, "pop://");
    p = strchr(url, '\0');
  }
  strcpy(p, C_PopHost);

  ret = pop_parse_path(url, &acct);
  FREE(&url);
  if (ret)
  {
    mutt_error(_("%s is an invalid POP path"), C_PopHost);
    return;
  }

  struct Connection *conn = mutt_conn_find(NULL, &acct);
  if (!conn)
    return;

  struct PopAccountData *adata = pop_adata_new();
  adata->conn = conn;

  if (pop_open_connection(adata) < 0)
  {
    //XXX mutt_socket_free(adata->conn);
    pop_adata_free((void **) &adata);
    return;
  }

  mutt_message(_("Checking for new messages..."));

  /* find out how many messages are in the mailbox. */
  mutt_str_strfcpy(buf, "STAT\r\n", sizeof(buf));
  ret = pop_query(adata, buf, sizeof(buf));
  if (ret == -1)
    goto fail;
  if (ret == -2)
  {
    mutt_error("%s", adata->err_msg);
    goto finish;
  }

  sscanf(buf, "+OK %d %d", &msgs, &bytes);

  /* only get unread messages */
  if ((msgs > 0) && C_PopLast)
  {
    mutt_str_strfcpy(buf, "LAST\r\n", sizeof(buf));
    ret = pop_query(adata, buf, sizeof(buf));
    if (ret == -1)
      goto fail;
    if (ret == 0)
      sscanf(buf, "+OK %d", &last);
  }

  if (msgs <= last)
  {
    mutt_message(_("No new mail in POP mailbox"));
    goto finish;
  }

  struct Mailbox *m_spool = mx_path_resolve(C_Spoolfile);
  struct Context *ctx = mx_mbox_open(m_spool, MUTT_OPEN_NO_FLAGS);
  if (!ctx)
  {
    mailbox_free(&m_spool);
    goto finish;
  }

  bool old_append = m_spool->append;
  m_spool->append = true;

  enum QuadOption delanswer =
      query_quadoption(C_PopDelete, _("Delete messages from server?"));

  snprintf(msgbuf, sizeof(msgbuf),
           ngettext("Reading new messages (%d byte)...",
                    "Reading new messages (%d bytes)...", bytes),
           bytes);
  mutt_message("%s", msgbuf);

  for (int i = last + 1; i <= msgs; i++)
  {
    struct Message *msg = mx_msg_open_new(ctx->mailbox, NULL, MUTT_ADD_FROM);
    if (!msg)
      ret = -3;
    else
    {
      snprintf(buf, sizeof(buf), "RETR %d\r\n", i);
      ret = pop_fetch_data(adata, buf, NULL, fetch_message, msg->fp);
      if (ret == -3)
        rset = 1;

      if ((ret == 0) && (mx_msg_commit(ctx->mailbox, msg) != 0))
      {
        rset = 1;
        ret = -3;
      }

      mx_msg_close(ctx->mailbox, &msg);
    }

    if ((ret == 0) && (delanswer == MUTT_YES))
    {
      /* delete the message on the server */
      snprintf(buf, sizeof(buf), "DELE %d\r\n", i);
      ret = pop_query(adata, buf, sizeof(buf));
    }

    if (ret == -1)
    {
      m_spool->append = old_append;
      mx_mbox_close(&ctx);
      goto fail;
    }
    if (ret == -2)
    {
      mutt_error("%s", adata->err_msg);
      break;
    }
    if (ret == -3)
    {
      mutt_error(_("Error while writing mailbox"));
      break;
    }

    /* L10N: The plural is picked by the second numerical argument, i.e.
       the %d right before 'messages', i.e. the total number of messages. */
    mutt_message(ngettext("%s [%d of %d message read]",
                          "%s [%d of %d messages read]", msgs - last),
                 msgbuf, i - last, msgs - last);
  }

  m_spool->append = old_append;
  mx_mbox_close(&ctx);

  if (rset)
  {
    /* make sure no messages get deleted */
    mutt_str_strfcpy(buf, "RSET\r\n", sizeof(buf));
    if (pop_query(adata, buf, sizeof(buf)) == -1)
      goto fail;
  }

finish:
  /* exit gracefully */
  mutt_str_strfcpy(buf, "QUIT\r\n", sizeof(buf));
  if (pop_query(adata, buf, sizeof(buf)) == -1)
    goto fail;
  mutt_socket_close(conn);
  FREE(&conn);
  pop_adata_free((void **) &adata);
  return;

fail:
  mutt_error(_("Server closed connection"));
  mutt_socket_close(conn);
  pop_adata_free((void **) &adata);
}

/**
 * pop_ac_find - Find a Account that matches a Mailbox path
 */
struct Account *pop_ac_find(struct Account *a, const char *path)
{
  if (!a || (a->magic != MUTT_POP) || !path)
    return NULL;

  struct Url *url = url_parse(path);
  if (!url)
    return NULL;

  struct PopAccountData *adata = a->adata;
  struct ConnAccount *ac = &adata->conn_account;

  if ((mutt_str_strcasecmp(url->host, ac->host) != 0) ||
      (mutt_str_strcasecmp(url->user, ac->user) != 0))
  {
    a = NULL;
  }

  url_free(&url);
  return a;
}

/**
 * pop_ac_add - Add a Mailbox to a Account
 */
int pop_ac_add(struct Account *a, struct Mailbox *m)
{
  if (!a || !m || (m->magic != MUTT_POP))
    return -1;

  if (!a->adata)
  {
    struct PopAccountData *adata = pop_adata_new();
    a->adata = adata;
    a->free_adata = pop_adata_free;

    struct Url *url = url_parse(m->path);
    if (url)
    {
      mutt_str_strfcpy(adata->conn_account.user, url->user,
                       sizeof(adata->conn_account.user));
      mutt_str_strfcpy(adata->conn_account.pass, url->pass,
                       sizeof(adata->conn_account.pass));
      mutt_str_strfcpy(adata->conn_account.host, url->host,
                       sizeof(adata->conn_account.host));
      adata->conn_account.port = url->port;
      adata->conn_account.type = MUTT_ACCT_TYPE_POP;

      if (adata->conn_account.user[0] != '\0')
        adata->conn_account.flags |= MUTT_ACCT_USER;
      if (adata->conn_account.pass[0] != '\0')
        adata->conn_account.flags |= MUTT_ACCT_PASS;
      url_free(&url);
    }
  }

  return 0;
}

/**
 * pop_mbox_open - Implements MxOps::mbox_open()
 *
 * Fetch only headers
 */
static int pop_mbox_open(struct Mailbox *m)
{
  if (!m || !m->account)
    return -1;

  char buf[PATH_MAX];
  struct ConnAccount acct = { { 0 } };
  struct Url url;

  if (pop_parse_path(m->path, &acct))
  {
    mutt_error(_("%s is an invalid POP path"), m->path);
    return -1;
  }

  mutt_account_tourl(&acct, &url);
  url.path = NULL;
  url_tostring(&url, buf, sizeof(buf), 0);

  mutt_str_strfcpy(m->path, buf, sizeof(m->path));
  mutt_str_strfcpy(m->realpath, m->path, sizeof(m->realpath));

  struct PopAccountData *adata = m->account->adata;
  if (!adata)
  {
    adata = pop_adata_new();
    m->account->adata = adata;
    m->account->free_adata = pop_adata_free;
  }

  struct Connection *conn = adata->conn;
  if (!conn)
  {
    adata->conn = mutt_conn_new(&acct);
    conn = adata->conn;
    if (!conn)
      return -1;
  }

  if (conn->fd < 0)
    mutt_account_hook(m->realpath);

  if (pop_open_connection(adata) < 0)
    return -1;

  adata->bcache = mutt_bcache_open(&acct, NULL);

  /* init (hard-coded) ACL rights */
  m->rights = MUTT_ACL_SEEN | MUTT_ACL_DELETE;
#ifdef USE_HCACHE
  /* flags are managed using header cache, so it only makes sense to
   * enable them in that case */
  m->rights |= MUTT_ACL_WRITE;
#endif

  while (true)
  {
    if (pop_reconnect(m) < 0)
      return -1;

    m->size = adata->size;

    mutt_message(_("Fetching list of messages..."));

    const int rc = pop_fetch_headers(m);

    if (rc >= 0)
      return 0;

    if (rc < -1)
      return -1;
  }
}

/**
 * pop_mbox_check - Implements MxOps::mbox_check()
 */
static int pop_mbox_check(struct Mailbox *m, int *index_hint)
{
  if (!m)
    return -1;

  struct PopAccountData *adata = pop_adata_get(m);

  if ((adata->check_time + C_PopCheckinterval) > time(NULL))
    return 0;

  pop_logout(m);

  mutt_socket_close(adata->conn);

  if (pop_open_connection(adata) < 0)
    return -1;

  m->size = adata->size;

  mutt_message(_("Checking for new messages..."));

  int old_msg_count = m->msg_count;
  int rc = pop_fetch_headers(m);
  pop_clear_cache(adata);
  if (m->msg_count > old_msg_count)
    mutt_mailbox_changed(m, MBN_INVALID);

  if (rc < 0)
    return -1;

  if (rc > 0)
    return MUTT_NEW_MAIL;

  return 0;
}

/**
 * pop_mbox_sync - Implements MxOps::mbox_sync()
 *
 * Update POP mailbox, delete messages from server
 */
static int pop_mbox_sync(struct Mailbox *m, int *index_hint)
{
  if (!m)
    return -1;

  int i, j, rc = 0;
  char buf[1024];
  struct PopAccountData *adata = pop_adata_get(m);
  struct Progress progress;
#ifdef USE_HCACHE
  header_cache_t *hc = NULL;
#endif

  adata->check_time = 0;

  int num_deleted = 0;
  for (i = 0; i < m->msg_count; i++)
  {
    if (m->emails[i]->deleted)
      num_deleted++;
  }

  while (true)
  {
    if (pop_reconnect(m) < 0)
      return -1;

    mutt_progress_init(&progress, _("Marking messages deleted..."),
                       MUTT_PROGRESS_MSG, C_WriteInc, num_deleted);

#ifdef USE_HCACHE
    hc = pop_hcache_open(adata, m->path);
#endif

    for (i = 0, j = 0, rc = 0; (rc == 0) && (i < m->msg_count); i++)
    {
      struct PopEmailData *edata = m->emails[i]->edata;
      if (m->emails[i]->deleted && (m->emails[i]->refno != -1))
      {
        j++;
        if (!m->quiet)
          mutt_progress_update(&progress, j, -1);
        snprintf(buf, sizeof(buf), "DELE %d\r\n", m->emails[i]->refno);
        rc = pop_query(adata, buf, sizeof(buf));
        if (rc == 0)
        {
          mutt_bcache_del(adata->bcache, cache_id(edata->uid));
#ifdef USE_HCACHE
          mutt_hcache_delete(hc, edata->uid, strlen(edata->uid));
#endif
        }
      }

#ifdef USE_HCACHE
      if (m->emails[i]->changed)
      {
        mutt_hcache_store(hc, edata->uid, strlen(edata->uid), m->emails[i], 0);
      }
#endif
    }

#ifdef USE_HCACHE
    mutt_hcache_close(hc);
#endif

    if (rc == 0)
    {
      mutt_str_strfcpy(buf, "QUIT\r\n", sizeof(buf));
      rc = pop_query(adata, buf, sizeof(buf));
    }

    if (rc == 0)
    {
      adata->clear_cache = true;
      pop_clear_cache(adata);
      adata->status = POP_DISCONNECTED;
      return 0;
    }

    if (rc == -2)
    {
      mutt_error("%s", adata->err_msg);
      return -1;
    }
  }
}

/**
 * pop_mbox_close - Implements MxOps::mbox_close()
 */
static int pop_mbox_close(struct Mailbox *m)
{
  if (!m)
    return -1;

  struct PopAccountData *adata = pop_adata_get(m);
  if (!adata)
    return 0;

  pop_logout(m);

  if (adata->status != POP_NONE)
  {
    mutt_socket_close(adata->conn);
    // FREE(&adata->conn);
  }

  adata->status = POP_NONE;

  adata->clear_cache = true;
  pop_clear_cache(adata);

  mutt_bcache_close(&adata->bcache);

  return 0;
}

/**
 * pop_msg_open - Implements MxOps::msg_open()
 */
static int pop_msg_open(struct Mailbox *m, struct Message *msg, int msgno)
{
  if (!m)
    return -1;

  char buf[1024];
  char path[PATH_MAX];
  struct Progress progress;
  struct PopAccountData *adata = pop_adata_get(m);
  struct Email *e = m->emails[msgno];
  struct PopEmailData *edata = e->edata;
  bool bcache = true;

  /* see if we already have the message in body cache */
  msg->fp = mutt_bcache_get(adata->bcache, cache_id(edata->uid));
  if (msg->fp)
    return 0;

  /* see if we already have the message in our cache in
   * case $message_cachedir is unset */
  struct PopCache *cache = &adata->cache[e->index % POP_CACHE_LEN];

  if (cache->path)
  {
    if (cache->index == e->index)
    {
      /* yes, so just return a pointer to the message */
      msg->fp = fopen(cache->path, "r");
      if (msg->fp)
        return 0;

      mutt_perror(cache->path);
      return -1;
    }
    else
    {
      /* clear the previous entry */
      unlink(cache->path);
      FREE(&cache->path);
    }
  }

  while (true)
  {
    if (pop_reconnect(m) < 0)
      return -1;

    /* verify that massage index is correct */
    if (e->refno < 0)
    {
      mutt_error(
          _("The message index is incorrect. Try reopening the mailbox."));
      return -1;
    }

    mutt_progress_init(&progress, _("Fetching message..."), MUTT_PROGRESS_SIZE,
                       C_NetInc, e->content->length + e->content->offset - 1);

    /* see if we can put in body cache; use our cache as fallback */
    msg->fp = mutt_bcache_put(adata->bcache, cache_id(edata->uid));
    if (!msg->fp)
    {
      /* no */
      bcache = false;
      mutt_mktemp(path, sizeof(path));
      msg->fp = mutt_file_fopen(path, "w+");
      if (!msg->fp)
      {
        mutt_perror(path);
        return -1;
      }
    }

    snprintf(buf, sizeof(buf), "RETR %d\r\n", e->refno);

    const int ret = pop_fetch_data(adata, buf, &progress, fetch_message, msg->fp);
    if (ret == 0)
      break;

    mutt_file_fclose(&msg->fp);

    /* if RETR failed (e.g. connection closed), be sure to remove either
     * the file in bcache or from POP's own cache since the next iteration
     * of the loop will re-attempt to put() the message */
    if (!bcache)
      unlink(path);

    if (ret == -2)
    {
      mutt_error("%s", adata->err_msg);
      return -1;
    }

    if (ret == -3)
    {
      mutt_error(_("Can't write message to temporary file"));
      return -1;
    }
  }

  /* Update the header information.  Previously, we only downloaded a
   * portion of the headers, those required for the main display.  */
  if (bcache)
    mutt_bcache_commit(adata->bcache, cache_id(edata->uid));
  else
  {
    cache->index = e->index;
    cache->path = mutt_str_strdup(path);
  }
  rewind(msg->fp);

  /* Detach the private data */
  e->edata = NULL;

  /* we replace envelope, key in subj_hash has to be updated as well */
  if (m->subj_hash && e->env->real_subj)
    mutt_hash_delete(m->subj_hash, e->env->real_subj, e);
  mutt_label_hash_remove(m, e);
  mutt_env_free(&e->env);
  e->env = mutt_rfc822_read_header(msg->fp, e, false, false);
  if (m->subj_hash && e->env->real_subj)
    mutt_hash_insert(m->subj_hash, e->env->real_subj, e);
  mutt_label_hash_add(m, e);

  /* Reattach the private data */
  e->edata = edata;
  e->free_edata = pop_edata_free;

  e->lines = 0;
  fgets(buf, sizeof(buf), msg->fp);
  while (!feof(msg->fp))
  {
    m->emails[msgno]->lines++;
    fgets(buf, sizeof(buf), msg->fp);
  }

  e->content->length = ftello(msg->fp) - e->content->offset;

  /* This needs to be done in case this is a multipart message */
  if (!WithCrypto)
    e->security = crypt_query(e->content);

  mutt_clear_error();
  rewind(msg->fp);

  return 0;
}

/**
 * pop_msg_close - Implements MxOps::msg_close()
 * @retval 0   Success
 * @retval EOF Error, see errno
 */
static int pop_msg_close(struct Mailbox *m, struct Message *msg)
{
  return mutt_file_fclose(&msg->fp);
}

/**
 * pop_msg_save_hcache - Save message to the header cache - Implements MxOps::msg_save_hcache()
 */
static int pop_msg_save_hcache(struct Mailbox *m, struct Email *e)
{
  int rc = 0;
#ifdef USE_HCACHE
  struct PopAccountData *adata = pop_adata_get(m);
  struct PopEmailData *edata = e->edata;
  header_cache_t *hc = pop_hcache_open(adata, m->path);
  rc = mutt_hcache_store(hc, edata->uid, strlen(edata->uid), e, 0);
  mutt_hcache_close(hc);
#endif

  return rc;
}

/**
 * pop_path_probe - Is this a POP mailbox? - Implements MxOps::path_probe()
 */
enum MailboxType pop_path_probe(const char *path, const struct stat *st)
{
  if (!path)
    return MUTT_UNKNOWN;

  if (mutt_str_startswith(path, "pop://", CASE_IGNORE))
    return MUTT_POP;

  if (mutt_str_startswith(path, "pops://", CASE_IGNORE))
    return MUTT_POP;

  return MUTT_UNKNOWN;
}

/**
 * pop_path_canon - Canonicalise a mailbox path - Implements MxOps::path_canon()
 */
int pop_path_canon(char *buf, size_t buflen)
{
  if (!buf)
    return -1;

  return 0;
}

/**
 * pop_path_pretty - Implements MxOps::path_pretty()
 */
int pop_path_pretty(char *buf, size_t buflen, const char *folder)
{
  /* Succeed, but don't do anything, for now */
  return 0;
}

/**
 * pop_path_parent - Implements MxOps::path_parent()
 */
int pop_path_parent(char *buf, size_t buflen)
{
  /* Succeed, but don't do anything, for now */
  return 0;
}

// clang-format off
/**
 * MxPopOps - POP mailbox - Implements ::MxOps
 */
struct MxOps MxPopOps = {
  .magic            = MUTT_POP,
  .name             = "pop",
  .ac_find          = pop_ac_find,
  .ac_add           = pop_ac_add,
  .mbox_open        = pop_mbox_open,
  .mbox_open_append = NULL,
  .mbox_check       = pop_mbox_check,
  .mbox_sync        = pop_mbox_sync,
  .mbox_close       = pop_mbox_close,
  .msg_open         = pop_msg_open,
  .msg_open_new     = NULL,
  .msg_commit       = NULL,
  .msg_close        = pop_msg_close,
  .msg_padding_size = NULL,
  .msg_save_hcache  = pop_msg_save_hcache,
  .tags_edit        = NULL,
  .tags_commit      = NULL,
  .path_probe       = pop_path_probe,
  .path_canon       = pop_path_canon,
  .path_pretty      = pop_path_pretty,
  .path_parent      = pop_path_parent,
};
// clang-format on
