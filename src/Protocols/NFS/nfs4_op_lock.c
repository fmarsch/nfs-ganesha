/*
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 * Copyright CEA/DAM/DIF  (2008)
 * contributeur : Philippe DENIEL   philippe.deniel@cea.fr
 *                Thomas LEIBOVICI  thomas.leibovici@cea.fr
 *
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 3 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 * ---------------------------------------
 */

/**
 * \file    nfs4_op_lock.c
 * \author  $Author: deniel $
 * \date    $Date: 2005/11/28 17:02:50 $
 * \version $Revision: 1.8 $
 * \brief   Routines used for managing the NFS4 COMPOUND functions.
 *
 * nfs4_op_lock.c : Routines used for managing the NFS4 COMPOUND functions.
 *
 *
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#ifdef _SOLARIS
#include "solaris_port.h"
#endif

#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include "HashData.h"
#include "HashTable.h"
#include "rpc.h"
#include "log_macros.h"
#include "stuff_alloc.h"
#include "nfs4.h"
#include "nfs_core.h"
#include "sal_functions.h"
#include "nfs_proto_functions.h"

/**
 *
 * nfs4_op_lock: The NFS4_OP_LOCK operation.
 *
 * This function implements the NFS4_OP_LOCK operation.
 *
 * @param op    [IN]    pointer to nfs4_op arguments
 * @param data  [INOUT] Pointer to the compound request's data
 * @param resp  [IN]    Pointer to nfs4_op results
 *
 * @return NFS4_OK if successfull, other values show an error.
 *
 * @see all the nfs4_op_<*> function
 * @see nfs4_Compound
 *
 */

#define arg_LOCK4 op->nfs_argop4_u.oplock
#define res_LOCK4 resp->nfs_resop4_u.oplock

extern char all_zero[];
extern char all_one[12];

