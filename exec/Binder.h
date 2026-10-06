#ifndef P2_BINDER_H
#define P2_BINDER_H

#include "exec/BoundExpr.h"
#include "schema/TableSchema.h"
#include "sql/Ast.h"
#include "tuple/Tuple.h"

#include <memory>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

namespace exec {

// --- Bound plans -------------------------------------------------------------
//
// The binder's output: statements fully resolved against the catalog —
// every table loaded, every column reference a creation-order index, every
// expression type-checked. Executors run these without ever seeing the AST.

/// One SELECT output column: its printed name and the bound expression
/// that produces its value (a plain Column node for column references).
struct BoundOutputItem {
    std::string name;
    std::unique_ptr<BoundExpr> expr;
};

struct BoundSelect {
    const schema::TableSchema* schema = nullptr;
    std::vector<BoundOutputItem> items; // output order
    std::unique_ptr<BoundExpr> where;   // null = every row
    std::uint64_t limit = 0;
    bool has_limit = false;
};

/// One INSERT value list, already validated against the table's
/// structure: every row in a bound plan is ready to insert. (The binder
/// refuses a statement whose value lists are invalid — invalid data is
/// fatal, never skipped.)
struct BoundInsertRow {
    tuple::Tuple row;
};

struct BoundInsert {
    const schema::TableSchema* schema = nullptr;
    std::vector<BoundInsertRow> rows; // statement order preserved
};

/// One SET assignment: the target column (creation-order index) and the
/// value expression, evaluated against the row's old values.
struct BoundAssignment {
    std::size_t index = 0;
    std::string column_name;
    std::unique_ptr<BoundExpr> value;
};

struct BoundUpdate {
    const schema::TableSchema* schema = nullptr;
    std::vector<BoundAssignment> assignments;
    std::unique_ptr<BoundExpr> where; // null = every row
};

struct BoundDelete {
    const schema::TableSchema* schema = nullptr;
    std::unique_ptr<BoundExpr> where; // null = every row
};

struct BoundCreateTable {
    schema::TableSchema schema{""}; // the new table's structure (data_file resolved)
    std::string catalog_path;       // where the catalog is written
    std::string data_file;          // the (possibly default) data file name
};

struct BoundDropTable {
    const schema::TableSchema* schema = nullptr; // the loaded catalog
    std::string catalog_path;
    std::string data_file;
};

using BoundStatement = std::variant<BoundSelect, BoundInsert, BoundUpdate,
                                    BoundDelete, BoundCreateTable,
                                    BoundDropTable>;

// --- The binder --------------------------------------------------------------

/// Resolves parsed statements against the catalog directory. Table schemas
/// load from `<catalog_dir>/<name>.json` and are cached by name; DDL must
/// call invalidate() so a CREATE or DROP is visible to later statements.
/// Every failure is a named error; a refused statement never produces a
/// plan.
class Binder {
public:
    explicit Binder(std::string catalog_dir = "tablecatalog");

    bool bind(const sql::Statement& statement, BoundStatement& out,
              std::string& error);

    /// The cached schema for `name`, loaded from
    /// `<catalog_dir>/<name>.json` on first use — the same resolution
    /// statements go through. Null with a named error when the catalog
    /// is missing or malformed.
    const schema::TableSchema* table_schema(const std::string& name,
                                            std::string& error);

    /// Forgets the cached schema for `name` (after CREATE/DROP TABLE).
    void invalidate(const std::string& name);

private:
    bool bind_select(const sql::SelectStatement& in, BoundStatement& out,
                     std::string& error);
    bool bind_insert(const sql::InsertStatement& in, BoundStatement& out,
                     std::string& error);
    bool bind_update(const sql::UpdateStatement& in, BoundStatement& out,
                     std::string& error);
    bool bind_delete(const sql::DeleteStatement& in, BoundStatement& out,
                     std::string& error);
    bool bind_create_table(const sql::CreateTableStatement& in,
                           BoundStatement& out, std::string& error);
    bool bind_drop_table(const sql::DropTableStatement& in,
                         BoundStatement& out, std::string& error);

    /// The cached schema for `name`, loading `<catalog_dir>/<name>.json`
    /// on first use. Names a missing table with the loader's reason.
    bool table_schema(const std::string& name, const schema::TableSchema*& out,
                      std::string& error);

    /// Binds one expression against `schema`; `qualifier_a`/`qualifier_b`
    /// are the table names a qualified reference may use (the table and,
    /// for SELECT, its alias; empty strings when absent).
    std::unique_ptr<BoundExpr> bind_expr(const sql::Expr& expr,
                                         const schema::TableSchema& schema,
                                         const std::string& qualifier_a,
                                         const std::string& qualifier_b,
                                         std::string& error);

    std::string catalog_dir_;
    std::unordered_map<std::string, schema::TableSchema> cache_;
};

}

#endif // P2_BINDER_H
