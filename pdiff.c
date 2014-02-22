/* Copyright (c) 2013-2014 Anton Titov.
 * Copyright (c) 2013-2014 pCloud Ltd.
 * All rights reserved.
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *     * Neither the name of pCloud Ltd nor the
 *       names of its contributors may be used to endorse or promote products
 *       derived from this software without specific prior written permission.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL pCloud Ltd BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "pcompat.h"
#include "pdiff.h"
#include "pstatus.h"
#include "psettings.h"
#include "plibs.h"
#include "papi.h"
#include "ptimer.h"
#include "psyncer.h"
#include "ptasks.h"
#include "pfolder.h"
#include "psyncer.h"
#include "pdownload.h"

#define PSYNC_SQL_DOWNLOAD "synctype&"NTO_STR(PSYNC_DOWNLOAD_ONLY)"="NTO_STR(PSYNC_DOWNLOAD_ONLY)

static uint64_t used_quota=0, current_quota=0;
static psync_socket_t exceptionsockwrite=INVALID_SOCKET;

static psync_socket *get_connected_socket(){
  char *auth, *user, *pass;
  psync_socket *sock;
  binresult *res;
  psync_sql_res *q;
  uint64_t result, userid, luserid;
  int saveauth;
  auth=user=pass=NULL;
  while (1){
    psync_free(auth);
    psync_free(user);
    psync_free(pass);
    psync_wait_status(PSTATUS_TYPE_RUN, PSTATUS_RUN_RUN|PSTATUS_RUN_PAUSE);
    auth=psync_sql_cellstr("SELECT value FROM setting WHERE id='auth'");
    user=psync_sql_cellstr("SELECT value FROM setting WHERE id='user'");
    pass=psync_sql_cellstr("SELECT value FROM setting WHERE id='pass'");
    if (!auth && psync_my_auth[0])
      auth=psync_strdup(psync_my_auth);
    if (!user && psync_my_user)
      user=psync_strdup(psync_my_user);
    if (!pass && psync_my_pass)
      pass=psync_strdup(psync_my_pass);
    if (!auth && (!pass || !user)){
      psync_set_status(PSTATUS_TYPE_AUTH, PSTATUS_AUTH_REQUIRED);
      psync_wait_status(PSTATUS_TYPE_AUTH, PSTATUS_AUTH_PROVIDED);
      continue;
    }
    saveauth=psync_setting_get_bool(_PS(saveauth));
    sock=psync_api_connect(psync_setting_get_bool(_PS(usessl)));
    if (unlikely_log(!sock)){
      psync_set_status(PSTATUS_TYPE_ONLINE, PSTATUS_ONLINE_OFFLINE);
      psync_milisleep(PSYNC_SLEEP_BEFORE_RECONNECT);
      continue;
    }
    if (user && pass){
      binparam params[]={P_STR("timeformat", "timestamp"), 
                         P_STR("filtermeta", PSYNC_DIFF_FILTER_META),  
                         P_STR("username", user), 
                         P_STR("password", pass), 
                         P_BOOL("getauth", 1)};
      res=send_command(sock, "userinfo", params);
    }
    else {
      binparam params[]={P_STR("timeformat", "timestamp"), 
                         P_STR("filtermeta", PSYNC_DIFF_FILTER_META),  
                         P_STR("auth", auth),
                         P_BOOL("getauth", 1)};
      res=send_command(sock, "userinfo", params);
    }
    if (unlikely_log(!res)){
      psync_socket_close(sock);
      psync_set_status(PSTATUS_TYPE_ONLINE, PSTATUS_ONLINE_OFFLINE);
      psync_milisleep(PSYNC_SLEEP_BEFORE_RECONNECT);
      psync_api_conn_fail_inc();
      continue;
    }
    psync_api_conn_fail_reset();
    result=psync_find_result(res, "result", PARAM_NUM)->num;
    if (unlikely_log(result)){
      psync_socket_close(sock);
      psync_free(res);
      if (result==2000){
        psync_set_status(PSTATUS_TYPE_AUTH, PSTATUS_AUTH_BADLOGIN);
        psync_wait_status(PSTATUS_TYPE_AUTH, PSTATUS_AUTH_PROVIDED);
      }
      else if (result==4000)
        psync_milisleep(5*60*1000);
      else
        psync_milisleep(PSYNC_SLEEP_BEFORE_RECONNECT);
      continue;
    }
    psync_my_userid=userid=psync_find_result(res, "userid", PARAM_NUM)->num;
    current_quota=psync_find_result(res, "quota", PARAM_NUM)->num;
    luserid=psync_sql_cellint("SELECT value FROM setting WHERE id='userid'", 0);
    if (luserid){
      if (unlikely_log(luserid!=userid)){
        psync_socket_close(sock);
        psync_free(res);
        psync_set_status(PSTATUS_TYPE_AUTH, PSTATUS_AUTH_MISMATCH);
        psync_wait_status(PSTATUS_TYPE_AUTH, PSTATUS_AUTH_PROVIDED);
        continue;
      }
      strcpy(psync_my_auth, psync_find_result(res, "auth", PARAM_STR)->str);
      if (saveauth){
        q=psync_sql_prep_statement("REPLACE INTO setting (id, value) VALUES (?, ?)");
        psync_sql_bind_string(q, 1, "auth");
        psync_sql_bind_string(q, 2, psync_my_auth);
        psync_sql_run(q);
        psync_sql_free_result(q);
      }
    }
    else{
      strcpy(psync_my_auth, psync_find_result(res, "auth", PARAM_STR)->str);
      used_quota=0;
      q=psync_sql_prep_statement("REPLACE INTO setting (id, value) VALUES (?, ?)");
      psync_sql_bind_string(q, 1, "userid");
      psync_sql_bind_uint(q, 2, userid);
      psync_sql_run(q);
      psync_sql_bind_string(q, 1, "quota");
      psync_sql_bind_uint(q, 2, current_quota);
      psync_sql_run(q);
      psync_sql_bind_string(q, 1, "usedquota");
      psync_sql_bind_uint(q, 2, 0);
      psync_sql_run(q);
      result=psync_find_result(res, "premium", PARAM_BOOL)->num;
      psync_sql_bind_string(q, 1, "premium");
      psync_sql_bind_uint(q, 2, result);
      psync_sql_run(q);
      if (result)
        result=psync_find_result(res, "premiumexpires", PARAM_NUM)->num;
      else
        result=0;
      psync_sql_bind_string(q, 1, "premiumexpires");
      psync_sql_bind_uint(q, 2, result);
      psync_sql_run(q);
      result=psync_find_result(res, "emailverified", PARAM_BOOL)->num;
      psync_sql_bind_string(q, 1, "emailverified");
      psync_sql_bind_uint(q, 2, result);
      psync_sql_run(q);
      psync_sql_bind_string(q, 1, "username");
      psync_sql_bind_string(q, 2, psync_find_result(res, "email", PARAM_STR)->str);
      psync_sql_run(q);
      psync_sql_bind_string(q, 1, "language");
      psync_sql_bind_string(q, 2, psync_find_result(res, "language", PARAM_STR)->str);
      psync_sql_run(q);
      if (saveauth){
        psync_sql_bind_string(q, 1, "auth");
        psync_sql_bind_string(q, 2, psync_my_auth);
        psync_sql_run(q);
      }
      psync_sql_free_result(q);
    }
    pthread_mutex_lock(&psync_my_auth_mutex);
    psync_free(psync_my_pass);
    psync_my_pass=NULL;
    pthread_mutex_unlock(&psync_my_auth_mutex);
    if (saveauth)
      psync_sql_statement("DELETE FROM setting WHERE id='pass'");
    else
      psync_sql_statement("DELETE FROM setting WHERE id IN ('pass', 'auth')");
    psync_free(res);
    psync_free(auth);
    psync_free(user);
    psync_free(pass);
    return sock;
  }
}

static uint64_t get_permissions(const binresult *meta){
  return 
    (psync_find_result(meta, "canread", PARAM_BOOL)->num?PSYNC_PERM_READ:0)+
    (psync_find_result(meta, "canmodify", PARAM_BOOL)->num?PSYNC_PERM_MODIFY:0)+
    (psync_find_result(meta, "candelete", PARAM_BOOL)->num?PSYNC_PERM_DELETE:0)+
    (psync_find_result(meta, "cancreate", PARAM_BOOL)->num?PSYNC_PERM_CREATE:0);
}

static void process_createfolder(const binresult *entry){
  static psync_sql_res *st=NULL;
  psync_sql_res *res, *stmt, *stmt2;
  const binresult *meta, *name;
  uint64_t userid, perms;
  psync_uint_row row;
  psync_folderid_t parentfolderid, folderid, localfolderid;
//  char *localname;
  psync_syncid_t syncid;
  if (!entry){
    if (st){
      psync_sql_free_result(st);
      st=NULL;
    }
    return;
  }
  if (!st){
    st=psync_sql_prep_statement("REPLACE INTO folder (id, parentfolderid, userid, permissions, name, ctime, mtime) VALUES (?, ?, ?, ?, ?, ?, ?)");
    if (!st)
      return;
  }
  meta=psync_find_result(entry, "metadata", PARAM_HASH);
  if (psync_find_result(meta, "ismine", PARAM_BOOL)->num){
    userid=psync_my_userid;
    perms=PSYNC_PERM_ALL;
  }
  else{
    userid=psync_find_result(meta, "userid", PARAM_NUM)->num;
    perms=get_permissions(meta);
  }
  name=psync_find_result(meta, "name", PARAM_STR);
  folderid=psync_find_result(meta, "folderid", PARAM_NUM)->num;
  parentfolderid=psync_find_result(meta, "parentfolderid", PARAM_NUM)->num;
  psync_sql_bind_uint(st, 1, folderid);
  psync_sql_bind_uint(st, 2, parentfolderid);
  psync_sql_bind_uint(st, 3, userid);
  psync_sql_bind_uint(st, 4, perms);
  psync_sql_bind_lstring(st, 5, name->str, name->length);
  psync_sql_bind_uint(st, 6, psync_find_result(meta, "created", PARAM_NUM)->num);
  psync_sql_bind_uint(st, 7, psync_find_result(meta, "modified", PARAM_NUM)->num);
  psync_sql_run(st);
  if (psync_is_folder_in_downloadlist(parentfolderid) && !psync_is_name_to_ignore(name->str)){
    psync_add_folder_to_downloadlist(folderid);
    res=psync_sql_query("SELECT syncid, localfolderid, synctype FROM syncedfolder WHERE folderid=? AND "PSYNC_SQL_DOWNLOAD);
    psync_sql_bind_uint(res, 1, parentfolderid);
    stmt=psync_sql_prep_statement("INSERT OR IGNORE INTO syncedfolder (syncid, folderid, localfolderid, synctype) VALUES (?, ?, ?, ?)");
    while ((row=psync_sql_fetch_rowint(res))){
      syncid=row[0];
      localfolderid=psync_create_local_folder_in_db(syncid, folderid, row[1], name->str);
      psync_sql_bind_uint(stmt, 1, syncid);
      psync_sql_bind_uint(stmt, 2, folderid);
      psync_sql_bind_uint(stmt, 3, localfolderid);
      psync_sql_bind_uint(stmt, 4, row[2]);
      psync_sql_run(stmt);
      if (psync_sql_affected_rows()==1)
        psync_task_create_local_folder(syncid, folderid, localfolderid);
      else{
        stmt2=psync_sql_prep_statement("UPDATE syncedfolder SET folderid=? WHERE syncid=? AND localfolderid=?");
        psync_sql_bind_uint(stmt2, 1, folderid);
        psync_sql_bind_uint(stmt2, 2, syncid);
        psync_sql_bind_uint(stmt2, 3, localfolderid);
        psync_sql_run_free(stmt2);
      }
    }
    psync_sql_free_result(stmt);
    psync_sql_free_result(res);
  }
}

static void group_results_by_col(psync_full_result_int *restrict r1, psync_full_result_int *restrict r2, uint32_t col){
  psync_def_var_arr(buff, uint64_t, r1->cols);
  size_t rowsize;
  uint32_t i, j, l;
  l=0;
  rowsize=sizeof(r1->data[0])*r1->cols;
  assert(r1->cols==r2->cols);
  for (i=0; i<r1->rows; i++)
    for (j=0; j<r2->rows; j++)
      if (psync_get_result_cell(r1, i, col)==psync_get_result_cell(r2, j, col)){
        if (i!=l){
          memcpy(buff, r1->data+i*r1->cols, rowsize);
          memcpy(r1->data+i*r1->cols, r1->data+l*r1->cols, rowsize);
          memcpy(r1->data+l*r1->cols, buff, rowsize);
        }
        if (j!=l){
          memcpy(buff, r2->data+j*r2->cols, rowsize);
          memcpy(r2->data+j*r2->cols, r2->data+l*r2->cols, rowsize);
          memcpy(r2->data+l*r2->cols, buff, rowsize);
        }
        l++;
      }
}

static void del_synced_folder_rec(psync_folderid_t folderid, psync_syncid_t syncid){
  psync_sql_res *res;
  psync_uint_row row;
  res=psync_sql_prep_statement("DELETE FROM syncedfolder WHERE folderid=? AND syncid=?");
  psync_sql_bind_uint(res, 1, folderid);
  psync_sql_bind_uint(res, 2, syncid);
  psync_sql_run_free(res);
  res=psync_sql_query("SELECT id FROM folder WHERE parentfolderid=?");
  psync_sql_bind_uint(res, 1, folderid);
  while ((row=psync_sql_fetch_rowint(res)))
    del_synced_folder_rec(row[0], syncid);
  psync_sql_free_result(res);
}

static void process_modifyfolder(const binresult *entry){
  static psync_sql_res *st=NULL;
  psync_sql_res *res;
  psync_full_result_int *fres1, *fres2;
  const binresult *meta, *name;
  uint64_t userid, perms;
  psync_variant_row vrow;
  psync_uint_row row;
  psync_folderid_t parentfolderid, folderid, oldparentfolderid, localfolderid;
  char *oldname;
  psync_syncid_t syncid;
  uint32_t i, cnt;
  int oldsync, newsync;
  if (!entry){
    process_createfolder(NULL);
    if (st){
      psync_sql_free_result(st);
      st=NULL;
    }
    return;
  }
  if (!st){
    st=psync_sql_prep_statement("REPLACE INTO folder (id, parentfolderid, userid, permissions, name, ctime, mtime) VALUES (?, ?, ?, ?, ?, ?, ?)");
    if (!st)
      return;
  }
  meta=psync_find_result(entry, "metadata", PARAM_HASH);
  if (psync_find_result(meta, "ismine", PARAM_BOOL)->num){
    userid=psync_my_userid;
    perms=PSYNC_PERM_ALL;
  }
  else{
    userid=psync_find_result(meta, "userid", PARAM_NUM)->num;
    perms=get_permissions(meta);
  }
  name=psync_find_result(meta, "name", PARAM_STR);
  folderid=psync_find_result(meta, "folderid", PARAM_NUM)->num;
  parentfolderid=psync_find_result(meta, "parentfolderid", PARAM_NUM)->num;
  res=psync_sql_query("SELECT parentfolderid, name FROM folder WHERE id=?");
  psync_sql_bind_uint(res, 1, folderid);
  vrow=psync_sql_fetch_row(res);
  if (likely(vrow)){
    oldparentfolderid=psync_get_number(vrow[0]);
    oldname=psync_dup_string(vrow[1]);
  }
  else{
    debug(D_ERROR, "got modify for non-existing folder %lu (%s), processing as create", (unsigned long)folderid, name->str);
    psync_sql_free_result(res);
    process_createfolder(entry);
    return;
  }
  psync_sql_free_result(res);
  psync_sql_bind_uint(st, 1, folderid);
  psync_sql_bind_uint(st, 2, parentfolderid);
  psync_sql_bind_uint(st, 3, userid);
  psync_sql_bind_uint(st, 4, perms);
  psync_sql_bind_lstring(st, 5, name->str, name->length);
  psync_sql_bind_uint(st, 6, psync_find_result(meta, "created", PARAM_NUM)->num);
  psync_sql_bind_uint(st, 7, psync_find_result(meta, "modified", PARAM_NUM)->num);
  psync_sql_run(st);
  /* We should check if oldparentfolderid is in downloadlist, not folderid. If parentfolderid is not in and
   * folderid is in, it means that folder that is a "root" of a syncid is modified, we do not care about that.
   */
  oldsync=psync_is_folder_in_downloadlist(oldparentfolderid);
  if (oldparentfolderid==parentfolderid)
    newsync=oldsync;
  else
    newsync=psync_is_folder_in_downloadlist(parentfolderid);
  if ((oldsync || newsync) && (oldparentfolderid!=parentfolderid || strcmp(name->str, oldname))){
    if (!oldsync) 
      psync_add_folder_to_downloadlist(folderid);
    else if (!newsync){
      res=psync_sql_query("SELECT id FROM syncfolder WHERE folderid=? AND "PSYNC_SQL_DOWNLOAD);
      psync_sql_bind_uint(res, 1, folderid);
      if (!psync_sql_fetch_rowint(res))
        psync_del_folder_from_downloadlist(folderid);
      psync_sql_free_result(res);
    }
    res=psync_sql_query("SELECT syncid, localfolderid, synctype FROM syncedfolder WHERE folderid=? AND "PSYNC_SQL_DOWNLOAD);
    psync_sql_bind_uint(res, 1, oldparentfolderid);
    fres1=psync_sql_fetchall_int(res);
    res=psync_sql_query("SELECT syncid, localfolderid, synctype FROM syncedfolder WHERE folderid=? AND "PSYNC_SQL_DOWNLOAD);
    psync_sql_bind_uint(res, 1, parentfolderid);
    fres2=psync_sql_fetchall_int(res);
    if (psync_is_name_to_ignore(name->str))
      fres2->rows=0;
    group_results_by_col(fres1, fres2, 0);
    cnt=fres2->rows>fres1->rows?fres1->rows:fres2->rows;
    for (i=0; i<cnt; i++){
      res=psync_sql_query("SELECT localfolderid FROM syncedfolder WHERE folderid=? AND syncid=?");
      psync_sql_bind_uint(res, 1, folderid);
      psync_sql_bind_uint(res, 2, psync_get_result_cell(fres1, i, 0));
      row=psync_sql_fetch_rowint(res);
      if (unlikely(!row)){
        debug(D_ERROR, "could not find local folder of folderid %lu", (unsigned long)folderid);
        psync_sql_free_result(res);
        continue;
      }
      localfolderid=row[0];
      psync_sql_free_result(res);
      psync_task_rename_local_folder(psync_get_result_cell(fres2, i, 0), folderid, localfolderid, psync_get_result_cell(fres2, i, 1), name->str);
      psync_increase_local_folder_taskcnt(localfolderid);
      if (psync_get_result_cell(fres2, i, 0)!=psync_get_result_cell(fres1, i, 0)){
        res=psync_sql_prep_statement("UPDATE syncedfolder SET syncid=?, synctype=? WHERE syncid=? AND folderid=?");
        psync_sql_bind_uint(res, 1, psync_get_result_cell(fres2, i, 0));
        psync_sql_bind_uint(res, 2, psync_get_result_cell(fres2, i, 2));
        psync_sql_bind_uint(res, 3, psync_get_result_cell(fres1, i, 0));
        psync_sql_bind_uint(res, 4, folderid);
        psync_sql_run_free(res);
      }
    }
    for (/*i is already=cnt*/; i<fres1->rows; i++){
      syncid=psync_get_result_cell(fres1, i, 0);
      res=psync_sql_query("SELECT localfolderid FROM syncedfolder WHERE folderid=? AND syncid=?");
      psync_sql_bind_uint(res, 1, folderid);
      psync_sql_bind_uint(res, 2, syncid);
      row=psync_sql_fetch_rowint(res);
      if (unlikely(!row)){
        debug(D_ERROR, "could not find local folder of folderid %lu", (unsigned long)folderid);
        psync_sql_free_result(res);
        continue;
      }
      localfolderid=row[0];
      psync_sql_free_result(res);
      del_synced_folder_rec(folderid, syncid);
      psync_task_delete_local_folder_recursive(syncid, folderid, localfolderid);
      psync_increase_local_folder_taskcnt(localfolderid);
    }
    for (/*i is already=cnt*/; i<fres2->rows; i++){
      syncid=psync_get_result_cell(fres2, i, 0);
      localfolderid=psync_create_local_folder_in_db(syncid, folderid, psync_get_result_cell(fres2, i, 1), name->str);
      psync_task_create_local_folder(syncid, folderid, localfolderid);
      psync_add_folder_for_downloadsync(syncid, psync_get_result_cell(fres2, i, 2), folderid, localfolderid);
    }
    psync_free(fres1);
    psync_free(fres2);
  }
  psync_free(oldname);
}

