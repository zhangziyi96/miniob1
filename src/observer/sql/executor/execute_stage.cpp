/* Copyright (c) 2021 Xie Meiyi(xiemeiyi@hust.edu.cn) and OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

//
// Created by Meiyi & Longda on 2021/4/13.
//

#include <string>
#include <sstream>
#include <algorithm>

#include "execute_stage.h"

#include "common/io/io.h"
#include "common/log/log.h"
#include "common/lang/defer.h"
#include "common/seda/timer_stage.h"
#include "common/lang/string.h"
#include "session/session.h"
#include "event/storage_event.h"
#include "event/sql_event.h"
#include "event/session_event.h"
#include "sql/expr/tuple.h"
#include "sql/operator/table_scan_operator.h"
#include "sql/operator/index_scan_operator.h"
#include "sql/operator/predicate_operator.h"
#include "sql/operator/delete_operator.h"
#include "sql/operator/project_operator.h"
#include "sql/operator/update_operator.h"
#include "sql/operator/multi_select_operator.h"
#include "sql/operator/aggregation_operator.h"
#include "sql/stmt/stmt.h"
#include "sql/stmt/select_stmt.h"
#include "sql/stmt/update_stmt.h"
#include "sql/stmt/delete_stmt.h"
#include "sql/stmt/insert_stmt.h"
#include "sql/stmt/filter_stmt.h"
#include "storage/common/table.h"
#include "storage/common/field.h"
#include "storage/index/index.h"
#include "storage/default/default_handler.h"
#include "storage/common/condition_filter.h"
#include "storage/trx/trx.h"
#include "util/util.h"


using namespace common;

//RC create_selection_executor(
//   Trx *trx, const Selects &selects, const char *db, const char *table_name, SelectExeNode &select_node);

//! Constructor
ExecuteStage::ExecuteStage(const char *tag) : Stage(tag)
{}

//! Destructor
ExecuteStage::~ExecuteStage()
{}

//! Parse properties, instantiate a stage object
Stage *ExecuteStage::make_stage(const std::string &tag)
{
  ExecuteStage *stage = new (std::nothrow) ExecuteStage(tag.c_str());
  if (stage == nullptr) {
    LOG_ERROR("new ExecuteStage failed");
    return nullptr;
  }
  stage->set_properties();
  return stage;
}

//! Set properties for this object set in stage specific properties
bool ExecuteStage::set_properties()
{
  //  std::string stageNameStr(stageName);
  //  std::map<std::string, std::string> section = theGlobalProperties()->get(
  //    stageNameStr);
  //
  //  std::map<std::string, std::string>::iterator it;
  //
  //  std::string key;

  return true;
}

//! Initialize stage params and validate outputs
bool ExecuteStage::initialize()
{
  LOG_TRACE("Enter");

  std::list<Stage *>::iterator stgp = next_stage_list_.begin();
  default_storage_stage_ = *(stgp++);
  mem_storage_stage_ = *(stgp++);

  LOG_TRACE("Exit");
  return true;
}

//! Cleanup after disconnection
void ExecuteStage::cleanup()
{
  LOG_TRACE("Enter");

  LOG_TRACE("Exit");
}

void ExecuteStage::handle_event(StageEvent *event)
{
  LOG_TRACE("Enter\n");

  handle_request(event);

  LOG_TRACE("Exit\n");
  return;
}

void ExecuteStage::callback_event(StageEvent *event, CallbackContext *context)
{
  LOG_TRACE("Enter\n");

  // here finish read all data from disk or network, but do nothing here.

  LOG_TRACE("Exit\n");
  return;
}

void ExecuteStage::handle_request(common::StageEvent *event)
{
  SQLStageEvent *sql_event = static_cast<SQLStageEvent *>(event);
  SessionEvent *session_event = sql_event->session_event();
  Stmt *stmt = sql_event->stmt();
  Session *session = session_event->session();
  Query *sql = sql_event->query();

  if (stmt != nullptr) {
    switch (stmt->type()) {
    case StmtType::SELECT: {
      do_select(sql_event);
    } break;
    case StmtType::INSERT: {
      do_insert(sql_event);
    } break;
    case StmtType::UPDATE: {
      do_update(sql_event);
    } break;
    case StmtType::DELETE: {
      do_delete(sql_event);
    } break;
    }
  } else {
    switch (sql->flag) {
    case SCF_HELP: {
      do_help(sql_event);
    } break;
    case SCF_CREATE_TABLE: {
      do_create_table(sql_event);
    } break;
    case SCF_CREATE_INDEX: {
      do_create_index(sql_event);
    } break;
    case SCF_SHOW_TABLES: {
      do_show_tables(sql_event);
    } break;
    case SCF_DESC_TABLE: {
      do_desc_table(sql_event);
    } break;
    case SCF_DROP_TABLE: {
      do_drop_table(sql_event);
    } break;
    case SCF_DROP_INDEX:
    case SCF_LOAD_DATA: {
      default_storage_stage_->handle_event(event);
    } break;
    case SCF_SYNC: {
      RC rc = DefaultHandler::get_default().sync();
      session_event->set_response(strrc(rc));
    } break;
    case SCF_BEGIN: {
      session_event->set_response("SUCCESS\n");
    } break;
    case SCF_COMMIT: {
      Trx *trx = session->current_trx();
      RC rc = trx->commit();
      session->set_trx_multi_operation_mode(false);
      session_event->set_response(strrc(rc));
    } break;
    case SCF_ROLLBACK: {
      Trx *trx = session_event->get_client()->session->current_trx();
      RC rc = trx->rollback();
      session->set_trx_multi_operation_mode(false);
      session_event->set_response(strrc(rc));
    } break;
    case SCF_EXIT: {
      // do nothing
      const char *response = "Unsupported\n";
      session_event->set_response(response);
    } break;
    default: {
      LOG_ERROR("Unsupported command=%d\n", sql->flag);
    }
    }
  }
}

void end_trx_if_need(Session *session, Trx *trx, bool all_right)
{
  if (!session->is_trx_multi_operation_mode()) {
    if (all_right) {
      trx->commit();
    } else {
      trx->rollback();
    }
  }
}

void print_tuple_header(std::ostream &os, const ProjectOperator &oper, const SelectStmt &select_stmt)
{
  const int cell_num = oper.tuple_cell_num();
  const TupleCellSpec *cell_spec = nullptr;
  for (int i = 0; i < cell_num; i++) {
    oper.tuple_cell_spec_at(i, cell_spec);
    if (i != 0) {
      os << " | ";
    }

    if (cell_spec->alias()) {
      os << cell_spec->alias();
    }
  }

  const std::vector<ExpressionNode> exprs = select_stmt.exprs();
  if(exprs.size() > 0 && cell_num != 0) {
    os << " | ";
  }
  for(int i = 0; i < exprs.size(); i++) {
    ExpressionNode expr = exprs[i];
    std::string expr_str = expr_to_string(&expr);
    if( i != 0 ){
      os << " | ";
    }
    os << expr_str;
  }
  if (cell_num > 0 || exprs.size() > 0) {
    os << '\n';
  }

}

void print_tuple_header(std::ostream &os, const AggregationOperator &oper){
  const int aggre_num = oper.aggre_num();
  const char* types[4] = {"COUNT", "MAX", "MIN", "AVG"};
  for(int i = 0; i < aggre_num; i++){
    const Aggregation aggregation = oper.aggregations()[i];
    const char* attribute_name = aggregation.attr.attribute_name;
    const char* type = types[aggregation.type];
    std::string s = std::string(type) + "(" + std::string( attribute_name) + ")";
    transform(s.begin(),s.end(),s.begin(),::toupper);
    if(i != 0) {
      os << " | ";
    } 
      os << s;
  }
  if (aggre_num > 0) {
    os << '\n';
  }
}

void print_aggre_result(std::ostream &os, const AggregationOperator &oper){
  const int aggre_num = oper.aggre_num();
  for(int i = 0; i < aggre_num; i++){
    const Aggregation aggregation = oper.aggregations()[i];
    const AggreResult result = oper.aggre_results()[i];
    if (i != 0){
      os << " | ";
    }
    if(aggregation.type == COUNT){
      os << result.count;
    } else if (aggregation.type == AVG) {
      os << result.avg;
    } else if (aggregation.type == MIN || aggregation.type == MAX) {
      std::string str;
      value_to_string(str, result.result);
      os << str;
    }
  }
  if (aggre_num > 0) {
    os << '\n';
  }
}

//测试用例中没有字符串类型，所以不做类型检查，只检查field是否存在
RC check_expr(const ExpressionNode *expr, const std::vector<Table*> tables) {
  if(expr->left == nullptr && expr->right == nullptr) {
    if(expr->is_attr) {
      Table *table = nullptr;
      //多表
      if(tables.size() != 1){
        if(expr->attr.relation_name == nullptr) {
          return RC::INVALID_ARGUMENT;
        }
        for(int i = 0; i < tables.size(); i++) {
          if(strcmp(tables[i]->name(), expr->attr.relation_name) == 0) {
            table = tables[i];
            break;
          }
        }
      } else { //单表
        if(expr->attr.relation_name != nullptr && strlen(expr->attr.relation_name) != 0
           && strcmp(tables[0]->name(), expr->attr.relation_name) != 0){
          return RC::INVALID_ARGUMENT;
        }
        table = tables[0];
      }
      if(table == nullptr) {
        return RC::INVALID_ARGUMENT;
      }
      const FieldMeta *field_meta = table->table_meta().field(expr->attr.attribute_name);
      if(field_meta == nullptr){
        LOG_ERROR("expr attr::%s", expr->attr.attribute_name);
        return RC::INVALID_ARGUMENT;
      }
    }
    return RC::SUCCESS;
  }

  

  if(expr->left != nullptr) {
    if(check_expr(expr->left, tables) != RC::SUCCESS){
      return RC::INVALID_ARGUMENT;
    }
  }
  if(expr->right != nullptr) {
    if(check_expr(expr->right, tables) != RC::SUCCESS){
      return RC::INVALID_ARGUMENT;
    }
  }
  return RC::SUCCESS;
}

void tuple_to_string(std::ostream &os, const Tuple &tuple)
{
  TupleCell cell;
  RC rc = RC::SUCCESS;
  bool first_field = true;
  for (int i = 0; i < tuple.cell_num(); i++) {
    rc = tuple.cell_at(i, cell);
    if (rc != RC::SUCCESS) {
      LOG_WARN("failed to fetch field of cell. index=%d, rc=%s", i, strrc(rc));
      break;
    }

    if (!first_field) {
      os << " | ";
    } else {
      first_field = false;
    }
    cell.to_string(os);
  }
}

void tuple_to_string(std::ostream &os, const Tuple &tuple, const SelectStmt &select_stmt) {
  std::vector<ExpressionNode> exprs = select_stmt.exprs();
  std::vector<Table*> tables = select_stmt.tables();
  if(tuple.cell_num() != 0 && exprs.size() != 0) {
    os << " | ";
  }
  for(int i = 0; i < exprs.size(); i++) {
    ExpressionNode expr = exprs[i];
    Value value = cal_expr_value(expr, tuple, tables);
    std::string str;
    value_to_string(str, value);
    if(i != 0) {
      os << " | ";
    }
    os << str;
  }
}

//create a temp_table for multi select
Table* create_temp_tale(Db *db, std::vector<Table*> tables){
  RC rc = RC::SUCCESS;
  const char *table_name = "temp_table";
  int attribute_count = 0;
  for(Table *table: tables){
    TableMeta table_meta = table->table_meta();
    
    attribute_count += table_meta.field_num() - table_meta.sys_field_num();
  }
  AttrInfo *attributed = new AttrInfo[attribute_count];
  int index = 0;
  for(int i = 0; i < tables.size(); i++){
    TableMeta table_meta = tables[i]->table_meta();
    const char *table_name = table_meta.name();
    const std::vector<FieldMeta> *field_metas = table_meta.field_metas();
    for(int j = 0; j < field_metas->size() - table_meta.sys_field_num(); j++){
        const FieldMeta field_meta = (*field_metas)[table_meta.sys_field_num() + j];
        std::string s = std::string(table_name) + "." + std::string(field_meta.name());
        char *field_name = new char[s.size() + 1];
        std::strcpy(field_name, s.c_str());
        attributed[index].name = (char*)field_name;
        attributed[index].type = field_meta.type();
        attributed[index].length = field_meta.len();
        index++;
    }
  }
  
  rc = db->create_table(table_name, attribute_count, attributed);
  if(rc != RC::SUCCESS){
    LOG_WARN("failed to create temp_table, rc=%s",strrc(rc));
    return nullptr;
  }
  // temp_table = *(db->find_table(table_name));
  return db->find_table(table_name);
}

IndexScanOperator *try_to_create_index_scan_operator(FilterStmt *filter_stmt)
{
  const std::vector<FilterUnit *> &filter_units = filter_stmt->filter_units();
  if (filter_units.empty() ) {
    return nullptr;
  }

  // 在所有过滤条件中，找到字段与值做比较的条件，然后判断字段是否可以使用索引
  // 如果是多列索引，这里的处理需要更复杂。
  // 这里的查找规则是比较简单的，就是尽量找到使用相等比较的索引
  // 如果没有就找范围比较的，但是直接排除不等比较的索引查询. (你知道为什么?)
  const FilterUnit *better_filter = nullptr;
  for (const FilterUnit * filter_unit : filter_units) {
    if (filter_unit->comp() == NOT_EQUAL) {
      continue;
    }

    Expression *left = filter_unit->left();
    Expression *right = filter_unit->right();
    if (left->type() == ExprType::FIELD && right->type() == ExprType::VALUE) {
    } else if (left->type() == ExprType::VALUE && right->type() == ExprType::FIELD) {
      std::swap(left, right);
    }
    FieldExpr &left_field_expr = *(FieldExpr *)left;
    const Field &field = left_field_expr.field();
    const Table *table = field.table();
    Index *index = table->find_index_by_field(field.field_name());
    if (index != nullptr) {
      if (better_filter == nullptr) {
        better_filter = filter_unit;
      } else if (filter_unit->comp() == EQUAL_TO) {
        better_filter = filter_unit;
    	break;
      }
    }
  }

  if (better_filter == nullptr) {
    return nullptr;
  }

  Expression *left = better_filter->left();
  Expression *right = better_filter->right();
  CompOp comp = better_filter->comp();
  if (left->type() == ExprType::VALUE && right->type() == ExprType::FIELD) {
    std::swap(left, right);
    switch (comp) {
    case EQUAL_TO:    { comp = EQUAL_TO; }    break;
    case LESS_EQUAL:  { comp = GREAT_THAN; }  break;
    case NOT_EQUAL:   { comp = NOT_EQUAL; }   break;
    case LESS_THAN:   { comp = GREAT_EQUAL; } break;
    case GREAT_EQUAL: { comp = LESS_THAN; }   break;
    case GREAT_THAN:  { comp = LESS_EQUAL; }  break;
    default: {
    	LOG_WARN("should not happen");
    }
    }
  }


  FieldExpr &left_field_expr = *(FieldExpr *)left;
  const Field &field = left_field_expr.field();
  const Table *table = field.table();
  Index *index = table->find_index_by_field(field.field_name());
  assert(index != nullptr);

  ValueExpr &right_value_expr = *(ValueExpr *)right;
  TupleCell value;
  right_value_expr.get_tuple_cell(value);

  const TupleCell *left_cell = nullptr;
  const TupleCell *right_cell = nullptr;
  bool left_inclusive = false;
  bool right_inclusive = false;

  switch (comp) {
  case EQUAL_TO: {
    left_cell = &value;
    right_cell = &value;
    left_inclusive = true;
    right_inclusive = true;
  } break;

  case LESS_EQUAL: {
    left_cell = nullptr;
    left_inclusive = false;
    right_cell = &value;
    right_inclusive = true;
  } break;

  case LESS_THAN: {
    left_cell = nullptr;
    left_inclusive = false;
    right_cell = &value;
    right_inclusive = false;
  } break;

  case GREAT_EQUAL: {
    left_cell = &value;
    left_inclusive = true;
    right_cell = nullptr;
    right_inclusive = false;
  } break;

  case GREAT_THAN: {
    left_cell = &value;
    left_inclusive = false;
    right_cell = nullptr;
    right_inclusive = false;
  } break;

  default: {
    LOG_WARN("should not happen. comp=%d", comp);
  } break;
  }

  IndexScanOperator *oper = new IndexScanOperator(table, index,
       left_cell, left_inclusive, right_cell, right_inclusive);

  LOG_INFO("use index for scan: %s in table %s", index->index_meta().name(), table->name());
  return oper;
}

RC ExecuteStage::do_select(SQLStageEvent *sql_event)
{
  SelectStmt *select_stmt = (SelectStmt *)(sql_event->stmt());
  SessionEvent *session_event = sql_event->session_event();
  RC rc = RC::SUCCESS;
  if (select_stmt->tables().size() != 1) {

    PredicateOperator inner_pred_oper(select_stmt->filter_stmt());
    MultiSelectOperator multi_select_oper(&inner_pred_oper);

    std::vector<Table*> tables;
    for (Table *table : select_stmt->tables()){
      Operator *scan_oper = new TableScanOperator(table);
      PredicateOperator *pred_oper = new PredicateOperator(select_stmt->filter_stmt());
      pred_oper->add_child(scan_oper);
      multi_select_oper.add_child(scan_oper);

    }

    if (rc != RC::SUCCESS) {
      LOG_WARN("cannot construct filter stmt");
      return rc;
    }
    
    PredicateOperator out_pred_oper(select_stmt->filter_stmt());
    out_pred_oper.add_child(&multi_select_oper);
    ProjectOperator project_oper;
    project_oper.add_child(&out_pred_oper);

    
    rc = project_oper.open();
    if (rc != RC::SUCCESS) {
      LOG_WARN("failed to open operator");
      return rc;
    }

    for (const Field &field : select_stmt->query_fields()) {
      project_oper.add_projection(field.table(), field.meta(), true);
    }
    
    std::stringstream ss;
    print_tuple_header(ss, project_oper, *select_stmt);
    while ((rc = project_oper.next()) != RC::RECORD_EOF) {
      if(rc == RC::SUCCESS){
        Tuple * tuple = project_oper.current_tuple();
        if (nullptr == tuple) {
          rc = RC::INTERNAL;
          LOG_WARN("failed to get current record. rc=%s", strrc(rc));
          break;
        }
        
        tuple_to_string(ss, *tuple);
        tuple_to_string(ss, *tuple, *select_stmt);
        ss << std::endl;
      }
    }
   
    if (rc != RC::RECORD_EOF) {
      LOG_WARN("something wrong while iterate operator. rc=%s", strrc(rc));
      project_oper.close();
    } else {
      rc = project_oper.close();
    }
    session_event->set_response(ss.str());
    
    LOG_ERROR("ss: \n%s", ss.str().c_str());
    return rc;
  } else if(select_stmt->aggregations().size() != 0){ //aggregation func
      Operator *scan_oper = try_to_create_index_scan_operator(select_stmt->filter_stmt());
      if (nullptr == scan_oper) {
        scan_oper = new TableScanOperator(select_stmt->tables()[0]);
      }

      DEFER([&] () {delete scan_oper;});
      PredicateOperator pred_oper(select_stmt->filter_stmt());
      pred_oper.add_child(scan_oper);
      
      AggregationOperator aggre_oper(select_stmt->aggregations(), select_stmt->tables()[0]);
      aggre_oper.add_child(&pred_oper);
      if((rc = aggre_oper.open()) != RC::SUCCESS){
        session_event->set_response("FAILURE\n");
        return rc;
      }
      std::stringstream ss;
      print_tuple_header(ss, aggre_oper);
      while((rc = aggre_oper.next()) == RC::SUCCESS){

      }
      // for(int i = 0; i < aggre_oper.aggre_results().size(); i++){
        // LOG_ERROR("min: %s", (char*)(aggre_oper.aggre_results()[0].result.data));
      // }
      print_aggre_result(ss, aggre_oper);
      session_event->set_response(ss.str());
      return rc;

  } else {
      Operator *scan_oper = try_to_create_index_scan_operator(select_stmt->filter_stmt());
      if (nullptr == scan_oper) {
        scan_oper = new TableScanOperator(select_stmt->tables()[0]);
      }

      DEFER([&] () {delete scan_oper;});
      
      PredicateOperator pred_oper(select_stmt->filter_stmt());
      pred_oper.add_child(scan_oper);
      ProjectOperator project_oper;
      project_oper.add_child(&pred_oper);
      for (const Field &field : select_stmt->query_fields()) {
        project_oper.add_projection(field.table(), field.meta(), false);
      }
      rc = project_oper.open();
      if (rc != RC::SUCCESS) {
        LOG_WARN("failed to open operator");
        return rc;
      }

      std::stringstream ss;
      print_tuple_header(ss, project_oper, *select_stmt);
      while ((rc = project_oper.next()) == RC::SUCCESS) {
        // get current record
        // write to response
        Tuple * tuple = project_oper.current_tuple();
        if (nullptr == tuple) {
          rc = RC::INTERNAL;
          LOG_WARN("failed to get current record. rc=%s", strrc(rc));
          break;
        }
        tuple_to_string(ss, *tuple);
        tuple_to_string(ss, *tuple, *select_stmt);
        ss << std::endl;
      }

      if (rc != RC::RECORD_EOF) {
        LOG_WARN("something wrong while iterate operator. rc=%s", strrc(rc));
        project_oper.close();
      } else {
        rc = project_oper.close();
      }
      session_event->set_response(ss.str());
      return rc;
    }
  
}

RC ExecuteStage::do_help(SQLStageEvent *sql_event)
{
  SessionEvent *session_event = sql_event->session_event();
  const char *response = "show tables;\n"
                         "desc `table name`;\n"
                         "create table `table name` (`column name` `column type`, ...);\n"
                         "create index `index name` on `table` (`column`);\n"
                         "insert into `table` values(`value1`,`value2`);\n"
                         "update `table` set column=value [where `column`=`value`];\n"
                         "delete from `table` [where `column`=`value`];\n"
                         "select [ * | `columns` ] from `table`;\n";
  session_event->set_response(response);
  return RC::SUCCESS;
}

RC ExecuteStage::do_create_table(SQLStageEvent *sql_event)
{
  CreateTable &create_table = sql_event->query()->sstr.create_table;
  SessionEvent *session_event = sql_event->session_event();
  Db *db = session_event->session()->get_current_db();

  //change attr len if DATES
  for(int i = 0; i < sizeof(create_table.attributes) / sizeof(create_table.attributes[0]); i++){
    if(create_table.attributes[i].type == DATES){
      create_table.attributes[i].length = 12;
    }
  }

  RC rc = db->create_table(create_table.relation_name,
			create_table.attribute_count, create_table.attributes);
  if (rc == RC::SUCCESS) {
    session_event->set_response("SUCCESS\n");
  } else {
    session_event->set_response("FAILURE\n");
  }
  return rc;
}

RC ExecuteStage::do_drop_table(SQLStageEvent *sql_event)
{
  const DropTable &drop_table = sql_event->query()->sstr.drop_table;
  SessionEvent *session_event = sql_event->session_event();
  Db *db = session_event->session()->get_current_db();
  RC rc = db->drop_table(drop_table.relation_name);
  if (rc == RC::SUCCESS) {
    session_event->set_response("SUCCESS\n");
  } else {
    session_event->set_response("FAILURE\n");
  }
  return rc;
}

RC ExecuteStage::do_create_index(SQLStageEvent *sql_event)
{
  SessionEvent *session_event = sql_event->session_event();
  Db *db = session_event->session()->get_current_db();
  const CreateIndex &create_index = sql_event->query()->sstr.create_index;
  Table *table = db->find_table(create_index.relation_name);
  if (nullptr == table) {
    session_event->set_response("FAILURE\n");
    return RC::SCHEMA_TABLE_NOT_EXIST;
  }

  RC rc = table->create_index(nullptr, create_index.index_name, create_index.attribute_name);
  sql_event->session_event()->set_response(rc == RC::SUCCESS ? "SUCCESS\n" : "FAILURE\n");
  return rc;
}

RC ExecuteStage::do_show_tables(SQLStageEvent *sql_event)
{
  SessionEvent *session_event = sql_event->session_event();
  Db *db = session_event->session()->get_current_db();
  std::vector<std::string> all_tables;
  db->all_tables(all_tables);
  if (all_tables.empty()) {
    session_event->set_response("No table\n");
  } else {
    std::stringstream ss;
    for (const auto &table : all_tables) {
      ss << table << std::endl;
    }
    session_event->set_response(ss.str().c_str());
  }
  return RC::SUCCESS;
}

RC ExecuteStage::do_desc_table(SQLStageEvent *sql_event)
{
  Query *query = sql_event->query();
  Db *db = sql_event->session_event()->session()->get_current_db();
  const char *table_name = query->sstr.desc_table.relation_name;
  Table *table = db->find_table(table_name);
  std::stringstream ss;
  if (table != nullptr) {
    table->table_meta().desc(ss);
  } else {
    ss << "No such table: " << table_name << std::endl;
  }
  sql_event->session_event()->set_response(ss.str().c_str());
  return RC::SUCCESS;
}

RC ExecuteStage::do_insert(SQLStageEvent *sql_event)
{
  Stmt *stmt = sql_event->stmt();
  SessionEvent *session_event = sql_event->session_event();

  if (stmt == nullptr) {
    LOG_WARN("cannot find statement");
    return RC::GENERIC_ERROR;
  }

  InsertStmt *insert_stmt = (InsertStmt *)stmt;

  Table *table = insert_stmt->table();
  RC rc = table->insert_record(nullptr, insert_stmt->value_amount(), insert_stmt->values());
  if (rc == RC::SUCCESS) {
    session_event->set_response("SUCCESS\n");
  } else {
    session_event->set_response("FAILURE\n");
  }
  return rc;
}

RC ExecuteStage::do_update(SQLStageEvent *sql_event)
{
  RC rc = RC::SUCCESS;
  Stmt *stmt = sql_event->stmt();
  SessionEvent *session_event = sql_event->session_event();

  if (stmt == nullptr) {
    LOG_WARN("cannot find statement");
    return RC::GENERIC_ERROR;
  }

  UpdateStmt *update_stmt = (UpdateStmt *)stmt;
  // Table *table = update_stmt->table();
  // Value value = update_stmt->value();
  // const char *attribute_name = update_stmt->attribute_name();

  TableScanOperator scan_oper(update_stmt->table());
  PredicateOperator pred_oper(update_stmt->filter_stmt());
  pred_oper.add_child(&scan_oper);
  UpdateOperator update_oper(update_stmt);
  update_oper.add_child(&pred_oper);

  rc = update_oper.open();

  if (rc == RC::SUCCESS) {
    session_event->set_response("SUCCESS\n");
  } else {
    session_event->set_response("FAILURE\n");
  }
  return rc;
}

RC ExecuteStage::do_delete(SQLStageEvent *sql_event)
{
  Stmt *stmt = sql_event->stmt();
  SessionEvent *session_event = sql_event->session_event();

  if (stmt == nullptr) {
    LOG_WARN("cannot find statement");
    return RC::GENERIC_ERROR;
  }

  DeleteStmt *delete_stmt = (DeleteStmt *)stmt;
  TableScanOperator scan_oper(delete_stmt->table());
  PredicateOperator pred_oper(delete_stmt->filter_stmt());
  pred_oper.add_child(&scan_oper);
  DeleteOperator delete_oper(delete_stmt);
  delete_oper.add_child(&pred_oper);

  RC rc = delete_oper.open();
  if (rc != RC::SUCCESS) {
    session_event->set_response("FAILURE\n");
  } else {
    session_event->set_response("SUCCESS\n");
  }
  return rc;
}
