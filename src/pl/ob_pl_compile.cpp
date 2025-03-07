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

#define USING_LOG_PREFIX PL

#include "pl/ob_pl_compile.h"
#include "lib/container/ob_iarray.h"
#include "lib/string/ob_sql_string.h"
#include "common/sql_mode/ob_sql_mode_utils.h"
#include "share/schema/ob_routine_info.h"
#include "share/schema/ob_schema_getter_guard.h"
#include "parser/ob_pl_parser.h"
#include "sql/resolver/expr/ob_raw_expr_util.h"
#include "pl/ob_pl_resolver.h"
#include "pl/ob_pl_code_generator.h"
#include "pl/ob_pl_package.h"
#include "lib/alloc/malloc_hook.h"

namespace oceanbase {
using namespace common;
using namespace share;
using namespace schema;
using namespace sql;
namespace pl {

int ObPLCompiler::init_anonymous_ast(
  ObPLFunctionAST &func_ast,
  ObIAllocator &allocator,
  ObSQLSessionInfo &session_info,
  ObMySQLProxy &sql_proxy,
  ObSchemaGetterGuard &schema_guard,
  ObPLPackageGuard &package_guard,
  const ParamStore *params,
  bool is_prepare_protocol)
{
  int ret = OB_SUCCESS;
  ObPLDataType pl_type;
  common::ObDataType data_type;

  func_ast.set_name(ObPLResolver::ANONYMOUS_BLOCK);
  data_type.set_obj_type(common::ObNullType);
  pl_type.set_data_type(data_type);
  func_ast.set_ret_type(pl_type);

  for (int64_t i = 0; OB_SUCC(ret) && OB_NOT_NULL(params) && i < params->count(); ++i) {
    const ObObjParam &param = params->at(i);
    if (param.is_pl_extend()) {
      if (param.get_udt_id() != OB_INVALID_ID) {
        const ObUserDefinedType *user_type = NULL;
        OZ (ObResolverUtils::get_user_type(&allocator,
                                           &session_info,
                                           &sql_proxy,
                                           &schema_guard,
                                           package_guard,
                                           param.get_udt_id(),
                                           user_type));
        CK (OB_NOT_NULL(user_type));
        OX (pl_type.reset());
        OX (pl_type = *user_type);
      } else {
        ret = OB_NOT_SUPPORTED;
        LOG_WARN(
          "anonymous block`s parameter has invalid udt id and not collection not supported",
          K(ret), K(param));
        LOG_USER_ERROR(OB_NOT_SUPPORTED,
                       "anonymous block`s parameter has invalid udt id and not a coll");
      }
    } else {
      data_type.reset();
      data_type.set_accuracy(params->at(i).get_accuracy());
      data_type.set_meta_type(params->at(i).get_meta());
      pl_type.reset();
      int64_t int_value = 0;
      // 参数化整型常量按照会按照numbger来生成param
      if (!is_prepare_protocol
          && (ObNumberType == param.get_type() || ObUNumberType == param.get_type())
          && param.get_number().is_valid_int64(int_value)
          && int_value <= INT32_MAX && int_value >= INT32_MIN) {
        pl_type.set_pl_integer_type(PL_SIMPLE_INTEGER, data_type);
      } else {
        pl_type.set_data_type(data_type);
      }
      OZ (ObPLResolver::adjust_routine_param_type(pl_type));
    }
    OZ (func_ast.add_argument(ObPLResolver::ANONYMOUS_ARG, pl_type, NULL, NULL, true));
  }
  return ret;
}

//for anonymous
int ObPLCompiler::compile(
  const ObStmtNodeTree *block,
  ObPLFunction &func,
  ParamStore *params/*=NULL*/,
  bool is_prepare_protocol/*=false*/)
{
  int ret = OB_SUCCESS;
  FLTSpanGuard(pl_compile);
  bool use_jitted_expr = false;

  //Step 1：构造匿名块的ObPLFunctionAST
  HEAP_VAR(ObPLFunctionAST, func_ast, allocator_) {

    func_ast.set_db_name(session_info_.get_database_name());
    OZ (init_anonymous_ast(func_ast,
                           allocator_,
                           session_info_,
                           sql_proxy_,
                           schema_guard_,
                           package_guard_,
                           params,
                           is_prepare_protocol));

    //Step 2：Resolver
    if (OB_SUCC(ret)) {
      ObPLResolver resolver(allocator_, session_info_, schema_guard_, package_guard_, sql_proxy_,
                            func_ast.get_expr_factory(), NULL/*parent ns*/, is_prepare_protocol,
                            false/*is_check_mode_ = false*/, false/*bool is_sql_scope_ = false*/,
                            params);
      if (OB_ISNULL(block)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("pl body is NULL", K(block), K(ret));
      } else if (OB_FAIL(resolver.init(func_ast))) {
        LOG_WARN("failed to init resolver", K(block), K(ret));
      } else if (OB_FAIL(resolver.resolve_root(block, func_ast))) {
        LOG_WARN("failed to analyze pl body", K(block), K(ret));
      } else if (session_info_.is_pl_debug_on()) {
        if (OB_FAIL(func_ast.generate_symbol_debuginfo())) {
          LOG_WARN("failed to generate symbol debuginfo", K(ret));
        }
      }
    }

    // Process Prepare SQL Ref
    if (OB_SUCC(ret)) {
      func.set_proc_type(STANDALONE_ANONYMOUS);
      func.set_ns(ObLibCacheNameSpace::NS_ANON);
      OZ (func.get_exec_env().load(session_info_));
    }
    //Step 3：Code Generator
    if (OB_SUCC(ret)) {
  #ifdef USE_MCJIT
      HEAP_VAR(ObPLCodeGenerator, cg ,allocator_, session_info_) {
  #else
      HEAP_VAR(ObPLCodeGenerator, cg, func.get_allocator(),
               session_info_,
               schema_guard_,
               func_ast,
               func.get_expressions(),
               func.get_helper(),
               func.get_di_helper(),
               lib::is_oracle_mode()) {
  #endif
        lib::ObMallocHookAttrGuard malloc_guard(lib::ObMemAttr(MTL_ID(), "PlCodeGen"));
        if (OB_FAIL(cg.init())) {
          LOG_WARN("failed to init code generator", K(ret));
        } else if (OB_FAIL(cg.generate(func))) {
          LOG_WARN("failed to code generate for stmt", K(ret));
        }
        OX (func.set_arg_count(func_ast.get_arg_count()));
        for (int64_t i = 0; OB_SUCC(ret) && i < func_ast.get_arg_count(); ++i) {
          const ObPLSymbolTable &symbol_table = func_ast.get_symbol_table();
          const ObPLVar *symbol = symbol_table.get_symbol(i);
          CK (OB_NOT_NULL(symbol));
          if (OB_FAIL(ret)) {
          } else if (!symbol->is_readonly()) {
            OZ (func.add_out_arg(i));
            if (OB_SUCC(ret)
                && 0 == symbol->get_name().case_compare(ObPLResolver::ANONYMOUS_INOUT_ARG)) {
              OZ (func.add_in_arg(i));
            }
          } else {
            OZ (func.add_in_arg(i));
          }
          if (OB_SUCC(ret)
              && OB_NOT_NULL(params)
              && 0 == symbol->get_name().case_compare(ObPLResolver::ANONYMOUS_SQL_ARG)) {
            params->at(i).set_need_to_check_type(false);
          }
        }
        if (OB_SUCC(ret)) {
          //anonymous + ps情况func也需要进plan cache，因此需要设置version
          int64_t tenant_id = session_info_.get_effective_tenant_id();
          int64_t tenant_schema_version = OB_INVALID_VERSION;
          int64_t sys_schema_version = OB_INVALID_VERSION;
          if (OB_FAIL(schema_guard_.get_schema_version(tenant_id, tenant_schema_version))
              || OB_FAIL(schema_guard_.get_schema_version(OB_SYS_TENANT_ID, sys_schema_version))) {
            LOG_WARN("fail to get schema version", K(ret), K(tenant_id));
          } else {
            func.set_tenant_schema_version(tenant_schema_version);
            func.set_sys_schema_version(sys_schema_version);
          }
        }
      } // end of HEAP_VAR
    }
  }
  return ret;
}

//for function/ procedure
int ObPLCompiler::compile(const uint64_t id, ObPLFunction &func)
{
  int ret = OB_SUCCESS;
  FLTSpanGuard(pl_compile);
  HEAP_VAR(ObPLFunctionAST, func_ast, allocator_) {
    const share::schema::ObRoutineInfo *proc = NULL;
    const share::schema::ObDatabaseSchema *db = NULL;
    ObExecEnv old_exec_env;
    ObSqlString old_db_name;
    uint64_t old_db_id = OB_INVALID_ID;
    bool need_reset_exec_env = false;
    bool need_reset_default_database = false;

    //Step 1：根据id从schema取出pl信息，并构造ObPLFunctionAST
    int64_t init_start = ObTimeUtility::current_time();
    const uint64_t tenant_id = get_tenant_id_by_object_id(id);
    if (OB_FAIL(schema_guard_.get_routine_info(tenant_id, id, proc))){
      LOG_WARN("failed to get procedure info", K(tenant_id), K(id), K(ret));
    } else if (OB_ISNULL(proc)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("procedure info is NULL", K(id), K(proc), K(ret));
    } else if (OB_FAIL(init_function(proc, func))) {
      LOG_WARN("failed to init pl function", K(proc), K(ret));
    }
    // 设置环境变量
    if (OB_SUCC(ret)) {
      if (OB_FAIL(old_exec_env.load(session_info_))) {
        LOG_WARN("failed to load exec_env", K(old_exec_env), K(func.get_exec_env()), K(ret));
      } else if (OB_UNLIKELY(old_exec_env != func.get_exec_env())) {
        if (OB_FAIL(func.get_exec_env().store(session_info_))) { //设置环境变量
          LOG_WARN("failed to set exec_env", K(old_exec_env), K(func.get_exec_env()), K(ret));
        } else {
          need_reset_exec_env = true;
        }
      } else { /*do nothing*/ }
    }
    // 设置默认数据库
    CHECK_COMPATIBILITY_MODE(&session_info_);

    CK (OB_UNLIKELY(OB_NOT_NULL(proc)));
    OZ (schema_guard_.get_database_schema(proc->get_tenant_id(), proc->get_database_id(), db));
    CK (OB_UNLIKELY(OB_NOT_NULL(db)));
    if (OB_SUCC(ret)
        && !proc->is_invoker_right()
        && proc->get_database_id() != old_db_id) {
      old_db_id = session_info_.get_database_id();
      if (OB_FAIL(old_db_name.append(session_info_.get_database_name()))) {
        LOG_WARN("failed to append database name", K(ret));
      } else if (OB_INVALID_ID == proc->get_database_id()) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("database id is invalid", K(proc->get_database_id()), K(ret));
      } else if (OB_FAIL(session_info_.set_default_database(db->get_database_name_str()))) {
        LOG_WARN("failed to set default database", K(ret));
      } else {
        session_info_.set_database_id(db->get_database_id());
        need_reset_default_database = true;
      }
    }
    if (OB_SUCC(ret)) {
      func_ast.set_name(proc->get_routine_name());
      func_ast.set_db_name(db->get_database_name_str());
      func_ast.set_id(proc->get_routine_id());
      func_ast.set_subprogram_id(proc->get_routine_id());
      if (proc->is_udt_routine()) {
        func_ast.set_is_udt_routine();
      }
      if (proc->is_invoker_right()) {
        func_ast.get_compile_flag().add_invoker_right();
      }
      if (proc->is_pipelined()) {
        func_ast.set_pipelined();
      }
      ObSchemaObjVersion obj_version;
      obj_version.object_id_ = proc->get_routine_id();
      obj_version.object_type_ = proc->is_procedure() ? DEPENDENCY_PROCEDURE : DEPENDENCY_FUNCTION;
      obj_version.version_ = proc->get_schema_version();
      if (OB_FAIL(func_ast.add_dependency_object(obj_version))) {
        LOG_WARN("add dependency table failed", K(ret));
      } else if (ROUTINE_PROCEDURE_TYPE == proc->get_routine_type()) {
        func_ast.set_proc_type(STANDALONE_PROCEDURE);
      } else if (ROUTINE_FUNCTION_TYPE == proc->get_routine_type()) {
        func_ast.set_proc_type(STANDALONE_FUNCTION);
      } else {}
      //添加参数列表
      for (int64_t i = 0; OB_SUCC(ret) && i < proc->get_routine_params().count(); ++i) {
        ObRoutineParam *param = proc->get_routine_params().at(i);
        ObPLDataType param_type;
        if (OB_ISNULL(param)) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("procedure param is NULL", K(param), K(ret));
        }
        if (OB_SUCC(ret)) {
          ObSchemaObjVersion obj_version;
          OZ (pl::ObPLDataType::transform_from_iparam(param,
                                                schema_guard_,
                                                session_info_,
                                                allocator_,
                                                sql_proxy_,
                                                param_type,
                                                &obj_version));
          if (OB_SUCC(ret) && obj_version.is_valid()) {
            OZ (func_ast.add_dependency_object(obj_version));
          }
        }
        if (OB_SUCC(ret)) {
          if (param->is_ret_param()) {
            func_ast.set_ret_type(param_type);
            if (ob_is_enum_or_set_type(param->get_param_type().get_obj_type())
                && OB_FAIL(func_ast.set_ret_type_info(param->get_extended_type_info()))) {
               LOG_WARN("fail to set ret type info", K(ret));
             }
          } else {
            OZ (func_ast.add_argument(param->get_param_name(),
                                      param_type,
                                      NULL,
                                      &(param->get_extended_type_info()),
                                      param->is_in_sp_param(),
                                      param->is_self_param()));
          }
        }
      }
    }

    int64_t init_end = ObTimeUtility::current_time();
    LOG_INFO(">>>>>>>>Init Time: ", K(id), K(init_end - init_start));

    //Step 2：Parser
    ObStmtNodeTree *parse_tree = NULL;
    if (OB_SUCC(ret)) {
      ObString body = proc->get_routine_body(); //获取body字符串
      ObDataTypeCastParams dtc_params = session_info_.get_dtc_params();
      ObPLParser parser(allocator_, dtc_params.connection_collation_);
      if (OB_FAIL(ObSQLUtils::convert_sql_text_from_schema_for_resolve(
                    allocator_, dtc_params, body))) {
        LOG_WARN("fail to do charset convert", K(ret), K(body));
      } else if (OB_FAIL(parser.parse_routine_body(body, parse_tree, session_info_.is_for_trigger_package()))) {
        LOG_WARN("parse routine body failed", K(ret), K(body));
      }
    }

    int64_t parse_end = ObTimeUtility::current_time();
    LOG_INFO(">>>>>>>>Parse Time: ", K(id), K(parse_end - init_end));

    //Step 3：Resolver
    if (OB_SUCC(ret)) {
      bool is_prepare_protocol = false;
      ObPLResolver resolver(allocator_, session_info_, schema_guard_, package_guard_, sql_proxy_,
                            func_ast.get_expr_factory(), NULL/*parent ns*/, is_prepare_protocol);
      if (OB_ISNULL(parse_tree)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("pl body is NULL", K(parse_tree), K(ret));
      } else if (OB_FAIL(resolver.init(func_ast))) {
        LOG_WARN("failed to init resolver", K(*proc), K(ret));
      } else if (OB_FAIL(resolver.init_default_exprs(func_ast, proc->get_routine_params()))) {
        LOG_WARN("failed to init routine default exprs", K(ret), KPC(proc));
      } else if (OB_FAIL(resolver.resolve_root(parse_tree, func_ast))) {
        LOG_WARN("failed to analyze pl body", K(*proc), K(ret));
      } else if (session_info_.is_pl_debug_on()) {
        if (OB_FAIL(func_ast.generate_symbol_debuginfo())) {
          LOG_WARN("failed to generate symbol debuginfo", K(ret));
        }
      }
    }
    int64_t resolve_end = ObTimeUtility::current_time();
    LOG_INFO(">>>>>>>>Resolve Time: ", K(id), K(resolve_end - parse_end));

    //Step 4：Code Generator
    if (OB_SUCC(ret)) {
  #ifdef USE_MCJIT
      HEAP_VAR(ObPLCodeGenerator, cg, allocator_, session_info_) {
  #else
      HEAP_VAR(ObPLCodeGenerator, cg, func.get_allocator(),
               session_info_,
               schema_guard_,
               func_ast,
               func.get_expressions(),
               func.get_helper(),
               func.get_di_helper(),
               lib::is_oracle_mode()) {
  #endif
        lib::ObMallocHookAttrGuard malloc_guard(lib::ObMemAttr(MTL_ID(), "PlCodeGen"));
        if (OB_FAIL(cg.init())) {
          LOG_WARN("failed to init code generator", K(ret));
        } else if (OB_FAIL(cg.generate(func))) {
          LOG_WARN("failed to code generate for stmt", K(ret));
        } else {
          int64_t tenant_id = session_info_.get_effective_tenant_id();
          int64_t tenant_schema_version = OB_INVALID_VERSION;
          int64_t sys_schema_version = OB_INVALID_VERSION;
          if (OB_FAIL(schema_guard_.get_schema_version(tenant_id, tenant_schema_version))
              || OB_FAIL(schema_guard_.get_schema_version(OB_SYS_TENANT_ID, sys_schema_version))) {
            LOG_WARN("fail to get schema version", K(ret), K(tenant_id));
          } else {
            func.set_tenant_schema_version(tenant_schema_version);
            func.set_sys_schema_version(sys_schema_version);
            func.set_ret_type(func_ast.get_ret_type());
          }
        }
      } // end of HEAP_VAR
    }

    int64_t cg_end = ObTimeUtility::current_time();
    LOG_INFO(">>>>>>>>CG Time: ", K(id), K(cg_end - resolve_end));

    //Step 5: 恢复环境变量
    int tmp_ret = OB_SUCCESS;
    if (need_reset_exec_env) {
      if ((tmp_ret = old_exec_env.store(session_info_)) != OB_SUCCESS) {
        // 不覆盖错误码
        ret = OB_SUCCESS == ret ? tmp_ret : ret;
        LOG_WARN("failed to set exec_env", K(old_exec_env), K(tmp_ret), K(ret));
      }
    }
    if (need_reset_default_database) {
      if ((tmp_ret = session_info_.set_default_database(old_db_name.string())) != OB_SUCCESS) {
        // 不覆盖错误码
        ret = OB_SUCCESS == ret ? tmp_ret : ret;
        LOG_WARN("failed to set default database", K(old_db_name), K(tmp_ret), K(ret));
      } else {
        session_info_.set_database_id(old_db_id);
      }
    }
    int64_t final_end = ObTimeUtility::current_time();
    LOG_INFO(">>>>>>>>Final Time: ", K(id), K(final_end - cg_end));
  }
  return ret;
}

int ObPLCompiler::update_schema_object_dep_info(ObIArray<ObSchemaObjVersion> &dp_tbl,
                                                uint64_t tenant_id,
                                                uint64_t owner_id,
                                                uint64_t dep_obj_id,
                                                uint64_t schema_version,
                                                ObObjectType dep_obj_type)
{
  int ret = OB_SUCCESS;
  ObMySQLProxy *sql_proxy = nullptr;
  ObMySQLTransaction trans;
  bool skip = false;
  if (GCTX.is_standby_cluster()) {
    skip = true;
  } else if (ObTriggerInfo::is_trigger_package_id(dep_obj_id)) {
    if (lib::is_oracle_mode()) {
      dep_obj_id = ObTriggerInfo::get_package_trigger_id(dep_obj_id);
      dep_obj_type = ObObjectType::TRIGGER;
    } else {
      skip = true;
    }
  }
  if (!skip) {
    if (OB_ISNULL(sql_proxy = GCTX.sql_proxy_)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(trans.start(sql_proxy, tenant_id))) {
      LOG_WARN("failed to start trans", K(ret), K(tenant_id));
    } else {
      OZ (ObDependencyInfo::delete_schema_object_dependency(trans,
                                              tenant_id, dep_obj_id,
                                              schema_version,
                                              dep_obj_type));
      ObSArray<ObDependencyInfo> dep_infos;
      ObString dummy;
      OZ (ObDependencyInfo::collect_dep_infos(dp_tbl,
                                              dep_infos,
                                              dep_obj_type,
                                              0, dummy, dummy));
      if (OB_FAIL(ret)) {
        LOG_WARN("delete failed", K(ret));
      } else if (OB_INVALID_ID == owner_id
      || OB_INVALID_ID == dep_obj_id
      || OB_INVALID_ID == tenant_id
      || OB_INVALID_SCHEMA_VERSION == schema_version) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("illegal schema version or owner id", K(ret), K(schema_version),
                                                      K(owner_id), K(dep_obj_id));
      } else {
        for (int64_t i = 0 ; OB_SUCC(ret) && i < dep_infos.count(); ++i) {
          ObDependencyInfo & dep = dep_infos.at(i);
          dep.set_tenant_id(tenant_id);
          dep.set_dep_obj_id(dep_obj_id);
          dep.set_dep_obj_owner_id(owner_id);
          dep.set_schema_version(schema_version);
          OZ (dep.insert_schema_object_dependency(trans));
          // 理论上pl是单线程编译的，但是如果把这个相同的pl在多个observer上同时编译，这个依赖关系可能会多次重建。
          // 发生这种情况下，简单的忽略错误码
          if (OB_ERR_PRIMARY_KEY_DUPLICATE == ret) {
            ret = OB_SUCCESS;
          }
        }
      }
    }
    if (trans.is_started()) {
      int temp_ret = OB_SUCCESS;
      if (OB_SUCCESS != (temp_ret = trans.end(OB_SUCC(ret)))) {
        LOG_WARN("trans end failed", "is_commit", OB_SUCCESS == ret, K(ret), K(temp_ret));
        ret = OB_SUCC(ret) ? temp_ret : ret;
      }
    }
  }
  return ret;
}

int ObPLCompiler::check_package_body_legal(const ObPLBlockNS *parent_ns,
                                           const ObPLPackageAST &package_ast)
{
  int ret = OB_SUCCESS;
  const ObPLRoutineTable* spec_routine_table = parent_ns->get_routine_table();
  const ObPLRoutineTable& body_routine_table = package_ast.get_routine_table();
  CK (OB_NOT_NULL(spec_routine_table));
  for (int i = ObPLRoutineTable::NORMAL_ROUTINE_START_IDX;
       OB_SUCC(ret) && i < spec_routine_table->get_count(); ++i) {
    const ObPLRoutineInfo* spec_routine_info = NULL;
    const ObPLRoutineInfo* body_routine_info = NULL;
    bool exist = false;
    OZ (spec_routine_table->get_routine_info(i, spec_routine_info));
    CK (OB_NOT_NULL(spec_routine_info));
    OZ (body_routine_table.get_routine_info(i, body_routine_info));
    if (OB_SUCC(ret) && OB_NOT_NULL(body_routine_info)) {
      OZ (spec_routine_info->is_equal(body_routine_info, exist));
    }
    if (OB_SUCC(ret) && !exist) {
      ret = OB_ERR_ITEM_NOT_IN_BODY;
      LOG_USER_ERROR(OB_ERR_ITEM_NOT_IN_BODY,
                     spec_routine_info->get_name().length(), spec_routine_info->get_name().ptr());
      LOG_WARN("PLS-00323: subprogram or cursor is declared in a package specification and must be defined in the package body",
               K(ret), K(i), K(spec_routine_info->get_decl_str()));
    }
  }
  CK (OB_NOT_NULL(parent_ns->get_cursor_table()));
  for (int i = 0; OB_SUCC(ret) && i < parent_ns->get_cursor_table()->get_count(); ++i) {
    const ObPLCursor *cursor = parent_ns->get_cursor_table()->get_cursor(i);
    CK (OB_NOT_NULL(cursor));
    if (OB_SUCC(ret) && cursor->get_state() != ObPLCursor::DEFINED) {
      const ObPLVar *var = NULL;
      OZ (parent_ns->get_cursor_var(
        cursor->get_package_id(), cursor->get_routine_id(), cursor->get_index(), var));
      CK (OB_NOT_NULL(var));
      if (OB_SUCC(ret)) {
        ret = OB_ERR_ITEM_NOT_IN_BODY;
        LOG_WARN("PLS-00323: subprogram or cursor is declared in a package specification and must be defined in the package body",
               K(ret), K(i));
        LOG_USER_ERROR(OB_ERR_ITEM_NOT_IN_BODY, var->get_name().length(), var->get_name().ptr());
      }
    }
  }
  return ret;
}

int ObPLCompiler::analyze_package(const ObString &source,
                                  const ObPLBlockNS *parent_ns,
                                  ObPLPackageAST &package_ast,
                                  bool is_for_trigger)
{
  int ret = OB_SUCCESS;
  bool origin_is_for_trigger = session_info_.is_for_trigger_package();
  session_info_.set_for_trigger_package(is_for_trigger);
  CK (!source.empty());
  CK (package_ast.is_inited());
  if (OB_SUCC(ret)) {
    ObPLParser parser(allocator_, session_info_.get_local_collation_connection());
    ObStmtNodeTree *parse_tree = NULL;
    CHECK_COMPATIBILITY_MODE(&session_info_);
    ObPLResolver resolver(allocator_,
                          session_info_,
                          schema_guard_,
                          package_guard_,
                          sql_proxy_,
                          package_ast.get_expr_factory(),
                          parent_ns,
                          false);
    const ObTriggerInfo *trg_info = NULL;
    if (PL_PACKAGE_BODY == package_ast.get_package_type() && is_for_trigger) {
      uint64_t trg_id = ObTriggerInfo::get_package_trigger_id(package_ast.get_id());
      OZ (schema_guard_.get_trigger_info(session_info_.get_effective_tenant_id(), trg_id, trg_info));
      CK (OB_NOT_NULL(trg_info));
    }
    if (OB_SUCC(ret) && is_for_trigger && PL_PACKAGE_SPEC == package_ast.get_package_type()) {
      const uint64_t trg_id = ObTriggerInfo::get_package_trigger_id(package_ast.get_id());
      const ObTriggerInfo *trg_info = NULL;
      const uint64_t tenant_id = session_info_.get_effective_tenant_id();
      OZ (schema_guard_.get_trigger_info(tenant_id, trg_id, trg_info));
      OV (OB_NOT_NULL(trg_info), OB_ERR_UNEXPECTED, trg_id);
      if (OB_SUCC(ret) && !trg_info->get_ref_trg_db_name().empty() && lib::is_oracle_mode()) {
        uint64_t ref_db_id = OB_INVALID_ID;
        const ObTriggerInfo *ref_trg_info = NULL;
        OV (!trg_info->get_ref_trg_db_name().empty());
        OZ (schema_guard_.get_database_id(tenant_id, trg_info->get_ref_trg_db_name(), ref_db_id));
        OZ (schema_guard_.get_trigger_info(tenant_id, ref_db_id, trg_info->get_ref_trg_name(), ref_trg_info));
        if (OB_SUCC(ret) && NULL == ref_trg_info) {
          ret = OB_ERR_TRIGGER_NOT_EXIST;
          LOG_WARN("ref_trg_info is NULL", K(trg_info->get_ref_trg_db_name()), K(trg_info->get_ref_trg_name()), K(ret));
          if (lib::is_oracle_mode()) {
            LOG_ORACLE_USER_ERROR(OB_ERR_TRIGGER_NOT_EXIST, trg_info->get_ref_trg_name().length(),
                                  trg_info->get_ref_trg_name().ptr());
          }
        }
        if (OB_SUCC(ret)) {
          ObSchemaObjVersion obj_version;
          obj_version.object_id_ = ref_trg_info->get_trigger_id();
          obj_version.object_type_ = DEPENDENCY_TRIGGER;
          obj_version.version_ = ref_trg_info->get_schema_version();
          OZ (package_ast.add_dependency_object(obj_version));
        }
      }
    }
    OZ (parser.parse_package(source, parse_tree, session_info_.get_dtc_params(), 
                             &schema_guard_, is_for_trigger, trg_info));
    OZ (resolver.init(package_ast));
    OZ (resolver.resolve(parse_tree, package_ast));
    if (OB_SUCC(ret) && PL_PACKAGE_SPEC == package_ast.get_package_type()) {
      package_ast.process_default_compile_flag();
      package_ast.process_generic_type();
    }
    //external_ns's lifetime is same with ObPLResolver, but we need package ast's namespace info outside(except external ns)
    //explicit set to null in case of wild pointer
    if (OB_NOT_NULL(package_ast.get_body())) {
      (const_cast<ObPLBlockNS &>(package_ast.get_body()->get_namespace())).set_external_ns(NULL);
    }
  }
  if (OB_SUCC(ret) && OB_NOT_NULL(parent_ns)) { // after resolve package body, check package legal
    OZ (check_package_body_legal(parent_ns, package_ast));
  }
  session_info_.set_for_trigger_package(origin_is_for_trigger);
  return ret;
}

int ObPLCompiler::generate_package(const ObString &exec_env, ObPLPackageAST &package_ast, ObPLPackage &package)
{
  int ret = OB_SUCCESS;
  CK (OB_NOT_NULL(session_info_.get_pl_engine()));
  if (OB_SUCC(ret)) {
    WITH_CONTEXT(package.get_mem_context()) {
      CK (package.is_inited());
      OZ (package.get_dependency_table().assign(package_ast.get_dependency_table()));
      OZ (generate_package_conditions(package_ast.get_condition_table(), package));
      OZ (generate_package_vars(package_ast, package_ast.get_symbol_table(), package));
      OZ (generate_package_types(package_ast.get_user_type_table(), package));
      OZ (generate_package_routines(exec_env, package_ast.get_routine_table(), package));
      OZ (generate_package_cursors(package_ast, package_ast.get_cursor_table(), package));
    }
  }
  return ret;
}

int ObPLCompiler::compile_package(const ObPackageInfo &package_info,
                                  const ObPLBlockNS *parent_ns,
                                  ObPLPackageAST &package_ast,
                                  ObPLPackage &package)
{
  int ret = OB_SUCCESS;
  bool saved_trigger_flag = session_info_.is_for_trigger_package();
  ObString source;
  bool use_jitted_expr = false;

  ObPLCompilerEnvGuard guard(package_info, session_info_, schema_guard_, ret);
  session_info_.set_for_trigger_package(package_info.is_for_trigger());
  if (OB_SUCC(ret) && package_info.is_invoker_right()) {
    OZ (package_ast.get_compile_flag().add_invoker_right());
  }
  const ObTriggerInfo *trigger_info = nullptr;
  if (OB_SUCC(ret) && package_info.is_for_trigger()) {
    uint64_t trigger_id = ObTriggerInfo::get_package_trigger_id(package_info.get_package_id());
    const uint64_t tenant_id = get_tenant_id_by_object_id(trigger_id);
    OZ (schema_guard_.get_trigger_info(tenant_id, trigger_id, trigger_info));
    OX (package_ast.set_priv_user(trigger_info->get_trigger_priv_user()));
  }
  if (OB_SUCC(ret)) {
    if (package_info.is_for_trigger()) {
      OZ (ObTriggerInfo::gen_package_source(package_info.get_tenant_id(),
                                            package_info.get_package_id(),
                                            source,
                                            PACKAGE_TYPE == package_info.get_type(),
                                            schema_guard_, package.get_allocator()));
      LOG_DEBUG("trigger package source", K(source), K(package_info.get_type()), K(ret));
    } else {
      source = package_info.get_source();
    }
  }
  OZ (ObSQLUtils::convert_sql_text_from_schema_for_resolve(
        allocator_, session_info_.get_dtc_params(), source));
  OZ (analyze_package(source, parent_ns,
                      package_ast, package_info.is_for_trigger()));

  if (OB_SUCC(ret)) {
#ifdef USE_MCJIT
    HEAP_VAR(ObPLCodeGenerator, cg ,allocator_, session_info_) {
#else
    HEAP_VAR(ObPLCodeGenerator, cg, package.get_allocator(),
               session_info_,
               schema_guard_,
               package_ast,
               package.get_expressions(),
               package.get_helper(),
               package.get_di_helper(),
               lib::is_oracle_mode()) {
#endif
      lib::ObMallocHookAttrGuard malloc_guard(lib::ObMemAttr(MTL_ID(), "PlCodeGen"));
      OZ (cg.init());
      OZ (cg.generate(package));
    }
  }

  OZ (generate_package(package_info.get_exec_env(), package_ast, package));
  OX (package.set_can_cached(package_ast.get_can_cached()));
  OX (package_ast.get_serially_reusable() ? package.set_serially_reusable() : void(NULL));
  session_info_.set_for_trigger_package(saved_trigger_flag);
  OZ (update_schema_object_dep_info(package_ast.get_dependency_table(),
                                    package_info.get_tenant_id(),
                                    package_info.get_owner_id(),
                                    package_info.get_package_id(),
                                    package_info.get_schema_version(),
                                    package_info.get_object_type()));
  if (OB_SUCC(ret)) {
    ObErrorInfo error_info;
    if (package_info.is_for_trigger()) {
      CK (OB_NOT_NULL(trigger_info));
      OZ (error_info.delete_error(trigger_info), ret);
    } else {
      if (OB_FAIL(error_info.delete_error(&package_info))) {
        LOG_WARN("delete error info failed", K(ret));
      }
    }
  }
  return ret;
}

int ObPLCompiler::init_function(const share::schema::ObRoutineInfo *routine, ObPLFunction &func)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(routine)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("routine info is NULL", K(routine), K(ret));
  } else if (OB_FAIL(func.get_exec_env().init(routine->get_exec_env()))) {
    LOG_WARN("Failed to load exec env", K(ret));
  } else {
    const ObUDTTypeInfo *udt_info = NULL;
    if (routine->is_udt_routine()) {
      OZ(schema_guard_.get_udt_info(routine->get_tenant_id(),
                                    routine->get_package_id(),
                                    udt_info));
      CK (OB_NOT_NULL(udt_info));
      // if type body is droped, the routine is exist in __all_routine, but we can't use it;
      if (OB_SUCC(ret) && !udt_info->has_type_body()) {
        ret = OB_ERR_TYPE_BODY_NOT_EXIST;
        LOG_WARN("type body does not exist", K(routine->get_package_id()), K(ret));
        LOG_USER_ERROR(OB_ERR_TYPE_BODY_NOT_EXIST,
                       udt_info->get_type_name().length(),
                       udt_info->get_type_name().ptr());
      }
    }
    if (OB_SUCC(ret)) {
    //FIXME: FATAL ERROR!!!! can't modify schema from schema_guard
    if (routine->is_udt_procedure()) {
      const_cast<share::schema::ObRoutineInfo *>(routine)->set_routine_type(ROUTINE_PROCEDURE_TYPE);
    } else if (routine->is_udt_function()) {
      const_cast<share::schema::ObRoutineInfo *>(routine)->set_routine_type(ROUTINE_FUNCTION_TYPE);
    }
    if (ROUTINE_PROCEDURE_TYPE == routine->get_routine_type()) {
      func.set_proc_type(STANDALONE_PROCEDURE);
      func.set_ns(ObLibCacheNameSpace::NS_PRCR);
    } else if (ROUTINE_FUNCTION_TYPE == routine->get_routine_type()) {
      func.set_proc_type(STANDALONE_FUNCTION);
      func.set_ns(ObLibCacheNameSpace::NS_SFC);
    } else {
      // do nothing...
    }
    func.set_tenant_id(routine->get_tenant_id());
    func.set_database_id(routine->get_database_id());
    func.set_package_id(routine->get_package_id());
    func.set_routine_id(routine->get_routine_id());
    func.set_arg_count(routine->get_param_count());
    func.set_owner(routine->get_owner_id());
    func.set_priv_user(routine->get_priv_user());
    if (routine->is_invoker_right()) {
      func.set_invoker_right();
    }
    // 对于function而言，输出参数也在params里面，这里不能简单的按照param_count进行遍历
    for (int64_t i = 0; OB_SUCC(ret) && i < routine->get_routine_params().count(); ++i) {
      ObRoutineParam *param = routine->get_routine_params().at(i);
      int64_t param_pos = param->get_param_position();
      if (OB_ISNULL(param)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("routine param is NULL", K(i), K(ret));
      } else if (param->is_ret_param()) {
        // 对于返回值, 既不是in也不是out, 不做处理
      } else {
         if (param->is_inout_sp_param() || param->is_out_sp_param()) {
          if (OB_FAIL(func.add_out_arg(param_pos - 1))) {
            LOG_WARN("Failed to add out arg", K(param_pos), K(ret));
          }
        }
        if (OB_SUCC(ret)) {
          if (param->is_inout_sp_param() || param->is_in_sp_param()) {
            if (OB_FAIL(func.add_in_arg(param_pos - 1))) {
              LOG_WARN("Failed to add out arg", K(i), K(param_pos), K(ret));
            }
          }
        }
      }
    }
      ObString database_name, package_name;
      OZ (format_object_name(schema_guard_,
                             routine->get_tenant_id(),
                             routine->get_database_id(),
                             routine->get_package_id(),
                             database_name,
                             package_name));
      OZ (func.set_database_name(database_name));
      OZ (func.set_package_name(package_name));
      OZ (func.set_function_name(routine->get_routine_name()));
    }
  }
  return ret;
}