static void process_deletefolder(const binresult *entry){
  static psync_sql_res *st=NULL;
  psync_sql_res *res, *stmt;
  char *path;
  psync_folderid_t folderid;
  psync_uint_row row;
  if (!entry){
    if (st){
      psync_sql_free_result(st);
      st=NULL;
    }
    return;
  }
  if (!st){
    st=psync_sql_prep_statement("DELETE FROM folder WHERE id=?");
    if (!st)
      return;
  }
  folderid=psync_find_result(psync_find_result(entry, "metadata", PARAM_HASH), "folderid", PARAM_NUM)->num;
  if (psync_is_folder_in_downloadlist(folderid)){
    psync_del_folder_from_downloadlist(folderid);
    res=psync_sql_query("SELECT syncid, localfolderid FROM syncedfolder WHERE folderid=?");
    psync_sql_bind_uint(res, 1, folderid);
    while ((row=psync_sql_fetch_rowint(res))){
      stmt=psync_sql_prep_statement("DELETE FROM syncedfolder WHERE syncid=? AND folderid=?");
      psync_sql_bind_uint(stmt, 1, row[0]);
      psync_sql_bind_uint(stmt, 2, folderid);
      psync_sql_run_free(stmt);
      if (psync_sql_affected_rows()==1){
        path=psync_get_path_by_folderid(folderid, NULL);
        psync_task_delete_local_folder(row[0], folderid, row[1], path);
        psync_free(path);
        psync_increase_local_folder_taskcnt(row[1]);
      }
    }
    psync_sql_free_result(res);
  }
  psync_sql_bind_uint(st, 1, folderid);
  psync_sql_run(st);
}

