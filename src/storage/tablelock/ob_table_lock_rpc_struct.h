/**
 * Copyright (c) 2021 OceanBase
 * OceanBase CE is licensed under Mulan PubL v2.
 * You can use this software according to the terms and conditions of the Mulan PubL v2.
 * You may obtain a copy of Mulan PubL v2 at:
 *          http://license.coscl.org.cn/MulanPubL-2.0
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PubL v2 for more details.
 */

#ifndef OCEANBASE_STORAGE_TABLELOCK_OB_TABLE_LOCK_RPC_STRUCT_H_
#define OCEANBASE_STORAGE_TABLELOCK_OB_TABLE_LOCK_RPC_STRUCT_H_

#include "lib/utility/ob_unify_serialize.h"

#include "common/ob_tablet_id.h"
#include "storage/tablelock/ob_table_lock_common.h"
#include "storage/tx/ob_trans_define.h"
#include "storage/tx/ob_trans_define_v4.h"

namespace oceanbase
{

namespace share
{
class ObLSID;
}

namespace transaction
{

namespace tablelock
{


enum ObTableLockTaskType
{
  INVALID_LOCK_TASK_TYPE = -1,
  LOCK_TABLE = 0,
  UNLOCK_TABLE = 1,
  LOCK_TABLET = 2,
  UNLOCK_TABLET = 3,
  PRE_CHECK_TABLET = 4,
  LOCK_PARTITION = 5,
  UNLOCK_PARTITION = 6,
  LOCK_SUBPARTITION = 7,
  UNLOCK_SUBPARTITION = 8,
  LOCK_OBJECT = 9,
  UNLOCK_OBJECT = 10,
  MAX_TASK_TYPE,
};

struct ObLockParam
{
  OB_UNIS_VERSION_V(1);
public:
  ObLockParam() :
      lock_id_(),
      lock_mode_(NO_LOCK),
      owner_id_(0),
      op_type_(UNKNOWN_TYPE),
      is_deadlock_avoid_enabled_(false),
      is_try_lock_(true),
      expired_time_(0),
      schema_version_(-1)
  {}
  virtual ~ObLockParam() {}
  int set(
      const ObLockID &lock_id,
      const ObTableLockMode lock_mode,
      const ObTableLockOwnerID &owner_id,
      const ObTableLockOpType op_type,
      const int64_t schema_version,
      const bool is_deadlock_avoid_enabled = false,
      const bool is_try_lock = true,
      const int64_t expired_time = 0);
  bool is_valid() const;
  TO_STRING_KV(K_(lock_id), K_(lock_mode), K_(owner_id), K_(op_type),
               K_(is_deadlock_avoid_enabled),
               K_(is_try_lock), K_(expired_time), K_(schema_version));

