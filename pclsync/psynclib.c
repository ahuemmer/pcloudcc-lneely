/*
   Copyright (c) 2013 Anton Titov.

   Copyright (c) 2013 pCloud Ltd.  All rights reserved.

   Redistribution and use in source and binary forms, with or without
   modification, are permitted provided that the following conditions
   are met: Redistributions of source code must retain the above
   copyright notice, this list of conditions and the following
   disclaimer.  Redistributions in binary form must reproduce the
   above copyright notice, this list of conditions and the following
   disclaimer in the documentation and/or other materials provided
   with the distribution.  Neither the name of pCloud Ltd nor the
   names of its contributors may be used to endorse or promote
   products derived from this software without specific prior written
   permission.

   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
   FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL pCloud
   Ltd BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
   EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
   PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
   PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
   OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE
   USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH
   DAMAGE.
*/

#include <ctype.h>
#ifdef __GLIBC__
#include <execinfo.h>
#else
#include <libunwind.h>
#endif
#include <pthread.h>
#include <stddef.h>
#include <string.h>

#include "papi.h"
#include "pbusinessaccount.h"
#include "pcache.h"
#include "ptevent.h"
#include "pqevent.h"
#include "pcryptofolder.h"
#include "pcontacts.h"
#include "pdevice.h"
#include "pdevmon.h"
#include "pdiff.h"
#include "pdownload.h"
#include "pfile.h"
#include "pfileops.h"
#include "pfoldersync.h"
#include "pfsfolder.h"
#include "plibs.h"
#include "plist.h"
#include "plocalnotify.h"
#include "plocalscan.h"
#include "pnetlibs.h"
#include "pnotify.h"
#include "prpc.h"
#include "pp2p.h"
#include "ppagecache.h"
#include "ppassword.h"
#include "ppath.h"
#include "ppathstatus.h"
#include "prun.h"
#include "psuggest.h"
#include "psettings.h"
#include "pshm.h"
#include "pssl.h"
#include "pstatus.h"
#include "pfoldersync.h"
#include "psynclib.h"
#include "psys.h"
#include "ptask.h"
#include "ptimer.h"
#include "ptools.h"
#include "publiclinks.h"
#include "pupload.h"
#include "putil.h"
#include "psql.h"

// Variable containing UNIX time of the last backup file deleted event
time_t lastBupDelEventTime = 0;
time_t bupNotifDelay = 300;

typedef struct {
  psync_list list;
  char str[];
} string_list;

PSYNC_THREAD const char *psync_thread_name = "no name";

extern int psync_recache_contacts;

const char *psync_database = NULL;

static int psync_libstate = 0;
static pthread_mutex_t psync_libstate_mutex = PTHREAD_MUTEX_INITIALIZER;
static time_t links_last_refresh_time;

extern int unlinked;
extern int tfa;

static inline int psync_status_is_offline() {
  return pstatus_get(PSTATUS_TYPE_ONLINE) == PSTATUS_ONLINE_OFFLINE;
}

uint32_t psync_get_last_error() { return psync_error; }

void psync_set_database_path(const char *databasepath) {
  psync_database = psync_strdup(databasepath);
}

static void psync_stop_crypto_on_sleep() {
  if (psync_setting_get_bool(_PS(sleepstopcrypto)) &&
      pcryptofolder_is_unlocked()) {
    pcryptofolder_lock();
    pdbg_logf(D_NOTICE, "stopped crypto due to sleep");
  }
}

static void ssl_debug_cb(void *ctx, int level, const char *msg, int TODO1,
                         const char *TODO2) {
  pdbg_logf(D_NOTICE, "%s (%s, %d)", msg, TODO2,
        TODO1); // trying to figure out what these are...
}

void psync_set_ssl_debug_callback(pssl_debug_callback_t cb) {
  pssl_log_threshold(PSYNC_SSL_DEBUG_LEVEL);
  pssl_debug_cb(cb, NULL);
}

void psync_set_apiserver(const char *binapi, uint32_t locationid) {
  if (binapi) {
    psync_apipool_set_server(binapi);
    psync_set_string_setting("api_server", binapi);
    psync_set_int_setting("location_id", locationid);
  }
}

void psync_apiserver_init() {
  if (psync_setting_get_bool(_PS(saveauth))) {
    psync_set_apiserver(psync_setting_get_string(_PS(api_server)),
                        psync_setting_get_uint(_PS(location_id)));
  }
}

int psync_init() {
  psync_thread_name = "main app thread";
  pdbg_logf(D_NOTICE, "initializing library version " PSYNC_LIB_VERSION);
  if (IS_DEBUG) {
    pthread_mutex_lock(&psync_libstate_mutex);
    if (psync_libstate != 0) {
      pthread_mutex_unlock(&psync_libstate_mutex);
      pdbg_logf(D_BUG, "you are not supposed to call psync_init for a second time");
      return 0;
    }
  }
  pcache_init();
  psys_init();

  if (!psync_database) {
    psync_database = ppath_default_db();
    if (pdbg_unlikely(!psync_database)) {
      if (IS_DEBUG)
        pthread_mutex_unlock(&psync_libstate_mutex);

      psync_error = PERROR_NO_HOMEDIR;
      return -1;
    }
  }
  if (psql_connect(psync_database)) {
    if (IS_DEBUG)
      pthread_mutex_unlock(&psync_libstate_mutex);
    psync_error = PERROR_DATABASE_OPEN;
    return -1;
  }
  psql_statement("UPDATE task SET inprogress=0 WHERE inprogress=1");
  ptimer_init();
  if (pdbg_unlikely(pssl_init())) {
    if (IS_DEBUG)
      pthread_mutex_unlock(&psync_libstate_mutex);
    psync_error = PERROR_SSL_INIT_FAILED;
    return -1;
  }

  psync_settings_init();
  pstatus_init();
  ptimer_sleep_handler(psync_stop_crypto_on_sleep);
  ppathstatus_init();
  if (IS_DEBUG) {
    psync_libstate = 1;
    pthread_mutex_unlock(&psync_libstate_mutex);
  }

  prun_thread("Overlay main thread", prpc_main_loop);
  prpc_init();
  if (PSYNC_SSL_DEBUG_LEVEL)
    psync_set_ssl_debug_callback(ssl_debug_cb);

  return 0;
}

void psync_start_sync(pstatus_change_callback_t status_callback,
                      pevent_callback_t event_callback) {
  pdbg_logf(D_NOTICE, "starting sync");
  if (IS_DEBUG) {
    pthread_mutex_lock(&psync_libstate_mutex);
    if (psync_libstate == 0) {
      pthread_mutex_unlock(&psync_libstate_mutex);
      pdbg_logf(D_BUG, "you are calling psync_start_sync before psync_init");
      return;
    } else if (psync_libstate == 2) {
      pthread_mutex_unlock(&psync_libstate_mutex);
      pdbg_logf(D_BUG, "you are calling psync_start_sync for a second time");
      return;
    } else
      psync_libstate = 2;
    pthread_mutex_unlock(&psync_libstate_mutex);
  }
  psync_apiserver_init();
  if (status_callback)
    pstatus_set_cb(status_callback);
  if (event_callback)
    pqevent_process(event_callback);
  psyncer_init();
  pdiff_init();
  pupload_init();
  pdownload_init();
  psync_netlibs_init();
  psync_localscan_init();
  pp2p_init();
  if (psync_setting_get_bool(_PS(autostartfs)))
    psync_fs_start();
  pdevmon_init();
}

void psync_set_notification_callback(
    pnotification_callback_t notification_callback, const char *thumbsize) {
  pnotify_set_callback(notification_callback, thumbsize);
}

psync_notification_list_t *psync_get_notifications() {
  return pnotify_get();
}

uint32_t psync_download_state() { return 0; }

void psync_destroy() {
  psync_do_run = 0;
  if (pshm_cleanup() == -1) {
    pdbg_logf(D_ERROR, "failed to cleanup shm");
  }
  psync_fs_stop();
  pstatus_wait_term();
  pstatus_send_status_update();
  ptask_stop_async();
  ptimer_wake();
  ptimer_notify_exception();
  psql_sync();
  psys_sleep_milliseconds(20);
  psql_lock();
  pcache_clean();
  psql_close();
}

void psync_get_status(pstatus_t *status) { pstatus_get_cb(status); }

char *psync_get_username() {
  return psql_cellstr("SELECT value FROM setting WHERE id='username'");
}

static void clear_db(int save) {
  psql_statement("DELETE FROM setting WHERE id IN ('pass', 'auth')");
  psync_setting_set_bool(_PS(saveauth), save);
}

void psync_set_user_pass(const char *username, const char *password, int save) {
  clear_db(save);
  if (save) {
    psync_set_string_value("user", username);
    if (password && password[0])
      psync_set_string_value("pass", password);
  } else {
    pthread_mutex_lock(&psync_my_auth_mutex);
    free(psync_my_user);
    psync_my_user = psync_strdup(username);
    free(psync_my_pass);
    if (password && password[0])
      psync_my_pass = psync_strdup(password);
    pthread_mutex_unlock(&psync_my_auth_mutex);
  }
  pstatus_set(PSTATUS_TYPE_AUTH, PSTATUS_AUTH_PROVIDED);
  psync_recache_contacts = 1;
}

void psync_set_pass(const char *password, int save) {
  clear_db(save);
  if (save)
    psync_set_string_value("pass", password);
  else {
    pthread_mutex_lock(&psync_my_auth_mutex);
    free(psync_my_pass);
    psync_my_pass = psync_strdup(password);
    pthread_mutex_unlock(&psync_my_auth_mutex);
  }
  pstatus_set(PSTATUS_TYPE_AUTH, PSTATUS_AUTH_PROVIDED);
}

void psync_set_auth(const char *auth, int save) {
  clear_db(save);
  if (save)
    psync_set_string_value("auth", auth);
  else
    psync_strlcpy(psync_my_auth, auth, sizeof(psync_my_auth));
  pstatus_set(PSTATUS_TYPE_AUTH, PSTATUS_AUTH_PROVIDED);
}

int psync_mark_notificaitons_read(uint32_t notificationid) {
  binparam params[] = {PAPI_STR("auth", psync_my_auth),
                       PAPI_NUM("notificationid", notificationid)};
  return psync_run_command("readnotifications", params, NULL) ? -1 : 0;
}

static void psync_invalidate_auth(const char *auth) {
  binparam params[] = {PAPI_STR("auth", auth)};
  psync_run_command("logout", params, NULL);
}

void psync_logout(uint32_t auth_status, int doinvauth) {
  tfa = 0;
  pdbg_logf(D_NOTICE, "logout");

  psql_statement("DELETE FROM setting WHERE id IN ('pass', 'auth', 'saveauth')");
  if (doinvauth) {
    psync_invalidate_auth(psync_my_auth);
  }
  putil_wipe(psync_my_auth, sizeof(psync_my_auth));
  pcryptofolder_lock();

  pthread_mutex_lock(&psync_my_auth_mutex);
  putil_wipe(psync_my_pass, sizeof(psync_my_pass));
  free(psync_my_pass);
  pthread_mutex_unlock(&psync_my_auth_mutex);

  pstatus_set(PSTATUS_TYPE_ONLINE, PSTATUS_ONLINE_CONNECTING);
  pstatus_set(PSTATUS_TYPE_AUTH, auth_status);

  psync_fs_pause_until_login();
  pdownload_stop_all();
  pupload_stop_all();
  ptask_stop_async();
  pcache_clean();
  psync_set_apiserver(PSYNC_API_HOST, PSYNC_LOCATIONID_DEFAULT);
  psync_restart_localscan();
  ptimer_notify_exception();
  if (psync_fs_need_per_folder_refresh()) {
    psync_fs_refresh_folder(0);
  }
}

