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

#define USING_LOG_PREFIX STORAGE
#include "ob_complement_data_task.h"
#include "lib/utility/ob_tracepoint.h"
#include "logservice/ob_log_service.h"
#include "observer/ob_server_struct.h"
#include "share/ob_dml_sql_splicer.h"
#include "share/ob_ddl_checksum.h"
#include "share/ob_ddl_error_message_table_operator.h"
#include "share/ob_freeze_info_proxy.h"
#include "share/ob_get_compat_mode.h"
#include "share/schema/ob_table_dml_param.h"
#include "sql/engine/px/ob_granule_util.h"
#include "sql/ob_sql_utils.h"
#include "storage/access/ob_multiple_scan_merge.h"
#include "storage/blocksstable/ob_index_block_builder.h"
#include "storage/compaction/ob_column_checksum_calculator.h"
#include "storage/ddl/ob_ddl_merge_task.h"
#include "storage/ddl/ob_ddl_redo_log_writer.h"
#include "storage/ob_i_table.h"
#include "storage/ob_parallel_external_sort.h"
#include "storage/ob_partition_range_spliter.h"
#include "storage/ob_row_reshape.h"
#include "storage/access/ob_multiple_scan_merge.h"
#include "storage/tx/ob_trans_service.h"
#include "storage/lob/ob_lob_util.h"
#include "logservice/ob_log_service.h"
#include "storage/ddl/ob_tablet_ddl_kv_mgr.h"

namespace oceanbase
{
using namespace common;
using namespace storage;
using namespace compaction;
using namespace share;
using namespace share::schema;
using namespace sql;
using namespace observer;
using namespace omt;
using namespace name;
using namespace transaction;
using namespace blocksstable;

namespace storage
{
int ObComplementDataParam::init(const ObDDLBuildSingleReplicaRequestArg &arg)
{
  int ret = OB_SUCCESS;
  ObSchemaGetterGuard schema_guard;
  const ObTenantSchema *tenant_schema = nullptr;
  const ObTableSchema *data_table_schema = nullptr;
  const ObTableSchema *hidden_table_schema = nullptr;
  MAKE_TENANT_SWITCH_SCOPE_GUARD(guard);
  const bool check_formal = arg.dest_schema_id_ > OB_MAX_CORE_TABLE_ID;       //avoid circular dependencies
  const uint64_t tenant_id = arg.tenant_id_;
  if (OB_UNLIKELY(is_inited_)) {
    ret = OB_INIT_TWICE;
    LOG_WARN("ObComplementDataParam has been inited before", K(ret));
  } else if (OB_UNLIKELY(!arg.is_valid())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid arg", K(ret), K(arg));
  } else if (OB_FAIL(ObMultiVersionSchemaService::get_instance().get_tenant_schema_guard(
             tenant_id, schema_guard, arg.schema_version_))) {
    LOG_WARN("fail to get tenant schema guard", K(ret), K(arg));
  } else if (check_formal && OB_FAIL(schema_guard.check_formal_guard())) {
    LOG_WARN("schema_guard is not formal", K(ret), K(arg));
  } else if (OB_FAIL(schema_guard.get_tenant_info(tenant_id, tenant_schema))) {
    LOG_WARN("fail to get tenant info", K(ret), K(arg));
  } else if (OB_ISNULL(tenant_schema)) {
    ret = OB_TABLE_NOT_EXIST;
    LOG_WARN("tenant not exist", K(ret), K(arg));
  } else if (OB_FAIL(schema_guard.get_table_schema(tenant_id,
             arg.dest_schema_id_, hidden_table_schema))) {
    LOG_WARN("fail to get hidden table schema", K(ret), K(arg));
  } else if (OB_ISNULL(hidden_table_schema)) {
    ret = OB_TABLE_NOT_EXIST;
    LOG_WARN("hidden table schema not exist", K(ret), K(arg));
  } else if (OB_UNLIKELY(hidden_table_schema->get_association_table_id() != arg.source_table_id_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("unexpected error", K(ret), K(arg), K(hidden_table_schema->get_association_table_id()));
  } else if (OB_FAIL(schema_guard.get_table_schema(tenant_id,
             arg.source_table_id_, data_table_schema))) {
    LOG_WARN("fail to get data table schema", K(ret), K(arg));
  } else if (OB_ISNULL(data_table_schema)) {
    ret = OB_TABLE_NOT_EXIST;
    LOG_WARN("data table schema not exist", K(ret), K(arg));
  } else if (OB_FAIL(guard.switch_to(tenant_id))) {
    LOG_WARN("fail to switch to tenant", K(ret), K(arg));
  } else if (FALSE_IT(snapshot_version_ = arg.snapshot_version_)) {
  } else if (OB_FAIL(split_task_ranges(arg.ls_id_, arg.source_tablet_id_, data_table_schema->get_tablet_size(), arg.parallelism_))) {
    LOG_WARN("fail to init concurrent params", K(ret), K(arg));
  } else if (OB_FAIL(ObCompatModeGetter::get_table_compat_mode(tenant_id, arg.source_table_id_, compat_mode_))) {
    LOG_WARN("failed to get compat mode", K(ret), K(arg));
  } else {
    is_inited_ = true;
    tenant_id_ = tenant_id;
    ls_id_ = arg.ls_id_;
    source_table_id_ = arg.source_table_id_;
    dest_table_id_ = arg.dest_schema_id_;
    source_tablet_id_ = arg.source_tablet_id_;
    dest_tablet_id_ = arg.dest_tablet_id_;
    schema_version_ = arg.schema_version_;
    task_id_ = arg.task_id_;
    execution_id_ = arg.execution_id_;
    tablet_task_id_ = arg.tablet_task_id_;
    data_format_version_ = arg.data_format_version_;
    FLOG_INFO("succeed to init ObComplementDataParam", K(ret), K(is_inited_), K(tenant_id_), K(ls_id_),
      K(source_tablet_id_), K(dest_tablet_id_), K(schema_version_), K(task_id_), K(arg), K(concurrent_cnt_),
      K(data_format_version_));
  }
  return ret;
}

// split task ranges to do table scan based on the whole range on the specified tablet.
int ObComplementDataParam::split_task_ranges(
    const share::ObLSID &ls_id,
    const common::ObTabletID &tablet_id,
    const int64_t tablet_size,
    const int64_t hint_parallelism)
{
  int ret = OB_SUCCESS;
  ObSimpleFrozenStatus frozen_status;
  const bool allow_not_ready = false;
  ObLSHandle ls_handle;
  ObTabletTableIterator iterator;
  ObLSTabletService *tablet_service = nullptr;
  if (OB_UNLIKELY(is_inited_)) {
    ret = OB_INIT_TWICE;
    LOG_WARN("ObComplementDataParam has been inited before", K(ret));
  } else if (OB_UNLIKELY(!ls_id.is_valid() || !tablet_id.is_valid())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid arguments", K(ret), K(ls_id), K(tablet_id));
  } else if (OB_FAIL(MTL(ObLSService *)->get_ls(ls_id, ls_handle, ObLSGetMod::DDL_MOD))) {
    LOG_WARN("fail to get log stream", K(ret), K(arg));
  } else if (OB_UNLIKELY(nullptr == ls_handle.get_ls())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("ls is null", K(ret), K(ls_handle));
  } else if (OB_ISNULL(tablet_service = ls_handle.get_ls()->get_tablet_svr())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("tablet service is nullptr", K(ret));
  } else {
    int64_t total_size = 0;
    int64_t expected_task_count = 0;
    ObStoreRange range;
    range.set_whole_range();
    ObSEArray<common::ObStoreRange, 32> ranges;
    ObArrayArray<ObStoreRange> multi_range_split_array;
    ObParallelBlockRangeTaskParams params;
    params.parallelism_ = hint_parallelism;
    params.expected_task_load_ = tablet_size / 1024 / 1024 <= 0 ? sql::OB_EXPECTED_TASK_LOAD : tablet_size / 1024 / 1024;
    if (OB_FAIL(ranges.push_back(range))) {
      LOG_WARN("push back range failed", K(ret));
    } else if (OB_FAIL(tablet_service->get_multi_ranges_cost(tablet_id,
                                                             ranges,
                                                             total_size))) {
      LOG_WARN("get multi ranges cost failed", K(ret));
      if (OB_REPLICA_NOT_READABLE == ret) {
        ret = OB_EAGAIN;
      }
    } else if (OB_FALSE_IT(total_size = total_size / 1024 / 1024 /* Byte -> MB */)) {
    } else if (OB_FAIL(ObGranuleUtil::compute_total_task_count(params, 
                                                               total_size,
                                                               expected_task_count))) {
      LOG_WARN("compute total task count failed", K(ret));
    } else if (OB_FAIL(tablet_service->split_multi_ranges(tablet_id, 
                                                          ranges, 
                                                          min(min(max(expected_task_count, 1), hint_parallelism), ObMacroDataSeq::MAX_PARALLEL_IDX + 1),
                                                          allocator_, 
                                                          multi_range_split_array))) {
      LOG_WARN("split multi ranges failed", K(ret));
      if (OB_REPLICA_NOT_READABLE == ret) {
        ret = OB_EAGAIN;
      }
    } else if (multi_range_split_array.count() <= 0) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("unexpected range split arr", K(ret), K(total_size), K(hint_parallelism), 
        K(expected_task_count), K(params), K(multi_range_split_array));
    } else {
      for (int64_t i = 0; OB_SUCC(ret) && i < multi_range_split_array.count(); i++) {
        ObIArray<ObStoreRange> &storage_task_ranges = multi_range_split_array.at(i);
        for (int64_t j = 0; OB_SUCC(ret) && j < storage_task_ranges.count(); j++) {
          if (OB_FAIL(ranges_.push_back(storage_task_ranges.at(j)))) {
            LOG_WARN("push back failed", K(ret));
          }
        }
      }
      if (OB_SUCC(ret)) {
        concurrent_cnt_ = ranges_.count();
        LOG_INFO("succeed to get range and concurrent cnt", K(ret), K(total_size), K(hint_parallelism),
          K(expected_task_count), K(params), K(multi_range_split_array), K(ranges_));
      }
    }
  }
  return ret;
}

int ObComplementDataParam::get_hidden_table_key(ObITable::TableKey &table_key) const
{
  int ret = OB_SUCCESS;
  table_key.reset();
  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("ObComplementDataParam is not inited", K(ret));
  } else {
    table_key.table_type_ = ObITable::TableType::MAJOR_SSTABLE;
    table_key.tablet_id_ = dest_tablet_id_;
    table_key.version_range_.base_version_ = ObNewVersionRange::MIN_VERSION;
    table_key.version_range_.snapshot_version_ = snapshot_version_;
  }
  return ret;
}

int ObComplementDataContext::init(const ObComplementDataParam &param, const ObDataStoreDesc &desc)
{
  int ret = OB_SUCCESS;
  void *builder_buf = nullptr;
  const ObSSTable *first_major_sstable = nullptr;
  if (OB_UNLIKELY(is_inited_)) {
    ret = OB_INIT_TWICE;
    LOG_WARN("ObComplementDataContext has already been inited", K(ret));
  } else if (OB_UNLIKELY(!param.is_valid() || !desc.is_valid())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid arguments", K(ret), K(param), K(desc));
  } else if (OB_FAIL(ObTabletDDLUtil::check_and_get_major_sstable(param.ls_id_, param.dest_tablet_id_, first_major_sstable))) {
    LOG_WARN("check if major sstable exist failed", K(ret), K(param));
  } else if (OB_FAIL(data_sstable_redo_writer_.init(param.ls_id_,
                                                    param.dest_tablet_id_))) {
    LOG_WARN("fail to init data sstable redo writer", K(ret), K(param));
  } else if (nullptr != index_builder_) {
    LOG_INFO("index builder is already exist", K(ret));
  } else if (OB_ISNULL(builder_buf = allocator_.alloc(sizeof(ObSSTableIndexBuilder)))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
    LOG_WARN("failed to alloc memory", K(ret));
  } else if (OB_ISNULL(index_builder_ = new (builder_buf) ObSSTableIndexBuilder())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("failed to new ObSSTableIndexBuilder", K(ret));
  } else if (OB_FAIL(index_builder_->init(desc,
                                          nullptr, // macro block flush callback
                                          ObSSTableIndexBuilder::DISABLE))) {
    LOG_WARN("failed to init index builder", K(ret), K(desc));
  } else {
    is_major_sstable_exist_ = nullptr != first_major_sstable ? true : false;
    concurrent_cnt_ = param.concurrent_cnt_;
    is_inited_ = true;
  }
  if (OB_FAIL(ret)) {
    if (nullptr != index_builder_) {
      index_builder_->~ObSSTableIndexBuilder();
      index_builder_ = nullptr;
    }
    if (nullptr != builder_buf) {
      allocator_.free(builder_buf);
      builder_buf = nullptr;
    }
  }
  return ret;
}

int ObComplementDataContext::write_start_log(const ObComplementDataParam &param)
{
  int ret = OB_SUCCESS;
  ObITable::TableKey hidden_table_key;
  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("ObComplementDataContext not init", K(ret));
  } else if (OB_UNLIKELY(!param.is_valid())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid arguments", K(ret), K(param));
  } else if (OB_FAIL(param.get_hidden_table_key(hidden_table_key))) {
    LOG_WARN("fail to get hidden table key", K(ret));
  } else if (OB_UNLIKELY(!hidden_table_key.is_valid())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid table key", K(ret), K(hidden_table_key));
  } else if (OB_FAIL(data_sstable_redo_writer_.start_ddl_redo(hidden_table_key,
    param.execution_id_, param.data_format_version_, ddl_kv_mgr_handle_))) {
    LOG_WARN("fail write start log", K(ret), K(hidden_table_key), K(param));
  } else {
    LOG_INFO("complement task start ddl redo success", K(hidden_table_key));
  }
  return ret;
}