  ObLockID lock_id_;
  ObTableLockMode lock_mode_;
  ObTableLockOwnerID owner_id_;
  ObTableLockOpType op_type_;
  // whether use deadlock avoid or not.
  bool is_deadlock_avoid_enabled_;
  // while a lock/unlock conflict occur, return immediately if
  // true, else retry until expired_time_.
  bool is_try_lock_;
  // wait until this time if it is not try lock/unlock op.
  int64_t expired_time_;
  // current schema version
  int64_t schema_version_;
};

struct ObLockRequest
{
  OB_UNIS_VERSION_V(1);
public:
  enum class ObLockMsgType
  {
    UNKNOWN_MSG_TYPE        = 0,
    INTRANS_LOCK_TABLE_REQ  = 1, // reserved for old LockTableMsg
    INTRANS_LOCK_TABLET_REQ = 2, // reserved for old LockTableMsg
    LOCK_OBJ_REQ =            3,
    LOCK_TABLE_REQ =          4,
    LOCK_PARTITION_REQ =      5,
    LOCK_TABLET_REQ =         6,
  };
public:
  ObLockRequest() :
      owner_id_(0),
      lock_mode_(NO_LOCK),
      op_type_(UNKNOWN_TYPE),
      timeout_us_(0),
      is_from_sql_(false)
  { type_ = ObLockMsgType::UNKNOWN_MSG_TYPE; }
  virtual ~ObLockRequest() { reset(); }
  virtual void reset();
  virtual bool is_valid() const;
  VIRTUAL_TO_STRING_KV(K_(owner_id), K_(lock_mode), K_(op_type), K_(timeout_us));
public:
  ObLockMsgType type_;
  ObTableLockOwnerID owner_id_;
  ObTableLockMode lock_mode_;
  ObTableLockOpType op_type_;
  int64_t timeout_us_;
  bool is_from_sql_;
};

struct ObLockObjRequest : public ObLockRequest
{
  OB_UNIS_VERSION_V(1);
public:
  ObLockObjRequest() :
      ObLockRequest(),
      obj_type_(ObLockOBJType::OBJ_TYPE_INVALID),
      obj_id_(0)
  { type_ = ObLockMsgType::LOCK_OBJ_REQ; }
  virtual ~ObLockObjRequest() { reset(); }
  virtual void reset();
  virtual bool is_valid() const;
  INHERIT_TO_STRING_KV("ObLockRequest", ObLockRequest, K_(obj_type), K_(obj_id));
public:
  // which object should we lock
  ObLockOBJType obj_type_;
  uint64_t obj_id_;
};
using ObUnLockObjRequest = ObLockObjRequest;

struct ObLockTableRequest : public ObLockRequest
{
  OB_UNIS_VERSION_V(1);
public:
  ObLockTableRequest() : ObLockRequest(), table_id_(0)
  { type_ = ObLockMsgType::LOCK_TABLE_REQ; }
  virtual ~ObLockTableRequest() { reset(); }
  virtual void reset();
  virtual bool is_valid() const;
  INHERIT_TO_STRING_KV("ObLockRequest", ObLockRequest, K_(table_id));
public:
  // which table should we lock
  uint64_t table_id_;
};
using ObUnLockTableRequest = ObLockTableRequest;

struct ObLockPartitionRequest : public ObLockTableRequest
{
  OB_UNIS_VERSION_V(1);
public:
  ObLockPartitionRequest() : ObLockTableRequest(), part_object_id_(0)
  { type_ = ObLockMsgType::LOCK_PARTITION_REQ; }
  virtual ~ObLockPartitionRequest() { reset(); }
  virtual void reset();
  virtual bool is_valid() const;
  INHERIT_TO_STRING_KV("ObLockTableRequest", ObLockTableRequest, K_(part_object_id));
public:
  uint64_t part_object_id_;
};
using ObUnLockPartitionRequest = ObLockPartitionRequest;

struct ObLockTabletRequest : public ObLockTableRequest
{
  OB_UNIS_VERSION_V(1);
public:
  ObLockTabletRequest() : tablet_id_()
  { type_ = ObLockMsgType::LOCK_TABLET_REQ; }
  virtual ~ObLockTabletRequest() { reset(); }
  virtual void reset();
  virtual bool is_valid() const;
  INHERIT_TO_STRING_KV("ObLockTableRequest", ObLockTableRequest, K_(tablet_id));
public:
  common::ObTabletID tablet_id_;
};
using ObUnLockTabletRequest = ObLockTabletRequest;

class ObTableLockTaskRequest final
{
  OB_UNIS_VERSION(1);
public:
  ObTableLockTaskRequest()
    : task_type_(INVALID_LOCK_TASK_TYPE),
      lsid_(),
      param_(),
      tx_desc_(nullptr),
      need_release_tx_(false)
  {}
  ~ObTableLockTaskRequest();
  int set(
      const ObTableLockTaskType task_type,
      const share::ObLSID &lsid,
      const ObLockParam &param,
      transaction::ObTxDesc *tx_desc);
  int assign(const ObTableLockTaskRequest &arg);

  bool is_valid() const
  {
    return (task_type_ < MAX_TASK_TYPE
            && lsid_.is_valid()
            && param_.is_valid()
            && OB_NOT_NULL(tx_desc_)
            && tx_desc_->is_valid());
  }

  bool is_timeout() const;

  TO_STRING_KV(K(task_type_), K(lsid_), K(param_), KP(tx_desc_));
public:
  ObTableLockTaskType task_type_;
  share::ObLSID lsid_; // go to which ls to lock.
  ObLockParam param_;
  transaction::ObTxDesc *tx_desc_;
private:
  bool need_release_tx_;
};

class ObLockTaskBatchRequest final
{
  OB_UNIS_VERSION(1);
 public:
  ObLockTaskBatchRequest() :
      task_type_(INVALID_LOCK_TASK_TYPE),
      lsid_(),
      params_(),
      tx_desc_(nullptr),
      need_release_tx_(false)
  {}
  ~ObLockTaskBatchRequest();
  int init(const ObTableLockTaskType task_type,
           const share::ObLSID &lsid,
           transaction::ObTxDesc *tx_desc);
  bool is_inited() const;
  bool is_valid() const;
  int assign(const ObLockTaskBatchRequest &arg);

  TO_STRING_KV(K(task_type_), K(lsid_), K(params_));
 public:
  ObTableLockTaskType task_type_;
  share::ObLSID lsid_; // go to which ls to lock.
  common::ObSArray<ObLockParam> params_;
  transaction::ObTxDesc *tx_desc_;
 private:
  bool need_release_tx_;
};


class ObTableLockTaskResult final
{
  OB_UNIS_VERSION(1);
public:
  ObTableLockTaskResult()
    : ret_code_(common::OB_SUCCESS),
    tx_result_ret_code_(common::OB_SUCCESS),
    tx_result_() {}
  ~ObTableLockTaskResult() {}

  int get_ret_code() const { return ret_code_; }
  int get_tx_result_code() const { return tx_result_ret_code_; }
  transaction::ObTxExecResult &get_tx_result() { return tx_result_; }