apiservers_list_t *psync_get_apiservers(char **err) {
  psock_t *api;
  binresult *bres;
  psync_list_builder_t *builder;
  const binresult *locations = 0, *location, *br;
  const char *errorret;
  apiservers_list_t *ret;
  apiserver_info_t *plocation;
  uint64_t result;
  int i, locationscnt, usessl;
  binparam params[] = {PAPI_STR("timeformat", "timestamp")};
  usessl = psync_setting_get_bool(_PS(usessl));
  api = psock_connect(
      PSYNC_API_HOST, usessl ? PSYNC_API_PORT_SSL : PSYNC_API_PORT, usessl);

  if (unlikely(!api)) {
    pdbg_logf(D_WARNING, "Can't get api from the pool. No pool ?\n");
    *err = psync_strndup("Can't get api from the pool.", 29);
    return NULL;
  }
  bres = papi_send2(api, "getlocationapi", params);
  if (likely(bres))
    psync_apipool_release(api);
  else {
    psync_apipool_release_bad(api);
    pdbg_logf(D_WARNING, "Send command returned invalid result.\n");
    *err = psync_strndup("Connection error.", 17);
    return NULL;
  }
  result = papi_find_result2(bres, "result", PARAM_NUM)->num;
  if (unlikely(result)) {
    errorret = papi_find_result2(bres, "error", PARAM_STR)->str;
    *err = psync_strndup(errorret, strlen(errorret));
    pdbg_logf(D_WARNING, "command getlocationapi returned error code %u",
          (unsigned)result);
    return NULL;
  }

  locations = papi_find_result2(bres, "locations", PARAM_ARRAY);
  locationscnt = locations->length;
  if (!locationscnt) {
    free(bres);
    return NULL;
  }
  builder = psync_list_builder_create(sizeof(apiserver_info_t),
                                      offsetof(apiservers_list_t, entries));

  for (i = 0; i < locationscnt; ++i) {
    location = locations->array[i];
    plocation = (apiserver_info_t *)psync_list_bulder_add_element(builder);
    br = papi_find_result2(location, "label", PARAM_STR);
    plocation->label = br->str;
    psync_list_add_lstring_offset(builder, offsetof(apiserver_info_t, label),
                                  br->length);
    br = papi_find_result2(location, "api", PARAM_STR);
    plocation->api = br->str;
    psync_list_add_lstring_offset(builder, offsetof(apiserver_info_t, api),
                                  br->length);
    br = papi_find_result2(location, "binapi", PARAM_STR);
    plocation->binapi = br->str;
    psync_list_add_lstring_offset(builder, offsetof(apiserver_info_t, binapi),
                                  br->length);
    plocation->locationid = papi_find_result2(location, "id", PARAM_NUM)->num;
  }
  ret = (apiservers_list_t *)psync_list_builder_finalize(builder);
  ret->serverscnt = locationscnt;
  return ret;
}

void psync_reset_apiserver() {
  psync_set_apiserver(PSYNC_API_HOST, PSYNC_LOCATIONID_DEFAULT);
}

void psync_unlink() {
  psync_sql_res *res;
  char *deviceid;
  int ret;
  char *errMsg;

  deviceid = psql_cellstr("SELECT value FROM setting WHERE id='deviceid'");
  pdbg_logf(D_NOTICE, "unlink");

  pdiff_lock();
  unlinked = 1;
  tfa = 0;
  pdownload_stop_all();
  pupload_stop_all();
  // Stop the root backup folder before unlinking the database. 0 means fetch
  // the deviceid from local DB.
  psync_stop_device(0, &errMsg);

  pstatus_download_recalc();
  pstatus_upload_recalc();
  psync_invalidate_auth(psync_my_auth);
  pcryptofolder_lock();
  psync_set_apiserver(PSYNC_API_HOST, PSYNC_LOCATIONID_DEFAULT);
  psys_sleep_milliseconds(20);
  psync_stop_localscan();
  psql_checkpt_lock();
  pstatus_set(PSTATUS_TYPE_ONLINE, PSTATUS_ONLINE_CONNECTING);
  pstatus_set(PSTATUS_TYPE_AUTH, PSTATUS_AUTH_REQUIRED);
  pstatus_set(PSTATUS_TYPE_RUN, PSTATUS_RUN_STOP);
  ptimer_notify_exception();
  psql_lock();
  pdbg_logf(D_NOTICE, "clearing database, locked");
  pcache_clean();
  ret = psql_close();
  pfile_delete(psync_database);
  if (ret) {
    free(deviceid);
    pdbg_logf(D_ERROR, "failed to close database, exiting");
    exit(1);
  }
  ppagecache_clean();
  psql_connect(psync_database);
  if (deviceid) {
    res = psql_prepare("REPLACE INTO setting (id, value) VALUES ('deviceid', ?)");
    psql_bind_str(res, 1, deviceid);
    psql_run_free(res);
    free(deviceid);
  }
  pthread_mutex_lock(&psync_my_auth_mutex);
  putil_wipe(psync_my_auth, sizeof(psync_my_auth));
  psync_my_user = NULL;
  putil_wipe(psync_my_pass, sizeof(psync_my_pass));
  psync_my_userid = 0;
  pthread_mutex_unlock(&psync_my_auth_mutex);
  pdbg_logf(D_NOTICE, "clearing database, finished");

  psync_fs_pause_until_login();
  psync_fs_clean_tasks();
  ppathstatus_init();
  psyncer_dl_queue_clear();
  psql_unlock();
  psql_checkpt_unlock();
  psync_settings_reset();
  pcache_clean();
  pnotify_clean();
  ppagecache_reopen_read();
  pdiff_unlock();
  pstatus_set(PSTATUS_TYPE_ONLINE, PSTATUS_ONLINE_CONNECTING);
  pstatus_set(PSTATUS_TYPE_ACCFULL, PSTATUS_ACCFULL_QUOTAOK);
  pstatus_set(PSTATUS_TYPE_AUTH, PSTATUS_AUTH_REQUIRED);
  pstatus_set(PSTATUS_TYPE_RUN, PSTATUS_RUN_RUN);
  psync_resume_localscan();
  if (psync_fs_need_per_folder_refresh()) {
    psync_fs_refresh_folder(0);
  }
}

int psync_tfa_has_devices() { return psync_my_2fa_has_devices; }

int psync_tfa_type() { return psync_my_2fa_type; }

static void check_tfa_result(uint64_t result) {
  if (result == 2064) {
    if (pstatus_get(PSTATUS_TYPE_AUTH) == PSTATUS_AUTH_TFAREQ) {
      free(psync_my_2fa_token);
      psync_my_2fa_token = NULL;
      pstatus_set(PSTATUS_TYPE_AUTH, PSTATUS_AUTH_PROVIDED);
    }
  }
}

static char *binresult_to_str(const binresult *res) {
  if (!res)
    return psync_strdup("field not found");
  if (res->type == PARAM_STR)
    return psync_strdup(res->str);
  else if (res->type == PARAM_NUM) {
    char buff[32], *ptr;
    uint64_t n;
    ptr = buff + sizeof(buff);
    *--ptr = 0;
    n = res->num;
    do {
      *--ptr = '0' + n % 10;
      n /= 10;
    } while (n);
    return psync_strdup(ptr);
  } else {
    return psync_strdup("bad field type");
  }
}

int psync_tfa_send_sms(char **country_code, char **phone_number) {
  if (country_code)
    *country_code = NULL;
  if (phone_number)
    *phone_number = NULL;
  if (!psync_my_2fa_token) {
    return -2;
  } else {
    binresult *res;
    uint64_t code;
    binparam params[] = {PAPI_STR("token", psync_my_2fa_token)};
    res = psync_api_run_command("tfa_sendcodeviasms", params);
    if (!res)
      return -1;
    code = papi_find_result2(res, "result", PARAM_NUM)->num;
    if (code) {
      free(res);
      check_tfa_result(code);
      return code;
    }
    if (country_code || phone_number) {
      const binresult *cres = papi_find_result2(res, "phonedata", PARAM_HASH);
      if (country_code)
        *country_code = binresult_to_str(papi_get_result2(cres, "countrycode"));
      if (phone_number)
        *phone_number = binresult_to_str(papi_get_result2(cres, "msisdn"));
    }
    free(res);
    return 0;
  }
}

int psync_tfa_send_nofification(plogged_device_list_t **devices_list) {
  if (devices_list)
    *devices_list = NULL;
  if (!psync_my_2fa_token) {
    return -2;
  } else {
    binresult *res;
    uint64_t code;
    binparam params[] = {PAPI_STR("token", psync_my_2fa_token)};
    res = psync_api_run_command("tfa_sendcodeviasysnotification", params);
    if (!res)
      return -1;
    code = papi_find_result2(res, "result", PARAM_NUM)->num;
    if (code) {
      free(res);
      check_tfa_result(code);
      return code;
    }
    if (devices_list) {
      const binresult *cres = papi_find_result2(res, "devices", PARAM_ARRAY);
      psync_list_builder_t *builder;
      uint32_t i;
      builder = psync_list_builder_create(
          sizeof(plogged_device_t), offsetof(plogged_device_list_t, devices));
      for (i = 0; i < cres->length; i++) {
        plogged_device_t *dev =
            (plogged_device_t *)psync_list_bulder_add_element(builder);
        const binresult *str =
            papi_find_result2(cres->array[i], "name", PARAM_STR);
        dev->type = papi_find_result2(cres->array[i], "type", PARAM_NUM)->num;
        dev->name = str->str;
        psync_list_add_lstring_offset(builder, offsetof(plogged_device_t, name),
                                      str->length);
      }
      *devices_list =
          (plogged_device_list_t *)psync_list_builder_finalize(builder);
    }
    free(res);
    return 0;
  }
}

plogged_device_list_t *psync_tfa_send_nofification_res() {
  plogged_device_list_t *devices_list;
  if (psync_tfa_send_nofification(&devices_list))
    return NULL;
  else
    return devices_list;
}

void psync_tfa_set_code(const char *code, int trusted, int is_recovery) {
  strncpy(psync_my_2fa_code, code, sizeof(psync_my_2fa_code));
  psync_my_2fa_code[sizeof(psync_my_2fa_code) - 1] = 0;
  psync_my_2fa_trust = trusted;
  psync_my_2fa_code_type = is_recovery ? 2 : 1;
  pstatus_set(PSTATUS_TYPE_AUTH, PSTATUS_AUTH_PROVIDED);
}