void ObComplementDataContext::destroy()
{
  is_inited_ = false;
  is_major_sstable_exist_ = false;
  complement_data_ret_ = OB_SUCCESS;
  concurrent_cnt_ = 0;
  if (OB_NOT_NULL(index_builder_)) {
    index_builder_->~ObSSTableIndexBuilder();
    allocator_.free(index_builder_);
    index_builder_ = nullptr;
  }
  ddl_kv_mgr_handle_.reset();
  allocator_.reset();
}

ObComplementDataDag::ObComplementDataDag()
  : ObIDag(ObDagType::DAG_TYPE_DDL), is_inited_(false), param_(), context_()
{
}


ObComplementDataDag::~ObComplementDataDag()
{
}

int ObComplementDataDag::init(const ObDDLBuildSingleReplicaRequestArg &arg)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(is_inited_)) {
    ret = OB_INIT_TWICE;
    LOG_WARN("ObComplementDataDag has already been inited", K(ret));
  } else if (OB_UNLIKELY(!arg.is_valid())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid arguments", K(ret), K(arg));
  } else if (OB_FAIL(param_.init(arg))) {
    LOG_WARN("fail to init dag param", K(ret));
  } else if (OB_UNLIKELY(!param_.is_valid())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("error unexpected", K(ret), K(param_));
  } else {
    consumer_group_id_ = arg.consumer_group_id_;
    is_inited_ = true;
  }
  LOG_INFO("finish to init complement data dag", K(ret), K(param_));
  return ret;
}

int ObComplementDataDag::create_first_task()
{
  int ret = OB_SUCCESS;
  ObComplementPrepareTask *prepare_task = nullptr;
  ObComplementWriteTask *write_task = nullptr;
  ObComplementMergeTask *merge_task = nullptr;
  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("not init", K(ret));
  } else if (OB_FAIL(alloc_task(prepare_task))) {
    LOG_WARN("allocate task failed", K(ret));
  } else if (OB_ISNULL(prepare_task)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("unexpected nullptr task", K(ret));
  } else if (OB_FAIL(prepare_task->init(param_, context_))) {
    LOG_WARN("init prepare task failed", K(ret));
  } else if (OB_FAIL(add_task(*prepare_task))) {
    LOG_WARN("add task failed", K(ret));
  } else if (OB_FAIL(alloc_task(write_task))) {
    LOG_WARN("alloc task failed", K(ret));
  } else if (OB_ISNULL(write_task)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("unexpected nullptr task", K(ret));
  } else if (OB_FAIL(write_task->init(0, param_, context_))) {
    LOG_WARN("init write task failed", K(ret));
  } else if (OB_FAIL(prepare_task->add_child(*write_task))) {
    LOG_WARN("add child task failed", K(ret));
  } else if (OB_FAIL(add_task(*write_task))) {
    LOG_WARN("add task failed", K(ret));
  } else if (OB_FAIL(alloc_task(merge_task))) {
    LOG_WARN("alloc task failed", K(ret));
  } else if (OB_ISNULL(merge_task)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("unexpected nullptr task", K(ret));
  } else if (OB_FAIL(merge_task->init(param_, context_))) {
    LOG_WARN("init merge task failed", K(ret));
  } else if (OB_FAIL(write_task->add_child(*merge_task))) {
    LOG_WARN("add child task failed", K(ret));
  } else if (OB_FAIL(add_task(*merge_task))) {
    LOG_WARN("add task failed");
  }

  return ret;
}

bool ObComplementDataDag::ignore_warning()
{
  return OB_EAGAIN == dag_ret_
    || OB_NEED_RETRY == dag_ret_
    || OB_TASK_EXPIRED == dag_ret_;
}

int ObComplementDataDag::prepare_context()
{
  int ret = OB_SUCCESS;
  ObDataStoreDesc data_desc;
  ObSchemaGetterGuard schema_guard;
  const ObTableSchema *hidden_table_schema = nullptr;
  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("ObComplementDataDag not init", K(ret));
  } else if (OB_UNLIKELY(!param_.is_valid())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("error unexpected", K(ret), K(param_));
  } else if (OB_FAIL(ObMultiVersionSchemaService::get_instance().get_tenant_schema_guard(
             param_.tenant_id_, schema_guard, param_.schema_version_))) {
    LOG_WARN("fail to get tenant schema guard", K(ret), K(param_));
  } else if (OB_FAIL(schema_guard.get_table_schema(param_.tenant_id_,
             param_.dest_table_id_, hidden_table_schema))) {
    LOG_WARN("fail to get hidden table schema", K(ret), K(param_));
  } else if (OB_ISNULL(hidden_table_schema)) {
    ret = OB_TABLE_NOT_EXIST;
    LOG_WARN("hidden table schema not exist", K(ret), K(param_));
  } else if (OB_FAIL(data_desc.init(*hidden_table_schema,
                                    param_.ls_id_,
                                    param_.dest_tablet_id_,
                                    MAJOR_MERGE,
                                    param_.snapshot_version_,
                                    param_.data_format_version_))) {
    LOG_WARN("fail to init data desc", K(ret));
  } else {
    data_desc.row_column_count_ = data_desc.rowkey_column_count_ + 1;
    data_desc.col_desc_array_.reset();
    data_desc.need_prebuild_bloomfilter_ = false;
    data_desc.is_ddl_ = true;
    if (OB_FAIL(data_desc.col_desc_array_.init(data_desc.row_column_count_))) {
      LOG_WARN("failed to reserve column desc array", K(ret));
    } else if (OB_FAIL(hidden_table_schema->get_rowkey_column_ids(data_desc.col_desc_array_))) {
      LOG_WARN("failed to get rowkey column ids", K(ret));
    } else if (OB_FAIL(storage::ObMultiVersionRowkeyHelpper::add_extra_rowkey_cols(data_desc.col_desc_array_))) {
      LOG_WARN("failed to add extra rowkey cols", K(ret));
    } else {
      ObObjMeta meta;
      meta.set_varchar();
      meta.set_collation_type(CS_TYPE_BINARY);
      share::schema::ObColDesc col;
      col.col_id_ = static_cast<uint64_t>(data_desc.row_column_count_ + OB_APP_MIN_COLUMN_ID);
      col.col_type_ = meta;
      col.col_order_ = DESC;
      if (OB_FAIL(data_desc.col_desc_array_.push_back(col))) {
        LOG_WARN("failed to push back last col for index", K(ret), K(col));
      }
    }
  }

  if (OB_FAIL(ret)) {
  } else if (OB_FAIL(context_.init(param_, data_desc))) {
    LOG_WARN("fail to init context", K(ret), K(param_), K(data_desc));
  }
  LOG_INFO("finish to prepare complement context", K(ret), K(param_), K(context_));
  return ret;
}

int64_t ObComplementDataDag::hash() const
{
  int tmp_ret = OB_SUCCESS;
  int64_t hash_val = 0;
  if (OB_UNLIKELY(!is_inited_ || !param_.is_valid())) {
    tmp_ret = OB_ERR_SYS;
    LOG_ERROR("table schema must not be NULL", K(tmp_ret), K(is_inited_), K(param_));
  } else {
    hash_val = param_.tenant_id_ + param_.ls_id_.hash()
             + param_.source_table_id_ + param_.dest_table_id_
             + param_.source_tablet_id_.hash() + param_.dest_tablet_id_.hash() + ObDagType::DAG_TYPE_DDL;
  }
  return hash_val;
}