  TO_STRING_KV(K(ret_code_), K(tx_result_));
private:
  DISALLOW_COPY_AND_ASSIGN(ObTableLockTaskResult);
public:
  int ret_code_;
  int tx_result_ret_code_;
  transaction::ObTxExecResult tx_result_;
};

// --------------------- used for client request ------------------------------
struct ObOutTransLockTableRequest
{
  OB_UNIS_VERSION(1);
public:
  ObOutTransLockTableRequest()
    : table_id_(),
    lock_mode_(),
    lock_owner_(),
    timeout_us_() {}
  ~ObOutTransLockTableRequest() {}
  int assign(const ObOutTransLockTableRequest &arg);

  TO_STRING_KV(K(table_id_), K(lock_mode_), K(lock_owner_), K(timeout_us_));

  uint64_t table_id_;
  ObTableLockMode lock_mode_;
  ObTableLockOwnerID lock_owner_;
  int64_t timeout_us_;
};

// the content of out trans unlock request is the same of lock request.
using ObOutTransUnLockTableRequest = ObOutTransLockTableRequest;

struct ObInTransLockTableRequest
{
  OB_UNIS_VERSION_V(1);
public:
  enum class ObLockTableMsgType
  {
    UNKNOWN_MSG_TYPE        = 0,
    INTRANS_LOCK_TABLE_REQ  = 1,
    INTRANS_LOCK_TABLET_REQ = 2,
    INTRANS_LOCK_OBJ_REQ    = 3,
  };
public:
  ObInTransLockTableRequest()
    : table_id_(),
      lock_mode_(),
      timeout_us_()
  { type_ = ObLockTableMsgType::INTRANS_LOCK_TABLE_REQ; }
  virtual ~ObInTransLockTableRequest();
  int assign(const ObInTransLockTableRequest &arg);

  VIRTUAL_TO_STRING_KV(K_(type), K_(table_id), K_(lock_mode), K_(timeout_us));

  ObLockTableMsgType type_;
  uint64_t table_id_; // as obj_id when lock_obj
  ObTableLockMode lock_mode_;
  int64_t timeout_us_;
};

struct ObInTransLockTabletRequest : public ObInTransLockTableRequest
{
  OB_UNIS_VERSION(1);
public:
  ObInTransLockTabletRequest() : ObInTransLockTableRequest(), tablet_id_()
  { type_ = ObLockTableMsgType::INTRANS_LOCK_TABLET_REQ; }
  ~ObInTransLockTabletRequest() { tablet_id_.reset(); }
  int assign(const ObInTransLockTabletRequest &arg);
  INHERIT_TO_STRING_KV("ObInTransLockTableRequest", ObInTransLockTableRequest, K_(tablet_id));
public:
  common::ObTabletID tablet_id_;
};

struct ObAdminRemoveLockOpArg
{
  OB_UNIS_VERSION(1);
public:
  ObAdminRemoveLockOpArg() : tenant_id_(OB_INVALID_TENANT_ID), ls_id_(), lock_op_() {}
  ~ObAdminRemoveLockOpArg()
  {
    tenant_id_ = OB_INVALID_TENANT_ID;
    ls_id_.reset();
  }
  int set(const uint64_t tenant_id,
          const share::ObLSID &ls_id,
          const ObTableLockOp &lock_op);
  int assign(const ObAdminRemoveLockOpArg &arg);
  bool is_valid() const;
  TO_STRING_KV(K_(tenant_id), K_(ls_id), K_(lock_op));
public:
  uint64_t tenant_id_;
  share::ObLSID ls_id_;
  ObTableLockOp lock_op_;
};

struct ObAdminUpdateLockOpArg
{
  OB_UNIS_VERSION(1);
public:
  ObAdminUpdateLockOpArg()
    : tenant_id_(OB_INVALID_TENANT_ID),
      ls_id_(),
      lock_op_(),
      commit_version_(),
      commit_scn_()
  {}
  ~ObAdminUpdateLockOpArg()
  {
    tenant_id_ = OB_INVALID_TENANT_ID;
    ls_id_.reset();
    commit_version_.reset();
    commit_scn_.reset();
  }
  int set(const uint64_t tenant_id,
          const share::ObLSID &ls_id,
          const ObTableLockOp &lock_op,
          const share::SCN &commit_version,
          const share::SCN &commit_scn);
  int assign(const ObAdminUpdateLockOpArg &arg);
  bool is_valid() const;
  TO_STRING_KV(K_(tenant_id), K_(ls_id), K_(lock_op), K_(commit_version), K_(commit_scn));
public:
  uint64_t tenant_id_;
  share::ObLSID ls_id_;
  ObTableLockOp lock_op_;
  share::SCN commit_version_;
  share::SCN commit_scn_;
};

}
}
}


#endif /* OCEANBASE_STORAGE_TABLELOCK_OB_TABLE_LOCK_RPC_STRUCT_H_ */