int psync_change_synctype(psync_syncid_t syncid, psync_synctype_t synctype) {
  psync_sql_res *res;
  psync_variant_row row;
  psync_uint_row urow;
  psync_folderid_t folderid;
  uint64_t perms;
  struct stat st;
  int unsigned mbedtls_md;
  psync_synctype_t oldsynctype;
  if (pdbg_unlikely(synctype < PSYNC_SYNCTYPE_MIN ||
                   synctype > PSYNC_SYNCTYPE_MAX))
    return_isyncid(PERROR_INVALID_SYNCTYPE);
  psql_start();
  res = psql_query(
      "SELECT folderid, localpath, synctype FROM syncfolder WHERE id=?");
  psql_bind_uint(res, 1, syncid);
  row = psql_fetch(res);
  if (pdbg_unlikely(!row)) {
    psql_free(res);
    psql_rollback();
    psync_error = PERROR_INVALID_SYNCID;
    return -1;
  }
  folderid = psync_get_number(row[0]);
  oldsynctype = psync_get_number(row[2]);
  if (oldsynctype == synctype) {
    psql_free(res);
    psql_rollback();
    return 0;
  }
  if (pdbg_unlikely(stat(psync_get_string(row[1]), &st)) ||
      pdbg_unlikely(!pfile_stat_isfolder(&st))) {
    psql_free(res);
    psql_rollback();
    return_isyncid(PERROR_LOCAL_FOLDER_NOT_FOUND);
  }
  psql_free(res);
  if (synctype & PSYNC_DOWNLOAD_ONLY)
    mbedtls_md = 7;
  else
    mbedtls_md = 5;
  if (pdbg_unlikely(!pfile_stat_mode_ok(&st, mbedtls_md))) {
    psql_rollback();
    return_isyncid(PERROR_LOCAL_FOLDER_ACC_DENIED);
  }
  if (folderid) {
    res = psql_query("SELECT permissions FROM folder WHERE id=?");
    if (pdbg_unlikely(!res))
      return_isyncid(PERROR_DATABASE_ERROR);
    psql_bind_uint(res, 1, folderid);
    urow = psql_fetch_int(res);
    if (pdbg_unlikely(!urow)) {
      psql_free(res);
      psql_rollback();
      return_isyncid(PERROR_REMOTE_FOLDER_NOT_FOUND);
    }
    perms = urow[0];
    psql_free(res);
  } else
    perms = PSYNC_PERM_ALL;
  if (pdbg_unlikely((synctype & PSYNC_DOWNLOAD_ONLY &&
                    (perms & PSYNC_PERM_READ) != PSYNC_PERM_READ) ||
                   (synctype & PSYNC_UPLOAD_ONLY &&
                    (perms & PSYNC_PERM_WRITE) != PSYNC_PERM_WRITE))) {
    psql_rollback();
    return_isyncid(PERROR_REMOTE_FOLDER_ACC_DENIED);
  }
  res = psql_prepare(
      "UPDATE syncfolder SET synctype=?, flags=0 WHERE id=?");
  psql_bind_uint(res, 1, synctype);
  psql_bind_uint(res, 2, syncid);
  psql_run_free(res);
  res = psql_query("SELECT folderid FROM syncedfolder WHERE syncid=?");
  psql_bind_uint(res, 1, syncid);
  while ((urow = psql_fetch_int(res)))
    psyncer_dl_queue_del(urow[0]);
  psql_free(res);
  res = psql_prepare("DELETE FROM syncedfolder WHERE syncid=?");
  psql_bind_uint(res, 1, syncid);
  psql_run_free(res);
  res = psql_prepare("DELETE FROM localfile WHERE syncid=?");
  psql_bind_uint(res, 1, syncid);
  psql_run_free(res);
  res = psql_prepare("DELETE FROM localfolder WHERE syncid=?");
  psql_bind_uint(res, 1, syncid);
  psql_run_free(res);
  ppathstatus_syncfldr_delete(syncid);
  psql_commit();
  psync_localnotify_del_sync(syncid);
  psync_restat_sync_folders_del(syncid);
  pdownload_stop_sync(syncid);
  pupload_stop_sync(syncid);
  psql_sync();
  ppathstatus_reload_syncs();
  psyncer_create(syncid);
  return 0;
}

static void psync_delete_local_recursive(psync_syncid_t syncid,
                                         psync_folderid_t localfolderid) {
  psync_sql_res *res;
  psync_uint_row row;
  res = psql_query(
      "SELECT id FROM localfolder WHERE localparentfolderid=? AND syncid=?");
  psql_bind_uint(res, 1, localfolderid);
  psql_bind_uint(res, 2, syncid);
  while ((row = psql_fetch_int(res)))
    psync_delete_local_recursive(syncid, row[0]);
  psql_free(res);
  res = psql_prepare(
      "DELETE FROM localfile WHERE localparentfolderid=? AND syncid=?");
  psql_bind_uint(res, 1, localfolderid);
  psql_bind_uint(res, 2, syncid);
  psql_run_free(res);
  res = psql_prepare(
      "DELETE FROM localfolder WHERE id=? AND syncid=?");
  psql_bind_uint(res, 1, localfolderid);
  psql_bind_uint(res, 2, syncid);
  psql_run_free(res);
  if (psql_affected()) {
    res = psql_prepare(
        "DELETE FROM syncedfolder WHERE localfolderid=?");
    psql_bind_uint(res, 1, localfolderid);
    psql_run_free(res);
  }
}

int psync_delete_sync(psync_syncid_t syncid) {
  psync_sql_res *res;
  psql_start();

  psync_delete_local_recursive(syncid, 0);
  res = psql_prepare("DELETE FROM syncfolder WHERE id=?");
  psql_bind_uint(res, 1, syncid);
  psql_run_free(res);

  if (psql_commit())
    return -1;
  else {
    pdownload_stop_sync(syncid);
    pupload_stop_sync(syncid);
    psync_localnotify_del_sync(syncid);
    psync_restat_sync_folders_del(syncid);
    psync_restart_localscan();
    psql_sync();
    ppathstatus_syncfldr_delete(syncid);
    ppathstatus_reload_syncs();

    return 0;
  }
}

psuggested_folders_t *psync_get_sync_suggestions() {
  char *home;
  psuggested_folders_t *ret;
  home = ppath_home();
  if (pdbg_likely(home)) {
    ret = psuggest_scan_folder(home);
    free(home);
    return ret;
  } else {
    psync_error = PERROR_NO_HOMEDIR;
    return NULL;
  }
}

pentry_t *psync_stat_path(const char *remotepath) {
  return pfolder_stat(remotepath);
}

int psync_is_lname_to_ignore(const char *name, size_t namelen) {
  const char *ign, *sc, *pt;
  char *namelower;
  unsigned char *lp;
  size_t ilen, off, pl;
  char buff[120];
  if (namelen >= sizeof(buff))
    namelower = (char *)malloc(namelen + 1);
  else
    namelower = buff;
  memcpy(namelower, name, namelen);
  namelower[namelen] = 0;
  lp = (unsigned char *)namelower;
  while (*lp) {
    *lp = tolower(*lp);
    lp++;
  }
  ign = psync_setting_get_string(_PS(ignorepatterns));
  ilen = strlen(ign);
  off = 0;
  do {
    sc = (const char *)memchr(ign + off, ';', ilen - off);
    if (sc)
      pl = sc - ign - off;
    else
      pl = ilen - off;
    pt = ign + off;
    off += pl + 1;
    while (pl && isspace((unsigned char)*pt)) {
      pt++;
      pl--;
    }
    while (pl && isspace((unsigned char)pt[pl - 1]))
      pl--;
    if (psync_match_pattern(namelower, pt, pl)) {
      if (namelower != buff)
        free(namelower);
      pdbg_logf(D_NOTICE, "ignoring file/folder %s", name);
      return 1;
    }
  } while (sc);
  if (namelower != buff)
    free(namelower);
  return 0;
}

int psync_is_name_to_ignore(const char *name) {
  return psync_is_lname_to_ignore(name, strlen(name));
}

static void psync_set_run_status(uint32_t status) {
  pstatus_set(PSTATUS_TYPE_RUN, status);
  psync_set_uint_value("runstatus", status);
}

int psync_pause() {
  psync_set_run_status(PSTATUS_RUN_PAUSE);
  return 0;
}

int psync_stop() {
  psync_set_run_status(PSTATUS_RUN_STOP);
  ptimer_notify_exception();
  return 0;
}

int psync_resume() {
  psync_set_run_status(PSTATUS_RUN_RUN);
  return 0;
}

void psync_run_localscan() { psync_wake_localscan(); }

#define run_command_get_res(cmd, params, err, res)                             \
  do_run_command_get_res(cmd, strlen(cmd), params,                             \
                         sizeof(params) / sizeof(binparam), err, res)

static int do_run_command_get_res(const char *cmd, size_t cmdlen,
                                  const binparam *params, size_t paramscnt,
                                  char **err, binresult **pres) {
  psock_t *api;
  binresult *res;
  uint64_t result;
  api = psync_apipool_get();
  if (unlikely(!api))
    goto neterr;
  res = papi_send(api, cmd, cmdlen, params, paramscnt, -1, 1);
  if (likely(res))
    psync_apipool_release(api);
  else {
    psync_apipool_release_bad(api);
    goto neterr;
  }
  result = papi_find_result2(res, "result", PARAM_NUM)->num;
  if (result) {
    pdbg_logf(D_WARNING, "command %s returned code %u", cmd, (unsigned)result);
    if (err)
      *err = psync_strdup(papi_find_result2(res, "error", PARAM_STR)->str);
    psync_process_api_error(result);
  }
  if (result)
    free(res);
  else
    *pres = res;
  return (int)result;
neterr:
  if (err)
    *err = psync_strdup("Could not connect to the server.");
  return -1;
}

int psync_register(const char *email, const char *password, int termsaccepted,
                   const char *binapi, unsigned int locationid, char **err) {
  binresult *res;
  psock_t *sock;
  uint64_t result;
  binparam params[] = {PAPI_STR("mail", email), PAPI_STR("password", password),
                       PAPI_STR("termsaccepted", termsaccepted ? "yes" : "0"),
                       PAPI_NUM("os", P_OS_ID)};
  if (binapi)
    psync_set_apiserver(binapi, locationid);
  else {
    if (err)
      *err = psync_strdup("Could not connect to the server.");
    psync_set_apiserver(PSYNC_API_HOST, PSYNC_LOCATIONID_DEFAULT);
    return -1;
  }
  sock = papi_connect(binapi, psync_setting_get_bool(_PS(usessl)));
  if (pdbg_unlikely(!sock)) {
    if (err)
      *err = psync_strdup("Could not connect to the server.");
    psync_set_apiserver(PSYNC_API_HOST, PSYNC_LOCATIONID_DEFAULT);
    return -1;
  }
  res = papi_send2(sock, "register", params);
  if (pdbg_unlikely(!res)) {
    psock_close(sock);
    if (err)
      *err = psync_strdup("Could not connect to the server.");
    psync_set_apiserver(PSYNC_API_HOST, PSYNC_LOCATIONID_DEFAULT);
    return -1;
  }
  result = papi_find_result2(res, "result", PARAM_NUM)->num;
  if (result) {
    pdbg_logf(D_WARNING, "command register returned code %u", (unsigned)result);
    if (err)
      *err = psync_strdup(papi_find_result2(res, "error", PARAM_STR)->str);
    psync_set_apiserver(PSYNC_API_HOST, PSYNC_LOCATIONID_DEFAULT);
  }
  psock_close(sock);
  free(res);
  return result;
}

int psync_verify_email(char **err) {
  binparam params[] = {PAPI_STR("auth", psync_my_auth)};
  return psync_run_command("sendverificationemail", params, err);
}

int psync_verify_email_restricted(char **err) {
  binparam params[] = {PAPI_STR("verifytoken", psync_my_verify_token)};
  return psync_run_command("sendverificationemail", params, err);
}

int psync_lost_password(const char *email, char **err) {
  binparam params[] = {PAPI_STR("mail", email)};
  return psync_run_command("lostpassword", params, err);
}

int psync_change_password(const char *currentpass, const char *newpass,
                          char **err) {
  char *device;
  int ret;
  binresult *res;
  device = pdevice_id();
  {
    binparam params[] = {PAPI_STR("auth", psync_my_auth),
                         PAPI_STR("oldpassword", currentpass),
                         PAPI_STR("newpassword", newpass), PAPI_STR("device", device),
                         PAPI_BOOL("regetauth", 1)};
    ret = run_command_get_res("changepassword", params, err, &res);
  }
  free(device);
  if (ret)
    return ret;
  psync_strlcpy(psync_my_auth, papi_find_result2(res, "auth", PARAM_STR)->str,
                sizeof(psync_my_auth));
  free(res);
  return 0;
}

int psync_create_remote_folder_by_path(const char *path, char **err) {
  binparam params[] = {PAPI_STR("auth", psync_my_auth), PAPI_STR("path", path),
                       PAPI_STR("timeformat", "timestamp")};
  binresult *res;
  int ret;
  ret = run_command_get_res("createfolder", params, err, &res);
  if (ret)
    return ret;
  pfileops_create_fldr(papi_find_result2(res, "metadata", PARAM_HASH));
  free(res);
  pdiff_wake();
  return 0;
}

int psync_create_remote_folder(psync_folderid_t parentfolderid,
                               const char *name, char **err) {
  binparam params[] = {PAPI_STR("auth", psync_my_auth),
                       PAPI_NUM("folderid", parentfolderid), PAPI_STR("name", name),
                       PAPI_STR("timeformat", "timestamp")};
  binresult *res;
  int ret;
  ret = run_command_get_res("createfolder", params, err, &res);
  if (ret)
    return ret;
  pfileops_create_fldr(papi_find_result2(res, "metadata", PARAM_HASH));
  free(res);
  pdiff_wake();
  return 0;
}

const char *psync_get_auth_string() { return psync_my_auth; }

int psync_get_bool_setting(const char *settingname) {
  return psync_setting_get_bool(psync_setting_getid(settingname));
}