bool ObComplementDataDag::operator==(const ObIDag &other) const
{
  int tmp_ret = OB_SUCCESS;
  bool is_equal = false;
  if (OB_UNLIKELY(this == &other)) {
    is_equal = true;
  } else if (get_type() == other.get_type()) {
    const ObComplementDataDag &dag = static_cast<const ObComplementDataDag &>(other);
    if (OB_UNLIKELY(!param_.is_valid() || !dag.param_.is_valid())) {
      tmp_ret = OB_ERR_SYS;
      LOG_ERROR("invalid argument", K(tmp_ret), K(param_), K(dag.param_));
    } else {
      is_equal = (param_.tenant_id_ == dag.param_.tenant_id_) && (param_.ls_id_ == dag.param_.ls_id_) &&
                 (param_.source_table_id_ == dag.param_.source_table_id_) && (param_.dest_table_id_ == dag.param_.dest_table_id_) &&
                 (param_.source_tablet_id_ == dag.param_.source_tablet_id_) && (param_.dest_tablet_id_ == dag.param_.dest_tablet_id_);
    }
  }
  return is_equal;
}

// build reponse here rather deconstruction of DAG, to avoid temporary dead lock of RS RPC queue.
//
int ObComplementDataDag::report_replica_build_status()
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("ObComplementDataDag has not been inited", K(ret));
  } else if (OB_UNLIKELY(!param_.is_valid())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid param", K(ret), K(param_));
  } else {
#ifdef ERRSIM
    if (OB_SUCC(ret)) {
      ret = OB_E(EventTable::EN_DDL_REPORT_REPLICA_BUILD_STATUS_FAIL) OB_SUCCESS;
      LOG_INFO("report replica build status errsim", K(ret));
    }
#endif
    obrpc::ObDDLBuildSingleReplicaResponseArg arg;
    ObAddr rs_addr;
    arg.tenant_id_ = param_.tenant_id_;
    arg.ls_id_ = param_.ls_id_;
    arg.tablet_id_ = param_.source_tablet_id_;
    arg.source_table_id_ = param_.source_table_id_;
    arg.dest_schema_id_ = param_.dest_table_id_;
    arg.ret_code_ = context_.complement_data_ret_;
    arg.snapshot_version_ = param_.snapshot_version_;
    arg.schema_version_ = param_.schema_version_;
    arg.task_id_ = param_.task_id_;
    arg.execution_id_ = param_.execution_id_;
    arg.row_scanned_ = context_.row_scanned_;
    arg.row_inserted_ = context_.row_inserted_;
    FLOG_INFO("send replica build status response to RS", K(ret), K(context_), K(arg));
    if (OB_FAIL(ret)) {
    } else if (OB_ISNULL(GCTX.rs_rpc_proxy_) || OB_ISNULL(GCTX.rs_mgr_)) {
      ret = OB_ERR_SYS;
      LOG_WARN("innner system error, rootserver rpc proxy or rs mgr must not be NULL", K(ret), K(GCTX));
    } else if (OB_FAIL(GCTX.rs_mgr_->get_master_root_server(rs_addr))) {
      LOG_WARN("fail to get rootservice address", K(ret));
    } else if (OB_FAIL(GCTX.rs_rpc_proxy_->to(rs_addr).build_ddl_single_replica_response(arg))) {
      LOG_WARN("fail to send build ddl single replica response", K(ret), K(arg));
    }
  }
  FLOG_INFO("complement data finished", K(ret), K(context_.complement_data_ret_));
  return ret;
}

int ObComplementDataDag::fill_comment(char *buf, const int64_t buf_len) const
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("ObComplementDataDag has not been initialized", K(ret));
  } else if (OB_UNLIKELY(!param_.is_valid())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid param", K(ret), K(param_));
  } else if (OB_FAIL(databuff_printf(buf, buf_len, "complement data task: logstream_id=%ld, source_tablet_id=%ld, dest_tablet_id=%ld, data_table_id=%ld, target_table_id=%ld, schema_version=%ld, snapshot_version=%ld",
      param_.ls_id_.id(), param_.source_tablet_id_.id(), param_.dest_tablet_id_.id(), param_.source_table_id_, param_.dest_table_id_, param_.schema_version_, param_.snapshot_version_))) {
    LOG_WARN("fail to fill comment", K(ret), K(param_));
  }
  return ret;
}

int ObComplementDataDag::fill_dag_key(char *buf, const int64_t buf_len) const
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("ObComplementDataDag has not been initialized", K(ret));
  } else if (OB_UNLIKELY(!param_.is_valid())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid params", K(ret), K(param_));
  } else if (OB_FAIL(databuff_printf(buf, buf_len, "logstream_id=%ld source_tablet_id=%ld dest_tablet_id=%ld",
                              param_.ls_id_.id(), param_.source_tablet_id_.id(), param_.dest_tablet_id_.id()))) {
    LOG_WARN("fill dag key for ddl table merge dag failed", K(ret), K(param_));
  }
  return ret;
}

ObComplementPrepareTask::ObComplementPrepareTask()
  : ObITask(TASK_TYPE_COMPLEMENT_PREPARE), is_inited_(false), param_(nullptr), context_(nullptr)
{
}

ObComplementPrepareTask::~ObComplementPrepareTask()
{
}

int ObComplementPrepareTask::init(ObComplementDataParam &param, ObComplementDataContext &context)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(is_inited_)) {
    ret = OB_INIT_TWICE;
    LOG_WARN("ObComplementPrepareTask has already been inited", K(ret));
  } else if (OB_UNLIKELY(!param.is_valid())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid arguments", K(ret), K(param), K(context));
  } else {
    param_ = &param;
    context_ = &context;
    is_inited_ = true;
  }
  return ret;
}

int ObComplementPrepareTask::process()
{
  int ret = OB_SUCCESS;
  ObIDag *tmp_dag = get_dag();
  ObComplementDataDag *dag = nullptr;
  ObComplementWriteTask *write_task = nullptr;
  ObComplementMergeTask *merge_task = nullptr;
  LOG_WARN("start to process ObComplementPrepareTask", K(ret));
  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("ObComplementPrepareTask has not been inited", K(ret));
  } else if (OB_ISNULL(tmp_dag) || ObDagType::DAG_TYPE_DDL != tmp_dag->get_type()) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("dag is invalid", K(ret), KP(tmp_dag));
  } else if (FALSE_IT(dag = static_cast<ObComplementDataDag *>(tmp_dag))) {
  } else if (OB_FAIL(dag->prepare_context())) {
    LOG_WARN("prepare complement context failed", K(ret));
  } else if (context_->is_major_sstable_exist_) {
    FLOG_INFO("major sstable exists, all task should finish", K(ret), K(*param_));
  } else if (OB_FAIL(context_->write_start_log(*param_))) {
    LOG_WARN("write start log failed", K(ret), KPC(param_));
  } else {
    LOG_INFO("finish the complement prepare task", K(ret));
  }

  if (OB_FAIL(ret)) {
    context_->complement_data_ret_ = ret;
    ret = OB_SUCCESS;
  }
  return ret;
}

ObComplementWriteTask::ObComplementWriteTask()
  : ObITask(TASK_TYPE_COMPLEMENT_WRITE), is_inited_(false), task_id_(0), param_(nullptr),
    context_(nullptr), write_row_(),
    col_ids_(), org_col_ids_(), output_projector_()
{
}

ObComplementWriteTask::~ObComplementWriteTask()
{
}

int ObComplementWriteTask::init(const int64_t task_id, ObComplementDataParam &param,
    ObComplementDataContext &context)
{
  int ret = OB_SUCCESS;
  ObSchemaGetterGuard schema_guard;
  const ObTableSchema *hidden_table_schema = nullptr;
  if (OB_UNLIKELY(is_inited_)) {
    ret = OB_INIT_TWICE;
    LOG_WARN("ObComplementWriteTask has already been inited", K(ret));
  } else if (task_id < 0 || !param.is_valid()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid arguments", K(ret), K(task_id), K(param), K(context));
  } else if (OB_FAIL(ObMultiVersionSchemaService::get_instance().get_tenant_schema_guard(
             param.tenant_id_, schema_guard, param.schema_version_))) {
    LOG_WARN("fail to get tenant schema guard", K(ret), K(param));
  } else if (OB_FAIL(schema_guard.get_table_schema(param.tenant_id_,
             param.dest_table_id_, hidden_table_schema))) {
    LOG_WARN("fail to get hidden table schema", K(ret), K(param));
  } else if (OB_ISNULL(hidden_table_schema)) {
    ret = OB_TABLE_NOT_EXIST;
    LOG_WARN("hidden table schema not exist", K(ret), K(param));
  } else if (OB_FAIL(write_row_.init(
              param.allocator_, hidden_table_schema->get_column_count() + storage::ObMultiVersionRowkeyHelpper::get_extra_rowkey_col_cnt()))) {
    LOG_WARN("Fail to init write row", K(ret));
  } else {
    write_row_.row_flag_.set_flag(ObDmlFlag::DF_INSERT);
    task_id_ = task_id;
    param_ = &param;
    context_ = &context;
    is_inited_ = true;
  }
  return ret;
}

int ObComplementWriteTask::process()
{
  int ret = OB_SUCCESS;
  MAKE_TENANT_SWITCH_SCOPE_GUARD(guard);
  ObIDag *tmp_dag = get_dag();
  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("ObComplementWriteTask has not been inited before", K(ret));
  } else if (OB_ISNULL(tmp_dag) || ObDagType::DAG_TYPE_DDL != tmp_dag->get_type()) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("dag is invalid", K(ret), KP(tmp_dag));
  } else if (OB_SUCCESS != (context_->complement_data_ret_)) {
    LOG_WARN("complement data has already failed", "ret", context_->complement_data_ret_);
  } else if (context_->is_major_sstable_exist_) {
  } else if (OB_FAIL(guard.switch_to(param_->tenant_id_))) {
    LOG_WARN("switch to tenant failed", K(ret), K(param_->tenant_id_));
  } else if (OB_FAIL(local_scan_by_range())) {
    LOG_WARN("fail to do local scan by range", K(ret), K(task_id_));
  } else {
    LOG_INFO("finish the complement write task", K(ret));
  }

  if (OB_FAIL(ret) && OB_NOT_NULL(context_)) {
    context_->complement_data_ret_ = ret;
    ret = OB_SUCCESS;
  }
  return ret;
}