static void process_createfile(const binresult *entry){
  static psync_sql_res *st=NULL;
  const binresult *meta, *name;
  psync_sql_res *res;
  psync_folderid_t parentfolderid;
  psync_fileid_t fileid;
  uint64_t size, userid;
  psync_uint_row row;
  if (!entry){
    if (st){
      psync_sql_free_result(st);
      st=NULL;
    }
    return;
  }
  if (!st)
    st=psync_sql_prep_statement("REPLACE INTO file (id, parentfolderid, userid, size, hash, name, ctime, mtime) VALUES (?, ?, ?, ?, ?, ?, ?, ?)");
  meta=psync_find_result(entry, "metadata", PARAM_HASH);
  size=psync_find_result(meta, "size", PARAM_NUM)->num;
  fileid=psync_find_result(meta, "fileid", PARAM_NUM)->num;
  parentfolderid=psync_find_result(meta, "parentfolderid", PARAM_NUM)->num;
  if (psync_find_result(meta, "ismine", PARAM_BOOL)->num){
    userid=psync_my_userid;
    used_quota+=size;
  }
  else
    userid=psync_find_result(meta, "userid", PARAM_NUM)->num;
  name=psync_find_result(meta, "name", PARAM_STR);
  psync_sql_bind_uint(st, 1, fileid);
  psync_sql_bind_uint(st, 2, parentfolderid);
  psync_sql_bind_uint(st, 3, userid);
  psync_sql_bind_uint(st, 4, size);
  psync_sql_bind_uint(st, 5, psync_find_result(meta, "hash", PARAM_NUM)->num);
  psync_sql_bind_lstring(st, 6, name->str, name->length);
  psync_sql_bind_uint(st, 7, psync_find_result(meta, "created", PARAM_NUM)->num);
  psync_sql_bind_uint(st, 8, psync_find_result(meta, "modified", PARAM_NUM)->num);
  psync_sql_run(st);
  if (psync_is_folder_in_downloadlist(parentfolderid) && !psync_is_name_to_ignore(name->str)){
    res=psync_sql_query("SELECT syncid, localfolderid FROM syncedfolder WHERE folderid=? AND "PSYNC_SQL_DOWNLOAD);
    psync_sql_bind_uint(res, 1, parentfolderid);
    while ((row=psync_sql_fetch_rowint(res)))
      psync_task_download_file(row[0], fileid, row[1], name->str);
    psync_sql_free_result(res);
  }
}