int ObPLCompiler::init_function(share::schema::ObSchemaGetterGuard &schema_guard,
                                const sql::ObExecEnv &exec_env,
                                const ObPLRoutineInfo &routine_info,
                                ObPLFunction &routine)
{
  int ret = OB_SUCCESS;
  routine.set_exec_env(exec_env);
  const ObIArray<ObPLRoutineParam *> &param_infos = routine_info.get_params();
  int64_t param_cnt = param_infos.count();
  routine.set_arg_count(param_cnt);
  routine.set_proc_type(routine_info.get_type());
  routine.set_tenant_id(routine_info.get_tenant_id());
  routine.set_database_id(routine_info.get_db_id());
  routine.set_package_id(routine_info.get_pkg_id());
  routine.set_routine_id(routine_info.get_id());
  routine.set_priv_user(routine_info.get_priv_user());
  if (routine_info.is_invoker_right()) {
    routine.set_invoker_right();
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < routine_info.get_params().count(); ++i) {
    const ObPLRoutineParam *param = routine_info.get_params().at(i);
    if (param->is_inout_param() || param->is_out_param()) {
      if (OB_FAIL(routine.add_out_arg(i))) {
        LOG_WARN("Failed to add out arg", K(i), K(ret));
      }
    }
    if (OB_SUCC(ret)) {
      if (param->is_inout_param() || param->is_in_param()) {
        if (OB_FAIL(routine.add_in_arg(i))) {
          LOG_WARN("Failed to add in arg", K(i), K(ret));
        }
      }
    }
  }
  ObString database_name, package_name;
  OZ (format_object_name(schema_guard,
                         routine_info.get_tenant_id(),
                         routine_info.get_db_id(),
                         routine_info.get_pkg_id(),
                         database_name,
                         package_name));
  OZ (routine.set_database_name(database_name));
  OZ (routine.set_package_name(package_name));
  OZ (routine.set_function_name(routine_info.get_name()));
  return ret;
}