int ObComplementWriteTask::generate_next_task(ObITask *&next_task)
{
  int ret = OB_SUCCESS;
  ObIDag *tmp_dag = get_dag();
  ObComplementDataDag *dag = nullptr;
  ObComplementWriteTask *write_task = nullptr;
  const int64_t next_task_id = task_id_ + 1;
  next_task = nullptr;
  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("ObComplementWriteTask has not been inited", K(ret));
  } else if (next_task_id == param_->concurrent_cnt_) {
    ret = OB_ITER_END;
  } else if (OB_ISNULL(tmp_dag)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("error unexpected, dag must not be NULL", K(ret));
  } else if (OB_UNLIKELY(ObDagType::DAG_TYPE_DDL != tmp_dag->get_type())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("error unexpected, dag type is invalid", K(ret), "dag type", dag->get_type());
  } else if (FALSE_IT(dag = static_cast<ObComplementDataDag *>(tmp_dag))) {
  } else if (OB_FAIL(dag->alloc_task(write_task))) {
    LOG_WARN("fail to alloc task", K(ret));
  } else if (OB_FAIL(write_task->init(next_task_id, *param_, *context_))) {
    LOG_WARN("fail to init complement write task", K(ret));
  } else {
    next_task = write_task;
    LOG_INFO("generate next complement write task", K(ret), K(param_->dest_table_id_));
  }
  if (OB_FAIL(ret) && OB_NOT_NULL(context_)) {
    if (OB_ITER_END != ret) {
      context_->complement_data_ret_ = ret;
    }
  }
  return ret;
}

//generate col_ids and projector based on table_schema
int ObComplementWriteTask::generate_col_param()
{
  int ret = OB_SUCCESS;
  col_ids_.reuse();
  org_col_ids_.reuse();
  output_projector_.reuse();
  ObArray<ObColDesc> tmp_col_ids;
  ObSchemaGetterGuard schema_guard;
  const ObTableSchema *data_table_schema = nullptr;
  const ObTableSchema *hidden_table_schema = nullptr;
  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("not init", K(ret));
  } else if (OB_FAIL(ObMultiVersionSchemaService::get_instance().get_tenant_schema_guard(
             param_->tenant_id_, schema_guard, param_->schema_version_))) {
    LOG_WARN("fail to get tenant schema guard", K(ret), KPC(param_));
  } else if (OB_FAIL(schema_guard.get_table_schema(param_->tenant_id_,
             param_->source_table_id_, data_table_schema))) {
    LOG_WARN("fail to get data table schema", K(ret), K(arg));
  } else if (OB_ISNULL(data_table_schema)) {
    ret = OB_TABLE_NOT_EXIST;
    LOG_WARN("data table schema not exist", K(ret), K(arg));
  } else if (OB_FAIL(schema_guard.get_table_schema(param_->tenant_id_,
             param_->dest_table_id_, hidden_table_schema))) {
    LOG_WARN("fail to get hidden table schema", K(ret), KPC(param_));
  } else if (OB_ISNULL(hidden_table_schema)) {
    ret = OB_TABLE_NOT_EXIST;
    LOG_WARN("hidden table schema not exist", K(ret), KPC(param_));
  } else if (OB_FAIL(hidden_table_schema->get_store_column_ids(tmp_col_ids, false))) {
    LOG_WARN("fail to get column ids", K(ret), KPC(hidden_table_schema));
  } else if (OB_FAIL(org_col_ids_.assign(tmp_col_ids))) {
    LOG_WARN("fail to assign col descs", K(ret), K(tmp_col_ids));
  } else {
    // generate col_ids
    for (int64_t i = 0; OB_SUCC(ret) && i < tmp_col_ids.count(); i++) {
      const uint64_t hidden_column_id = tmp_col_ids.at(i).col_id_;
      const ObColumnSchemaV2 *hidden_column_schema = hidden_table_schema->get_column_schema(hidden_column_id);
      if (OB_ISNULL(hidden_column_schema)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("unexpected null column schema", K(ret), K(hidden_column_id));
      } else {
        const ObString &hidden_column_name = hidden_column_schema->get_column_name_str();
        const ObColumnSchemaV2 *data_column_schema = data_table_schema->get_column_schema(hidden_column_name);
        ObColDesc tmp_col_desc = tmp_col_ids.at(i);
        if (nullptr == data_column_schema) {
          // may be newly added column, can not find in data table.
        } else if (FALSE_IT(tmp_col_desc.col_id_ = data_column_schema->get_column_id())) {
        } else if (OB_FAIL(col_ids_.push_back(tmp_col_desc))) {
          LOG_WARN("fail to push back col desc", K(ret), K(tmp_col_ids.at(i)), K(tmp_col_desc));
        }
      }
    }
  }
  // generate output_projector.
  if (OB_FAIL(ret)) {
  } else {
    // notice that, can not find newly added column, get the row firstly, and then resolve it.
    for (int64_t i = 0; OB_SUCC(ret) && i < tmp_col_ids.count(); i++) {
      const ObColumnSchemaV2 *hidden_column_schema = hidden_table_schema->get_column_schema(tmp_col_ids.at(i).col_id_);
      const ObString &hidden_column_name = hidden_column_schema->get_column_name_str();
      for (int64_t j = 0; OB_SUCC(ret) && j < col_ids_.count(); j++) {
        const ObColumnSchemaV2 *data_column_schema = data_table_schema->get_column_schema(col_ids_.at(j).col_id_);
        if (nullptr == data_column_schema) {
          // may be newly added column.
        } else if (hidden_column_name == data_column_schema->get_column_name_str()) {
          if (OB_FAIL(output_projector_.push_back(static_cast<int32_t>(j)))) {
            LOG_WARN("fail to push back output projector", K(ret));
          }
          break;
        }
      }
    }
  }
  if (OB_FAIL(ret)) {
  } else if (OB_UNLIKELY(col_ids_.count() != output_projector_.count())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("unexpected error", K(ret), K_(col_ids), K_(output_projector));
  }
  return ret;
}

//For reordering column operations, such as drop column or add column after, we need to rewrite all
//storage data based on the newest table schema.
int ObComplementWriteTask::local_scan_by_range()
{
  int ret = OB_SUCCESS;
  int64_t start_time = ObTimeUtility::current_time();
  int64_t concurrent_cnt = 0;
  if (OB_ISNULL(param_) || OB_UNLIKELY(!param_->is_valid())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid arguments", K(ret), K(idx), KP(param_));
  } else {
    concurrent_cnt = param_->concurrent_cnt_;
    LOG_INFO("start to do local scan by range", K(task_id_), K(concurrent_cnt), KPC(param_));
  }
  if (OB_FAIL(ret)) {
    // do nothing
  } else if (OB_FAIL(generate_col_param())) {
    LOG_WARN("fail to get column ids", K(ret));
  } else if (OB_FAIL(do_local_scan())) {
    LOG_WARN("fail to do local scan", K(ret), K_(col_ids), K_(org_col_ids));
  } else {
    int64_t cost_time = ObTimeUtility::current_time() - start_time;
    LOG_INFO("finish local scan by range", K(ret), K(cost_time), K(task_id_), K(concurrent_cnt));
  }
  return ret;
}

int ObComplementWriteTask::do_local_scan()
{
  int ret = OB_SUCCESS;
  int end_trans_ret = OB_SUCCESS;
  SMART_VAR(ObLocalScan, local_scan) {
    ObQueryFlag query_flag(ObQueryFlag::Forward,
        true, /*is daily merge scan*/
        true, /*is read multiple macro block*/
        false, /*sys task scan, read one macro block in single io*/
        false /*is full row scan?*/,
        false,
        false);
    ObStoreRange range;
    ObArenaAllocator allocator;
    ObDatumRange datum_range;
    const bool allow_not_ready = false;
    ObLSHandle ls_handle;
    ObTabletTableIterator iterator;
    ObSSTable *sstable = nullptr;
    ObTransService *trans_service = nullptr;
    ObSEArray<ObITable *, MAX_SSTABLE_CNT_IN_STORAGE> sstables;
    const uint64_t tenant_id = param_->tenant_id_;
    ObTxDesc *read_tx_desc = nullptr; // for reading lob column from aux_lob_table by table_scan

    if (OB_FAIL(MTL(ObLSService *)->get_ls(param_->ls_id_, ls_handle, ObLSGetMod::DDL_MOD))) {
      LOG_WARN("fail to get log stream", K(ret), KPC(param_));
    } else if (OB_UNLIKELY(nullptr == ls_handle.get_ls())) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("ls is null", K(ret), K(ls_handle));
    } else if (OB_FAIL(ls_handle.get_ls()->get_tablet_svr()->get_read_tables(param_->source_tablet_id_,
            param_->snapshot_version_, iterator, allow_not_ready))) {
      if (OB_REPLICA_NOT_READABLE == ret) {
        ret = OB_EAGAIN;
      } else {
        LOG_WARN("snapshot version has been discarded", K(ret));
      }
    } else if (OB_ISNULL(trans_service = MTL(ObTransService*))) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("trans_service is null", K(ret));
    } else if (OB_FAIL(param_->ranges_.at(task_id_, range))) {
      LOG_WARN("fail to get range", K(ret));
    } else if (OB_FAIL(datum_range.from_range(range, allocator))) {
      STORAGE_LOG(WARN, "Failed to transfer datum range", K(ret), K(range));
    } else {
      ObSchemaGetterGuard schema_guard;
      const ObTableSchema *data_table_schema = nullptr;
      const ObTableSchema *hidden_table_schema = nullptr;
      if (OB_FAIL(ObMultiVersionSchemaService::get_instance().get_tenant_schema_guard(
                tenant_id, schema_guard, param_->schema_version_))) {
        LOG_WARN("fail to get tenant schema guard", K(ret), KPC(param_));
      } else if (OB_FAIL(schema_guard.get_table_schema(tenant_id,
                param_->source_table_id_, data_table_schema))) {
        LOG_WARN("fail to get data table schema", K(ret), K(arg));
      } else if (OB_ISNULL(data_table_schema)) {
        ret = OB_TABLE_NOT_EXIST;
        LOG_WARN("data table schema not exist", K(ret), K(arg));
      } else if (OB_FAIL(schema_guard.get_table_schema(tenant_id,
                param_->dest_table_id_, hidden_table_schema))) {
        LOG_WARN("fail to get hidden table schema", K(ret), KPC(param_));
      } else if (OB_ISNULL(hidden_table_schema)) {
        ret = OB_TABLE_NOT_EXIST;
        LOG_WARN("hidden table schema not exist", K(ret), KPC(param_));
      } else if (OB_FAIL(local_scan.init(col_ids_,
                                        org_col_ids_,
                                        output_projector_,
                                        *data_table_schema,
                                        param_->snapshot_version_,
                                        trans_service,
                                        *hidden_table_schema,
                                        false/*output all columns of hidden table*/))) {
        LOG_WARN("fail to init local scan param", K(ret), K(*param_));
      } else if (OB_FAIL(ObInsertLobColumnHelper::start_trans(
          param_->ls_id_, true/*is_for_read*/, INT64_MAX, read_tx_desc))) {
        LOG_WARN("fail to get tx_desc", K(ret));
      } else if (OB_FAIL(local_scan.table_scan(*data_table_schema,
                                               param_->ls_id_,
                                               param_->source_tablet_id_,
                                               iterator,
                                               query_flag,
                                               datum_range, read_tx_desc))) {
        LOG_WARN("fail to do table scan", K(ret));
      }
    }

    if (FAILEDx(append_row(local_scan))) {
      LOG_WARN("append row failed", K(ret));
    }

    const int64_t timeout_ts = ObTimeUtility::current_time() + 3000000; // 3s
    if (nullptr != read_tx_desc) {
      if (OB_SUCCESS != (end_trans_ret = ObInsertLobColumnHelper::end_trans(read_tx_desc, OB_SUCCESS != ret, timeout_ts))) {
        LOG_WARN("fail to end read trans", K(ret), K(end_trans_ret));
        ret = end_trans_ret;
      }
    }
  }

  return ret;
}