static void process_modifyfile(const binresult *entry){
  static psync_sql_res *sq=NULL, *st=NULL;
  psync_sql_res *res;
  psync_full_result_int *fres1, *fres2;
  const binresult *meta, *name;
  const char *oldname;
  size_t oldnamelen;
  psync_variant_row row;
  psync_fileid_t fileid;
  psync_folderid_t parentfolderid, oldparentfolderid;
  uint64_t size, userid, hash, oldsize;
  int oldsync, newsync, needdownload, needrename;
  uint32_t cnt, i;
  if (!entry){
    if (sq){
      psync_sql_free_result(sq);
      sq=NULL;
    }
    if (st){
      psync_sql_free_result(st);
      st=NULL;
    }
    process_createfile(NULL);
    return;
  }
  meta=psync_find_result(entry, "metadata", PARAM_HASH);
  fileid=psync_find_result(meta, "fileid", PARAM_NUM)->num;
  name=psync_find_result(meta, "name", PARAM_STR);
  if (sq)
    psync_sql_reset(sq);
  else
    sq=psync_sql_query("SELECT parentfolderid, userid, size, hash, name FROM file WHERE id=?");
  psync_sql_bind_uint(sq, 1, fileid);
  row=psync_sql_fetch_row(sq);
  if (!row){
    debug(D_ERROR, "got modify for non-existing file %lu (%s), processing as create", (unsigned long)fileid, name->str);
    process_createfile(entry);
    return;
  }
  oldsize=psync_get_number(row[2]);
  if (psync_get_number(row[1])==psync_my_userid)
    used_quota-=oldsize;
  if (!st)
    st=psync_sql_prep_statement("REPLACE INTO file (id, parentfolderid, userid, size, hash, name, ctime, mtime) VALUES (?, ?, ?, ?, ?, ?, ?, ?)");
  size=psync_find_result(meta, "size", PARAM_NUM)->num;
  parentfolderid=psync_find_result(meta, "parentfolderid", PARAM_NUM)->num;
  hash=psync_find_result(meta, "hash", PARAM_NUM)->num;
  if (psync_find_result(meta, "ismine", PARAM_BOOL)->num){
    userid=psync_my_userid;
    used_quota+=size;
  }
  else
    userid=psync_find_result(meta, "userid", PARAM_NUM)->num;
  psync_sql_bind_uint(st, 1, fileid);
  psync_sql_bind_uint(st, 2, parentfolderid);
  psync_sql_bind_uint(st, 3, userid);
  psync_sql_bind_uint(st, 4, size);
  psync_sql_bind_uint(st, 5, hash);
  psync_sql_bind_lstring(st, 6, name->str, name->length);
  psync_sql_bind_uint(st, 7, psync_find_result(meta, "created", PARAM_NUM)->num);
  psync_sql_bind_uint(st, 8, psync_find_result(meta, "modified", PARAM_NUM)->num);
  psync_sql_run(st);
  oldparentfolderid=psync_get_number(row[0]);
  oldsync=psync_is_folder_in_downloadlist(oldparentfolderid);
  if (oldparentfolderid==parentfolderid)
    newsync=oldsync;
  else
    newsync=psync_is_folder_in_downloadlist(parentfolderid);
  if (oldsync || newsync){
    if (psync_is_name_to_ignore(name->str)){
      char *path;
      psync_delete_download_tasks_for_file(fileid);
      path=psync_get_path_by_fileid(fileid, NULL);
      psync_task_delete_local_file(fileid, path);
      psync_free(path);
      return;
    }
    needdownload=hash!=psync_get_number(row[3]) || size!=oldsize;
    oldname=psync_get_lstring(row[4], &oldnamelen);
    if (needdownload)
      psync_delete_download_tasks_for_file(fileid);
    needrename=oldparentfolderid!=parentfolderid || name->length!=oldnamelen || memcmp(name->str, oldname, oldnamelen);
    res=psync_sql_query("SELECT syncid, localfolderid, synctype FROM syncedfolder WHERE folderid=? AND "PSYNC_SQL_DOWNLOAD);
    psync_sql_bind_uint(res, 1, oldparentfolderid);
    fres1=psync_sql_fetchall_int(res);
    res=psync_sql_query("SELECT syncid, localfolderid, synctype FROM syncedfolder WHERE folderid=? AND "PSYNC_SQL_DOWNLOAD);
    psync_sql_bind_uint(res, 1, parentfolderid);
    fres2=psync_sql_fetchall_int(res);
    group_results_by_col(fres1, fres2, 0);
    cnt=fres2->rows>fres1->rows?fres1->rows:fres2->rows;
    for (i=0; i<cnt; i++){
      if (needrename)
        psync_task_rename_local_file(psync_get_result_cell(fres1, i, 0), psync_get_result_cell(fres2, i, 0), fileid,
                                     psync_get_result_cell(fres1, i, 1), psync_get_result_cell(fres2, i, 1),
                                     name->str);
      if (needdownload)
        psync_task_download_file(psync_get_result_cell(fres2, i, 0), fileid, psync_get_result_cell(fres2, i, 1), name->str);
    }
    for (/*i is already=cnt*/; i<fres2->rows; i++)
      psync_task_download_file(psync_get_result_cell(fres2, i, 0), fileid, psync_get_result_cell(fres2, i, 1), name->str);
    for (/*i is already=cnt*/; i<fres1->rows; i++)
      psync_task_delete_local_file_syncid(psync_get_result_cell(fres1, i, 0), fileid);
    psync_free(fres1);
    psync_free(fres2);
  }
}