int ObPLCompiler::generate_package_conditions(const ObPLConditionTable &ast_condition_table,
                                              ObPLPackage &package)
{
  int ret = OB_SUCCESS;
  for (int64_t i = 0; OB_SUCC(ret) && i < ast_condition_table.get_count(); ++i) {
    const ObPLCondition *ast_condition = ast_condition_table.get_condition(i);
    if (OB_ISNULL(ast_condition)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("pl condition is null", K(ret), K(i), K(package.get_id()));
    } else {
      ObIAllocator &alloc = package.get_allocator();
      ObPLCondition *package_condition =
        static_cast<ObPLCondition *>(alloc.alloc(sizeof(ObPLCondition)));
      if (OB_ISNULL(package_condition)) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
        LOG_WARN("allocate memory failed", K(ret), K(package_condition));
      } else {
        ObIArray<ObPLCondition *> &package_condition_table =
          const_cast<ObIArray<ObPLCondition *> &>(package.get_condition_table());
        new (package_condition) ObPLCondition();
        OZ (package_condition->deep_copy(*ast_condition, alloc));
        OZ (package_condition_table.push_back(package_condition));
      }
    }
  }
  return ret;
}

int ObPLCompiler::generate_package_cursors(
  const ObPLPackageAST &package_ast, const ObPLCursorTable &ast_cursor_table, ObPLPackage &package)
{
  int ret = OB_SUCCESS;
  ObPLCursorTable &cursor_table = const_cast<ObPLCursorTable &>(package.get_cursor_table());
  for (int64_t i = 0; OB_SUCC(ret) && i < ast_cursor_table.get_count(); ++i) {
    const ObPLCursor *ast_cursor = ast_cursor_table.get_cursor(i);
    if (OB_ISNULL(ast_cursor)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("package ast cursor is null", K(ret), K(i), K(ast_cursor));
    } else {
      ObString sql;
      ObString ps_sql;
      ObRecordType *row_desc = NULL;
      ObPLDataType cursor_type;
      OZ (ob_write_string(package.get_allocator(), ast_cursor->get_sql(), sql));
      OZ (ob_write_string(package.get_allocator(), ast_cursor->get_ps_sql(), ps_sql));
      if (OB_SUCC(ret) && OB_NOT_NULL(ast_cursor->get_row_desc())) {
        row_desc = static_cast<ObRecordType *>(package.get_allocator().alloc(sizeof(ObRecordType)));
        if (OB_ISNULL(row_desc)) {
          ret = OB_ALLOCATE_MEMORY_FAILED;
          LOG_WARN("failed to alloc record memory for record type", K(ret), K(row_desc));
        }
        OX (row_desc = new(row_desc)ObRecordType());
        OZ (row_desc->deep_copy(package.get_allocator(), *(ast_cursor->get_row_desc()), false));
      }
      OZ (cursor_type.deep_copy(package.get_allocator(), ast_cursor->get_cursor_type()));
      //Sql参数表达式,需要Copy下
      ObSEArray<int64_t, 4> sql_params;
      for (int64_t i = 0; OB_SUCC(ret) && i < ast_cursor->get_sql_params().count(); ++i) {
        ObRawExpr *expr = NULL;
        CK (OB_NOT_NULL(package_ast.get_expr(ast_cursor->get_sql_params().at(i))));
        OZ (ObPLExprCopier::copy_expr(package.get_expr_factory(),
                                      package_ast.get_expr(ast_cursor->get_sql_params().at(i)),
                                      expr));
        CK (OB_NOT_NULL(expr));
        //不再构造Exprs,直接将表达式存在int64的数组上
        OZ (sql_params.push_back(reinterpret_cast<int64_t>(expr)));
      }
      OZ (cursor_table.add_cursor(ast_cursor->get_package_id(),
                                  ast_cursor->get_routine_id(),
                                  ast_cursor->get_index(),//代表在Package符号表中的位置
                                  sql,//Cursor Sql
                                  sql_params,//Cursor参数表达式
                                  ps_sql,
                                  ast_cursor->get_stmt_type(),
                                  ast_cursor->is_for_update(),
                                  ast_cursor->has_hidden_rowid(),
                                  ast_cursor->get_rowid_table_id(),
                                  ast_cursor->get_ref_objects(),
                                  row_desc,
                                  cursor_type,
                                  ast_cursor->get_formal_params(),
                                  ast_cursor->get_state(),
                                  ast_cursor->is_dup_column()));
    }
  }
  return ret;
}

