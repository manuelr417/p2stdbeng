#include "exec/Binder.h"

#include "schema/TableSchemaJson.h"

#include <cstdint>
#include <fstream>

namespace exec {

namespace {

const char* type_name(tuple::ColumnType type) {
    switch (type) {
        case tuple::ColumnType::Integer: return "Integer";
        case tuple::ColumnType::String:  return "String";
        case tuple::ColumnType::Double:  return "Double";
    }
    return "?";
}

bool is_numeric_type(tuple::ColumnType type) {
    return type == tuple::ColumnType::Integer ||
           type == tuple::ColumnType::Double;
}

// A statement's table name must be a bare name: it composes catalog and
// data file paths.
bool valid_table_name(const std::string& name) {
    return !name.empty() && name.find('/') == std::string::npos;
}

// Catalog text for one column definition, in exactly the loader's shape.
std::string column_def_json(const std::string& name, const char* type,
                            std::size_t size) {
    return "    { \"column_name\": \"" + name + "\", \"column_type\": \"" +
           type + "\", \"size\": " + std::to_string(size) + " }";
}

int binary_op_code(sql::BinaryOp op) {
    switch (op) {
        case sql::BinaryOp::Eq:    return BoundExpr::OP_EQ;
        case sql::BinaryOp::Ne:    return BoundExpr::OP_NE;
        case sql::BinaryOp::Lt:    return BoundExpr::OP_LT;
        case sql::BinaryOp::Le:    return BoundExpr::OP_LE;
        case sql::BinaryOp::Gt:    return BoundExpr::OP_GT;
        case sql::BinaryOp::Ge:    return BoundExpr::OP_GE;
        case sql::BinaryOp::And:   return BoundExpr::OP_AND;
        case sql::BinaryOp::Or:    return BoundExpr::OP_OR;
        case sql::BinaryOp::Plus:  return BoundExpr::OP_PLUS;
        case sql::BinaryOp::Minus: return BoundExpr::OP_MINUS;
        case sql::BinaryOp::Mul:   return BoundExpr::OP_MUL;
        case sql::BinaryOp::Div:   return BoundExpr::OP_DIV;
        case sql::BinaryOp::Mod:   return BoundExpr::OP_MOD;
    }
    return BoundExpr::OP_EQ;
}

}

Binder::Binder(std::string catalog_dir)
    : catalog_dir_(std::move(catalog_dir)) {}

bool Binder::bind(const sql::Statement& statement, BoundStatement& out,
                  std::string& error) {
    switch (statement.kind) {
        case sql::Statement::Kind::Select:
            return bind_select(static_cast<const sql::SelectStatement&>(statement),
                               out, error);
        case sql::Statement::Kind::Insert:
            return bind_insert(static_cast<const sql::InsertStatement&>(statement),
                               out, error);
        case sql::Statement::Kind::Update:
            return bind_update(static_cast<const sql::UpdateStatement&>(statement),
                               out, error);
        case sql::Statement::Kind::Delete:
            return bind_delete(static_cast<const sql::DeleteStatement&>(statement),
                               out, error);
        case sql::Statement::Kind::CreateTable:
            return bind_create_table(
                static_cast<const sql::CreateTableStatement&>(statement), out,
                error);
        case sql::Statement::Kind::DropTable:
            return bind_drop_table(
                static_cast<const sql::DropTableStatement&>(statement), out,
                error);
    }
    error = "unknown statement kind";
    return false;
}

void Binder::invalidate(const std::string& name) {
    cache_.erase(name);
}

const schema::TableSchema* Binder::table_schema(const std::string& name,
                                                std::string& error) {
    const schema::TableSchema* schema = nullptr;
    if (!table_schema(name, schema, error)) {
        return nullptr;
    }
    return schema;
}

bool Binder::table_schema(const std::string& name,
                          const schema::TableSchema*& out, std::string& error) {
    if (!valid_table_name(name)) {
        error = "invalid table name '" + name + "'";
        return false;
    }
    const auto it = cache_.find(name);
    if (it != cache_.end()) {
        out = &it->second;
        return true;
    }
    schema::TableSchema loaded("");
    const std::string path = catalog_dir_ + "/" + name + ".json";
    if (!schema::load_table_schema(path, loaded, error)) {
        error = "unknown table '" + name + "' (" + error + ")";
        return false;
    }
    const auto inserted = cache_.emplace(name, std::move(loaded));
    out = &inserted.first->second;
    return true;
}

std::unique_ptr<BoundExpr> Binder::bind_expr(const sql::Expr& expr,
                                             const schema::TableSchema& schema,
                                             const std::string& qualifier_a,
                                             const std::string& qualifier_b,
                                             std::string& error) {
    auto bound = std::make_unique<BoundExpr>();
    switch (expr.kind) {
        case sql::Expr::Kind::Literal: {
            bound->kind = BoundExpr::Kind::Literal;
            switch (expr.value.tag) {
                case sql::LiteralValue::Tag::Int:
                    if (expr.value.i < INT32_MIN || expr.value.i > INT32_MAX) {
                        error = "integer literal out of range";
                        return nullptr;
                    }
                    bound->literal = tuple::Column(static_cast<int>(expr.value.i));
                    break;
                case sql::LiteralValue::Tag::Double:
                    bound->literal = tuple::Column(expr.value.d);
                    break;
                case sql::LiteralValue::Tag::String:
                    bound->literal = tuple::Column(expr.value.s);
                    break;
            }
            bound->type = bound->literal.column_type();
            return bound;
        }
        case sql::Expr::Kind::ColumnRef: {
            if (!expr.table.empty() && expr.table != qualifier_a &&
                expr.table != qualifier_b) {
                error = "unknown table '" + expr.table + "' in column reference";
                return nullptr;
            }
            const auto index = schema.column_index(expr.column);
            if (!index) {
                error = "unknown column '" + expr.column + "' in table '" +
                        schema.table_name() + "'";
                return nullptr;
            }
            bound->kind = BoundExpr::Kind::Column;
            bound->index = *index;
            bound->type = schema.column_info(*index).column_type();
            return bound;
        }
        case sql::Expr::Kind::Binary: {
            auto left = bind_expr(*expr.left, schema, qualifier_a, qualifier_b, error);
            if (!left) {
                return nullptr;
            }
            auto right = bind_expr(*expr.right, schema, qualifier_a, qualifier_b, error);
            if (!right) {
                return nullptr;
            }
            const int op = binary_op_code(expr.bin_op);
            const tuple::ColumnType lt = left->type;
            const tuple::ColumnType rt = right->type;
            bound->kind = BoundExpr::Kind::Binary;
            bound->op = op;
            bool ok = true;
            switch (expr.bin_op) {
                case sql::BinaryOp::Eq:
                case sql::BinaryOp::Ne:
                case sql::BinaryOp::Lt:
                case sql::BinaryOp::Le:
                case sql::BinaryOp::Gt:
                case sql::BinaryOp::Ge:
                    // Strict affinity: comparisons refuse mixed types.
                    if (lt != rt) {
                        error = std::string("cannot compare ") + type_name(lt) +
                                " and " + type_name(rt);
                        ok = false;
                    } else {
                        bound->type = tuple::ColumnType::Integer;
                    }
                    break;
                case sql::BinaryOp::And:
                case sql::BinaryOp::Or:
                    if (lt != tuple::ColumnType::Integer ||
                        rt != tuple::ColumnType::Integer) {
                        error = std::string(expr.bin_op == sql::BinaryOp::And
                                                ? "AND"
                                                : "OR") +
                                " needs Integer operands";
                        ok = false;
                    } else {
                        bound->type = tuple::ColumnType::Integer;
                    }
                    break;
                case sql::BinaryOp::Mod:
                    if (lt != tuple::ColumnType::Integer ||
                        rt != tuple::ColumnType::Integer) {
                        error = "operator '%' needs Integer operands";
                        ok = false;
                    } else {
                        bound->type = tuple::ColumnType::Integer;
                    }
                    break;
                default: // Plus, Minus, Mul, Div
                    if (!is_numeric_type(lt) || !is_numeric_type(rt)) {
                        error = "arithmetic needs numeric operands";
                        ok = false;
                    } else {
                        bound->type =
                            (lt == tuple::ColumnType::Double ||
                             rt == tuple::ColumnType::Double)
                                ? tuple::ColumnType::Double
                                : tuple::ColumnType::Integer;
                    }
                    break;
            }
            if (!ok) {
                return nullptr;
            }
            bound->left = std::move(left);
            bound->right = std::move(right);
            return bound;
        }
        case sql::Expr::Kind::Unary: {
            auto operand = bind_expr(*expr.operand, schema, qualifier_a,
                                     qualifier_b, error);
            if (!operand) {
                return nullptr;
            }
            bound->kind = BoundExpr::Kind::Unary;
            if (expr.un_op == sql::UnaryOp::Not) {
                if (operand->type != tuple::ColumnType::Integer) {
                    error = "NOT needs an Integer operand";
                    return nullptr;
                }
                bound->op = BoundExpr::OP_NOT;
                bound->type = tuple::ColumnType::Integer;
            } else { // Neg
                if (!is_numeric_type(operand->type)) {
                    error = "negation needs a numeric operand";
                    return nullptr;
                }
                bound->op = BoundExpr::OP_NEG;
                bound->type = operand->type;
            }
            bound->operand = std::move(operand);
            return bound;
        }
        case sql::Expr::Kind::Function:
            error = "aggregate functions are not supported in p2";
            return nullptr;
    }
    error = "malformed expression";
    return nullptr;
}

bool Binder::bind_select(const sql::SelectStatement& in, BoundStatement& out,
                         std::string& error) {
    if (!in.joins.empty()) {
        error = "JOIN is not supported in p2";
        return false;
    }
    if (!in.group_by.empty()) {
        error = "GROUP BY is not supported in p2";
        return false;
    }
    if (!in.order_by.empty()) {
        error = "ORDER BY is not supported in p2";
        return false;
    }
    const schema::TableSchema* schema = nullptr;
    if (!table_schema(in.from.name, schema, error)) {
        return false;
    }
    BoundSelect bound;
    bound.schema = schema;
    std::size_t expr_ordinal = 0;
    for (const sql::SelectItem& item : in.items) {
        if (item.star) {
            for (std::size_t i = 0; i < schema->column_count(); ++i) {
                BoundOutputItem output;
                output.name = schema->column_info(i).column_name();
                output.expr = std::make_unique<BoundExpr>();
                output.expr->kind = BoundExpr::Kind::Column;
                output.expr->index = i;
                output.expr->type = schema->column_info(i).column_type();
                bound.items.push_back(std::move(output));
            }
            continue;
        }
        auto expr = bind_expr(*item.expr, *schema, in.from.name, in.from.alias,
                              error);
        if (!expr) {
            return false;
        }
        BoundOutputItem output;
        if (!item.alias.empty()) {
            output.name = item.alias;
        } else if (item.expr->kind == sql::Expr::Kind::ColumnRef) {
            output.name = item.expr->column;
        } else {
            ++expr_ordinal;
            output.name = "expr" + std::to_string(expr_ordinal);
        }
        output.expr = std::move(expr);
        bound.items.push_back(std::move(output));
    }
    if (in.where) {
        bound.where = bind_expr(*in.where, *schema, in.from.name, in.from.alias,
                                error);
        if (!bound.where) {
            return false;
        }
    }
    bound.has_limit = in.has_limit;
    bound.limit = in.limit;
    out = std::move(bound);
    return true;
}

bool Binder::bind_insert(const sql::InsertStatement& in, BoundStatement& out,
                         std::string& error) {
    const schema::TableSchema* schema = nullptr;
    if (!table_schema(in.table, schema, error)) {
        return false;
    }
    BoundInsert bound;
    bound.schema = schema;
    // Invalid data is fatal: the first value list that violates the
    // table's structure refuses the whole statement with a named error,
    // before any row is stored. (Bulk CSV loads skip bad rows instead —
    // that policy lives in TableCsv, not here.)
    for (std::size_t list_index = 0; list_index < in.rows.size();
         ++list_index) {
        const std::vector<sql::ExprPtr>& list = in.rows[list_index];
        std::string reason;
        tuple::Tuple tuple;
        if (list.size() != schema->column_count()) {
            reason = "expected " +
                     std::to_string(schema->column_count()) +
                     " value(s), got " + std::to_string(list.size());
        } else {
            for (std::size_t i = 0; i < list.size(); ++i) {
                const sql::Expr& value = *list[i];
                const schema::ColumnSchema& column = schema->column_info(i);
                if (value.kind != sql::Expr::Kind::Literal) {
                    reason = "column '" + column.column_name() +
                             "': INSERT values must be literals";
                    break;
                }
                switch (column.column_type()) {
                    case tuple::ColumnType::Integer:
                        if (value.value.tag != sql::LiteralValue::Tag::Int) {
                            reason = "column '" + column.column_name() +
                                     "': expected an Integer value";
                        } else if (value.value.i < INT32_MIN ||
                                   value.value.i > INT32_MAX) {
                            reason = "column '" + column.column_name() +
                                     "': integer literal out of range";
                        } else {
                            tuple.add(static_cast<int>(value.value.i));
                        }
                        break;
                    case tuple::ColumnType::Double:
                        if (value.value.tag == sql::LiteralValue::Tag::Double) {
                            tuple.add(value.value.d);
                        } else if (value.value.tag ==
                                   sql::LiteralValue::Tag::Int) {
                            // Safe widening: an integer literal fills a
                            // Double column exactly.
                            tuple.add(static_cast<double>(value.value.i));
                        } else {
                            reason = "column '" + column.column_name() +
                                     "': expected a Double value";
                        }
                        break;
                    case tuple::ColumnType::String:
                        if (value.value.tag != sql::LiteralValue::Tag::String) {
                            reason = "column '" + column.column_name() +
                                     "': expected a String value";
                        } else if (value.value.s.size() >
                                   column.column_size()) {
                            reason = "column '" + column.column_name() +
                                     "': string is wider than the declared "
                                     "width " +
                                     std::to_string(column.column_size());
                        } else {
                            tuple.add(value.value.s);
                        }
                        break;
                }
                if (!reason.empty()) {
                    break;
                }
            }
        }
        if (!reason.empty()) {
            error = "value list " + std::to_string(list_index + 1) + ": " +
                    reason;
            return false;
        }
        BoundInsertRow row;
        row.row = std::move(tuple);
        bound.rows.push_back(std::move(row));
    }
    out = std::move(bound);
    return true;
}

bool Binder::bind_update(const sql::UpdateStatement& in, BoundStatement& out,
                         std::string& error) {
    const schema::TableSchema* schema = nullptr;
    if (!table_schema(in.table, schema, error)) {
        return false;
    }
    BoundUpdate bound;
    bound.schema = schema;
    for (const sql::SetClause& clause : in.sets) {
        const auto index = schema->column_index(clause.column);
        if (!index) {
            error = "unknown column '" + clause.column + "' in SET";
            return false;
        }
        BoundAssignment assignment;
        assignment.index = *index;
        assignment.column_name = clause.column;
        assignment.value = bind_expr(*clause.value, *schema, "", "", error);
        if (!assignment.value) {
            return false;
        }
        bound.assignments.push_back(std::move(assignment));
    }
    if (in.where) {
        bound.where = bind_expr(*in.where, *schema, "", "", error);
        if (!bound.where) {
            return false;
        }
    }
    out = std::move(bound);
    return true;
}

bool Binder::bind_delete(const sql::DeleteStatement& in, BoundStatement& out,
                         std::string& error) {
    const schema::TableSchema* schema = nullptr;
    if (!table_schema(in.table, schema, error)) {
        return false;
    }
    BoundDelete bound;
    bound.schema = schema;
    if (in.where) {
        bound.where = bind_expr(*in.where, *schema, "", "", error);
        if (!bound.where) {
            return false;
        }
    }
    out = std::move(bound);
    return true;
}

bool Binder::bind_create_table(const sql::CreateTableStatement& in,
                               BoundStatement& out, std::string& error) {
    if (!valid_table_name(in.table)) {
        error = "invalid table name '" + in.table + "'";
        return false;
    }
    const std::string catalog_path = catalog_dir_ + "/" + in.table + ".json";
    std::ifstream existing(catalog_path, std::ios::binary);
    if (existing) {
        error = "table '" + in.table + "' already exists";
        return false;
    }
    if (in.columns.empty()) {
        error = "table '" + in.table + "' declares no columns";
        return false;
    }
    // Build the schema through the catalog's own path: generate the JSON
    // the loader parses, so column validation, creation order, declared
    // widths, and the data_file default all come from one place.
    std::string text = "{\n  \"table_name\": \"" + in.table + "\",\n"
                       "  \"columns\": [\n";
    for (std::size_t i = 0; i < in.columns.size(); ++i) {
        const sql::ColumnDef& def = in.columns[i];
        if (def.name.empty()) {
            error = "table '" + in.table + "': a column name is empty";
            return false;
        }
        if (def.name.find('"') != std::string::npos) {
            error = "column '" + def.name + "': name contains '\"'";
            return false;
        }
        const char* type_text = "";
        std::size_t size = 0;
        switch (def.type) {
            case sql::DataType::Integer:
                type_text = "Integer";
                size = 4;
                break;
            case sql::DataType::Double:
                type_text = "Double";
                size = 8;
                break;
            case sql::DataType::String:
                type_text = "String";
                if (def.size == 0) {
                    error = "column '" + def.name +
                            "': VARCHAR width must be positive";
                    return false;
                }
                size = def.size;
                break;
        }
        text += column_def_json(def.name, type_text, size);
        if (i + 1 < in.columns.size()) {
            text += ",";
        }
        text += "\n";
    }
    text += "  ]\n}";
    schema::TableSchema loaded("");
    if (!schema::parse_table_schema(text, loaded, error)) {
        return false;
    }
    BoundCreateTable bound;
    bound.schema = std::move(loaded);
    bound.catalog_path = catalog_path;
    bound.data_file = bound.schema.data_file();
    out = std::move(bound);
    return true;
}

bool Binder::bind_drop_table(const sql::DropTableStatement& in,
                             BoundStatement& out, std::string& error) {
    const schema::TableSchema* schema = nullptr;
    if (!table_schema(in.table, schema, error)) {
        return false; // unknown table 'x' (...)
    }
    BoundDropTable bound;
    bound.schema = schema;
    bound.catalog_path = catalog_dir_ + "/" + in.table + ".json";
    bound.data_file = schema->data_file();
    out = std::move(bound);
    return true;
}

}