int psync_set_bool_setting(const char *settingname, int value) {
  return psync_setting_set_bool(psync_setting_getid(settingname), value);
}

int64_t psync_get_int_setting(const char *settingname) {
  return psync_setting_get_int(psync_setting_getid(settingname));
}

int psync_set_int_setting(const char *settingname, int64_t value) {
  return psync_setting_set_int(psync_setting_getid(settingname), value);
}

uint64_t psync_get_uint_setting(const char *settingname) {
  return psync_setting_get_uint(psync_setting_getid(settingname));
}

int psync_set_uint_setting(const char *settingname, uint64_t value) {
  return psync_setting_set_uint(psync_setting_getid(settingname), value);
}

const char *psync_get_string_setting(const char *settingname) {
  return psync_setting_get_string(psync_setting_getid(settingname));
}

int psync_set_string_setting(const char *settingname, const char *value) {
  return psync_setting_set_string(psync_setting_getid(settingname), value);
}

int psync_reset_setting(const char *settingname) {
  return psync_setting_reset(psync_setting_getid(settingname));
}

int psync_has_value(const char *valuename) {
  psync_sql_res *res;
  psync_uint_row row;
  int ret;
  res = psql_query_rdlock("SELECT COUNT(*) FROM setting WHERE id=?");
  psql_bind_str(res, 1, valuename);
  row = psql_fetch_int(res);
  if (row)
    ret = row[0];
  else
    ret = 0;
  psql_free(res);
  return ret;
}

int psync_get_bool_value(const char *valuename) {
  return !!psync_get_uint_value(valuename);
}

void psync_set_bool_value(const char *valuename, int value) {
  psync_set_uint_value(valuename, (uint64_t)(!!value));
}

int64_t psync_get_int_value(const char *valuename) {
  return (int64_t)psync_get_uint_value(valuename);
}

void psync_set_int_value(const char *valuename, int64_t value) {
  psync_set_uint_value(valuename, (uint64_t)value);
}

uint64_t psync_get_uint_value(const char *valuename) {
  psync_sql_res *res;
  psync_uint_row row;
  uint64_t ret;
  res = psql_query_rdlock("SELECT value FROM setting WHERE id=?");
  psql_bind_str(res, 1, valuename);
  row = psql_fetch_int(res);
  if (row)
    ret = row[0];
  else
    ret = 0;
  psql_free(res);
  return ret;
}

void psync_set_uint_value(const char *valuename, uint64_t value) {
  psync_sql_res *res;
  res = psql_prepare(
      "REPLACE INTO setting (id, value) VALUES (?, ?)");
  psql_bind_str(res, 1, valuename);
  psql_bind_uint(res, 2, value);
  psql_run_free(res);
}

char *psync_get_string_value(const char *valuename) {
  psync_sql_res *res;
  psync_str_row row;
  char *ret;
  res = psql_query_rdlock("SELECT value FROM setting WHERE id=?");
  psql_bind_str(res, 1, valuename);
  row = psql_fetch_str(res);
  if (row)
    ret = psync_strdup(row[0]);
  else
    ret = NULL;
  psql_free(res);
  return ret;
}

void psync_set_string_value(const char *valuename, const char *value) {
  psync_sql_res *res;
  res = psql_prepare(
      "REPLACE INTO setting (id, value) VALUES (?, ?)");
  psql_bind_str(res, 1, valuename);
  psql_bind_str(res, 2, value);
  psql_run_free(res);
}

void psync_network_exception() { ptimer_notify_exception(); }

static int create_request(psync_list_builder_t *builder, void *element,
                          psync_variant_row row) {
  psync_sharerequest_t *request;
  const char *str;
  uint32_t perms;
  size_t len;
  request = (psync_sharerequest_t *)element;
  request->sharerequestid = psync_get_number(row[0]);
  request->folderid = psync_get_number(row[1]);
  request->created = psync_get_number(row[2]);
  perms = psync_get_number(row[3]);
  request->userid = psync_get_number_or_null(row[4]);
  str = psync_get_lstring(row[5], &len);
  request->email = str;
  psync_list_add_lstring_offset(builder, offsetof(psync_sharerequest_t, email),
                                len);
  str = psync_get_lstring(row[6], &len);
  request->sharename = str;
  psync_list_add_lstring_offset(builder,
                                offsetof(psync_sharerequest_t, sharename), len);
  str = psync_get_lstring_or_null(row[7], &len);
  if (str) {
    request->message = str;
    psync_list_add_lstring_offset(builder,
                                  offsetof(psync_sharerequest_t, message), len);
  } else {
    request->message = "";
  }
  request->permissions = perms;
  request->canread = (perms & PSYNC_PERM_READ) / PSYNC_PERM_READ;
  request->cancreate = (perms & PSYNC_PERM_CREATE) / PSYNC_PERM_CREATE;
  request->canmodify = (perms & PSYNC_PERM_MODIFY) / PSYNC_PERM_MODIFY;
  request->candelete = (perms & PSYNC_PERM_DELETE) / PSYNC_PERM_DELETE;
  request->isba = psync_get_number(row[8]);
  return 0;
}

psync_sharerequest_list_t *psync_list_sharerequests(int incoming) {
  psync_list_builder_t *builder;
  psync_sql_res *res;
  builder = psync_list_builder_create(
      sizeof(psync_sharerequest_t),
      offsetof(psync_sharerequest_list_t, sharerequests));
  incoming = !!incoming;
  res = psql_query_rdlock(
      "SELECT id, folderid, ctime, permissions, userid, mail, name, message, "
      "ifnull(isba, 0) FROM sharerequest WHERE isincoming=? ORDER BY name");
  psql_bind_uint(res, 1, incoming);
  psql_list_add(builder, res, create_request);
  return (psync_sharerequest_list_t *)psync_list_builder_finalize(builder);
}

static int create_share(psync_list_builder_t *builder, void *element,
                        psync_variant_row row) {
  psync_share_t *share;
  const char *str;
  uint32_t perms;
  size_t len;
  share = (psync_share_t *)element;
  share->shareid = psync_get_number(row[0]);
  share->folderid = psync_get_number(row[1]);
  share->created = psync_get_number(row[2]);
  perms = psync_get_number(row[3]);
  share->userid = psync_get_number(row[4]);
  if (row[5].type != PSYNC_TNULL) {
    str = psync_get_lstring(row[5], &len);
    share->toemail = str;
    psync_list_add_lstring_offset(builder, offsetof(psync_share_t, toemail),
                                  len);
  } else
    share->toemail = "";
  if (row[6].type != PSYNC_TNULL) {
    str = psync_get_lstring(row[6], &len);
    share->fromemail = str;
    psync_list_add_lstring_offset(builder, offsetof(psync_share_t, fromemail),
                                  len);
  } else
    share->fromemail = "";
  if (row[7].type != PSYNC_TNULL) {
    str = psync_get_lstring(row[7], &len);
    share->sharename = str;
    psync_list_add_lstring_offset(builder, offsetof(psync_share_t, sharename),
                                  len);
  } else
    share->sharename = "";
  share->permissions = perms;
  share->canread = (perms & PSYNC_PERM_READ) / PSYNC_PERM_READ;
  share->cancreate = (perms & PSYNC_PERM_CREATE) / PSYNC_PERM_CREATE;
  share->canmodify = (perms & PSYNC_PERM_MODIFY) / PSYNC_PERM_MODIFY;
  share->candelete = (perms & PSYNC_PERM_DELETE) / PSYNC_PERM_DELETE;
  share->canmanage = (perms & PSYNC_PERM_MANAGE) / PSYNC_PERM_MANAGE;
  if (psync_get_number(row[8]))
    share->isba = 1;
  else
    share->isba = 0;
  share->isteam = psync_get_number(row[9]);
  return 0;
}

psync_share_list_t *psync_list_shares(int incoming) {
  psync_list_builder_t *builder;
  psync_sql_res *res;
  builder = psync_list_builder_create(sizeof(psync_share_t),
                                      offsetof(psync_share_list_t, shares));
  incoming = !!incoming;
  if (incoming) {
    res = psql_query_rdlock(
        "SELECT id, folderid, ctime, permissions, userid, ifnull(mail, ''), "
        "ifnull(mail, '') as frommail, name, ifnull(bsharedfolderid, 0), 0 "
        "FROM sharedfolder WHERE isincoming=1 AND id >= 0 "
        " UNION ALL "
        " select id, folderid, ctime, permissions, fromuserid as userid , "
        " case when isteam = 1 then (select name from baccountteam where id = "
        "toteamid) "
        "  else (select mail from baccountemail where id = touserid) end as "
        "mail, "
        " (select mail from baccountemail where id = fromuserid) as frommail,"
        " name, id as bsharedfolderid, 0 from bsharedfolder where isincoming = "
        "1 "
        " ORDER BY name;");
    psql_list_add(builder, res, create_share);
  } else {
    res = psql_query_rdlock(
        "SELECT sf.id, sf.folderid, sf.ctime, sf.permissions, sf.userid, "
        "ifnull(sf.mail, ''), ifnull(sf.mail, '') as frommail, f.name as "
        "fname, ifnull(sf.bsharedfolderid, 0), 0 "
        " FROM sharedfolder sf, folder f WHERE sf.isincoming=0 AND sf.id >= 0 "
        "and sf.folderid = f.id "
        " UNION ALL "
        " select bsf.id, bsf.folderid, bsf.ctime,  bsf.permissions, "
        " case when bsf.isincoming = 0 and bsf.isteam = 1 then bsf.toteamid "
        "else bsf.touserid end as userid , "
        " case when bsf.isincoming = 0 and bsf.isteam = 1 then (select name "
        "from baccountteam where id = bsf.toteamid) "
        " else (select mail from baccountemail where id = bsf.touserid) end as "
        "mail, "
        " (select mail from baccountemail where id = bsf.fromuserid) as "
        "frommail, "
        " bsf.name as fname, bsf.id, bsf.isteam from bsharedfolder bsf, folder "
        "f where bsf.isincoming = 0 "
        " and bsf.folderid = f.id ORDER BY fname ");
    psql_list_add(builder, res, create_share);
  }

  return (psync_share_list_t *)psync_list_builder_finalize(builder);
}

static uint32_t convert_perms(uint32_t permissions) {
  return (permissions & PSYNC_PERM_CREATE) / PSYNC_PERM_CREATE * 1 +
         (permissions & PSYNC_PERM_MODIFY) / PSYNC_PERM_MODIFY * 2 +
         (permissions & PSYNC_PERM_DELETE) / PSYNC_PERM_DELETE * 4 +
         (permissions & PSYNC_PERM_MANAGE) / PSYNC_PERM_MANAGE * 8;
}

int psync_share_folder(psync_folderid_t folderid, const char *name,
                       const char *mail, const char *message,
                       uint32_t permissions, char **err) {
  binparam params[] = {PAPI_STR("auth", psync_my_auth),
                       PAPI_NUM("folderid", folderid),
                       PAPI_STR("name", name),
                       PAPI_STR("mail", mail),
                       PAPI_STR("message", message),
                       PAPI_NUM("permissions", convert_perms(permissions)),
                       PAPI_NUM("strictmode", 1)};
  return psync_run_command("sharefolder", params, err);
}

int psync_crypto_share_folder(psync_folderid_t folderid, const char *name,
                              const char *mail, const char *message,
                              uint32_t permissions, char *hint, char *temppass,
                              char **err) {
  char *priv_key = NULL;
  char *signature = NULL;
  int change_err;

  if (!temppass) {
    binparam params[] = {PAPI_STR("auth", psync_my_auth),
                         PAPI_NUM("folderid", folderid),
                         PAPI_STR("name", name),
                         PAPI_STR("mail", mail),
                         PAPI_STR("message", message),
                         PAPI_NUM("permissions", convert_perms(permissions)),
                         PAPI_STR("hint", hint),
                         PAPI_NUM("strictmode", 1)};
    return psync_run_command("sharefolder", params, err);
  }
  if ((change_err = pcryptofolder_change_pass_unlocked(
           temppass, PSYNC_CRYPTO_FLAG_TEMP_PASS, &priv_key, &signature))) {
    return change_err;
  }
  {
    binparam params[] = {PAPI_STR("auth", psync_my_auth),
                         PAPI_NUM("folderid", folderid),
                         PAPI_STR("name", name),
                         PAPI_STR("mail", mail),
                         PAPI_STR("message", message),
                         PAPI_NUM("permissions", convert_perms(permissions)),
                         PAPI_STR("hint", hint),
                         PAPI_STR("privatekey", priv_key),
                         PAPI_STR("signature", signature),
                         PAPI_NUM("strictmode", 1)};
    return psync_run_command("sharefolder", params, err);
  }
}