static void process_deletefile(const binresult *entry){
  static psync_sql_res *st=NULL;
  const binresult *meta;
  char *path;
  psync_fileid_t fileid;
  if (!entry){
    if (st){
      psync_sql_free_result(st);
      st=NULL;
    }
    return;
  }
  if (!st){
    st=psync_sql_prep_statement("DELETE FROM file WHERE id=?");
    if (!st)
      return;
  }
  meta=psync_find_result(entry, "metadata", PARAM_HASH);
  fileid=psync_find_result(meta, "fileid", PARAM_NUM)->num;
  if (psync_is_folder_in_downloadlist(psync_find_result(meta, "parentfolderid", PARAM_NUM)->num)){
    psync_delete_download_tasks_for_file(fileid);
    path=psync_get_path_by_fileid(fileid, NULL);
    psync_task_delete_local_file(fileid, path);
    psync_free(path);
  }
  psync_sql_bind_uint(st, 1, fileid);
  psync_sql_run(st);
  if (psync_find_result(meta, "ismine", PARAM_BOOL)->num)
    used_quota-=psync_find_result(meta, "size", PARAM_NUM)->num;
}

#define FN(n) {process_##n, #n, sizeof(#n)-1, 0}

static struct {
  void (*process)(const binresult *);
  const char *name;
  uint32_t len;
  uint8_t used;
} event_list[] = {
  FN(createfolder),
  FN(modifyfolder),
  FN(deletefolder),
  FN(createfile),
  FN(modifyfile),
  FN(deletefile)
};