int ObPLCompiler::generate_package_vars(
  const ObPLPackageAST &package_ast, const ObPLSymbolTable &ast_var_table, ObPLPackage &package)
{
  int ret = OB_SUCCESS;
  const ObIArray<int64_t> *symbols = NULL;
  for (int64_t i = 0; OB_SUCC(ret) && i < ast_var_table.get_count(); ++i) {
    const ObPLVar *ast_var = ast_var_table.get_symbol(i);
    if (OB_ISNULL(ast_var)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("pl var is null", "package_id", package.get_id(), K(i), K(ret));
    } else {
      ObIAllocator &alloc = package.get_allocator();
      ObPLVar *package_var = static_cast<ObPLVar *>(alloc.alloc(sizeof(ObPLVar)));
      if (OB_ISNULL(package_var)) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
        LOG_WARN("allocate memory failed", K(ret));
      } else {
        new (package_var) ObPLVar();
        OZ (package_var->deep_copy(*ast_var, alloc), K(package.get_id()), K(i));
        if (OB_SUCC(ret)) {
          ObIArray<ObPLVar *> &package_var_table = const_cast<ObIArray<ObPLVar *> &>(package.get_var_table());
          if (OB_FAIL(package_var_table.push_back(package_var))) {
            LOG_WARN("package var table add var failed", "package_id", package.get_id(), K(i), K(ret));
          }
        }
      }
    }
  }
  return ret;
}