int psync_account_teamshare(psync_folderid_t folderid, const char *name,
                            psync_teamid_t teamid, const char *message,
                            uint32_t permissions, char **err) {
  binparam params[] = {PAPI_STR("auth", psync_my_auth),
                       PAPI_NUM("folderid", folderid),
                       PAPI_STR("name", name),
                       PAPI_NUM("teamid", teamid),
                       PAPI_STR("message", message),
                       PAPI_NUM("permissions", convert_perms(permissions))};
  return psync_run_command("account_teamshare", params, err);
}

int psync_crypto_account_teamshare(psync_folderid_t folderid, const char *name,
                                   psync_teamid_t teamid, const char *message,
                                   uint32_t permissions, char *hint,
                                   char *temppass, char **err) {
  char *priv_key = NULL;
  char *signature = NULL;
  int change_err;

  if (!temppass) {
    binparam params[] = {PAPI_STR("auth", psync_my_auth),
                         PAPI_NUM("folderid", folderid),
                         PAPI_STR("name", name),
                         PAPI_NUM("teamid", teamid),
                         PAPI_STR("message", message),
                         PAPI_NUM("permissions", convert_perms(permissions)),
                         PAPI_STR("hint", hint)};
    return psync_run_command("account_teamshare", params, err);
  }

  if ((change_err = pcryptofolder_change_pass_unlocked(
           temppass, PSYNC_CRYPTO_FLAG_TEMP_PASS, &priv_key, &signature))) {
    return change_err;
  }

  {
    binparam params[] = {PAPI_STR("auth", psync_my_auth),
                         PAPI_NUM("folderid", folderid),
                         PAPI_STR("name", name),
                         PAPI_NUM("teamid", teamid),
                         PAPI_STR("message", message),
                         PAPI_NUM("permissions", convert_perms(permissions)),
                         PAPI_STR("hint", hint),
                         PAPI_STR("privatekey", priv_key),
                         PAPI_STR("signature", signature)};
    return psync_run_command("account_teamshare", params, err);
  }
}

int psync_cancel_share_request(psync_sharerequestid_t requestid, char **err) {
  binparam params[] = {PAPI_STR("auth", psync_my_auth),
                       PAPI_NUM("sharerequestid", requestid)};
  return psync_run_command("cancelsharerequest", params, err);
}

int psync_decline_share_request(psync_sharerequestid_t requestid, char **err) {
  binparam params[] = {PAPI_STR("auth", psync_my_auth),
                       PAPI_NUM("sharerequestid", requestid)};
  return psync_run_command("declineshare", params, err);
}

int psync_accept_share_request(psync_sharerequestid_t requestid,
                               psync_folderid_t tofolderid, const char *name,
                               char **err) {
  if (name) {
    binparam params[] = {PAPI_STR("auth", psync_my_auth),
                         PAPI_NUM("sharerequestid", requestid),
                         PAPI_NUM("folderid", tofolderid), PAPI_STR("name", name)};
    return psync_run_command("acceptshare", params, err);
  } else {
    binparam params[] = {PAPI_STR("auth", psync_my_auth),
                         PAPI_NUM("sharerequestid", requestid),
                         PAPI_NUM("folderid", tofolderid)};
    return psync_run_command("acceptshare", params, err);
  }
}

int psync_account_stopshare(psync_shareid_t shareid, char **err) {
  psync_shareid_t shareidarr[] = {shareid};
  pdbg_logf(D_NOTICE, "shareidarr %lld", (long long)shareidarr[0]);
  int result = do_psync_account_stopshare(shareidarr, 1, shareidarr, 1, err);
  return result;
}

int psync_remove_share(psync_shareid_t shareid, char **err) {
  int result;
  char *err1 = NULL;
  binparam params[] = {PAPI_STR("auth", psync_my_auth), PAPI_NUM("shareid", shareid)};
  result = psync_run_command("removeshare", params, err);
  if (result == 2025) {
    result = psync_account_stopshare(shareid, &err1);
    if (result == 2075) {
      result = 2025;
      free(err1);
    } else {
      free(*err);
      *err = err1;
    }
    pdbg_logf(D_NOTICE, "erroris  %s", *err);
  }
  return result;
}

static int psync_account_modifyshare(psync_shareid_t shareid,
                                     uint32_t permissions, char **err) {
  psync_shareid_t shareidarr[] = {shareid};
  uint32_t permsarr[] = {permissions};
  pdbg_logf(D_NOTICE, "shareidarr %lld", (long long)shareidarr[0]);
  int result = do_psync_account_modifyshare(shareidarr, permsarr, 1, shareidarr,
                                            permsarr, 1, err);
  return result;
}

int psync_modify_share(psync_shareid_t shareid, uint32_t permissions,
                       char **err) {
  int result;
  char *err1 = NULL;
  binparam params[] = {PAPI_STR("auth", psync_my_auth), PAPI_NUM("shareid", shareid),
                       PAPI_NUM("permissions", convert_perms(permissions))};
  result = psync_run_command("changeshare", params, err);
  if (result == 2025) {
    result =
        psync_account_modifyshare(shareid, convert_perms(permissions), &err1);
    if (result == 2075) {
      result = 2025;
      free(err1);
    } else {
      free(*err);
      *err = err1;
    }
    pdbg_logf(D_NOTICE, "erroris  %s", *err);
  }
  return result;
}

static void psync_del_all_except(void *ptr, ppath_fast_stat *st) {
  const char **nmarr;
  char *fp;
  nmarr = (const char **)ptr;
  if (!strcmp(st->name, nmarr[1]) || pfile_stat_fast_isfolder(st))
    return;
  fp = psync_strcat(nmarr[0], "/", st->name, NULL);
  pdbg_logf(D_NOTICE, "deleting old update file %s", fp);
  if (pfile_delete(fp))
    pdbg_logf(D_WARNING, "could not delete %s", fp);
  free(fp);
}

static char *psync_filename_from_res(const binresult *res) {
  const char *nm;
  char *nmd, *path, *ret;
  const char *nmarr[2];
  nm = strrchr(papi_find_result2(res, "path", PARAM_STR)->str, '/');
  if (pdbg_unlikely(!nm))
    return NULL;
  path = ppath_private_tmp();
  if (pdbg_unlikely(!path))
    return NULL;
  nmd = psync_url_decode(nm + 1);
  nmarr[0] = path;
  nmarr[1] = nmd;
  ppath_ls_fast(path, psync_del_all_except, (void *)nmarr);
  ret = psync_strcat(path, "/", nmd, NULL);
  free(nmd);
  free(path);
  return ret;
}

static int psync_upload_result(binresult *res, psync_fileid_t *fileid) {
  uint64_t result;
  result = papi_find_result2(res, "result", PARAM_NUM)->num;
  if (likely(!result)) {
    const binresult *meta =
        papi_find_result2(res, "metadata", PARAM_ARRAY)->array[0];
    *fileid = papi_find_result2(meta, "fileid", PARAM_NUM)->num;
    free(res);
    pdiff_wake();
    return 0;
  } else {
    pdbg_logf(D_WARNING, "uploadfile returned error %u: %s", (unsigned)result,
          papi_find_result2(res, "error", PARAM_STR)->str);
    free(res);
    psync_process_api_error(result);
    return result;
  }
}

static int psync_upload_params(binparam *params, size_t paramcnt,
                               const void *data, size_t length,
                               psync_fileid_t *fileid) {
  psock_t *api;
  binresult *res;
  int tries;
  tries = 0;
  do {
    api = psync_apipool_get();
    if (unlikely(!api))
      break;
    if (likely(papi_send(api, "uploadfile", strlen("uploadfile"), params,
                               paramcnt, length, 0))) {
      if (psock_writeall(api, data, length) == length) {
        res = papi_result(api);
        if (likely(res)) {
          psync_apipool_release(api);
          return psync_upload_result(res, fileid);
        }
      }
    }
    psync_apipool_release_bad(api);
  } while (++tries <= PSYNC_RETRY_REQUEST);
  ptimer_notify_exception();
  return -1;
}

int psync_upload_data(psync_folderid_t folderid, const char *remote_filename,
                      const void *data, size_t length, psync_fileid_t *fileid) {
  binparam params[] = {
      PAPI_STR("auth", psync_my_auth), PAPI_NUM("folderid", folderid),
      PAPI_STR("filename", remote_filename), PAPI_BOOL("nopartial", 1)};
  return psync_upload_params(params, ARRAY_SIZE(params), data, length, fileid);
}

int psync_upload_data_as(const char *remote_path, const char *remote_filename,
                         const void *data, size_t length,
                         psync_fileid_t *fileid) {
  binparam params[] = {PAPI_STR("auth", psync_my_auth), PAPI_STR("path", remote_path),
                       PAPI_STR("filename", remote_filename),
                       PAPI_BOOL("nopartial", 1)};
  return psync_upload_params(params, ARRAY_SIZE(params), data, length, fileid);
}

static int psync_load_file(const char *local_path, char **data,
                           size_t *length) {
  int fd;
  struct stat st1, st2;
  char *buff;
  size_t len, off;
  ssize_t rd;
  int tries;
  for (tries = 0; tries < 15; tries++) {
    fd = pfile_open(local_path, O_RDONLY, 0);
    if (fd == INVALID_HANDLE_VALUE)
      goto err0;
    if (fstat(fd, &st1))
      goto err1;
    len = pfile_stat_size(&st1);
    buff = malloc(len);
    if (!buff)
      goto err1;
    off = 0;
    while (off < len) {
      rd = pfile_pread(fd, buff + off, len - off, off);
      if (rd < 0)
        break;
      off += rd;
    }
    pfile_close(fd);
    if (off == len && !stat(local_path, &st2) &&
        pfile_stat_size(&st2) == len &&
        pfile_stat_mtime_native(&st1) == pfile_stat_mtime_native(&st2)) {
      *data = buff;
      *length = len;
      return 0;
    }
    free(buff);
  }
  return -1;
err1:
  pfile_close(fd);
err0:
  return -1;
}

int psync_upload_file(psync_folderid_t folderid, const char *remote_filename,
                      const char *local_path, psync_fileid_t *fileid) {
  char *data;
  size_t length;
  int ret;
  if (psync_load_file(local_path, &data, &length))
    return -2;
  ret = psync_upload_data(folderid, remote_filename, data, length, fileid);
  free(data);
  return ret;
}

int psync_upload_file_as(const char *remote_path, const char *remote_filename,
                         const char *local_path, psync_fileid_t *fileid) {
  char *data;
  size_t length;
  int ret;
  if (psync_load_file(local_path, &data, &length))
    return -2;
  ret =
      psync_upload_data_as(remote_path, remote_filename, data, length, fileid);
  free(data);
  return ret;
}

int psync_password_quality(const char *password) {
  uint64_t score = ppassword_score(password);
  if (score < (uint64_t)1 << 30)
    return 0;
  if (score < (uint64_t)1 << 40)
    return 1;
  else
    return 2;
}

int psync_password_quality10000(const char *password) {
  uint64_t score = ppassword_score(password);
  if (score < (uint64_t)1 << 30)
    return score / (((uint64_t)1 << 30) / 10000 + 1);
  if (score < (uint64_t)1 << 40)
    return (score - ((uint64_t)1 << 30)) /
               ((((uint64_t)1 << 40) - ((uint64_t)1 << 30)) / 10000 + 1) +
           10000;
  else {
    if (score >= ((uint64_t)1 << 45) - ((uint64_t)1 << 40))
      return 29999;
    else
      return (score - ((uint64_t)1 << 40)) /
                 ((((uint64_t)1 << 45) - ((uint64_t)1 << 40)) / 10000 + 1) +
             20000;
  }
}