int nfs4_op_lock(struct nfs_argop4 *op, compound_data_t * data, struct nfs_resop4 *resp)
{
  char __attribute__ ((__unused__)) funcname[] = "nfs4_op_lock";

#ifndef _WITH_NFSV4_LOCKS
  /* Lock are not supported */
  resp->resop = NFS4_OP_LOCK;
  res_LOCK4.status = NFS4ERR_LOCK_NOTSUPP;

  return res_LOCK4.status;
#else

  state_status_t            state_status;
  state_data_t              candidate_data;
  state_type_t              candidate_type;
  int                       rc = 0;
  state_t                 * file_state = NULL;
  state_t                 * pstate_exists = NULL;
  state_t                 * pstate_open = NULL;
  state_t                 * pstate_found = NULL;
  state_t                 * pstate_previous_iterate = NULL;
  state_t                 * pstate_found_iterate = NULL;
  state_owner_t           * powner = NULL;
  state_owner_t           * popen_owner = NULL;
  state_owner_t           * powner_exists = NULL;
  uint64_t                  a, b, a1, b1;
  unsigned int              overlap = FALSE;
  state_nfs4_owner_name_t   owner_name;
  nfs_client_id_t           nfs_client_id;
  state_lock_desc_t         lock_desc;
  state_blocking_t          blocking;

  /* Initialize to sane starting values */
  resp->resop = NFS4_OP_LOCK;

  /* If there is no FH */
  if(nfs4_Is_Fh_Empty(&(data->currentFH)))
    {
      res_LOCK4.status = NFS4ERR_NOFILEHANDLE;
      return res_LOCK4.status;
    }

  /* If the filehandle is invalid */
  if(nfs4_Is_Fh_Invalid(&(data->currentFH)))
    {
      res_LOCK4.status = NFS4ERR_BADHANDLE;
      return res_LOCK4.status;
    }

  /* Tests if the Filehandle is expired (for volatile filehandle) */
  if(nfs4_Is_Fh_Expired(&(data->currentFH)))
    {
      res_LOCK4.status = NFS4ERR_FHEXPIRED;
      return res_LOCK4.status;
    }

  /* Commit is done only on a file */
  if(data->current_filetype != REGULAR_FILE)
    {
      /* Type of the entry is not correct */
      switch (data->current_filetype)
        {
        case DIR_BEGINNING:
        case DIR_CONTINUE:
          res_LOCK4.status = NFS4ERR_ISDIR;
          break;
        default:
          res_LOCK4.status = NFS4ERR_INVAL;
          break;
        }
    }

  /* Lock length should not be 0 */
  if(arg_LOCK4.length == 0LL)
    {
      res_LOCK4.status = NFS4ERR_INVAL;
      return res_LOCK4.status;
    }

  /* Convert lock parameters to internal types */
  switch(arg_LOCK4.locktype)
    {
      case READ_LT:
        lock_desc.sld_type = STATE_LOCK_R;
        blocking           = STATE_NON_BLOCKING;
        break;

      case WRITE_LT:
        lock_desc.sld_type = STATE_LOCK_W;
        blocking           = STATE_NON_BLOCKING;
        break;

      case READW_LT:
        lock_desc.sld_type = STATE_LOCK_R;
        blocking           = STATE_NFSV4_BLOCKING;
        break;

      case WRITEW_LT:
        lock_desc.sld_type = STATE_LOCK_W;
        blocking           = STATE_NFSV4_BLOCKING;
        break;
    }

  lock_desc.sld_offset = arg_LOCK4.offset;

  if(arg_LOCK4.length != STATE_LOCK_OFFSET_EOF)
    lock_desc.sld_length = arg_LOCK4.length;
  else
    lock_desc.sld_length = 0;

  /* Check for range overflow.
   * Comparing beyond 2^64 is not possible int 64 bits precision,
   * but off+len > 2^64-1 is equivalent to len > 2^64-1 - off
   */
  if(lock_desc.sld_length > (STATE_LOCK_OFFSET_EOF - lock_desc.sld_offset))
    {
      res_LOCK4.status = NFS4ERR_INVAL;
      return res_LOCK4.status;
    }

  switch (arg_LOCK4.locker.new_lock_owner)
    {
    case TRUE:
      /* New lock owner
       * Find the open owner
       */
      if(state_get(arg_LOCK4.locker.locker4_u.open_owner.open_stateid.other,
                   &pstate_open,
                   data->pclient, &state_status) != STATE_SUCCESS)
        {
          res_LOCK4.status = NFS4ERR_STALE_STATEID;
          return res_LOCK4.status;
        }

      popen_owner = pstate_open->state_powner;

      break;

    case FALSE:
      /* Existing lock owner
       * Find the lock stateid
       * From that, get the open_owner
       */
      if(state_get(arg_LOCK4.locker.locker4_u.lock_owner.lock_stateid.other,
                   &pstate_exists,
                   data->pclient, &state_status) != STATE_SUCCESS)
        {
          /* Handle the case where all-0 stateid is used */
          if(!
             (!memcmp((char *)all_zero,
                      arg_LOCK4.locker.locker4_u.lock_owner.lock_stateid.other,
                      12)
              && arg_LOCK4.locker.locker4_u.lock_owner.lock_stateid.seqid == 0))
            {
              if(state_status == STATE_NOT_FOUND)
                res_LOCK4.status = NFS4ERR_STALE_STATEID;
              else
                res_LOCK4.status = NFS4ERR_INVAL;

              return res_LOCK4.status;
            }
        }

      if(pstate_exists != NULL)
        {
          /* Get the old lockowner. We can do the following 'cast', in NFSv4 lock_owner4 and open_owner4
           * are different types but with the same definition*/
          powner        = pstate_exists->state_powner;
          powner_exists = pstate_exists->state_powner;
          popen_owner   = pstate_exists->state_powner->so_owner.so_nfs4_owner.so_related_owner;
        }

      break;
    }                           /* switch( arg_LOCK4.locker.new_lock_owner ) */

  /* Check for conflicts with previously obtained states */
  /* At this step of the code, if pstate_exists == NULL, then all-0 or all-1 stateid is used */

  /* TODO FSF:
   * This will eventually all go into the function of state_lock()
   * For now, we will keep checking against SHARE
   * Check against LOCK will be removed
   * We eventually need to handle special stateids, I don't think we'll ever see them on a real lock
   * call, but read and write need to get temporary lock, whether read/write is from NFS v2/3 or NFS v4.x
   */

  /* loop into the states related to this pentry to find the related lock */
  pstate_found_iterate = NULL;
  pstate_previous_iterate = pstate_found;
  do
    {
      state_iterate(data->current_entry,
                    &pstate_found_iterate,
                    pstate_previous_iterate,
                    data->pclient, data->pcontext, &state_status);
      if((state_status == STATE_STATE_ERROR)
         || (state_status == STATE_INVALID_ARGUMENT))
        {
          res_LOCK4.status = NFS4ERR_INVAL;
          return res_LOCK4.status;
        }

      if(pstate_found_iterate != NULL)
        {
          if(pstate_found_iterate->state_type == STATE_TYPE_LOCK)
            {
              /* Check lock upgrade/downgrade */
              if(pstate_exists != NULL)
                {
                  if((pstate_exists == pstate_found_iterate) &&
                     (pstate_exists->state_data.lock.lock_type != arg_LOCK4.locktype))
                    LogFullDebug(COMPONENT_NFS_V4_LOCK,
                        "&&&&&&&&&&&&&& CAS FOIREUX !!!!!!!!!!!!!!!!!!");
                }

              a = pstate_found_iterate->state_data.lock.offset;
              b = pstate_found_iterate->state_data.lock.offset +
                  pstate_found_iterate->state_data.lock.length;
              a1 = arg_LOCK4.offset;
              b1 = arg_LOCK4.offset + arg_LOCK4.length;

              /* Locks overlap is a <= a1 < b or a < b1 <= b */
              overlap = FALSE;
              if(a <= a1)
                {
                  if(a1 < b)
                    overlap = TRUE;
                }
              else
                {
                  if(a < b1)
                    {
                      if(b1 <= b)
                        overlap = TRUE;
                    }
                }

              if(overlap == TRUE)

                /* Locks overlap is a < a1 < b or a < b1 < b */
                if(overlap == TRUE)
                  {
                    /* Locks are overlapping */

                    /* If both lock are READ, this is not a case of error  */
                    if((arg_LOCK4.locktype != READ_LT)
                       || (pstate_found_iterate->state_data.lock.lock_type != READ_LT))
                      {
                        /* Overlapping lock is found, if owner is different than the calling owner, return NFS4ERR_DENIED */
                        if((pstate_exists != NULL) &&   /* all-O/all-1 stateid is considered a different owner */
                           ((powner_exists->so_owner_len ==
                             pstate_found_iterate->state_powner->so_owner_len)
                            &&
                            (!memcmp
                             (powner_exists->so_owner_val,
                              pstate_found_iterate->state_powner->so_owner_val,
                              pstate_found_iterate->state_powner->so_owner_len))))
                          {
                            /* The calling state owner is the same. There is a discussion on this case at page 161 of RFC3530. I choose to ignore this
                             * lock and continue iterating on the other states */
                          }
                        else
                          {
                            /* Increment seqid */
                            if(pstate_exists != NULL)
                              {
                                P(pstate_exists->state_powner->so_mutex);
                                pstate_exists->state_powner->so_owner.so_nfs4_owner.so_seqid += 1;
                                V(pstate_exists->state_powner->so_mutex);
                              }

                            /* A  conflicting lock from a different lock_owner, returns NFS4ERR_DENIED */
                            res_LOCK4.LOCK4res_u.denied.offset =
                                pstate_found_iterate->state_data.lock.offset;
                            res_LOCK4.LOCK4res_u.denied.length =
                                pstate_found_iterate->state_data.lock.length;
                            res_LOCK4.LOCK4res_u.denied.locktype =
                                pstate_found_iterate->state_data.lock.lock_type;
                            res_LOCK4.LOCK4res_u.denied.owner.owner.owner_len =
                                pstate_found_iterate->state_powner->so_owner_len;
                            res_LOCK4.LOCK4res_u.denied.owner.owner.owner_val =
                                pstate_found_iterate->state_powner->so_owner_val;
                            res_LOCK4.status = NFS4ERR_DENIED;
                            return res_LOCK4.status;
                          }
                      }
                  }
            } /* if( ... == STATE_TYPE_LOCK */

          /* For now still check conflicts with SHARE here */
          if(pstate_found_iterate->state_type == STATE_TYPE_SHARE)
            {
              /* In a correct POSIX behavior, a write lock should not be allowed on a read-mode file */
              if((pstate_found_iterate->state_data.share.
                  share_deny & OPEN4_SHARE_DENY_WRITE)
                 && !(pstate_found_iterate->state_data.share.
                      share_access & OPEN4_SHARE_ACCESS_WRITE)
                 && (arg_LOCK4.locktype == WRITE_LT))
                {
                  if(pstate_exists != NULL)
                    {
                      /* Increment seqid */
                      P(pstate_exists->state_powner->so_mutex);
                      pstate_exists->state_powner->so_owner.so_nfs4_owner.so_seqid += 1;
                      V(pstate_exists->state_powner->so_mutex);
                    }

                  /* A conflicting open state, return NFS4ERR_OPENMODE
                   * This behavior is implemented to comply with newpynfs's test LOCK4 */
                  res_LOCK4.status = NFS4ERR_OPENMODE;
                  return res_LOCK4.status;

                }
            }

        }                       /* if( pstate_found_iterate != NULL ) */
      pstate_previous_iterate = pstate_found_iterate;
    }
  while(pstate_found_iterate != NULL);

  /* TODO FSF:
   * Ok from here on out, stuff is broken...
   * For new lock owner, need to create a new stateid
   * And then call state_lock()
   * If that fails, need to back out any stateid changes
   * If that succeeds, need to increment seqids
   */
  switch (arg_LOCK4.locker.new_lock_owner)
    {
    case TRUE:
      /* A lock owner is always associated with a previously made open
       * which has itself a previously made stateid */

      /* Check stateid correctness */
      if((rc = nfs4_Check_Stateid(&arg_LOCK4.locker.locker4_u.open_owner.open_stateid,
                                  data->current_entry, 0LL)) != NFS4_OK)
        {
          res_LOCK4.status = rc;
          return res_LOCK4.status;
        }

      /* Check is the clientid is known or not */
      if(nfs_client_id_get(arg_LOCK4.locker.locker4_u.open_owner.lock_owner.clientid,
                           &nfs_client_id) == CLIENT_ID_NOT_FOUND)
        {
          res_LOCK4.status = NFS4ERR_STALE_CLIENTID;
          return res_LOCK4.status;
        }

      /* The related stateid is already stored in pstate_open */

      /* An open state has been found. Check its type */
      if(pstate_open->state_type != STATE_TYPE_SHARE)
        {
          res_LOCK4.status = NFS4ERR_BAD_STATEID;
          return res_LOCK4.status;
        }

      LogFullDebug(COMPONENT_NFS_V4_LOCK, "=== New Owner ===> %u %u %u",
             arg_LOCK4.locker.locker4_u.open_owner.open_stateid.seqid,
             arg_LOCK4.locker.locker4_u.open_owner.open_seqid, pstate_found->state_seqid);

      /* check the stateid */
      if(arg_LOCK4.locker.locker4_u.open_owner.open_stateid.seqid < pstate_open->state_seqid)
        {
          res_LOCK4.status = NFS4ERR_OLD_STATEID;
          return res_LOCK4.status;
        }

      /* Check validity of the seqid */
      if((arg_LOCK4.locker.locker4_u.open_owner.open_seqid < popen_owner->so_owner.so_nfs4_owner.so_seqid) ||
         (arg_LOCK4.locker.locker4_u.open_owner.open_seqid > popen_owner->so_owner.so_nfs4_owner.so_seqid + 2))
        {
          res_LOCK4.status = NFS4ERR_BAD_SEQID;
          return res_LOCK4.status;
        }

      /* Sanity check : Is this the right file ? */
      if(pstate_open->state_pentry != data->current_entry)
        {
          res_LOCK4.status = NFS4ERR_BAD_STATEID;
          return res_LOCK4.status;
        }

      /* Lock seqid (seqid wanted for new lock) should be 0 (see newpynfs test LOCK8c)  */
      if(arg_LOCK4.locker.locker4_u.open_owner.lock_seqid != 0)
        {
          res_LOCK4.status = NFS4ERR_BAD_SEQID;
          return res_LOCK4.status;
        }

      /* Is this lock_owner known ? */
      if(!convert_nfs4_owner
         ((open_owner4 *) & arg_LOCK4.locker.locker4_u.open_owner.lock_owner,
          &owner_name))
        {
          res_LOCK4.status = NFS4ERR_SERVERFAULT;
          return res_LOCK4.status;
        }

      /* This lock owner is not known yet, allocated and set up a new one */
      powner = create_nfs4_owner(data->pclient,
                                 &owner_name,
                                 (open_owner4 *) &arg_LOCK4.locker.locker4_u.open_owner.lock_owner,
                                 pstate_open->state_powner,
                                 0);

      if(powner == NULL)
        {
          res_LOCK4.status = NFS4ERR_SERVERFAULT;
          return res_LOCK4.status;
        }

      /* Prepare state management structure */
      candidate_type = STATE_TYPE_LOCK;
      candidate_data.lock.lock_type = arg_LOCK4.locktype;
      candidate_data.lock.offset = arg_LOCK4.offset;
      candidate_data.lock.length = arg_LOCK4.length;
      candidate_data.lock.popenstate = (void *)pstate_open;

      /* Add the lock state to the lock table */
      if(state_add(data->current_entry,
                   candidate_type,
                   &candidate_data,
                   powner,
                   data->pclient,
                   data->pcontext,
                   &file_state, &state_status) != STATE_SUCCESS)
        {
          res_LOCK4.status = NFS4ERR_STALE_STATEID;
          return res_LOCK4.status;
        }

        /** @todo BUGAZOMEU: Manage the case if lock conflicts */
      res_LOCK4.LOCK4res_u.resok4.lock_stateid.seqid = file_state->state_seqid;
      memcpy(res_LOCK4.LOCK4res_u.resok4.lock_stateid.other, file_state->stateid_other,
             12);

      /* increment the open state */
      P(pstate_open->state_powner->so_mutex);
      pstate_open->state_powner->so_owner.so_nfs4_owner.so_seqid += 1;
      V(pstate_open->state_powner->so_mutex);

      /* update the lock counter in the related open-stateid */
      pstate_open->state_data.share.lockheld += 1;

      break;

    case FALSE:
      /* The owner already exists, use the provided owner to create a new state */
      /* Get the former state */
      if(state_get(arg_LOCK4.locker.locker4_u.lock_owner.lock_stateid.other,
                   &pstate_found,
                   data->pclient, &state_status) != STATE_SUCCESS)
        {
          res_LOCK4.status = NFS4ERR_STALE_STATEID;
          return res_LOCK4.status;
        }

      /* An lock state has been found. Check its type */
      if(pstate_found->state_type != STATE_TYPE_LOCK)
        {
          res_LOCK4.status = NFS4ERR_BAD_STATEID;
          return res_LOCK4.status;
        }
      LogFullDebug(COMPONENT_NFS_V4_LOCK, "=== Konwn LockOwner ===> %u %u %u",
             arg_LOCK4.locker.locker4_u.lock_owner.lock_stateid.seqid,
             arg_LOCK4.locker.locker4_u.lock_owner.lock_seqid, pstate_found->state_seqid);

      /* Check if stateid is not too old */
      if(arg_LOCK4.locker.locker4_u.lock_owner.lock_stateid.seqid < pstate_found->state_seqid)
        {
          res_LOCK4.status = NFS4ERR_OLD_STATEID;
          return res_LOCK4.status;
        }

      /* Check validity of the desired seqid */
      if((arg_LOCK4.locker.locker4_u.lock_owner.lock_seqid != pstate_found->state_seqid) &&
         (arg_LOCK4.locker.locker4_u.lock_owner.lock_seqid != pstate_found->state_seqid + 1))
        {
          res_LOCK4.status = NFS4ERR_BAD_SEQID;
          return res_LOCK4.status;
        }
#ifdef _CONFORM_TO_TEST_LOCK8c
      /* Check validity of the seqid */
      if(arg_LOCK4.locker.locker4_u.lock_owner.lock_seqid != 0)
        {
          res_LOCK4.status = NFS4ERR_BAD_SEQID;
          return res_LOCK4.status;
        }
#endif
      /* Sanity check : Is this the right file ? */
      if(pstate_found->state_pentry != data->current_entry)
        {
          res_LOCK4.status = NFS4ERR_BAD_STATEID;
          return res_LOCK4.status;
        }

      /* Increment the seqid */
      pstate_found->state_seqid += 1;
      res_LOCK4.LOCK4res_u.resok4.lock_stateid.seqid = pstate_found->state_seqid;
      memcpy(res_LOCK4.LOCK4res_u.resok4.lock_stateid.other,
             pstate_found->stateid_other,
             12);

      /* Increment the related seqid for the related popen_owner */
      if(pstate_found->state_powner->so_owner.so_nfs4_owner.so_related_owner != NULL)
        {
          P(pstate_found->state_powner->so_owner.so_nfs4_owner.so_related_owner->so_mutex);
          pstate_found->state_powner->so_owner.so_nfs4_owner.so_related_owner->so_owner.so_nfs4_owner.so_seqid += 1;
          V(pstate_found->state_powner->so_owner.so_nfs4_owner.so_related_owner->so_mutex);
        }
      else
        LogDebug(COMPONENT_NFS_V4_LOCK,
            "/!\\ : IMPLEMENTATION ERROR File=%s Line=%u pstate_found->state_powner->so_owner.so_nfs4_owner.so_related_owner should not be NULL",
             __FILE__, __LINE__);

      break;
    }                           /* switch( arg_LOCK4.locker.new_lock_owner ) */

  res_LOCK4.status = NFS4_OK;
  return res_LOCK4.status;
#endif
}                               /* nfs4_op_lock */

/**
 * nfs4_op_lock_Free: frees what was allocared to handle nfs4_op_lock.
 *
 * Frees what was allocared to handle nfs4_op_lock.
 *
 * @param resp  [INOUT]    Pointer to nfs4_op results
 *
 * @return nothing (void function )
 *
 */
void nfs4_op_lock_Free(LOCK4res * resp)
{
  /* Nothing to Mem_Free */
  return;
}                               /* nfs4_op_lock_Free */