int ObPLCompiler::compile_types(const ObIArray<const ObUserDefinedType*> &types,
                                ObPLCompileUnit &unit)
{
  int ret = OB_SUCCESS;
  for (int64_t i = 0; OB_SUCC(ret) && i < types.count(); ++i) {
    ObUserDefinedType *ast_type = const_cast<ObUserDefinedType *>(types.at(i));
    if (OB_ISNULL(ast_type)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("pl user defined type is null", K(i), K(ret));
    } else {
      ObIAllocator &alloc = unit.get_allocator();
      ObUserDefinedType *user_type = NULL;
      switch(ast_type->get_type()) {
      case PL_RECORD_TYPE: {
        ObRecordType *record_type = static_cast<ObRecordType *>(alloc.alloc(sizeof(ObRecordType)));
        if (OB_ISNULL(record_type)) {
          ret = OB_ALLOCATE_MEMORY_FAILED;
          LOG_WARN("allocate memory failed", K(ret), KPC(ast_type), K(i));
        } else {
          new (record_type) ObRecordType();
          if (OB_FAIL(record_type->deep_copy(alloc,
                                             *(static_cast<ObRecordType *>(ast_type)),
                                             false))) {
            LOG_WARN("pl user type deep copy failed", K(ret), KPC(ast_type), K(i));
          } else {
            user_type = record_type;
          }
        }
      }
        break;
      case PL_CURSOR_TYPE: {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("unexpected package cursor type, should ref cursor type", K(ret));
      }
        break;
      case PL_REF_CURSOR_TYPE:
       {
        ObRefCursorType *cursor_type = static_cast<ObRefCursorType *>(alloc.alloc(sizeof(ObRefCursorType)));
        if (OB_ISNULL(cursor_type)) {
          ret = OB_ALLOCATE_MEMORY_FAILED;
          LOG_WARN("allocate memory for cursor type failed", K(ret), K(cursor_type), KPC(ast_type), K(i));
        } else {
          new (cursor_type) ObRefCursorType();
          if (OB_FAIL(cursor_type->deep_copy(alloc, *(static_cast<ObRefCursorType *>(ast_type))))) {
            LOG_WARN("deep copy ref cursor type failed", K(ret), KPC(ast_type), K(i));
          } else {
            user_type = cursor_type;
          }
        }
      }
        break;
      default: {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("invalid pl type", K(ret), KPC(ast_type), K(i));
      }
        break;
      }

      if (OB_SUCC(ret)) {
        ObIArray<ObUserDefinedType *> &unit_type_table
            = const_cast<ObIArray<ObUserDefinedType *> &>(unit.get_type_table());
        if (OB_FAIL(unit_type_table.push_back(user_type))) {
          LOG_WARN("package user type table add type failed", K(ret));
        }
      }
    }
  }
  return ret;
}