char *psync_derive_password_from_passphrase(const char *username,
                                            const char *passphrase) {
  return psymkey_derive(username, passphrase);
}

int psync_crypto_get_hint(char **hint) {
  if (psync_status_is_offline())
    return PSYNC_CRYPTO_HINT_CANT_CONNECT;
  else
    return pcryptofolder_get_hint(hint);
}

int psync_crypto_mkdir(psync_folderid_t folderid, const char *name,
                       const char **err, psync_folderid_t *newfolderid) {
  if (psync_status_is_offline())
    return PSYNC_CRYPTO_CANT_CONNECT;
  else
    return pcryptofolder_mkdir(folderid, name, err, newfolderid);
}

int psync_crypto_hassubscription() {
  return psql_cellint(
      "SELECT value FROM setting WHERE id='cryptosubscription'", 0);
}

int psync_crypto_isexpired() {
  int64_t ce;
  ce = psql_cellint("SELECT value FROM setting WHERE id='cryptoexpires'",
                         0);
  return ce ? (ce < ptimer_time()) : 0;
}

time_t psync_crypto_expires() {
  return psql_cellint("SELECT value FROM setting WHERE id='cryptoexpires'",
                           0);
}

int psync_crypto_reset() {
  if (psync_status_is_offline())
    return PSYNC_CRYPTO_RESET_CANT_CONNECT;
  else
    return pcryptofolder_reset();
}

psync_folderid_t psync_crypto_folderid() {
  int64_t id;
  id = psql_cellint(
      "SELECT id FROM folder WHERE parentfolderid=0 AND "
      "flags&" NTO_STR(PSYNC_FOLDER_FLAG_ENCRYPTED) "=" NTO_STR(
          PSYNC_FOLDER_FLAG_ENCRYPTED) " LIMIT 1",
      0);
  if (id)
    return id;
  id = psql_cellint(
      "SELECT f1.id FROM folder f1, folder f2 WHERE f1.parentfolderid=f2.id "
      "AND "
      "f1.flags&" NTO_STR(PSYNC_FOLDER_FLAG_ENCRYPTED) "=" NTO_STR(
          PSYNC_FOLDER_FLAG_ENCRYPTED) " AND "
                                       "f2.flags&" NTO_STR(
                                           PSYNC_FOLDER_FLAG_ENCRYPTED) "=0 "
                                                                        "LIMIT "
                                                                        "1",
      0);
  if (id)
    return id;
  else
    return PSYNC_CRYPTO_INVALID_FOLDERID;
}

psync_folderid_t *psync_crypto_folderids() {
  psync_sql_res *res;
  psync_uint_row row;
  psync_folderid_t *ret;
  size_t alloc, l;
  alloc = 2;
  l = 0;
  ret = malloc(sizeof(psync_folderid_t) * alloc);
  res = psql_query_rdlock(
      "SELECT f1.id FROM folder f1, folder f2 WHERE f1.parentfolderid=f2.id "
      "AND "
      "f1.flags&" NTO_STR(PSYNC_FOLDER_FLAG_ENCRYPTED) "=" NTO_STR(
          PSYNC_FOLDER_FLAG_ENCRYPTED) " AND "
                                       "f2.flags&" NTO_STR(
                                           PSYNC_FOLDER_FLAG_ENCRYPTED) "=0");
  while ((row = psql_fetch_int(res))) {
    ret[l] = row[0];
    if (++l == alloc) {
      alloc *= 2;
      ret = (psync_folderid_t *)realloc(ret,
                                              sizeof(psync_folderid_t) * alloc);
    }
  }
  psql_free(res);
  ret[l] = PSYNC_CRYPTO_INVALID_FOLDERID;
  return ret;
}

int psync_crypto_change_crypto_pass(const char *oldpass, const char *newpass,
                                    const char *hint, const char *code) {
  psock_t *api;
  binresult *res;
  uint64_t result;
  int tries = 0, err;
  char *priv_key = NULL;
  char *signature = NULL;

  if ((err = pcryptofolder_change_pass(oldpass, newpass, 0, &priv_key,
                                            &signature))) {
    return err;
  }
  {
    binparam params[] = {PAPI_STR("auth", psync_my_auth),
                         PAPI_STR("privatekey", priv_key),
                         PAPI_STR("signature", signature), PAPI_STR("hint", hint),
                         PAPI_STR("code", code)};
    pdbg_logf(D_NOTICE, "uploading re-encoded private key");
    while (1) {
      api = psync_apipool_get();
      if (!api)
        return pdbg_return_const(PSYNC_CRYPTO_SETUP_CANT_CONNECT);
      res = papi_send2(api, "crypto_changeuserprivate", params);
      if (pdbg_unlikely(!res)) {
        psync_apipool_release_bad(api);
        if (++tries > 5)
          return pdbg_return_const(PSYNC_CRYPTO_SETUP_CANT_CONNECT);
      } else {
        psync_apipool_release(api);
        break;
      }
    }
    result = papi_find_result2(res, "result", PARAM_NUM)->num;
    free(res);
    if (result != 0)
      pdbg_logf(D_WARNING, "crypto_changeuserprivate returned %u",
            (unsigned)result);
    if (result == 0) {
      psync_delete_cached_crypto_keys();
      return PSYNC_CRYPTO_SETUP_SUCCESS;
    }
    return pdbg_return_const(PSYNC_CRYPTO_SETUP_UNKNOWN_ERROR);
  }
}

int psync_crypto_change_crypto_pass_unlocked(const char *newpass,
                                             const char *hint,
                                             const char *code) {
  psock_t *api;
  binresult *res;
  uint64_t result;
  int tries = 0, err;
  char *priv_key = NULL;
  char *signature = NULL;

  if ((err = pcryptofolder_change_pass_unlocked(newpass, 0, &priv_key,
                                                     &signature))) {
    return err;
  }
  {
    binparam params[] = {PAPI_STR("auth", psync_my_auth),
                         PAPI_STR("privatekey", priv_key),
                         PAPI_STR("signature", signature), PAPI_STR("hint", hint),
                         PAPI_STR("code", code)};
    pdbg_logf(D_NOTICE, "uploading re-encoded private key");
    while (1) {
      api = psync_apipool_get();
      if (!api)
        return pdbg_return_const(PSYNC_CRYPTO_SETUP_CANT_CONNECT);
      res = papi_send2(api, "crypto_changeuserprivate", params);
      if (pdbg_unlikely(!res)) {
        psync_apipool_release_bad(api);
        if (++tries > 5)
          return pdbg_return_const(PSYNC_CRYPTO_SETUP_CANT_CONNECT);
      } else {
        psync_apipool_release(api);
        break;
      }
    }
    result = papi_find_result2(res, "result", PARAM_NUM)->num;
    free(res);
    if (result != 0)
      pdbg_logf(D_WARNING, "crypto_changeuserprivate returned %u",
            (unsigned)result);
    if (result == 0) {
      psync_delete_cached_crypto_keys();
      return PSYNC_CRYPTO_SETUP_SUCCESS;
    }
    return pdbg_return_const(PSYNC_CRYPTO_SETUP_UNKNOWN_ERROR);
  }
}

int psync_crypto_crypto_send_change_user_private() {
  psock_t *api;
  binresult *res;
  uint64_t result;
  binparam params[] = {PAPI_STR("auth", psync_my_auth)};
  pdbg_logf(D_NOTICE, "Requesting code for changing the private key password");
  api = psync_apipool_get();
  if (!api)
    return pdbg_return_const(PSYNC_CRYPTO_SETUP_CANT_CONNECT);
  res = papi_send2(api, "crypto_sendchangeuserprivate", params);
  if (pdbg_unlikely(!res)) {
    psync_apipool_release_bad(api);
    return pdbg_return_const(PSYNC_CRYPTO_SETUP_CANT_CONNECT);
  } else {
    psync_apipool_release(api);
  }
  result = papi_find_result2(res, "result", PARAM_NUM)->num;
  free(res);
  if (result != 0)
    pdbg_logf(D_WARNING, "crypto_sendchangeuserprivate returned %u",
          (unsigned)result);
  if (result == 0)
    return PSYNC_CRYPTO_SETUP_SUCCESS;
  return pdbg_return_const(PSYNC_CRYPTO_SETUP_UNKNOWN_ERROR);
}

external_status_t psync_filesystem_status(const char *path) {
  switch (ppathstatus_get_status(ppathstatus_get(path))) {
  case PSYNC_PATH_STATUS_IN_SYNC:
    return INSYNC;
  case PSYNC_PATH_STATUS_IN_PROG:
    return INPROG;
  case PSYNC_PATH_STATUS_PAUSED:
  case PSYNC_PATH_STATUS_REMOTE_FULL:
  case PSYNC_PATH_STATUS_LOCAL_FULL:
    return NOSYNC;
  default:
    return INVSYNC;
  }
}

external_status_t psync_status_file(const char *path) {
  return psync_filesystem_status(path);
}

external_status_t psync_status_folder(const char *path) {
  return psync_filesystem_status(path);
}

int64_t psync_file_public_link(const char *path, char **link /*OUT*/,
                               char **err /*OUT*/) {
  int64_t ret = 0;
  do_psync_file_public_link(path, &ret, link, err, 0, 0, 0);
  return ret;
}

int64_t psync_screenshot_public_link(const char *path, int hasdelay,
                                     int64_t delay, char **link /*OUT*/,
                                     char **err /*OUT*/) {
  return do_psync_screenshot_public_link(path, hasdelay, delay, link, err);
}

int64_t psync_folder_public_link(const char *path, char **link /*OUT*/,
                                 char **err /*OUT*/) {
  return do_psync_folder_public_link(path, link, err, 0, 0, 0);
}

int64_t psync_folder_public_link_full(const char *path, char **link /*OUT*/,
                                      char **err /*OUT*/,
                                      unsigned long long expire,
                                      int maxdownloads, int maxtraffic,
                                      const char *password) {
  return do_psync_folder_public_link_full(path, link, err, expire, maxdownloads,
                                          maxtraffic, password);
}

int psync_change_link(unsigned long long linkid, unsigned long long expire,
                      int delete_expire, const char *linkpassword,
                      int delete_password, unsigned long long maxtraffic,
                      unsigned long long maxdownloads,
                      int enableuploadforeveryone,
                      int enableuploadforchosenusers, int disableupload,
                      char **err) {
  return do_psync_change_link(linkid, expire, delete_expire, linkpassword,
                              delete_password, maxtraffic, maxdownloads,
                              enableuploadforeveryone,
                              enableuploadforchosenusers, disableupload, err);
}

int64_t psync_folder_updownlink_link(int canupload, unsigned long long folderid,
                                     const char *mail, char **err /*OUT*/) {
  return do_psync_folder_updownlink_link(canupload, folderid, mail, err);
}

int64_t ptree_public_link(const char *linkname, const char *root,
                               char **folders, int numfolders, char **files,
                               int numfiles, char **link /*OUT*/,
                               char **err /*OUT*/) {
  return do_ptree_public_link(linkname, root, folders, numfolders, files,
                                   numfiles, link, err, 0, 0, 0);
}

plink_info_list_t *psync_list_links(char **err /*OUT*/) {
  return do_psync_list_links(err);
}

plink_contents_t *psync_show_link(const char *code, char **err /*OUT*/) {
  return do_show_link(code, err);
}

int psync_delete_link(int64_t linkid, char **err /*OUT*/) {
  return do_psync_delete_link(linkid, err);
}

int64_t psync_upload_link(const char *path, const char *comment,
                          char **link /*OUT*/, char **err /*OUT*/) {
  return do_psync_upload_link(path, comment, link, err, 0, 0, 0);
}

int psync_delete_upload_link(int64_t uploadlinkid, char **err /*OUT*/) {
  return do_psync_delete_upload_link(uploadlinkid, err);
}

int psync_delete_all_links_folder(psync_folderid_t folderid, char **err) {
  return do_delete_all_folder_links(folderid, err);
}