int ObComplementWriteTask::add_extra_rowkey(const int64_t rowkey_cnt, const int64_t extra_rowkey_cnt, const blocksstable::ObDatumRow &row)
{
  int ret = OB_SUCCESS;
  int64_t rowkey_column_count = rowkey_cnt;
  if (OB_UNLIKELY(write_row_.get_capacity() < row.count_ + extra_rowkey_cnt ||
                  row.count_ < rowkey_column_count)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("Unexpected row", K(ret), K(write_row_), K(row.count_), K(rowkey_column_count));
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < row.count_; i++) {
      if (i < rowkey_column_count) {
        write_row_.storage_datums_[i] = row.storage_datums_[i];
      } else {
        write_row_.storage_datums_[i + extra_rowkey_cnt] = row.storage_datums_[i];
      }
    }
    write_row_.storage_datums_[rowkey_column_count].set_int(-param_->snapshot_version_);
    write_row_.storage_datums_[rowkey_column_count + 1].set_int(0);
    write_row_.count_ = row.count_ + extra_rowkey_cnt;
  }
  return ret;
}

int ObComplementWriteTask::append_row(ObLocalScan &local_scan)
{
  int ret = OB_SUCCESS;
  ObDataStoreDesc data_desc;
  HEAP_VAR(ObMacroBlockWriter, writer) {
    ObArray<int64_t> report_col_checksums;
    ObArray<int64_t> report_col_ids;
    ObDDLSSTableRedoWriter sstable_redo_writer;
    ObDDLRedoLogWriterCallback callback;
    ObITable::TableKey hidden_table_key;
    ObMacroDataSeq macro_start_seq(0);
    int64_t get_next_row_time = 0;
    int64_t append_row_time = 0;
    int64_t t1 = 0;
    int64_t t2 = 0;
    int64_t t3 = 0;
    int64_t lob_cnt = 0;
    ObArenaAllocator lob_allocator(ObModIds::OB_LOB_ACCESS_BUFFER, OB_MALLOC_NORMAL_BLOCK_SIZE, MTL_ID());
    ObStoreRow reshaped_row;
    reshaped_row.flag_.set_flag(ObDmlFlag::DF_INSERT);
    ObArenaAllocator allocator(lib::ObLabel("CompDataTaskTmp"));
    ObRowReshape *reshape_ptr = nullptr;
    ObSQLMode sql_mode_for_ddl_reshape = SMO_TRADITIONAL;
    ObDatumRow datum_row;
    int64_t rowkey_column_cnt = 0;
    const int64_t extra_rowkey_cnt = storage::ObMultiVersionRowkeyHelpper::get_extra_rowkey_col_cnt();
    bool ddl_committed = false;
    if (OB_UNLIKELY(!is_inited_)) {
      ret = OB_NOT_INIT;
      LOG_WARN("ObComplementWriteTask is not inited", K(ret));
    } else if (OB_ISNULL(param_) || OB_UNLIKELY(!param_->is_valid()) || OB_ISNULL(context_)) {
      ret = OB_INVALID_ARGUMENT;
      LOG_WARN("invalid arguments", K(param_), KP(context_), K(ret));
    } else if (OB_FAIL(macro_start_seq.set_parallel_degree(task_id_))) {
      LOG_WARN("set parallel degree failed", K(ret), K(task_id_));
    } else {
      ObSchemaGetterGuard schema_guard;
      const ObTableSchema *hidden_table_schema = nullptr;
      if (OB_FAIL(ObMultiVersionSchemaService::get_instance().get_tenant_schema_guard(
                param_->tenant_id_, schema_guard, param_->schema_version_))) {
        LOG_WARN("fail to get tenant schema guard", K(ret), KPC(param_));
      } else if (OB_FAIL(schema_guard.get_table_schema(param_->tenant_id_,
                param_->dest_table_id_, hidden_table_schema))) {
        LOG_WARN("fail to get hidden table schema", K(ret), KPC(param_));
      } else if (OB_ISNULL(hidden_table_schema)) {
        ret = OB_TABLE_NOT_EXIST;
        LOG_WARN("table not exist", K(ret), K(param_->tenant_id_), K(param_->dest_table_id_));
      } else if (OB_FAIL(data_desc.init(*hidden_table_schema,
                                        param_->ls_id_,
                                        param_->dest_tablet_id_,
                                        MAJOR_MERGE,
                                        param_->snapshot_version_,
                                        param_->data_format_version_))) {
        LOG_WARN("fail to init data store desc", K(ret), K(*param_), K(param_->dest_tablet_id_));
      } else if (FALSE_IT(data_desc.sstable_index_builder_ = context_->index_builder_)) {
      } else if (FALSE_IT(data_desc.is_ddl_ = true)) {
      } else if (OB_FAIL(param_->get_hidden_table_key(hidden_table_key))) {
        LOG_WARN("fail to get hidden table key", K(ret), K(*param_));
      } else if (OB_UNLIKELY(!hidden_table_key.is_valid())) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("hidden table key is invalid", K(ret), K(hidden_table_key));
      } else if (OB_FAIL(sstable_redo_writer.init(param_->ls_id_, param_->dest_tablet_id_))) {
        LOG_WARN("fail to init sstable redo writer", K(ret));
      } else if (OB_UNLIKELY(nullptr == static_cast<ObComplementDataDag *>(get_dag()))) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("the dag of this task is null", K(ret));
      } else if (FALSE_IT(sstable_redo_writer.set_start_scn(
          static_cast<ObComplementDataDag *>(get_dag())->get_context().data_sstable_redo_writer_.get_start_scn()))) {
      } else if (OB_FAIL(callback.init(DDL_MB_DATA_TYPE, hidden_table_key, param_->task_id_, &sstable_redo_writer, context_->ddl_kv_mgr_handle_))) {
        LOG_WARN("fail to init data callback", K(ret), K(hidden_table_key));
      } else if (OB_FAIL(writer.open(data_desc, macro_start_seq, &callback))) {
        LOG_WARN("fail to open macro block writer", K(ret), K(data_desc));
      } else {
        rowkey_column_cnt = hidden_table_schema->get_rowkey_column_num();
      }

      ObRelativeTable relative_table;
      ObTableSchemaParam schema_param(allocator);
      // Hack to prevent row reshaping from converting empty string to null.
      //
      // Supposing we have a row of type varchar with some spaces and an index on this column,
      // and then we convert this column to char. In this case, the DDL routine will first rebuild
      // the data table and then rebuilding the index table. The row may be reshaped as follows.
      //
      // - without hack: '  '(varchar) => ''(char) => null(char)
      // - with hack: '  '(varchar) => ''(char) => ''(char)
      if (OB_FAIL(ret)) {
      } else if (OB_FAIL(schema_param.convert(hidden_table_schema))) {
        LOG_WARN("failed to convert schema param", K(ret));
        if (OB_SCHEMA_ERROR == ret) {
          ret = OB_CANCELED;
        }
      } else if (OB_FAIL(relative_table.init(&schema_param, param_->dest_tablet_id_))) {
        LOG_WARN("fail to init relative_table", K(ret), K(schema_param), K(param_->dest_tablet_id_));
      } else if (OB_FAIL(ObRowReshapeUtil::malloc_rows_reshape_if_need(
                    allocator, data_desc.col_desc_array_, 1, relative_table, sql_mode_for_ddl_reshape, reshape_ptr))) {
        LOG_WARN("failed to malloc row reshape", K(ret));
      } else if (OB_FAIL(datum_row.init(allocator, data_desc.col_desc_array_.count()))) {
        LOG_WARN("failed to init datum row", K(ret), K(data_desc.col_desc_array_));
      }
    }

    while (OB_SUCC(ret)) {      //get each row from row_iter
      const ObDatumRow *tmp_row = nullptr;
      ObStoreRow tmp_store_row;
      ObColumnChecksumCalculator *checksum_calculator = nullptr;
      t1 = ObTimeUtility::current_time();
      dag_yield();
      if (OB_FAIL(local_scan.get_next_row(tmp_row))) {
        if (OB_UNLIKELY(OB_ITER_END != ret)) {
          LOG_WARN("fail to get next row", K(ret));
        }
      }
      for (int64_t i = 0; OB_SUCC(ret) && i < org_col_ids_.count(); i++) {
        ObStorageDatum &datum = tmp_row->storage_datums_[i];
        if (datum.is_nop() || datum.is_null()) {
          // do nothing
        } else if (org_col_ids_.at(i).col_type_.is_lob_storage()) {
          lob_cnt++;
          const int64_t timeout_ts = ObTimeUtility::current_time() + 60000000; // 60s
          if (OB_FAIL(ObInsertLobColumnHelper::insert_lob_column(
              lob_allocator, param_->ls_id_, param_->dest_tablet_id_, org_col_ids_.at(i), datum, timeout_ts, true))) {
            LOG_WARN("fail to insert_lob_col", K(ret), K(datum));
          }
        }
      }
      if (OB_FAIL(ret)) {
        // do nothing
      } else if (OB_FAIL(add_extra_rowkey(rowkey_column_cnt, extra_rowkey_cnt, *tmp_row))) {
        LOG_WARN("fail to add extra rowkey", K(ret));
      } else if (OB_FAIL(write_row_.to_store_row(data_desc.col_desc_array_, tmp_store_row))) {
      } else if (OB_FAIL(ObRowReshapeUtil::reshape_table_rows(
          &tmp_store_row.row_val_, reshape_ptr, data_desc.col_desc_array_.count(), &reshaped_row, 1, sql_mode_for_ddl_reshape))) {
        LOG_WARN("failed to malloc and reshape row", K(ret));
      } else if (OB_FAIL(datum_row.from_store_row(reshaped_row))) {
        STORAGE_LOG(WARN, "Failed to transfer store row ", K(ret), K(reshaped_row));
      } else {
        t2 = ObTimeUtility::current_time();
        get_next_row_time += t2 - t1;
        context_->row_scanned_++;
        if (!ddl_committed && OB_FAIL(writer.append_row(datum_row))) {
          LOG_WARN("fail to append row to macro block", K(ret), K(datum_row));
          if (OB_TRANS_COMMITED == ret) {
            ret = OB_SUCCESS;
            ddl_committed = true;
          }
        }
        if (OB_FAIL(ret)) {
        } else if (OB_ISNULL(checksum_calculator = local_scan.get_checksum_calculator())) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("checksum calculator is nullptr", K(ret), KP(checksum_calculator));
        } else if (OB_FAIL(checksum_calculator->calc_column_checksum(data_desc.col_desc_array_, &write_row_, nullptr, nullptr))) {
          LOG_WARN("fail to calc column checksum", K(ret), K(write_row_));
        } else {
          t3 = ObTimeUtility::current_time();
          append_row_time += t3 - t2;
          context_->row_inserted_++;
        }
        if (lob_cnt % 128 == 0) {
          lob_allocator.reuse(); // reuse after append_row to macro block to save memory
        }
      }
    }
    if (OB_ITER_END == ret) {
      ret = OB_SUCCESS;
    }
    LOG_INFO("print append row to macro block cost time", K(ret), K(task_id_), K(context_->row_inserted_),
        K(get_next_row_time), K(append_row_time));
    ObRowReshapeUtil::free_row_reshape(allocator, reshape_ptr, 1);
    if (OB_FAIL(ret)) {
    } else if (!ddl_committed && OB_FAIL(writer.close())) {
      if (OB_TRANS_COMMITED == ret) {
        ret = OB_SUCCESS;
        ddl_committed = true;
      } else {
        LOG_WARN("fail to close writer", K(ret));
      }
    }
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(local_scan.get_origin_table_checksum(report_col_checksums, report_col_ids))) {
      LOG_WARN("fail to get origin table columns checksum", K(ret));
    } else if (OB_FAIL(ObDDLChecksumOperator::update_checksum(param_->tenant_id_,
                                                              param_->source_table_id_,
                                                              param_->task_id_,
                                                              report_col_checksums,
                                                              report_col_ids,
                                                              1/*execution_id*/,
                                                              param_->tablet_task_id_ << 48 | task_id_,
                                                              *GCTX.sql_proxy_))) {
      LOG_WARN("fail to report origin table checksum", K(ret));
    } else {/* do nothing. */}
  }
  return ret;
}