int ObPLCompiler::compile_type_table(const ObPLUserTypeTable &ast_type_table, ObPLCompileUnit &unit)
{
  int ret = OB_SUCCESS;
  OZ (ObPLCompiler::compile_types(ast_type_table.get_types(), unit));
  OZ (ObPLCompiler::compile_types(ast_type_table.get_external_types(), unit));
  return ret;
}

int ObPLCompiler::generate_package_types(const ObPLUserTypeTable &ast_type_table,
                                         ObPLCompileUnit &compile_unit)
{
  return ObPLCompiler::compile_types(ast_type_table.get_types(), compile_unit);
}

int ObPLCompiler::compile_subprogram_table(common::ObIAllocator &allocator,
                                           sql::ObSQLSessionInfo &session_info,
                                           const sql::ObExecEnv &exec_env,
                                           ObPLRoutineTable &routine_table,
                                           ObPLCompileUnit &compile_unit,
                                           ObSchemaGetterGuard &schema_guard)
{
  int ret = OB_SUCCESS;
  compile_unit.init_routine_table(routine_table.get_count());
  for(int64_t routine_idx = 0; OB_SUCC(ret) && routine_idx < routine_table.get_count(); routine_idx++) {
    ObPLFunctionAST *routine_ast = NULL;
    const ObPLRoutineInfo *routine_info = NULL;
    if (OB_FAIL(routine_table.get_routine_info(routine_idx, routine_info))) {
      LOG_WARN("get routine info failed", K(ret));
    } else if (OB_ISNULL(routine_info)) {
      //empty routine info is possible when package body is empty, push null to make idx correct
      if (OB_FAIL(compile_unit.add_routine(NULL))) {
        LOG_WARN("package add routine failed", K(ret));
      }
    } else if (OB_FAIL(routine_table.get_routine_ast(routine_idx, routine_ast))) {
      LOG_WARN("get routine ast failed", K(routine_idx), K(ret));
    } else if (OB_ISNULL(routine_ast)) {
      //empty routine ast is possible when package spec generating or package body is empty
      // push null to make idx correct, will report error when called
      if (OB_FAIL(compile_unit.add_routine(NULL))) {
        LOG_WARN("add routine failed", K(ret));
      }
    } else {
      ObPLFunction *routine = NULL;
      if (OB_ISNULL(routine
        = static_cast<ObPLFunction *>(compile_unit.get_allocator().alloc(sizeof(ObPLFunction))))) {
        LOG_WARN("allocate memory failed", K(ret));
      } else {
        new (routine) ObPLFunction(compile_unit.get_mem_context());
        OZ (init_function(schema_guard, exec_env, *routine_info, *routine));
        if (OB_SUCC(ret)) {

#ifdef USE_MCJIT
          HEAP_VAR(ObPLCodeGenerator, cg ,allocator_, session_info) {
#else
          HEAP_VAR(ObPLCodeGenerator, cg, allocator,
                   session_info,
                   schema_guard,
                   *routine_ast,
                   routine->get_expressions(),
                   routine->get_helper(),
                   routine->get_di_helper(),
                   lib::is_oracle_mode()) {
#endif
            lib::ObMallocHookAttrGuard malloc_guard(lib::ObMemAttr(MTL_ID(), "PlCodeGen"));
            if (OB_FAIL(cg.init())) {
              LOG_WARN("init code generator failed", K(ret));
            } else if (OB_FAIL(cg.generate(*routine))) {
              LOG_WARN("code generate failed", "routine name", routine_ast->get_name(), K(ret));
            } else {
              routine->set_ret_type(routine_ast->get_ret_type());
              if (OB_FAIL(compile_unit.add_routine(routine))) {
                LOG_WARN("package add routine failed", K(ret));
              }
            }
          } // end of HEAP_VAR
        }
      }
      if (OB_FAIL(ret) && OB_NOT_NULL(routine)) {
        routine->~ObPLFunction();
        compile_unit.get_allocator().free(routine);
        routine = NULL;
      }
    }
  }
  return ret;
}

int ObPLCompiler::generate_package_routines(
  const ObString &exec_env, ObPLRoutineTable &routine_table, ObPLPackage &package)
{
  int ret = OB_SUCCESS;
  sql::ObExecEnv env;
  if (OB_FAIL(env.init(exec_env))) {
    LOG_WARN("failed to init exec env", K(exec_env), K(ret));
  } else if (OB_FAIL(ObPLCompiler::compile_subprogram_table(allocator_,
      session_info_,
      env,
      routine_table,
      package, schema_guard_))) {
    LOG_WARN("failed to compile subprogram table",
             K(package.get_id()),
             K(package.get_db_name()),
             K(package.get_name()),
             K(ret));
  } else { /*do nothing*/ }
  return ret;
}

int ObPLCompiler::format_object_name(ObSchemaGetterGuard &schema_guard,
                                     const uint64_t tenant_id,
                                     const uint64_t db_id,
                                     const uint64_t package_id,
                                     ObString &database_name,
                                     ObString &package_name)
{
  int ret = OB_SUCCESS;
  const ObPackageInfo *pkg_info = NULL;
  const ObUDTTypeInfo *udt_info = NULL;
  const ObDatabaseSchema *database_schema = NULL;
  if (OB_INVALID_ID != db_id) {
    OZ (schema_guard.get_database_schema(tenant_id, db_id, database_schema));
  }
  if (OB_INVALID_ID != package_id) {
    OZ (schema_guard.get_package_info(tenant_id, package_id, pkg_info));
    if (OB_SUCCESS == ret && NULL == pkg_info) {
      OZ (schema_guard.get_udt_info(tenant_id, package_id, udt_info));
    }
  }
  if (OB_SUCC(ret)) {
    if (OB_NOT_NULL(database_schema)) {
      OX (database_name = database_schema->get_database_name_str());
    }
    if (OB_NOT_NULL(pkg_info)) {
      OX (package_name = pkg_info->get_package_name());
    } else if (OB_NOT_NULL(udt_info)) {
      OX (package_name = udt_info->get_type_name());
    }
  }
  return ret;
}

ObPLCompilerEnvGuard::ObPLCompilerEnvGuard(const ObPackageInfo &info,
                                           ObSQLSessionInfo &session_info,
                                           share::schema::ObSchemaGetterGuard &schema_guard,
                                           int &ret)
  : ret_(ret), session_info_(session_info)
{
  init(info, session_info, schema_guard, ret);
}

ObPLCompilerEnvGuard::ObPLCompilerEnvGuard(const ObRoutineInfo &info,
                                           ObSQLSessionInfo &session_info,
                                           share::schema::ObSchemaGetterGuard &schema_guard,
                                           int &ret)
  : ret_(ret), session_info_(session_info)
{
  init(info, session_info, schema_guard, ret);
}

template<class Info>
void ObPLCompilerEnvGuard::init(const Info &info, ObSQLSessionInfo &session_info, share::schema::ObSchemaGetterGuard &schema_guard, int &ret)
{
  ObExecEnv env;
  OX (need_reset_exec_env_ = false);
  OX (need_reset_default_database_ = false);
  OZ (old_exec_env_.load(session_info_));
  OZ (env.init(info.get_exec_env()));
  if (OB_SUCC(ret) && old_exec_env_ != env) {
    OZ (env.store(session_info_));
    OX (need_reset_exec_env_ = true);
  }
  if (OB_SUCC(ret)
      && !info.is_invoker_right()
      && info.get_database_id() != session_info_.get_database_id()) {
    const share::schema::ObDatabaseSchema *db_schema = NULL;
    old_db_id_ = session_info_.get_database_id();
    OZ (old_db_name_.append(session_info_.get_database_name()));
    OZ (schema_guard.get_database_schema(info.get_tenant_id(), info.get_database_id(), db_schema));
    CK (OB_NOT_NULL(db_schema));
    OZ (session_info_.set_default_database(db_schema->get_database_name_str()));
    OX (session_info_.set_database_id(db_schema->get_database_id()));
    OX (need_reset_default_database_ = true);
  }
}

ObPLCompilerEnvGuard::~ObPLCompilerEnvGuard()
{
  int ret = OB_SUCCESS;
  if (need_reset_exec_env_) {
    if ((ret = old_exec_env_.store(session_info_)) != OB_SUCCESS) {
      ret_ = OB_SUCCESS == ret_ ? ret : ret_;
      LOG_WARN("failed to restore exec env in pl env guard", K(ret), K(ret_), K(old_exec_env_));
    }
  }
  if (need_reset_default_database_) {
    if ((ret = session_info_.set_default_database(old_db_name_.string())) != OB_SUCCESS) {
      ret_ = OB_SUCCESS == ret_ ? ret : ret_;
      LOG_WARN("failed to reset default database in pl env guard", K(ret), K(ret_), K(old_db_name_));
    } else {
      session_info_.set_database_id(old_db_id_);
    }
  }
}

} //end namespace pl
} //end namespace oceanbase