#define event_list_size ARRAY_SIZE(event_list)

static uint64_t process_entries(const binresult *entries, uint64_t newdiffid){
  const binresult *entry, *etype;
  uint32_t i, j;
  psync_sql_start_transaction();
  for (i=0; i<entries->length; i++){
    entry=entries->array[i];
    etype=psync_find_result(entry, "event", PARAM_STR);
    for (j=0; j<event_list_size; j++)
      if (etype->length==event_list[j].len && !memcmp(etype->str, event_list[j].name, etype->length)){
        event_list[j].process(entry);
        event_list[j].used=1;
      }
  }
  for (j=0; j<event_list_size; j++)
    if (event_list[j].used)
      event_list[j].process(NULL);
  psync_set_uint_value("diffid", newdiffid);
  psync_set_uint_value("usedquota", used_quota);
  psync_sql_commit_transaction();
  used_quota=psync_sql_cellint("SELECT value FROM setting WHERE id='usedquota'", 0);
  return psync_sql_cellint("SELECT value FROM setting WHERE id='diffid'", 0);
}

static void check_overquota(){
  static int lisover=0;
  int isover=(used_quota>=current_quota);
  if (isover!=lisover){
    lisover=isover;
    if (isover)
      psync_set_status(PSTATUS_TYPE_ACCFULL, PSTATUS_ACCFULL_OVERQUOTA);
    else
      psync_set_status(PSTATUS_TYPE_ACCFULL, PSTATUS_ACCFULL_QUOTAOK);
  }
}