ObComplementMergeTask::ObComplementMergeTask()
  : ObITask(TASK_TYPE_COMPLEMENT_MERGE), is_inited_(false), param_(nullptr), context_(nullptr)
{
}

ObComplementMergeTask::~ObComplementMergeTask()
{
}

int ObComplementMergeTask::init(ObComplementDataParam &param, ObComplementDataContext &context)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(is_inited_)) {
    ret = OB_INIT_TWICE;
    LOG_WARN("ObComplementMergeTask has already been inited", K(ret));
  } else if (!param.is_valid()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid arguments", K(ret), K(param), K(context));
  } else {
    param_ = &param;
    context_ = &context;
    is_inited_ = true;
  }
  return ret;
}

int ObComplementMergeTask::process()
{
  int ret = OB_SUCCESS;
  MAKE_TENANT_SWITCH_SCOPE_GUARD(guard);
  int tmp_ret = OB_SUCCESS;
  ObIDag *tmp_dag = get_dag();
  ObComplementDataDag *dag = nullptr;
  ObLSHandle ls_handle;
  ObTabletHandle tablet_handle;
  ObTablet *tablet = nullptr;
  if (OB_ISNULL(tmp_dag) || ObDagType::DAG_TYPE_DDL != tmp_dag->get_type()) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("dag is invalid", K(ret), KP(tmp_dag));
  } else if (FALSE_IT(dag = static_cast<ObComplementDataDag *>(tmp_dag))) {
  } else if (OB_SUCCESS != (context_->complement_data_ret_)) {
    LOG_WARN("complement data has already failed", "ret", context_->complement_data_ret_);
  } else if (OB_FAIL(guard.switch_to(param_->tenant_id_))) {
    LOG_WARN("switch to tenant failed", K(ret), K(param_->tenant_id_));
  } else if (context_->is_major_sstable_exist_) {
    if (OB_FAIL(MTL(ObLSService *)->get_ls(param_->ls_id_, ls_handle, ObLSGetMod::DDL_MOD))) {
      LOG_WARN("failed to get log stream", K(ret), K(*param_));
    } else if (OB_FAIL(ObDDLUtil::ddl_get_tablet(ls_handle,
                                                 param_->dest_tablet_id_,
                                                 tablet_handle,
                                                 ObTabletCommon::NO_CHECK_GET_TABLET_TIMEOUT_US))) {
      LOG_WARN("get tablet handle failed", K(ret), K(*param_));
    } else if (OB_ISNULL(tablet_handle.get_obj())) {
      ret = OB_ERR_SYS;
      LOG_WARN("tablet handle is null", K(ret), K(*param_));
    } else {
      const ObSSTable *first_major_sstable = static_cast<ObSSTable *>(
        tablet_handle.get_obj()->get_table_store().get_major_sstables().get_boundary_table(false/*first*/));
      if (OB_ISNULL(first_major_sstable)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("unexpected error, major sstable shoud not be null", K(ret), K(*param_));
      } else if (OB_FAIL(ObTabletDDLUtil::report_ddl_checksum(param_->ls_id_,
                                                              param_->dest_tablet_id_,
                                                              param_->dest_table_id_,
                                                              1 /* execution_id */,
                                                              param_->task_id_,
                                                              first_major_sstable->get_meta().get_col_checksum()))) {
        LOG_WARN("report ddl column checksum failed", K(ret), K(*param_));
      } else if (OB_FAIL(GCTX.ob_service_->submit_tablet_update_task(param_->tenant_id_, param_->ls_id_, param_->dest_tablet_id_))) {
        LOG_WARN("fail to submit tablet update task", K(ret), K(*param_));
      }
    }
  } else if (OB_FAIL(add_build_hidden_table_sstable())) {
    LOG_WARN("fail to build new sstable and write macro redo", K(ret));
  }
  if (OB_FAIL(ret) && OB_NOT_NULL(context_)) {
    context_->complement_data_ret_ = ret;
    ret = OB_SUCCESS;
  }
  if (OB_NOT_NULL(dag) &&
    OB_SUCCESS != (tmp_ret = dag->report_replica_build_status())) {
    // do not override ret if it has already failed.
    ret = OB_SUCCESS == ret ? tmp_ret : ret;
    LOG_WARN("fail to report replica build status", K(ret), K(tmp_ret));
  }
  return ret;
}

int ObComplementMergeTask::add_build_hidden_table_sstable()
{
  int ret = OB_SUCCESS;
  ObLSHandle ls_handle;
  ObITable::TableKey hidden_table_key;
  SCN commit_scn;
  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("ObComplementMergetask has not been inited", K(ret));
  } else if (OB_ISNULL(param_)
      || OB_ISNULL(context_)
      || OB_UNLIKELY(!param_->is_valid())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("error unexpected", K(ret), KP(param_), KP(context_));
  } else if (OB_FAIL(MTL(ObLSService *)->get_ls(param_->ls_id_, ls_handle, ObLSGetMod::DDL_MOD))) {
    LOG_WARN("failed to get log stream", K(ret), K(param_->ls_id_));
  } else if (OB_FAIL(param_->get_hidden_table_key(hidden_table_key))) {
    LOG_WARN("fail to get hidden table key", K(ret), K(hidden_table_key));
  } else if (OB_FAIL(context_->data_sstable_redo_writer_.end_ddl_redo_and_create_ddl_sstable(
      ls_handle, hidden_table_key, param_->dest_table_id_, param_->execution_id_, param_->task_id_))) {
    LOG_WARN("failed to end ddl redo", K(ret));
  }
  return ret;
}

/**
 * -----------------------------------ObLocalScan-----------------------------------------
 */

ObLocalScan::ObLocalScan() : is_inited_(false), tenant_id_(OB_INVALID_TENANT_ID), source_table_id_(OB_INVALID_ID),
    dest_table_id_(OB_INVALID_ID), schema_version_(0), extended_gc_(), snapshot_version_(common::OB_INVALID_VERSION),
    txs_(nullptr), default_row_(), tmp_row_(), row_iter_(nullptr), scan_merge_(nullptr), ctx_(), access_param_(),
    access_ctx_(), get_table_param_(), allocator_("ObLocalScan"), calc_buf_(ObModIds::OB_SQL_EXPR_CALC),
    col_params_(), read_info_(), exist_column_mapping_(allocator_), checksum_calculator_()
{}

ObLocalScan::~ObLocalScan()
{
  if (OB_NOT_NULL(scan_merge_)) {
    scan_merge_->~ObMultipleScanMerge();
    scan_merge_ = NULL;
  }
  for (int64_t i = 0; i < col_params_.count(); i++) {
    ObColumnParam *&tmp_col_param = col_params_.at(i);
    if (OB_NOT_NULL(tmp_col_param)) {
      tmp_col_param->~ObColumnParam();
      allocator_.free(tmp_col_param);
      tmp_col_param = nullptr;
    }
  }
  default_row_.reset();
  tmp_row_.reset();
  access_ctx_.reset();
}