int psync_delete_all_links_file(psync_fileid_t fileid, char **err) {
  return do_delete_all_file_links(fileid, err);
}

void psync_cache_links_all() {
  if (psync_current_time - links_last_refresh_time >=
      PSYNC_LINKS_REFRESH_INTERVAL) {
    links_last_refresh_time = psync_current_time;
    cache_links_all();
  } else
    pdbg_logf(D_WARNING, "refreshing link too early %ld",
          (unsigned)psync_current_time - links_last_refresh_time);
}

preciever_list_t *psync_list_email_with_access(unsigned long long linkid,
                                               char **err) {
  return do_list_email_with_access(linkid, err);
}

int psync_link_add_access(unsigned long long linkid, const char *mail,
                          char **err) {
  return do_link_add_access(linkid, mail, err);
}

int psync_link_remove_access(unsigned long long linkid,
                             unsigned long long receiverid, char **err) {
  return do_link_remove_access(linkid, receiverid, err);
}

bookmarks_list_t *psync_cache_bookmarks(char **err) {
  return do_cache_bookmarks(err);
}

int psync_remove_bookmark(const char *code, int locationid, char **err) {
  return do_remove_bookmark(code, locationid, err);
}

int psync_change_bookmark(const char *code, int locationid, const char *name,
                          const char *description, char **err) {
  return do_change_bookmark(code, locationid, name, description, err);
}

int psync_psync_change_link(unsigned long long linkid,
                            unsigned long long expire, int delete_expire,
                            const char *linkpassword, int delete_password,
                            unsigned long long maxtraffic,
                            unsigned long long maxdownloads,
                            int enableuploadforeveryone,
                            int enableuploadforchosenusers, int disableupload,
                            char **err) {
  return do_psync_change_link(linkid, expire, delete_expire, linkpassword,
                              delete_password, maxtraffic, maxdownloads,
                              enableuploadforeveryone,
                              enableuploadforchosenusers, disableupload, err);
}
int psync_change_link_expire(unsigned long long linkid,
                             unsigned long long expire, char **err) {
  return do_change_link_expire(linkid, expire, err);
}

int psync_change_link_password(unsigned long long linkid, const char *password,
                               char **err) {
  return do_change_link_password(linkid, password, err);
}

int psync_change_link_enable_upload(unsigned long long linkid,
                                    int enableuploadforeveryone,
                                    int enableuploadforchosenusers,
                                    char **err) {
  return do_change_link_enable_upload(linkid, enableuploadforeveryone,
                                      enableuploadforchosenusers, err);
}

pcontacts_list_t *psync_list_contacts() { return do_psync_list_contacts(); }

pcontacts_list_t *psync_list_myteams() { return do_psync_list_myteams(); }

void psync_register_account_events_callback(
    paccount_cache_callback_t callback) {
  do_register_account_events_callback(callback);
}

void psync_get_current_userid(psync_userid_t *ret) {
  psync_sql_res *res;
  psync_uint_row row;

  res = psql_query_rdlock("SELECT value FROM setting WHERE id= 'userid' ");
  while ((row = psql_fetch_int(res)))
    *ret = row[0];
  psql_free(res);
}

void psync_get_folder_ownerid(psync_folderid_t folderid, psync_userid_t *ret) {
  psync_sql_res *res;
  psync_uint_row row;

  res = psql_query_rdlock("SELECT userid FROM folder WHERE id=?");
  psql_bind_uint(res, 1, folderid);
  while ((row = psql_fetch_int(res)))
    *ret = row[0];
  psql_free(res);
}

int psync_setlanguage(const char *language, char **err) {
  binparam params[] = {PAPI_STR("language", language)};
  return psync_run_command("setlanguage", params, err);
}

void psync_fs_clean_read_cache() { ppagecache_clean_read(); }

int psync_fs_move_cache(const char *path) {
  return ppagecache_move(path);
}

char *psync_get_token() {
  if (psync_my_auth[0])
    return psync_strdup(psync_my_auth);
  else
    return NULL;
}

int psync_get_promo(char **url, uint64_t *width, uint64_t *height) {
  uint64_t result;
  binresult *res;
  binparam params[] = {PAPI_STR("auth", psync_my_auth), PAPI_NUM("os", P_OS_ID)};
  *url = 0;

  res = psync_api_run_command("getpromourl", params);

  if (pdbg_unlikely(!res)) {
    return -1;
  }

  result = papi_find_result2(res, "result", PARAM_NUM)->num;

  if (result) {
    pdbg_logf(D_WARNING, "getpromourl returned %d", (int)result);
    free(res);
    return result;
  }

  if (!papi_find_result2(res, "haspromo", PARAM_BOOL)->num) {
    free(res);

    return result;
  }

  *url = psync_strdup(papi_find_result2(res, "url", PARAM_STR)->str);

  if (!papi_find_result2(res, "width", PARAM_NUM)->num) {
    pdbg_logf(D_NOTICE, "Parameter width not found.");

    free(res);
    return result;
  }

  *width = papi_find_result2(res, "width", PARAM_NUM)->num;
  pdbg_logf(D_NOTICE, "Promo window Width: [%lu]", *width);

  if (!papi_find_result2(res, "height", PARAM_NUM)->num) {
    pdbg_logf(D_NOTICE, "Parameter height not found.");

    free(res);
    return result;
  }

  *height = papi_find_result2(res, "height", PARAM_NUM)->num;
  pdbg_logf(D_NOTICE, "Promo window Height: [%lu]", *height);

  free(res);

  return 0;
}

psync_folderid_t psync_get_fsfolderid_by_path(const char *path,
                                              uint32_t *pflags,
                                              uint32_t *pPerm) {
  return psync_fsfolderidperm_by_path(path, pflags, pPerm);
}

uint32_t psync_get_fsfolderflags_by_id(psync_folderid_t folderid,
                                       uint32_t *pPerm) {
  return psync_fsfolderflags_by_id(folderid, pPerm);
}

uint64_t psync_crypto_priv_key_flags() {
  psync_sql_res *res;
  psync_uint_row row;
  uint64_t ret = 0;
  res = psql_rdlock_nocache(
      "SELECT value FROM setting WHERE id='crypto_private_flags'");
  if ((row = psql_fetch_int(res))) {
    ret = row[0];
    psql_free(res);
    return ret;
  } else
    pdbg_logf(D_NOTICE, "Can't read private key flags from the DB");
  psql_free(res);
  return ret;
}

int psync_has_crypto_folders() {
  psync_sql_res *res;
  psync_uint_row row;
  uint64_t cnt = 0;
  res = psql_rdlock_nocache(
      "SELECT count(*) FROM folder WHERE flags&" NTO_STR(
          PSYNC_FOLDER_FLAG_ENCRYPTED) "");
  if ((row = psql_fetch_int(res))) {
    cnt = row[0];
  } else
    pdbg_logf(D_NOTICE, "There are no crypto folders in the DB");
  psql_free(res);
  return cnt > 0;
}

void set_tfa_flag(int value) {
  pdbg_logf(D_NOTICE, "set tfa %u", value);
  tfa = value;
}

int psync_send_publink(const char *code, const char *mail, const char *message,
                       char **err) {
  binparam params[] = {PAPI_STR("auth", psync_my_auth), PAPI_STR("code", code),
                       PAPI_STR("mails", mail), PAPI_STR("message", message),
                       PAPI_NUM("source", 1)};
  return psync_run_command("sendpublink", params, err);
}

int psync_is_folder_syncable(char *localPath, char **errMsg) {
  psync_sql_res *sql;
  psync_str_row srow;
  folderPath folders;

  char *syncmp;
  const char *ignorePaths;

  int i;

  pdbg_logf(D_NOTICE, "Check if folder is already synced. LocalPath [%s]",
        localPath);

  sql = psql_query("SELECT localpath FROM syncfolder");

  if (pdbg_unlikely(!sql)) {
    return_isyncid(PERROR_DATABASE_ERROR);
  }

  while ((srow = psql_fetch_str(sql))) {
    if (psyncer_str_has_prefix(srow[0], localPath)) {
      psql_free(sql);

      *errMsg = psync_strdup("There is already an active sync or backup for a "
                             "parent of this folder.");
      return PERROR_PARENT_OR_SUBFOLDER_ALREADY_SYNCING;
    } else if (!strcmp(srow[0], localPath)) {
      psql_free(sql);

      *errMsg = psync_strdup(
          "There is already an active sync or backup for this folder.");
      return PERROR_FOLDER_ALREADY_SYNCING;
    }
  }
  psql_free(sql);

  pdbg_logf(D_NOTICE, "Check if folder is not on the Drive.");

  syncmp = psync_fs_getmountpoint();

  pdbg_logf(D_NOTICE, "Mount point: [%s].", syncmp);
  if (syncmp) {
    size_t len = strlen(syncmp);

    pdbg_logf(D_NOTICE, "Do check.");
    if (!memcmp(syncmp, localPath, len) &&
        (localPath[len] == 0 || localPath[len] == '/' ||
         localPath[len] == '\\')) {
      free(syncmp);

      *errMsg = psync_strdup("Folder is located on pCloud drive.");
      return PERROR_LOCAL_IS_ON_PDRIVE;
    }
    free(syncmp);
  }

  // Check if folder is not a child of an igrnored folder
  ignorePaths = psync_setting_get_string(_PS(ignorepaths));
  ptools_parse_os_path((char *)ignorePaths, &folders, (char *)DELIM_SEMICOLON, 0);

  for (i = 0; i < folders.cnt; i++) {
    pdbg_logf(D_NOTICE, "Check ignored folder: [%s]=[%s]", folders.folders[i],
          localPath);

    if (psyncer_str_starts_with(folders.folders[i], localPath)) {
      *errMsg = psync_strdup(
          "This folder is a child  of a folder in your ignore folders list.");
      return PERROR_PARENT_IS_IGNORED;
    }
  }

  return 0;
}

psync_folderid_t create_bup_mach_folder(char **msgErr) {
  binresult *rootFolIdObj;
  binresult *retData;

  psync_sql_res *sql;

  char bRootFoName[64];
  char *tmpBuff;
  int res;

  rootFolIdObj = NULL;
  tmpBuff = get_pc_name();
  psync_strlcpy(bRootFoName, tmpBuff, 64);

  free(tmpBuff);

  eventParams requiredParams = {3, // Number of parameters we are passing below.
                                {PAPI_STR("auth", psync_my_auth),
                                 PAPI_STR("name", bRootFoName),
                                 PAPI_NUM("os", P_OS_ID)}};

  eventParams optionalParams = {0};

  pdbg_logf(D_NOTICE, "Call backend [backup/createdevice].");
  res = ptools_backend_call(apiserver, "backup/createdevice", FOLDER_META,
                     &requiredParams, &optionalParams, &retData, msgErr);

  if (res == 0) {
    rootFolIdObj =
        (binresult *)papi_find_result2(retData, "folderid", PARAM_NUM);

    // Store the root folder id in the local DB
    sql = psql_prepare(
        "REPLACE INTO setting (id, value) VALUES ('BackupRootFoId', ?)");
    psql_bind_uint(sql, 1, rootFolIdObj->num);
    psql_run_free(sql);

    free(retData);
  }
  if (rootFolIdObj) {
    return rootFolIdObj->num;
  } else {
    return -1;
  }
}