static void delete_delayed_sync(uint64_t id){
  psync_sql_res *res;
  res=psync_sql_prep_statement("DELETE FROM syncfolderdelayed WHERE id=?");
  psync_sql_bind_uint(res, 1, id);
  psync_sql_run_free(res);
}

static void check_delayed_syncs(){
  psync_stat_t st;
  psync_sql_res *res, *stmt;
  psync_variant_row row;
  const char *localpath, *remotepath;
  uint64_t id, synctype;
  int64_t syncid;
  psync_folderid_t folderid;
  int unsigned md;
  res=psync_sql_query("SELECT id, localpath, remotepath, synctype FROM syncfolderdelayed");
  while ((row=psync_sql_fetch_row(res))){
    id=psync_get_number(row[0]);
    localpath=psync_get_string(row[1]);
    remotepath=psync_get_string(row[2]);
    synctype=psync_get_number(row[3]);
    if (synctype&PSYNC_DOWNLOAD_ONLY)
      md=7;
    else
      md=5;
    if (unlikely_log(psync_stat(localpath, &st)) || unlikely_log(!psync_stat_isfolder(&st)) || unlikely_log(!psync_stat_mode_ok(&st, md))){
      debug(D_WARNING, "ignoring delayed sync id %"P_PRI_U64" for local path %s", id, localpath);
      delete_delayed_sync(id);
      continue;
    }
    folderid=psync_get_folderid_by_path_or_create(remotepath);
    if (unlikely_log(folderid==PSYNC_INVALID_FOLDERID)){
      if (psync_error!=PERROR_OFFLINE)
        delete_delayed_sync(id);
      continue;        
    }
    psync_sql_start_transaction();
    stmt=psync_sql_prep_statement("INSERT OR IGNORE INTO syncfolder (folderid, localpath, synctype, flags) VALUES (?, ?, ?, 0)");
    psync_sql_bind_uint(stmt, 1, folderid);
    psync_sql_bind_string(stmt, 2, localpath);
    psync_sql_bind_uint(stmt, 3, synctype);
    psync_sql_run(stmt);
    if (likely_log(psync_sql_affected_rows()))
      syncid=psync_sql_insertid();
    else
      syncid=-1;
    psync_sql_free_result(stmt);
    delete_delayed_sync(id);
    if (!psync_sql_commit_transaction() && syncid!=-1)
      psync_syncer_new(syncid);
  }
  psync_sql_free_result(res);
}

static void diff_exception_handler(){
  debug(D_NOTICE, "got exception");
  if (likely(exceptionsockwrite!=INVALID_SOCKET))
    psync_pipe_write(exceptionsockwrite, "e", 1);
}

static psync_socket_t setup_exeptions(){
  psync_socket_t pfds[2];
  if (psync_pipe(pfds)==SOCKET_ERROR)
    return INVALID_SOCKET;
  exceptionsockwrite=pfds[1];
  psync_timer_exception_handler(diff_exception_handler);
  return pfds[0];
}