int ObLocalScan::init(
    const ObIArray<share::schema::ObColDesc> &col_ids,
    const ObIArray<share::schema::ObColDesc> &org_col_ids,
    const ObIArray<int32_t> &projector,
    const ObTableSchema &data_table_schema,
    const int64_t snapshot_version,
    ObTransService *txs,
    const ObTableSchema &hidden_table_schema,
    const bool output_org_cols_only)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(is_inited_)) {
    ret = OB_INIT_TWICE;
    LOG_WARN("ObLocalScan has been initialized before", K(ret));
  } else if (org_col_ids.count() < 1 || col_ids.count() < 1 || projector.count() < 1
      || !data_table_schema.is_valid() || !hidden_table_schema.is_valid() || snapshot_version < 1 || OB_ISNULL(txs)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid auguments", K(ret), K(data_table_schema), K(hidden_table_schema),
        K(col_ids), K(org_col_ids), K(projector), K(snapshot_version));
  } else {
    snapshot_version_ = snapshot_version;
    txs_ = txs;
    output_org_cols_only_ = output_org_cols_only;
    if (OB_FAIL(check_generated_column_exist(hidden_table_schema, org_col_ids))) {
      LOG_WARN("fail to init generated columns", K(ret), K(org_col_ids));
    } else if (OB_FAIL(extended_gc_.extended_col_ids_.assign(col_ids))) {
      LOG_WARN("fail to assign", K(ret));
    } else if (OB_FAIL(extended_gc_.org_extended_col_ids_.assign(org_col_ids))) {
      LOG_WARN("fail to assign", K(ret));
    } else if (OB_FAIL(extended_gc_.output_projector_.assign(projector))) {
      LOG_WARN("fail to assign", K(ret));
    } else if (OB_FAIL(default_row_.init(allocator_, org_col_ids.count()))) {
      STORAGE_LOG(WARN, "Failed to init datum row", K(ret));
    } else if (OB_FAIL(tmp_row_.init(allocator_, org_col_ids.count()))) {
      STORAGE_LOG(WARN, "Failed to init datum row", K(ret));
    } else if (OB_FAIL(get_exist_column_mapping(data_table_schema, hidden_table_schema))){
      LOG_WARN("fail to init positions for resolving row", K(ret));
    } else if (OB_FAIL(checksum_calculator_.init(extended_gc_.org_extended_col_ids_.count()
            + storage::ObMultiVersionRowkeyHelpper::get_extra_rowkey_col_cnt()))) {
      LOG_WARN("fail to init checksum calculator", K(ret));
    } else {
      default_row_.row_flag_.set_flag(ObDmlFlag::DF_INSERT);
      tmp_row_.row_flag_.set_flag(ObDmlFlag::DF_INSERT);
      if (OB_FAIL(hidden_table_schema.get_orig_default_row(org_col_ids, default_row_))) {
        LOG_WARN("fail to get default row from table schema", K(ret));
      } else {
        tenant_id_ = data_table_schema.get_tenant_id();
        source_table_id_ = data_table_schema.get_table_id();
        dest_table_id_ = hidden_table_schema.get_table_id();
        schema_version_ = hidden_table_schema.get_schema_version();
        is_inited_ = true;
      }
    }
  }
  return ret;
}

int ObLocalScan::get_output_columns(
    const ObTableSchema &hidden_table_schema,
    ObIArray<ObColDesc> &col_ids)
{
  int ret = OB_SUCCESS;
  if (output_org_cols_only_) {
    if (OB_FAIL(col_ids.assign(extended_gc_.extended_col_ids_))) {
      LOG_WARN("assign tmp col ids failed", K(ret));
    }
  } else {
    if (OB_FAIL(hidden_table_schema.get_store_column_ids(col_ids, false))) {
      LOG_WARN("fail to get column ids", K(ret), K(hidden_table_schema));
    }
  }
  return ret;
}

// record the position of data table columns in hidden table by exist_column_mapping_.
int ObLocalScan::get_exist_column_mapping(
    const ObTableSchema &data_table_schema,
    const ObTableSchema &hidden_table_schema)
{
  int ret = OB_SUCCESS;
  void *buf = nullptr;
  ObArray<ObColDesc> tmp_col_ids;

  if (OB_FAIL(get_output_columns(hidden_table_schema, tmp_col_ids))) {
    LOG_WARN("get output columns failed", K(ret), K(hidden_table_schema));
  } else if (exist_column_mapping_.is_inited() && OB_FAIL(exist_column_mapping_.expand_size(tmp_col_ids.count()))) {
    LOG_WARN("fail to expand size of bitmap", K(ret));
  } else if (!exist_column_mapping_.is_inited() && OB_FAIL(exist_column_mapping_.init(tmp_col_ids.count(), false))) {
    LOG_WARN("fail to init exist column mapping", K(ret));
  } else {
    exist_column_mapping_.reuse(false);
    for (int64_t i = 0; OB_SUCC(ret) && i < tmp_col_ids.count(); i++) {
      const ObColumnSchemaV2 *hidden_column_schema = hidden_table_schema.get_column_schema(tmp_col_ids.at(i).col_id_);
      const ObString &hidden_column_name = hidden_column_schema->get_column_name_str();
      const ObColumnSchemaV2 *data_column_schema = data_table_schema.get_column_schema(hidden_column_name);
      if (nullptr == data_column_schema) {
        // newly added column, can not find in data table.
      } else if (OB_FAIL(exist_column_mapping_.set(i))) {
        LOG_WARN("fail to set bit map", K(ret), K(*data_column_schema));
      } else {/* do nothing. */}
    }
  }
  return ret;
}

int ObLocalScan::check_generated_column_exist(
    const ObTableSchema &hidden_table_schema,
    const ObIArray<share::schema::ObColDesc> &org_col_ids)
{
  int ret = OB_SUCCESS;
  for (int64_t i = 0; OB_SUCC(ret) && i < org_col_ids.count(); ++i) {
    const ObColumnSchemaV2 *column_schema = nullptr;
    if (OB_ISNULL(column_schema = hidden_table_schema.get_column_schema(org_col_ids.at(i).col_id_))) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("The column schema should not be null", K(ret), K(org_col_ids.at(i)));
    } else if (OB_UNLIKELY(column_schema->is_stored_generated_column())) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("unexpected error, table redefinition is selected for table with stored column", K(ret), K(*column_schema));
    } else {/* do nothing. */}
  }
  return ret;
}

int ObLocalScan::table_scan(
    const ObTableSchema &data_table_schema,
    const share::ObLSID &ls_id,
    const ObTabletID &tablet_id,
    ObTabletTableIterator &table_iter,
    ObQueryFlag &query_flag,
    blocksstable::ObDatumRange &range,
    transaction::ObTxDesc *tx_desc)
{
  int ret = OB_SUCCESS;
  const ObTableReadInfo &full_read_info = table_iter.tablet_handle_.get_obj()->get_full_read_info();
  if (OB_FAIL(construct_column_schema(data_table_schema))) {
    LOG_WARN("fail to construct column schema", K(ret), K(col_params_));
  } else if (OB_FAIL(construct_access_param(data_table_schema, tablet_id, full_read_info))) {
    LOG_WARN("fail to construct access param", K(ret), K(col_params_));
  } else if (OB_FAIL(construct_range_ctx(query_flag, ls_id, tx_desc))) {
    LOG_WARN("fail to construct range ctx", K(ret), K(query_flag));
  } else if (OB_FAIL(construct_multiple_scan_merge(table_iter, range))) {
    LOG_WARN("fail to construct multiple scan merge", K(ret), K(table_iter), K(range));
  } else if (OB_FAIL(ObLobManager::fill_lob_header(allocator_, extended_gc_.org_extended_col_ids_, default_row_))) {
    LOG_WARN("fail to fill lob header for default row", K(ret));
  }
  return ret;
}

//convert column schema to column param
int ObLocalScan::construct_column_schema(const ObTableSchema &data_table_schema)
{
  int ret = OB_SUCCESS;
  ObArray<ObColDesc> &extended_col_ids = extended_gc_.extended_col_ids_;
  for (int64_t i = 0; OB_SUCC(ret) && i < extended_col_ids.count(); i++) {
    const ObColumnSchemaV2 *col = data_table_schema.get_column_schema(extended_col_ids.at(i).col_id_);
    if (OB_ISNULL(col)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("fail to get column schema", K(ret), K(extended_col_ids.at(i).col_id_));
    } else {
      void *buf = allocator_.alloc(sizeof(ObColumnParam));
      ObColumnParam *tmp_col_param = nullptr;
      if (OB_ISNULL(buf)) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
        LOG_WARN("fail to alloc memory", K(ret));
      } else {
        tmp_col_param = new (buf) ObColumnParam(allocator_);
        if (OB_FAIL(ObTableParam::convert_column_schema_to_param(*col, *tmp_col_param))) {
          LOG_WARN("fail to convert column schema to param", K(ret));
        } else if (OB_FAIL(col_params_.push_back(tmp_col_param))) {
          LOG_WARN("fail to push to array", K(ret), K(tmp_col_param));
        }
        if (OB_FAIL(ret) && OB_NOT_NULL(tmp_col_param)) {
          tmp_col_param->~ObColumnParam();
          allocator_.free(tmp_col_param);
          tmp_col_param = nullptr;
        }
      }
    }
  }
  if (OB_FAIL(ret)) {     //clear col_params
    for (int64_t i = 0; i < col_params_.count(); i++) {
      ObColumnParam *&tmp_col_param = col_params_.at(i);
      if (OB_NOT_NULL(tmp_col_param)) {
        tmp_col_param->~ObColumnParam();
        allocator_.free(tmp_col_param);
        tmp_col_param = nullptr;
      }
    }
  }
  return ret;
}