int psync_create_backup(char *path, char **errMsg) {
  psync_folderid_t bFId;
  psync_syncid_t syncFId;
  binresult *folId;
  binresult *retData;
  folderPath folders;

  char *optFolName;
  int res = 0, oParCnt = 0;

  if (path[0] == 0) {
    *errMsg = strdup(PSYNC_BACKUP_PATH_EMPTY_MSG);

    return PSYNC_BACKUP_PATH_EMPTY_ERR;
  }

  res = psync_is_folder_syncable(path, errMsg);

  if (res != 0) {
    return res;
  }

  bFId = psql_cellint(
      "SELECT value FROM setting WHERE id='BackupRootFoId'", 0);

  if (bFId == 0) {
  retryRootCrt:
    bFId = create_bup_mach_folder(errMsg);
    if (bFId < 0) {
      pdbg_logf(D_BUG,
            "error occurred in create_bup_mach_folder: rootFolIdObj was NULL");
      exit(255);
    }
  }

  ptools_parse_os_path(path, &folders, (char *)DELIM_DIR, 1);

  if (folders.cnt > 1) {
    oParCnt = 1;
    optFolName = psync_strdup(folders.folders[folders.cnt - 2]);
  } else {
    oParCnt = 0;
    optFolName = psync_strdup("");
  }

  eventParams reqPar = {4, // Number of parameters we are passing below.
                        {PAPI_STR("auth", psync_my_auth),
                         PAPI_STR("name", folders.folders[folders.cnt - 1]),
                         PAPI_NUM("folderid", bFId),
                         PAPI_STR("timeformat", "timestamp")}};

  eventParams optPar = {oParCnt, {PAPI_STR(PARENT_FOLDER_NAME, optFolName)}};

  pdbg_logf(D_NOTICE, "Call backend [backup/createbackup].");

  res = ptools_backend_call(apiserver, "backup/createbackup", FOLDER_META, &reqPar,
                     &optPar, &retData, errMsg);

  if (res == 0) {
    pdiff_fldr_update(retData);

    folId = (binresult *)papi_find_result2(retData, FOLDER_ID, PARAM_NUM);

    syncFId = pfolder_add_sync(path, folId->num, PSYNC_BACKUPS);

    free(retData);

    if (syncFId < 0) {
      *errMsg = psync_strdup("Error creating backup.");
      return syncFId;
    }

    pdbg_logf(D_NOTICE, "Created sync with id[%d].", syncFId);
  } else if (res == 2002) {
    // The backup folder for the machine was deleted for wathever reason. Delete
    // the id stored in DB and create the new one.
    pdbg_logf(D_NOTICE,
          "Backup folder id is not valid. Delete it and create a new one.");

    psql_start();
    psql_statement("DELETE FROM setting WHERE id='BackupRootFoId'");
    psql_commit();

    goto retryRootCrt;
  }

  return res;
}

int psync_delete_backup(psync_syncid_t syncId, char **errMsg) {
  binresult *retData;
  psync_sql_res *sqlRes;
  psync_uint_row row;
  psync_folderid_t folderId;

  int res = 0;

  sqlRes =
      psql_query_rdlock("SELECT folderid FROM syncfolder WHERE id = ?");

  psql_bind_uint(sqlRes, 1, syncId);
  row = psql_fetch_int(sqlRes);

  if (unlikely(!row)) {
    pdbg_logf(D_ERROR, "Failed to find folder id for syncId: [%u]", syncId);
    psql_free(sqlRes);

    res = -1;
  } else {
    folderId = row[0];

    psql_free(sqlRes);
  }

  if (res == 0) {
    eventParams reqPar = {
        2, // Number of parameters we are passing below.
        {PAPI_STR("auth", psync_my_auth), PAPI_NUM("folderid", folderId)}};

    eventParams optPar = {0};

    pdbg_logf(D_NOTICE, "Call backend [backup/stopbackup].");

    res = ptools_backend_call(apiserver, "backup/stopbackup", NO_PAYLOAD, &reqPar,
                       &optPar, &retData, errMsg);

    if (res == 0) {
      res = psync_delete_sync(syncId);
    }
  }

  pdbg_logf(D_NOTICE, "Stop sync result: [%d].", res);

  return res;
}

void psync_stop_device(psync_folderid_t folderId, char **errMsg) {
  binresult *retData;
  psync_folderid_t bFId;
  int res = 0;

  if (folderId == 0) {
    bFId = psql_cellint(
        "SELECT value FROM setting WHERE id='BackupRootFoId'", 0);
  } else {
    bFId = folderId;
  }

  if (bFId > 0) {
    eventParams reqPar = {
        2, // Number of parameters we are passing below.
        {PAPI_STR("auth", psync_my_auth), PAPI_NUM("folderid", bFId)}};

    eventParams optPar = {0};

    pdbg_logf(D_NOTICE, "Call backend [backup/stopdevice].");

    res = ptools_backend_call(apiserver, "backup/stopdevice", NO_PAYLOAD, &reqPar,
                       &optPar, &retData, errMsg);

    if (res != 0) {
      pdbg_logf(D_ERROR, "Failed to stop device in the backend Message: [%s].",
            *errMsg);
    }
  } else {
    pdbg_logf(D_ERROR, "Can't find device id in local DB.");
  }
}

char *get_backup_root_name() {
  return psql_cellstr("SELECT name FROM setting s JOIN folder f ON "
                           "s.value = f.id AND s.id = 'BackupRootFoId'");
}

char *get_pc_name() { return ptools_get_machine_name(); }

void psync_async_delete_sync(void *ptr) {
  psync_syncid_t syncId = (psync_syncid_t)(uintptr_t)ptr;
  int res;

  res = psync_delete_sync(syncId);

  pdbg_logf(D_NOTICE, "Backup stopped on the Web.");

  if (res == 0) {
    pqevent_queue_eventid(PEVENT_BACKUP_STOP);
  }
}

void psync_async_ui_callback(void *ptr) {
  int eventId = *(int *)ptr;
  time_t currTime = psys_time_seconds();

  if (((currTime - lastBupDelEventTime) > bupNotifDelay) ||
      (lastBupDelEventTime == 0)) {
    pdbg_logf(D_NOTICE, "Send event to UI. Event id: [%d]", eventId);

    pqevent_queue_eventid(eventId);

    lastBupDelEventTime = currTime;
  }
}

int psync_delete_sync_by_folderid(psync_folderid_t fId) {
  psync_sql_res *sqlRes;
  psync_uint_row row;
  psync_syncid_t syncId;

  sqlRes =
      psql_query_rdlock("SELECT id FROM syncfolder WHERE folderid = ?");
  psql_bind_uint(sqlRes, 1, fId);
  row = psql_fetch_int(sqlRes);
  if (unlikely(!row)) {
    pdbg_logf(D_ERROR, "Sync to delete not found!");
    psql_free(sqlRes);
    return -1;
  }

  syncId = (psync_syncid_t)row[0];
  psql_free(sqlRes);

  prun_thread1("psync_async_sync_delete", psync_async_delete_sync,
                    (void *)(uintptr_t)syncId);

  return 0;
}

int psync_delete_backup_device(psync_folderid_t fId) {
  psync_folderid_t bFId;

  pdbg_logf(D_NOTICE, "Check if the local device was stopped. Id: [%lu]", fId);

  bFId = psql_cellint(
      "SELECT value FROM setting WHERE id='BackupRootFoId'", 0);

  if (bFId == fId) {
    psql_start();

    psql_statement("DELETE FROM setting WHERE id='BackupRootFoId'");

    psql_commit();
  } else {
    pdbg_logf(D_NOTICE, "Stop for different device. Id: [%lu]", bFId);
  }

  return 1;
}

void psync_send_backup_del_event(psync_fileorfolderid_t remoteFId) {
  time_t currTime = psys_time_seconds();

  if (((currTime - lastBupDelEventTime) > bupNotifDelay) ||
      (lastBupDelEventTime == 0)) {
    if (remoteFId == 0) {
      pqevent_queue_eventid(PEVENT_BKUP_F_DEL_NOTSYNCED);
    } else {
      pqevent_queue_eventid(PEVENT_BKUP_F_DEL_SYNCED);
    }

    lastBupDelEventTime = currTime;
  }
}

userinfo_t *psync_get_userinfo() {
  if (psync_my_auth[0]) {
    size_t lemail, lcurrency, llanguage;
    const char *email, *currency, *language;
    const binresult *cres;
    char *ptr;
    binresult *res;
    uint64_t err;
    userinfo_t *info;
    binparam params[] = {PAPI_STR("auth", psync_my_auth),
                         PAPI_STR("timeformat", "timestamp")};
    res = psync_api_run_command("userinfo", params);
    if (!res) {
      free(res);
      return NULL;
    }
    err = papi_find_result2(res, "result", PARAM_NUM)->num;
    if (err) {
      free(res);
      return NULL;
    }

    cres = papi_find_result2(res, "email", PARAM_STR);
    email = cres->str;
    lemail = (cres->length + sizeof(void *)) / sizeof(void *) * sizeof(void *);
    cres = papi_find_result2(res, "currency", PARAM_STR);
    currency = cres->str;
    lcurrency =
        (cres->length + sizeof(void *)) / sizeof(void *) * sizeof(void *);
    cres = papi_find_result2(res, "language", PARAM_STR);
    language = cres->str;
    llanguage =
        (cres->length + sizeof(void *)) / sizeof(void *) * sizeof(void *);
    info = (userinfo_t *)malloc(sizeof(userinfo_t) + lemail + lcurrency +
                                      llanguage);
    ptr = (char *)(info + 1);
    memcpy(ptr, email, lemail);
    info->email = ptr;
    ptr += lemail;
    memcpy(ptr, currency, lcurrency);
    info->currency = ptr;
    ptr += lcurrency;
    memcpy(ptr, language, llanguage);
    info->language = ptr;

    info->cryptosetup = papi_find_result2(res, "cryptosetup", PARAM_BOOL)->num;
    info->cryptosubscription =
        papi_find_result2(res, "cryptosubscription", PARAM_BOOL)->num;
    info->cryptolifetime =
        papi_find_result2(res, "cryptolifetime", PARAM_BOOL)->num;
    info->emailverified =
        papi_find_result2(res, "emailverified", PARAM_BOOL)->num;
    info->usedpublinkbranding =
        papi_find_result2(res, "usedpublinkbranding", PARAM_BOOL)->num;
    info->haspassword = papi_find_result2(res, "haspassword", PARAM_BOOL)->num;
    info->premium = papi_find_result2(res, "premium", PARAM_BOOL)->num;
    info->premiumlifetime =
        papi_find_result2(res, "premiumlifetime", PARAM_BOOL)->num;
    info->business = papi_find_result2(res, "business", PARAM_BOOL)->num;
    info->haspaidrelocation =
        papi_find_result2(res, "haspaidrelocation", PARAM_BOOL)->num;
    cres = papi_check_result2(res, "efh", PARAM_BOOL);
    if (cres)
      info->efh = cres->num;
    else
      info->efh = 0;
    cres = papi_check_result2(res, "premiumexpires", PARAM_NUM);
    if (cres)
      info->premiumexpires = cres->num;
    else
      info->premiumexpires = 0;
    info->trashrevretentiondays =
        papi_find_result2(res, "trashrevretentiondays", PARAM_NUM)->num;
    info->plan = papi_find_result2(res, "plan", PARAM_NUM)->num;
    info->publiclinkquota =
        papi_find_result2(res, "publiclinkquota", PARAM_NUM)->num;
    info->userid = papi_find_result2(res, "userid", PARAM_NUM)->num;
    info->quota = papi_find_result2(res, "quota", PARAM_NUM)->num;
    info->usedquota = papi_find_result2(res, "usedquota", PARAM_NUM)->num;
    info->freequota = papi_find_result2(res, "freequota", PARAM_NUM)->num;
    info->registered = papi_find_result2(res, "registered", PARAM_NUM)->num;
    free(res);
    return info;
  }

  return NULL;
}

int psync_ptools_create_backend_event(const char *category, const char *action,
                               const char *label, eventParams params,
                               char *err) {
  time_t rawtime;
  time(&rawtime);
  return ptools_create_backend_event(apiserver, category, action, label, psync_my_auth,
                              P_OS_ID, rawtime, &params, &err);
}

void psync_init_data_event_handler(void *ptr) { ptevent_init(ptr); }

// moved from pdiff
void psync_delete_cached_crypto_keys() { 
  psql_statement(
      "DELETE FROM setting WHERE id IN ('crypto_public_key', "
      "'crypto_private_key', 'crypto_private_iter', "
      "'crypto_private_salt', 'crypto_private_sha1', 'crypto_public_sha1')");
  if (psql_affected()) {
    pdbg_logf(D_NOTICE, "deleted cached crypto keys");
    pcryptofolder_cache_clean();
  }
  psql_statement("DELETE FROM cryptofolderkey");
  psql_statement("DELETE FROM cryptofilekey");
}