static void handle_exception(psync_socket **sock, char ex){
  if (ex=='r' || 
      psync_status_get(PSTATUS_TYPE_RUN)==PSTATUS_RUN_STOP || 
      psync_status_get(PSTATUS_TYPE_AUTH)!=PSTATUS_AUTH_PROVIDED ||
      psync_setting_get_bool(_PS(usessl))!=psync_socket_isssl(*sock)){
    psync_socket_close(*sock);
    *sock=get_connected_socket();
    psync_set_status(PSTATUS_TYPE_ONLINE, PSTATUS_ONLINE_ONLINE);
  }
  else if (ex=='e'){
    binparam diffparams[]={P_STR("id", "ignore")};
    if (!send_command_no_res(*sock, "nop", diffparams) || psync_select_in(&(*sock)->sock, 1, PSYNC_SOCK_TIMEOUT_ON_EXCEPTION*1000)!=0){
      psync_socket_close(*sock);
      *sock=get_connected_socket();
      psync_set_status(PSTATUS_TYPE_ONLINE, PSTATUS_ONLINE_ONLINE);
    }
    else
      (*sock)->pending=1;
  }
}

static int send_diff_command(psync_socket *sock, uint64_t diffid){
  binparam diffparams[]={P_STR("timeformat", "timestamp"), P_NUM("limit", PSYNC_DIFF_LIMIT), P_NUM("diffid", diffid), P_BOOL("block", 1)};
  return send_command_no_res(sock, "diff", diffparams)?0:-1;
}

static void psync_diff_thread(){
  psync_socket *sock;
  binresult *res;
  const binresult *entries;
  uint64_t diffid, newdiffid, result;
  psync_socket_t exceptionsock, socks[2];
  int sel;
  char ex;
  psync_set_status(PSTATUS_TYPE_ONLINE, PSTATUS_ONLINE_CONNECTING);
  psync_milisleep(2);
restart:
  sock=get_connected_socket();
  psync_set_status(PSTATUS_TYPE_ONLINE, PSTATUS_ONLINE_SCANNING);
  diffid=psync_sql_cellint("SELECT value FROM setting WHERE id='diffid'", 0);
  used_quota=psync_sql_cellint("SELECT value FROM setting WHERE id='usedquota'", 0);
  do{
    binparam diffparams[]={P_STR("timeformat", "timestamp"), P_NUM("limit", PSYNC_DIFF_LIMIT), P_NUM("diffid", diffid)};
    if (!psync_do_run)
      break;
    res=send_command(sock, "diff", diffparams);
    if (!res){
      psync_socket_close(sock);
      goto restart;
    }
    result=psync_find_result(res, "result", PARAM_NUM)->num;
    if (unlikely(result)){
      debug(D_ERROR, "diff returned error %u: %s", (unsigned int)result, psync_find_result(res, "error", PARAM_STR)->str);
      psync_free(res);
      psync_socket_close(sock);
      psync_milisleep(PSYNC_SLEEP_BEFORE_RECONNECT);
      goto restart;
    }
    entries=psync_find_result(res, "entries", PARAM_ARRAY);
    if (entries->length){
      newdiffid=psync_find_result(res, "diffid", PARAM_NUM)->num;
      diffid=process_entries(entries, newdiffid);
    }
    result=entries->length;
    psync_free(res);
  } while (result);
  check_overquota();
  check_delayed_syncs();
  psync_set_status(PSTATUS_TYPE_ONLINE, PSTATUS_ONLINE_ONLINE);
  exceptionsock=setup_exeptions();
  if (unlikely(exceptionsock==INVALID_SOCKET)){
    debug(D_ERROR, "could not create pipe");
    psync_socket_close(sock);
    return;
  }
  socks[0]=exceptionsock;
  socks[1]=sock->sock;
  send_diff_command(sock, diffid);
  while (psync_do_run){
    if (psync_socket_pendingdata(sock))
      sel=1;
    else
      sel=psync_select_in(socks, 2, -1);
    if (sel==0){
      if (!psync_do_run)
        break;
      if (psync_pipe_read(exceptionsock, &ex, 1)!=1)
        continue;
      handle_exception(&sock, ex);
      socks[1]=sock->sock;
    }
    else if (sel==1){
      sock->pending=1;
      res=get_result(sock);
      if (unlikely_log(!res)){
        psync_timer_notify_exception();
        handle_exception(&sock, 'r');
        socks[1]=sock->sock;
        continue;
      }
      result=psync_find_result(res, "result", PARAM_NUM)->num;
      if (unlikely(result)){
        debug(D_ERROR, "diff returned error %u: %s", (unsigned int)result, psync_find_result(res, "error", PARAM_STR)->str);
        psync_free(res);
        handle_exception(&sock, 'r');
        socks[1]=sock->sock;
        continue;
      }
      entries=psync_check_result(res, "entries", PARAM_ARRAY);
      if (entries){
        if (entries->length){
          newdiffid=psync_find_result(res, "diffid", PARAM_NUM)->num;
          diffid=process_entries(entries, newdiffid);
        }
        send_diff_command(sock, diffid);
      }
      psync_free(res);
    }
  }
  psync_socket_close(sock);
  psync_pipe_close(exceptionsock);
  psync_pipe_close(exceptionsockwrite);
}

void psync_diff_init(){
  psync_run_thread(psync_diff_thread);
}