//construct table access param
int ObLocalScan::construct_access_param(
    const ObTableSchema &data_table_schema,
    const ObTabletID &tablet_id,
    const ObTableReadInfo &full_read_info)
{
  int ret = OB_SUCCESS;
  read_info_.reset();
  ObArray<int32_t> cols_index;
  ObArray<ObColDesc> tmp_col_ids;
  bool is_oracle_mode = false;
  // to construct column index, i.e., cols_index.
  if (OB_FAIL(data_table_schema.get_store_column_ids(tmp_col_ids, false))) {
    LOG_WARN("fail to get store columns id", K(ret), K(tmp_col_ids));
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < extended_gc_.extended_col_ids_.count(); i++) {
      bool is_found = false;
      for (int64_t j = 0; OB_SUCC(ret) && !is_found && j < tmp_col_ids.count(); j++) {
        if (extended_gc_.extended_col_ids_.at(i).col_id_ == tmp_col_ids.at(j).col_id_) {
          if (OB_FAIL(cols_index.push_back(j))) {
            LOG_WARN("fail to push back cols index in data table", K(ret), K(cols_index));
          } else {
            is_found = true;
          }
        }
      }
      if (OB_SUCC(ret) && !is_found) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("error unexpected, column is not in data table", K(ret),
          K(extended_gc_.extended_col_ids_.at(i)), K(tmp_col_ids), K(data_table_schema));
      }
    }
  }
  if (OB_FAIL(ret)) {
  } else if (cols_index.count() != extended_gc_.extended_col_ids_.count()) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("unexpected error", K(ret), K(cols_index), K(extended_gc_));
  } else if (OB_FAIL(data_table_schema.check_if_oracle_compat_mode(is_oracle_mode))) {
      STORAGE_LOG(WARN, "Failed to check oralce mode", K(ret));
  } else if (OB_FAIL(read_info_.init(allocator_,
                                     data_table_schema.get_column_count(),
                                     data_table_schema.get_rowkey_column_num(),
                                     is_oracle_mode,
                                     extended_gc_.extended_col_ids_, // TODO @yiren, remove column id.
                                     false /*is_multi_version_full*/,
                                     &cols_index,
                                     &col_params_))) {
    LOG_WARN("fail to init read info", K(ret));
  } else {
    ObArray<ObColDesc> &extended_col_ids = extended_gc_.extended_col_ids_;
    ObArray<int32_t> &output_projector = extended_gc_.output_projector_;
    access_param_.iter_param_.tablet_id_ = tablet_id;
    access_param_.iter_param_.table_id_ = data_table_schema.get_table_id();
    access_param_.iter_param_.out_cols_project_ = &output_projector;
    access_param_.iter_param_.read_info_ = &read_info_;
    access_param_.iter_param_.full_read_info_ = &full_read_info;
    if (OB_FAIL(access_param_.iter_param_.refresh_lob_column_out_status())) {
      STORAGE_LOG(WARN, "Failed to refresh lob column", K(ret), K(access_param_.iter_param_));
    } else {
      access_param_.is_inited_ = true;
    }
  }
  LOG_INFO("construct table access param", K(ret), K(tmp_col_ids), K(cols_index), K(extended_gc_.extended_col_ids_),
      K(extended_gc_.output_projector_), K(access_param_));
  return ret;
}

//construct version range and ctx
int ObLocalScan::construct_range_ctx(ObQueryFlag &query_flag,
                                     const share::ObLSID &ls_id,
                                     transaction::ObTxDesc *tx_desc)
{
  int ret = OB_SUCCESS;
  common::ObVersionRange trans_version_range;
  trans_version_range.snapshot_version_ = snapshot_version_;
  trans_version_range.multi_version_start_ = snapshot_version_;
  trans_version_range.base_version_ = 0;
  SCN tmp_scn;
  if (OB_FAIL(tmp_scn.convert_for_tx(snapshot_version_))) {
    LOG_WARN("convert fail", K(ret), K(ls_id), K_(snapshot_version));
  } else if (OB_FAIL(ctx_.init_for_read(ls_id,
                                        INT64_MAX,
                                        -1,
                                        tmp_scn))) {
    LOG_WARN("fail to init store ctx", K(ret), K(ls_id));
  } else if (FALSE_IT(ctx_.mvcc_acc_ctx_.tx_desc_ = tx_desc)) {

  } else if (OB_FAIL(access_ctx_.init(query_flag, ctx_, allocator_, allocator_, trans_version_range))) {
    LOG_WARN("fail to init accesss ctx", K(ret));
  } else if (OB_NOT_NULL(access_ctx_.lob_locator_helper_)) {
    int64_t tx_id = (tx_desc == nullptr) ? 0 : tx_desc->tid().get_id();
    access_ctx_.lob_locator_helper_->update_lob_locator_ctx(access_param_.iter_param_.table_id_,
                                                            access_param_.iter_param_.tablet_id_.id(),
                                                            tx_id);
  }
  return ret;
}

//construct multiple scan merge
int ObLocalScan::construct_multiple_scan_merge(
    ObTabletTableIterator &table_iter,
    ObDatumRange &range)
{
  int ret = OB_SUCCESS;
  void *buf = nullptr;
  get_table_param_.tablet_iter_ = table_iter;
  LOG_INFO("start to do output_store.scan");
  if (OB_ISNULL(buf = allocator_.alloc(sizeof(ObMultipleScanMerge)))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
    LOG_WARN("fail to alloc memory for ObMultipleScanMerge", K(ret));
  } else if (FALSE_IT(scan_merge_ = new(buf)ObMultipleScanMerge())) {
    ret = OB_ERR_SYS;
    LOG_WARN("fail to do placement new", K(ret));
  } else if (OB_FAIL(scan_merge_->init(access_param_, access_ctx_, get_table_param_))) {
    LOG_WARN("fail to init scan merge", K(ret), K(access_param_), K(access_ctx_));
  } else if (OB_FAIL(scan_merge_->open(range))) {
    LOG_WARN("fail to open scan merge", K(ret), K(access_param_), K(access_ctx_), K(range));
  } else {
    scan_merge_->disable_padding();
    scan_merge_->disable_fill_virtual_column();
    row_iter_ = scan_merge_;
  }

  if (OB_FAIL(ret) && OB_NOT_NULL(scan_merge_)) {
    scan_merge_->~ObMultipleScanMerge();
    allocator_.free(scan_merge_);
    scan_merge_ = nullptr;
    row_iter_ = nullptr;
  }
  return ret;
}

int ObLocalScan::get_origin_table_checksum(
    ObArray<int64_t> &report_col_checksums,
    ObArray<int64_t> &report_col_ids)
{
  int ret = OB_SUCCESS;
  report_col_checksums.reuse();
  report_col_ids.reuse();
  ObArray<ObColDesc> tmp_col_ids;
  ObSchemaGetterGuard schema_guard;
  const ObTableSchema *data_table_schema = nullptr;
  const ObTableSchema *hidden_table_schema = nullptr;
  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("not init", K(ret));
  } else if (OB_FAIL(ObMultiVersionSchemaService::get_instance().get_tenant_schema_guard(
             tenant_id_, schema_guard, schema_version_))) {
    LOG_WARN("fail to get tenant schema guard", K(ret), K(tenant_id_), K(schema_version_));
  } else if (OB_FAIL(schema_guard.get_table_schema(tenant_id_,
             source_table_id_, data_table_schema))) {
    LOG_WARN("get data table schema failed", K(ret), K(tenant_id_), K(source_table_id_));
  } else if (OB_ISNULL(data_table_schema)) {
    ret = OB_TABLE_NOT_EXIST;
    LOG_WARN("data table not exist", K(ret), K(tenant_id_), K(source_table_id_));
  } else if (OB_FAIL(schema_guard.get_table_schema(tenant_id_,
             dest_table_id_, hidden_table_schema))) {
    LOG_WARN("fail to get hidden table schema", K(ret), K(tenant_id_), K(dest_table_id_));
  } else if (OB_ISNULL(hidden_table_schema)) {
    ret = OB_TABLE_NOT_EXIST;
    LOG_WARN("hidden table schema not exist", K(ret), K(tenant_id_), K(dest_table_id_));
  } else if (OB_FAIL(get_output_columns(*hidden_table_schema, tmp_col_ids))) {
    LOG_WARN("get output column failed", K(ret));
  } else if (tmp_col_ids.size() != exist_column_mapping_.size()) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("unexpected error", K(ret), K(tmp_col_ids), K(exist_column_mapping_.size()));
  } else {
    const int64_t rowkey_cols_cnt = hidden_table_schema->get_rowkey_column_num();
    const int64_t extra_rowkey_cnt = storage::ObMultiVersionRowkeyHelpper::get_extra_rowkey_col_cnt();
    // get data table columns id and corresponding checksum.
    for (int64_t i = 0; OB_SUCC(ret) && i < exist_column_mapping_.size(); i++) {
      if (exist_column_mapping_.test(i)) {
        const ObColumnSchemaV2 *hidden_col_schema = hidden_table_schema->get_column_schema(tmp_col_ids.at(i).col_id_);
        const ObString &hidden_column_name = hidden_col_schema->get_column_name_str();
        const ObColumnSchemaV2 *data_col_schema = data_table_schema->get_column_schema(hidden_column_name);
        const int64_t index_in_array = i < rowkey_cols_cnt ? i : i + extra_rowkey_cnt;
        if (OB_ISNULL(data_col_schema)) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("data column schema should not be null", K(ret), K(hidden_column_name));
        } else if (OB_FAIL(report_col_ids.push_back(data_col_schema->get_column_id()))) {
          LOG_WARN("fail to push back col id", K(ret), K(*data_col_schema));
        } else if (OB_FAIL(report_col_checksums.push_back(checksum_calculator_.get_column_checksum()[index_in_array]))) {
          LOG_WARN("fail to push back col checksum", K(ret));
        } else {/* do nothing. */}
      } else {/* do nothing. */}
    }
  }
  return ret;
}

int ObLocalScan::get_next_row(const ObDatumRow *&tmp_row)
{
  int ret = OB_SUCCESS;
  tmp_row = nullptr;
  calc_buf_.reuse();
  const ObDatumRow *row = nullptr;
  if (OB_FAIL(row_iter_->get_next_row(row))) {
    if (OB_UNLIKELY(OB_ITER_END != ret)) {
      LOG_WARN("fail to get next row", K(ret));
    }
  } else if (OB_ISNULL(row) || !row->is_valid()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid arguments", K(ret), KP(row));
  } else {
    for (int64_t i = 0, j = 0; OB_SUCC(ret) && i < exist_column_mapping_.size(); i++) {
      if (exist_column_mapping_.test(i)) {
        // fill with value stored in origin data table.
        if (OB_UNLIKELY(j >= extended_gc_.extended_col_ids_.count())) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("unexpected error", K(ret), K(j), K(extended_gc_.extended_col_ids_.count()));
        } else {
            tmp_row_.storage_datums_[i] = row->storage_datums_[j++];
        }
      } else {
        // the column is newly added, thus fill with default value.
        tmp_row_.storage_datums_[i] = default_row_.storage_datums_[i];
      }
    }
  }
  if (OB_SUCC(ret)) {
    tmp_row = &tmp_row_;
  }
  return ret;
}

} //end namespace stroage
} //end namespace oceanbase
