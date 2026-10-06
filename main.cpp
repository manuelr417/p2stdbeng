#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <unistd.h>
#include <algorithm>
#include <array>
#include <random>
#include <vector>

#include "bufpool/BufferManager.h"
#include "exec/Binder.h"
#include "exec/BoundExpr.h"
#include "exec/Executor.h"
#include "policy/LRUPolicy.h"
#include "schema/TableSchema.h"
#include "schema/TableSchemaJson.h"
#include "sql/Ast.h"
#include "sql/Parser.h"
#include "tuple/Column.h"

namespace {

int passed_ = 0;
int failed_ = 0;

void check(bool condition, const char* what) {
    if (condition) {
        ++passed_;
        std::printf("PASS: %s\n", what);
    } else {
        ++failed_;
        std::printf("FAIL: %s\n", what);
    }
    std::fflush(stdout);
}

// --- shared fixtures ---------------------------------------------------------

// A throwaway catalog directory with two tables: person (pid Integer,
// name String(10), age Integer) and demo (id Integer, price Double,
// tag String(6) — a Double column for promotion checks).
const std::string& catalog_dir() {
    static const std::string dir =
        "/tmp/p2-checks-" + std::to_string(::getpid());
    static const bool ready = [] {
        std::filesystem::create_directories(dir);
        std::string error;
        schema::TableSchema person("");
        schema::parse_table_schema(
            R"({"table_name": "person",
                "columns": [
                  {"column_name": "pid",  "column_type": "Integer", "size": 4},
                  {"column_name": "name", "column_type": "String",  "size": 10},
                  {"column_name": "age",  "column_type": "Integer", "size": 4}
                ]})",
            person, error);
        schema::write_table_schema(dir + "/person.json", person, error);
        schema::TableSchema demo("");
        schema::parse_table_schema(
            R"({"table_name": "demo",
                "columns": [
                  {"column_name": "id",    "column_type": "Integer", "size": 4},
                  {"column_name": "price", "column_type": "Double",  "size": 8},
                  {"column_name": "tag",   "column_type": "String",  "size": 6}
                ]})",
            demo, error);
        schema::write_table_schema(dir + "/demo.json", demo, error);
        return true;
    }();
    (void)ready;
    return dir;
}

// Parses one statement; nullptr with a named error on failure. Parsed
// results are kept alive for the whole run — the checks read the AST
// long after the call.
sql::Statement* parse_one(const char* text, std::string& error) {
    static std::vector<sql::ParseResult> keep_alive;
    sql::ParseResult result;
    if (!sql::parse(text, result, error) || result.statements.empty()) {
        if (error.empty()) {
            error = "no statement";
        }
        return nullptr;
    }
    keep_alive.push_back(std::move(result));
    return keep_alive.back().statements[0].get();
}

// Parses and binds one statement; false with a named error when either
// stage refuses. The success-path twin of the parse-then-bind checks.
bool parse_bind(exec::Binder& binder, const char* text,
                exec::BoundStatement& bound, std::string& error) {
    sql::Statement* stmt = parse_one(text, error);
    if (stmt == nullptr) {
        if (error.empty()) {
            error = "no statement";
        }
        return false;
    }
    return binder.bind(*stmt, bound, error);
}

tuple::Tuple person_row(int pid, const std::string& name, int age) {
    tuple::Tuple row;
    row.add(pid);
    row.add(name);
    row.add(age);
    return row;
}

// Evaluates a bound expression against a row, reporting failure as an
// optional-less boolean with the out value untouched.
bool eval(const exec::BoundExpr& expr, const tuple::Tuple& row,
          tuple::Column& out, std::string& error) {
    return exec::evaluate(expr, row, out, error);
}

int int_of(const tuple::Column& column) {
    return std::get<int>(column.column_value());
}

void catalog_checks() {
    schema::TableSchema schema("");
    std::string error;

    // Explicit data_file key.
    check(parse_table_schema(
              R"({"table_name": "person", "data_file": "people.bin",
                  "columns": [{"column_name": "pid", "column_type": "Integer", "size": 4}]})",
              schema, error),
          "catalog with explicit data_file parses");
    check(error.empty(), "explicit data_file: no error");
    check(schema.data_file() == "people.bin", "explicit data_file is kept");

    // Absent key resolves to the default.
    check(parse_table_schema(
              R"({"table_name": "person",
                  "columns": [{"column_name": "pid", "column_type": "Integer", "size": 4}]})",
              schema, error),
          "catalog without data_file parses");
    check(schema.data_file() == "person.bin",
          "absent data_file defaults to <table_name>.bin");

    // A path separator is refused.
    check(!parse_table_schema(
              R"({"table_name": "person", "data_file": "sub/people.bin",
                  "columns": [{"column_name": "pid", "column_type": "Integer", "size": 4}]})",
              schema, error),
          "data_file with '/' is refused");
    check(!error.empty() && error.find("bare file name") != std::string::npos,
          "data_file refusal names the reason");

    // An empty data_file is refused (distinct from an absent key).
    check(!parse_table_schema(
              R"({"table_name": "person", "data_file": "",
                  "columns": [{"column_name": "pid", "column_type": "Integer", "size": 4}]})",
              schema, error),
          "empty data_file is refused");
    check(!error.empty() && error.find("empty") != std::string::npos,
          "empty data_file refusal names the reason");
}

void writer_checks() {
    const std::string path = "/tmp/p2-catalog-check.json";
    std::string error;

    // Build through the public loader path — the surface DDL will use.
    schema::TableSchema schema("");
    check(schema::parse_table_schema(
              R"({"table_name": "person",
                  "columns": [
                    {"column_name": "pid",  "column_type": "Integer", "size": 4},
                    {"column_name": "name", "column_type": "String",  "size": 10}
                  ]})",
              schema, error),
          "writer_checks: schema prepared");

    // Round trip with a default data file: the key is omitted on disk.
    check(schema::write_table_schema(path, schema, error),
          "writer emits a catalog for a schema with default data file");
    check(error.empty(), "writer: no error for the default case");
    std::ifstream written(path, std::ios::binary);
    std::ostringstream text;
    text << written.rdbuf();
    check(text.str().find("data_file") == std::string::npos,
          "writer omits data_file when it is the default");
    written.close();

    schema::TableSchema loaded("");
    check(schema::load_table_schema(path, loaded, error),
          "writer output loads through the ordinary loader");
    check(loaded.table_name() == "person" &&
              loaded.data_file() == "person.bin" &&
              loaded.column_count() == 2 &&
              loaded.column_info(0).column_name() == "pid" &&
              loaded.column_info(0).column_type() == tuple::ColumnType::Integer &&
              loaded.column_info(0).column_size() == 4 &&
              loaded.column_info(1).column_name() == "name" &&
              loaded.column_info(1).column_type() == tuple::ColumnType::String &&
              loaded.column_info(1).column_size() == 10,
          "round trip preserves name, default data file, and columns");

    // Round trip with a non-default data file.
    check(schema::parse_table_schema(
              R"({"table_name": "person", "data_file": "people.bin",
                  "columns": [{"column_name": "pid", "column_type": "Integer", "size": 4}]})",
              schema, error),
          "explicit-file schema prepared");
    check(schema::write_table_schema(path, schema, error),
          "writer emits a catalog with a non-default data file");
    check(schema::load_table_schema(path, loaded, error) &&
              loaded.data_file() == "people.bin",
          "round trip preserves an explicit data file");
    std::remove(path.c_str());

    // Refusals: unnamed schema, undeclared String column.
    schema::TableSchema unnamed("");
    check(!schema::write_table_schema(path, unnamed, error),
          "writer refuses an unnamed schema");
    check(!schema::parse_table_schema(
              R"({"table_name": "t",
                  "columns": [{"column_name": "s", "column_type": "String", "size": 0}]})",
              schema, error),
          "loader refuses a zero size (writer's undeclared-String guard's twin)");
    check(error.find("positive") != std::string::npos,
          "zero-size refusal names the reason");
}

void binder_checks() {
    const std::string& dir = catalog_dir();
    exec::Binder binder(dir);
    std::string error;

    // --- 3.1 expressions: binding, evaluation, affinity ---

    sql::Statement* stmt = parse_one(
        "SELECT * FROM person WHERE age > 30", error);
    check(stmt != nullptr, "binder: WHERE comparison parses and parses cleanly");
    exec::BoundStatement bound;
    check(stmt != nullptr && binder.bind(*stmt, bound, error) &&
              std::holds_alternative<exec::BoundSelect>(bound),
          "binder: SELECT binds to a BoundSelect");
    exec::BoundSelect& select = std::get<exec::BoundSelect>(bound);
    check(select.schema->table_name() == "person" && select.items.size() == 3 &&
              select.items[0].name == "pid" && select.items[2].name == "age",
          "binder: star projects all columns in catalog order");
    check(select.where != nullptr, "binder: WHERE binds to a predicate");

    tuple::Column out;
    check(eval(*select.where, person_row(1, "Ada", 53), out, error) &&
              int_of(out) == 1,
          "eval: age > 30 is true for age 53");
    check(eval(*select.where, person_row(2, "Bob", 25), out, error) &&
              int_of(out) == 0,
          "eval: age > 30 is false for age 25");

    check(parse_bind(binder, "SELECT * FROM person WHERE name > 'B'", bound, error), "binder: String comparison binds");
    check(eval(*std::get<exec::BoundSelect>(bound).where,
               person_row(1, "Ada", 53), out, error) &&
              int_of(out) == 0,
          "eval: 'Ada' > 'B' is false");
    check(eval(*std::get<exec::BoundSelect>(bound).where,
               person_row(3, "Cleo", 40), out, error) &&
              int_of(out) == 1,
          "eval: 'Cleo' > 'B' is true");

    stmt = parse_one("SELECT * FROM person WHERE name = 5", error);
    check(stmt != nullptr && !binder.bind(*stmt, bound, error) &&
              error.find("cannot compare") != std::string::npos,
          "binder: String = Integer refused with a type error");
    stmt = parse_one("SELECT * FROM person WHERE age > '30'", error);
    check(stmt != nullptr && !binder.bind(*stmt, bound, error) &&
              error.find("cannot compare") != std::string::npos,
          "binder: age > '30' refused (strict affinity)");

    stmt = parse_one("SELECT * FROM person WHERE wrong = 1", error);
    check(stmt != nullptr && !binder.bind(*stmt, bound, error) &&
              error.find("unknown column 'wrong'") != std::string::npos,
          "binder: unknown column named in the error");
    stmt = parse_one("SELECT * FROM person WHERE pid = 99999999999", error);
    check(stmt != nullptr && !binder.bind(*stmt, bound, error) &&
              error.find("out of range") != std::string::npos,
          "binder: integer literal overflow refused");
    stmt = parse_one("SELECT * FROM nosuch WHERE pid = 1", error);
    check(stmt != nullptr && !binder.bind(*stmt, bound, error) &&
              error.find("unknown table 'nosuch'") != std::string::npos,
          "binder: unknown table named in the error");
    stmt = parse_one("SELECT p.pid FROM person WHERE pid = 1", error);
    check(stmt != nullptr && !binder.bind(*stmt, bound, error) &&
              error.find("unknown table 'p'") != std::string::npos,
          "binder: wrong qualifier refused");
    stmt = parse_one("SELECT person.pid FROM person WHERE pid = 1", error);
    check(binder.bind(*stmt, bound, error),
          "binder: self-qualifier accepted");
    stmt = parse_one("SELECT p.pid FROM person p WHERE pid = 1", error);
    check(binder.bind(*stmt, bound, error),
          "binder: alias qualifier accepted");

    stmt = parse_one("SELECT age + 10 AS later FROM person WHERE pid = 1", error);
    check(binder.bind(*stmt, bound, error), "binder: arithmetic select item binds");
    exec::BoundSelect& arith = std::get<exec::BoundSelect>(bound);
    check(arith.items.size() == 1 && arith.items[0].name == "later",
          "binder: alias becomes the output name");
    check(eval(*arith.items[0].expr, person_row(1, "Ada", 53), out, error) &&
              int_of(out) == 63,
          "eval: age + 10 = 63");
    check(exec::bound_expr_type(*arith.items[0].expr) ==
              tuple::ColumnType::Integer,
          "binder: Integer + Integer stays Integer");

    stmt = parse_one("SELECT age * 1.5 FROM person WHERE pid = 1", error);
    check(parse_bind(binder, "SELECT age * 1.5 FROM person WHERE pid = 1", bound, error), "binder: mixed arithmetic binds");
    exec::BoundSelect& mixed = std::get<exec::BoundSelect>(bound);
    check(exec::bound_expr_type(*mixed.items[0].expr) ==
              tuple::ColumnType::Double,
          "binder: Integer * Double widens to Double");
    check(eval(*mixed.items[0].expr, person_row(1, "Ada", 53), out, error) &&
              std::get<double>(out.column_value()) == 79.5,
          "eval: 53 * 1.5 = 79.5");

    stmt = parse_one("SELECT age / (pid - pid) FROM person", error);
    check(parse_bind(binder, "SELECT age / (pid - pid) FROM person", bound, error), "binder: division binds");
    exec::BoundSelect& divz = std::get<exec::BoundSelect>(bound);
    check(!eval(*divz.items[0].expr, person_row(1, "Ada", 53), out, error) &&
              error == "division by zero",
          "eval: division by zero is a named run-time error");

    stmt = parse_one("SELECT pid % (pid - pid) FROM person", error);
    check(parse_bind(binder, "SELECT pid % (pid - pid) FROM person", bound, error), "binder: modulo expression binds");
    exec::BoundSelect& modz = std::get<exec::BoundSelect>(bound);
    check(!eval(*modz.items[0].expr, person_row(1, "Ada", 53), out, error) &&
              error == "modulo by zero",
          "eval: modulo by zero is a named run-time error");

    stmt = parse_one("SELECT 2000000000 + 2000000000 FROM person", error);
    check(parse_bind(binder, "SELECT 2000000000 + 2000000000 FROM person", bound, error), "binder: in-range literals bind; the sum fails at eval");
    exec::BoundSelect& ovf = std::get<exec::BoundSelect>(bound);
    check(!eval(*ovf.items[0].expr, person_row(1, "Ada", 53), out, error) &&
              error == "integer overflow",
          "eval: integer overflow is a named run-time error");

    stmt = parse_one("SELECT * FROM person WHERE NOT pid = 1", error);
    check(parse_bind(binder, "SELECT * FROM person WHERE NOT pid = 1", bound, error), "binder: NOT binds");
    check(eval(*std::get<exec::BoundSelect>(bound).where,
               person_row(1, "Ada", 53), out, error) &&
              int_of(out) == 0,
          "eval: NOT pid = 1 is false for pid 1");

    stmt = parse_one("SELECT * FROM person WHERE pid = 1 AND age > 20", error);
    check(parse_bind(binder, "SELECT * FROM person WHERE pid = 1 AND age > 20", bound, error), "binder: AND binds");
    check(eval(*std::get<exec::BoundSelect>(bound).where,
               person_row(1, "Ada", 53), out, error) &&
              int_of(out) == 1,
          "eval: both-true AND is true");
    stmt = parse_one("SELECT pid % 2 FROM person", error);
    check(parse_bind(binder, "SELECT pid % 2 FROM person", bound, error), "binder: Integer modulo binds");
    stmt = parse_one("SELECT pid % 1.5 FROM person", error);
    check(stmt != nullptr && !binder.bind(*stmt, bound, error) &&
              error.find("'%'") != std::string::npos,
          "binder: modulo on Double refused");
    stmt = parse_one("SELECT COUNT(pid) FROM person", error);
    check(stmt != nullptr && !binder.bind(*stmt, bound, error) &&
              error.find("not supported") != std::string::npos,
          "binder: aggregate function refused");

    // --- 3.2 SELECT refusals and shapes ---

    stmt = parse_one(
        "SELECT * FROM person JOIN demo ON pid = id", error);
    check(stmt != nullptr && !binder.bind(*stmt, bound, error) &&
              error.find("JOIN") != std::string::npos,
          "binder: JOIN refused by name");
    stmt = parse_one("SELECT pid FROM person GROUP BY pid", error);
    check(stmt != nullptr && !binder.bind(*stmt, bound, error) &&
              error.find("GROUP BY") != std::string::npos,
          "binder: GROUP BY refused by name");
    stmt = parse_one("SELECT pid FROM person ORDER BY pid", error);
    check(stmt != nullptr && !binder.bind(*stmt, bound, error) &&
              error.find("ORDER BY") != std::string::npos,
          "binder: ORDER BY refused by name");

    stmt = parse_one("SELECT name, pid FROM person LIMIT 5", error);
    check(parse_bind(binder, "SELECT name, pid FROM person LIMIT 5", bound, error), "binder: explicit projection binds");
    exec::BoundSelect& proj = std::get<exec::BoundSelect>(bound);
    check(proj.items.size() == 2 && proj.items[0].name == "name" &&
              proj.items[1].name == "pid",
          "binder: explicit projection keeps written order");
    check(proj.has_limit && proj.limit == 5, "binder: LIMIT captured");

    // --- 3.3 INSERT / UPDATE / DELETE binding ---

    stmt = parse_one(
        "INSERT INTO person VALUES (1, 'Ada', 30), (2, 'Bob', 25)", error);
    check(stmt != nullptr && binder.bind(*stmt, bound, error) &&
              std::holds_alternative<exec::BoundInsert>(bound),
          "binder: INSERT binds to a BoundInsert");
    exec::BoundInsert& ins = std::get<exec::BoundInsert>(bound);
    check(ins.rows.size() == 2 && ins.rows[0].row.column_count() == 3,
          "binder: two valid rows ready to insert");

    stmt = parse_one("INSERT INTO person VALUES (1, 'Ada')", error);
    check(stmt != nullptr && !binder.bind(*stmt, bound, error) &&
              error.find("expected 3 value(s), got 2") != std::string::npos,
          "binder: wrong value count refuses the statement");

    stmt = parse_one("INSERT INTO person VALUES (1, 'TooLongName', 30)", error);
    check(stmt != nullptr && !binder.bind(*stmt, bound, error) &&
              error.find("wider") != std::string::npos &&
              error.find("'name'") != std::string::npos,
          "binder: too-wide string refuses the statement, naming its column");

    stmt = parse_one(
        "INSERT INTO person VALUES (1, 'Ada', 30), (2, 'WayTooLongX', 25)",
        error);
    check(stmt != nullptr && !binder.bind(*stmt, bound, error) &&
              error.find("value list 2") != std::string::npos,
          "binder: an invalid list among several refuses the whole statement");

    stmt = parse_one("INSERT INTO person VALUES ('x', 'Ada', 30)", error);
    check(stmt != nullptr && !binder.bind(*stmt, bound, error) &&
              error.find("expected an Integer") != std::string::npos,
          "binder: Integer column refuses a String literal");
    stmt = parse_one("INSERT INTO person VALUES (1, 'Ada', 30.5)", error);
    check(stmt != nullptr && !binder.bind(*stmt, bound, error) &&
              error.find("expected an Integer") != std::string::npos,
          "binder: Integer column refuses a Double literal");

    stmt = parse_one("INSERT INTO demo VALUES (1, 2, 'x')", error);
    check(parse_bind(binder, "INSERT INTO demo VALUES (1, 2, 'x')", bound, error), "binder: INSERT into Double table binds");
    exec::BoundInsert& ins_widen = std::get<exec::BoundInsert>(bound);
    check(std::get<double>(
              ins_widen.rows[0].row.column(1).column_value()) == 2.0,
          "binder: integer literal widens into a Double column");
    stmt = parse_one("INSERT INTO demo VALUES (1, 'x', 'y')", error);
    check(stmt != nullptr && !binder.bind(*stmt, bound, error) &&
              error.find("expected a Double") != std::string::npos,
          "binder: Double column refuses a String literal");
    // The parser refuses expressions in VALUES (the grammar takes
    // literals only), so the binder's own guard is exercised with a
    // hand-built AST.
    sql::InsertStatement direct;
    direct.table = "person";
    std::vector<sql::ExprPtr> expr_list;
    auto colref = std::make_unique<sql::Expr>();
    colref->kind = sql::Expr::Kind::ColumnRef;
    colref->column = "pid";
    expr_list.push_back(std::move(colref));
    auto str = std::make_unique<sql::Expr>();
    str->kind = sql::Expr::Kind::Literal;
    str->value.tag = sql::LiteralValue::Tag::String;
    str->value.s = "Ada";
    expr_list.push_back(std::move(str));
    auto num = std::make_unique<sql::Expr>();
    num->kind = sql::Expr::Kind::Literal;
    num->value.tag = sql::LiteralValue::Tag::Int;
    num->value.i = 30;
    expr_list.push_back(std::move(num));
    direct.rows.push_back(std::move(expr_list));
    check(!binder.bind(direct, bound, error) &&
              error.find("literals") != std::string::npos,
          "binder: expressions refused in INSERT values");

    stmt = parse_one("UPDATE person SET age = age + 1 WHERE pid = 1", error);
    check(stmt != nullptr && binder.bind(*stmt, bound, error) &&
              std::holds_alternative<exec::BoundUpdate>(bound),
          "binder: UPDATE binds to a BoundUpdate");
    exec::BoundUpdate& upd = std::get<exec::BoundUpdate>(bound);
    check(upd.assignments.size() == 1 &&
              upd.assignments[0].index == 2 &&
              upd.assignments[0].column_name == "age" &&
              upd.where != nullptr,
          "binder: SET target resolved to its creation-order index");
    stmt = parse_one("UPDATE person SET wrong = 1", error);
    check(stmt != nullptr && !binder.bind(*stmt, bound, error) &&
              error.find("unknown column 'wrong' in SET") != std::string::npos,
          "binder: unknown SET column refused by name");
    check(parse_bind(binder, "UPDATE person SET age = 30, pid = pid + 1", bound, error) &&
              std::get<exec::BoundUpdate>(bound).assignments.size() == 2,
          "binder: multi-assignment UPDATE binds");
    stmt = parse_one("DELETE FROM person WHERE age < 18", error);
    check(stmt != nullptr && binder.bind(*stmt, bound, error) &&
              std::holds_alternative<exec::BoundDelete>(bound) &&
              std::get<exec::BoundDelete>(bound).where != nullptr,
          "binder: DELETE with WHERE binds");
    stmt = parse_one("DELETE FROM person", error);
    check(stmt != nullptr && binder.bind(*stmt, bound, error) &&
              std::get<exec::BoundDelete>(bound).where == nullptr,
          "binder: DELETE without WHERE binds to all rows");

    // --- 3.4 CREATE / DROP binding ---

    stmt = parse_one(
        "CREATE TABLE part (part_id INTEGER, name VARCHAR(15), price DOUBLE)",
        error);
    check(stmt != nullptr && binder.bind(*stmt, bound, error) &&
              std::holds_alternative<exec::BoundCreateTable>(bound),
          "binder: CREATE TABLE binds to a BoundCreateTable");
    exec::BoundCreateTable& create = std::get<exec::BoundCreateTable>(bound);
    check(create.schema.table_name() == "part" &&
              create.schema.column_count() == 3 &&
              create.schema.column_info(0).column_type() ==
                  tuple::ColumnType::Integer &&
              create.schema.column_info(0).column_size() == 4 &&
              create.schema.column_info(1).column_type() ==
                  tuple::ColumnType::String &&
              create.schema.column_info(1).column_size() == 15 &&
              create.schema.column_info(2).column_type() ==
                  tuple::ColumnType::Double &&
              create.schema.column_info(2).column_size() == 8,
          "binder: type mapping INTEGER/VARCHAR(n)/DOUBLE with widths");
    check(create.data_file == "part.bin" &&
              create.catalog_path == dir + "/part.json",
          "binder: data file defaults to <table>.bin in the catalog dir");
    stmt = parse_one("CREATE TABLE t2 (a INT)", error);
    check(stmt != nullptr && binder.bind(*stmt, bound, error) &&
              std::get<exec::BoundCreateTable>(bound).schema.column_info(0)
                      .column_type() == tuple::ColumnType::Integer,
          "binder: INT alias maps to Integer");
    stmt = parse_one("CREATE TABLE person (x INTEGER)", error);
    check(stmt != nullptr && !binder.bind(*stmt, bound, error) &&
              error.find("already exists") != std::string::npos,
          "binder: CREATE refuses an existing catalog");
    stmt = parse_one("CREATE TABLE t3 (s VARCHAR(0))", error);
    check(stmt == nullptr || !binder.bind(*stmt, bound, error),
          "binder: VARCHAR(0) refused somewhere between parser and binder");

    stmt = parse_one("DROP TABLE person", error);
    check(stmt != nullptr && binder.bind(*stmt, bound, error) &&
              std::holds_alternative<exec::BoundDropTable>(bound),
          "binder: DROP TABLE binds to a BoundDropTable");
    exec::BoundDropTable& drop = std::get<exec::BoundDropTable>(bound);
    check(drop.data_file == "person.bin" &&
              drop.catalog_path == dir + "/person.json",
          "binder: DROP resolves catalog and data file paths");
    stmt = parse_one("DROP TABLE nosuch", error);
    check(stmt != nullptr && !binder.bind(*stmt, bound, error) &&
              error.find("unknown table 'nosuch'") != std::string::npos,
          "binder: DROP refuses an unknown table by name");
}

// Binds `text` through `binder`, expecting the given bound alternative.
template <typename T>
const T* bind_as(exec::Binder& binder, const char* text,
                 exec::BoundStatement& bound, std::string& error) {
    if (!parse_bind(binder, text, bound, error)) {
        return nullptr;
    }
    return std::get_if<T>(&bound);
}

void executor_checks() {
    const std::string& dir = catalog_dir();
    // The suite may run repeatedly in one working directory: clear this
    // section's leftovers so every run starts from nothing.
    for (const char* name : {"exec_person", "exec_swap", "exec_corrupt"}) {
        std::filesystem::remove(std::string(name) + ".bin");
        std::filesystem::remove(dir + "/" + name + ".json");
    }
    exec::Binder binder(dir);
    bufman::BufferManager manager;
    std::string init_error;
    manager.init(8, std::make_unique<bufman::LRUPolicy>(), init_error);
    check(init_error.empty(), "exec: buffer manager initializes cleanly");
    std::string error;
    exec::BoundStatement bound;

    // --- 4.4 CREATE: catalog + empty data file, or neither ---

    const exec::BoundCreateTable* create = bind_as<exec::BoundCreateTable>(
        binder,
        "CREATE TABLE exec_person (pid INTEGER, name VARCHAR(10), age INTEGER)",
        bound, error);
    check(create != nullptr && exec::execute_create_table(*create, manager, error),
          "exec: CREATE TABLE writes the catalog and creates the data file");
    check(std::filesystem::exists(dir + "/exec_person.json") &&
              std::filesystem::exists("exec_person.bin"),
          "exec: both halves of the created table exist on disk");

    // --- 4.2 INSERT: counts, fatal validation, file creation ---

    const exec::BoundInsert* ins = bind_as<exec::BoundInsert>(
        binder,
        "INSERT INTO exec_person VALUES (1, 'Ada', 53), (2, 'Bob', 25), "
        "(3, 'Cleo', 40)",
        bound, error);
    check(ins != nullptr, "exec: multi-row INSERT binds");
    std::unique_ptr<heapfile::HeapFile> file;
    check(ins != nullptr &&
              exec::open_table_file(manager, *ins->schema, true, file, error),
          "exec: the table's data file opens");
    std::size_t inserted = 0;
    check(file && exec::execute_insert(*ins, *file, inserted, error) &&
              inserted == 3,
          "exec: three rows inserted");

    // --- 4.1 SELECT: filter, project, limit, empty result ---

    std::vector<std::vector<tuple::Column>> rows;
    auto collect = [&](const std::vector<tuple::Column>& out) {
        rows.push_back(out);
        return true;
    };
    const exec::BoundSelect* sel = bind_as<exec::BoundSelect>(
        binder, "SELECT * FROM exec_person", bound, error);
    check(sel != nullptr && file &&
              exec::execute_select(*sel, *file, collect, error) &&
              rows.size() == 3 && int_of(rows[0][0]) == 1 &&
              std::get<std::string>(rows[0][1].column_value()) == "Ada" &&
              int_of(rows[0][2]) == 53,
          "exec: SELECT * returns every row with its values");

    sel = bind_as<exec::BoundSelect>(binder,
                                     "SELECT name, pid FROM exec_person "
                                     "WHERE age > 30",
                                     bound, error);
    rows.clear();
    check(sel != nullptr && file &&
              exec::execute_select(*sel, *file, collect, error) &&
              rows.size() == 2 &&
              std::get<std::string>(rows[0][0].column_value()) == "Ada" &&
              int_of(rows[0][1]) == 1 &&
              std::get<std::string>(rows[1][0].column_value()) == "Cleo",
          "exec: WHERE filters and the projection keeps written order");

    sel = bind_as<exec::BoundSelect>(
        binder, "SELECT pid FROM exec_person LIMIT 1", bound, error);
    rows.clear();
    check(sel != nullptr && file &&
              exec::execute_select(*sel, *file, collect, error) &&
              rows.size() == 1 && int_of(rows[0][0]) == 1,
          "exec: LIMIT stops the scan at one row");

    sel = bind_as<exec::BoundSelect>(
        binder, "SELECT pid FROM exec_person LIMIT 0", bound, error);
    rows.clear();
    check(sel != nullptr && file &&
              exec::execute_select(*sel, *file, collect, error) &&
              rows.empty(),
          "exec: LIMIT 0 yields zero rows");

    sel = bind_as<exec::BoundSelect>(
        binder, "SELECT pid FROM exec_person WHERE age > 100", bound, error);
    rows.clear();
    check(sel != nullptr && file &&
              exec::execute_select(*sel, *file, collect, error) &&
              rows.empty(),
          "exec: no matches yield an empty result, not an error");

    // Invalid data is fatal: a statement holding one invalid value list is
    // refused whole, storing nothing.
    ins = bind_as<exec::BoundInsert>(
        binder,
        "INSERT INTO exec_person VALUES (5, 'Ed', 44), (6, 'Toolongname', 44)",
        bound, error);
    check(ins == nullptr && error.find("wider") != std::string::npos &&
              error.find("value list 2") != std::string::npos,
          "exec: a statement with an invalid value list is refused");
    sel = bind_as<exec::BoundSelect>(
        binder, "SELECT pid FROM exec_person", bound, error);
    rows.clear();
    check(sel != nullptr && file &&
              exec::execute_select(*sel, *file, collect, error) &&
              rows.size() == 3,
          "exec: the refused INSERT stored no rows");


    // --- 4.3 UPDATE: rebuild semantics ---

    const exec::BoundUpdate* upd = bind_as<exec::BoundUpdate>(
        binder, "UPDATE exec_person SET age = age + 1 WHERE pid = 1", bound,
        error);
    std::size_t updated = 0;
    check(upd != nullptr && file &&
              exec::execute_update(*upd, *file, updated, error) && updated == 1,
          "exec: self-referential UPDATE applied once");
    sel = bind_as<exec::BoundSelect>(
        binder, "SELECT age FROM exec_person WHERE pid = 1", bound, error);
    rows.clear();
    check(file && exec::execute_select(*sel, *file, collect, error) &&
              rows.size() == 1 && int_of(rows[0][0]) == 54,
          "exec: age became 54 after age + 1");

    upd = bind_as<exec::BoundUpdate>(
        binder, "UPDATE exec_person SET age = 0 WHERE pid = 999", bound, error);
    updated = 0;
    check(upd != nullptr && file &&
              exec::execute_update(*upd, *file, updated, error) && updated == 0,
          "exec: an UPDATE matching nothing changes nothing");

    // The swap: simultaneous SET semantics through the rebuild.
    const exec::BoundCreateTable* swap_create = bind_as<exec::BoundCreateTable>(
        binder, "CREATE TABLE exec_swap (a INTEGER, b INTEGER)", bound, error);
    check(swap_create != nullptr &&
              exec::execute_create_table(*swap_create, manager, error),
          "exec: swap table created");
    const exec::BoundInsert* swap_ins = bind_as<exec::BoundInsert>(
        binder, "INSERT INTO exec_swap VALUES (1, 2)", bound, error);
    std::unique_ptr<heapfile::HeapFile> swap_file;
    check(swap_ins != nullptr &&
              exec::open_table_file(manager, *swap_ins->schema, true,
                                    swap_file, error) &&
              swap_file &&
              exec::execute_insert(*swap_ins, *swap_file, inserted,
                                   error) &&
              inserted == 1,
          "exec: swap row inserted");
    const exec::BoundUpdate* swap_upd = bind_as<exec::BoundUpdate>(
        binder, "UPDATE exec_swap SET a = b, b = a", bound, error);
    check(swap_upd != nullptr && swap_file &&
              exec::execute_update(*swap_upd, *swap_file,
                                                   updated, error) &&
              updated == 1,
          "exec: the swap UPDATE applied");
    sel = bind_as<exec::BoundSelect>(binder, "SELECT a, b FROM exec_swap",
                                     bound, error);
    rows.clear();
    check(swap_file && exec::execute_select(*sel, *swap_file, collect, error) &&
              rows.size() == 1 && int_of(rows[0][0]) == 2 && int_of(rows[0][1]) == 1,
          "exec: SET a = b, b = a exchanged the values");

    // --- 4.3 DELETE: collect-then-erase, freed slots reusable ---

    const exec::BoundDelete* del = bind_as<exec::BoundDelete>(
        binder, "DELETE FROM exec_person WHERE age < 30", bound, error);
    std::size_t deleted = 0;
    check(del != nullptr && file &&
              exec::execute_delete(*del, *file, deleted, error) && deleted == 1,
          "exec: the matching row was deleted");
    sel = bind_as<exec::BoundSelect>(binder, "SELECT pid FROM exec_person",
                                     bound, error);
    rows.clear();
    check(file && exec::execute_select(*sel, *file, collect, error) && rows.size() == 2,
          "exec: the remaining rows survive (1 and 3)");
    ins = bind_as<exec::BoundInsert>(binder,
                                     "INSERT INTO exec_person VALUES "
                                     "(7, 'Fay', 29)",
                                     bound, error);
    check(ins != nullptr && file &&
              exec::execute_insert(*ins, *file, inserted, error) &&
              inserted == 1,
          "exec: an insert after deletes reuses freed space");

    // --- 4.1 fail-fast: an unreadable record blocks reads and applies ---

    const exec::BoundCreateTable* corrupt_create =
        bind_as<exec::BoundCreateTable>(
            binder,
            "CREATE TABLE exec_corrupt (pid INTEGER, name VARCHAR(10), age "
            "INTEGER)",
            bound, error);
    check(corrupt_create != nullptr &&
              exec::execute_create_table(*corrupt_create, manager, error),
          "exec: corrupt-test table created");
    ins = bind_as<exec::BoundInsert>(
        binder, "INSERT INTO exec_corrupt VALUES (1, 'Ada', 53)", bound, error);
    std::unique_ptr<heapfile::HeapFile> corrupt_file;
    check(ins != nullptr &&
              exec::open_table_file(manager, *ins->schema, true, corrupt_file,
                                    error) &&
              corrupt_file &&
              exec::execute_insert(*ins, *corrupt_file, inserted, error),
          "exec: corrupt-test row inserted");
    check(corrupt_file && corrupt_file->close(error),
          "exec: corrupt-test file closed");
    corrupt_file.reset();

    // Damage slot 0's String padding through the raw block layer: the
    // record is pid(4) name(10) age(4); 'Ada' occupies [4,7), so byte 9
    // is padding and must be NUL.
    {
        bufman::File raw;
        check(bufman::open_for_append("exec_corrupt.bin", raw, error),
              "exec: raw block access for the corruption step");
        std::array<char, bufman::kBlockSize> block{};
        check(bufman::read_block(raw, 1, block, error),
              "exec: data page read back");
        block[9] = 'X';
        check(bufman::write_block(raw, 1, block.data(), block.size(), error),
              "exec: damaged page written back");
        bufman::close(raw);
    }
    std::unique_ptr<heapfile::HeapFile> reopened;
    check(ins != nullptr &&
              exec::open_table_file(manager, *ins->schema, false, reopened,
                                    error),
          "exec: the damaged file still opens");
    sel = bind_as<exec::BoundSelect>(binder, "SELECT * FROM exec_corrupt",
                                     bound, error);
    rows.clear();
    error.clear();
    check(sel != nullptr && reopened &&
              !exec::execute_select(*sel, *reopened, collect, error) &&
              !error.empty(),
          "exec: SELECT fails fast on the unreadable record");
    upd = bind_as<exec::BoundUpdate>(binder,
                                     "UPDATE exec_corrupt SET age = 0",
                                     bound, error);
    updated = 0;
    error.clear();
    check(upd != nullptr && reopened &&
              !exec::execute_update(*upd, *reopened, updated, error) &&
              updated == 0,
          "exec: UPDATE applies nothing when collection fails");
    del = bind_as<exec::BoundDelete>(binder, "DELETE FROM exec_corrupt", bound,
                                     error);
    deleted = 0;
    error.clear();
    check(del != nullptr && reopened &&
              !exec::execute_delete(*del, *reopened, deleted, error) &&
              deleted == 0,
          "exec: DELETE applies nothing when collection fails");
    check(reopened && reopened->close(error),
          "exec: damaged file closed after fail-fast");
    reopened.reset();

    // --- 4.4 DROP: both files, guarded while open ---

    // Keep a file open through the session, then refuse the drop. The
    // first handle is closed properly first: destroying an open HeapFile
    // would leave its manager registration behind.
    check(swap_file && swap_file->close(error),
          "exec: first swap handle closed");
    swap_file.reset();
    ins = bind_as<exec::BoundInsert>(binder,
                                     "INSERT INTO exec_swap VALUES (3, 4)",
                                     bound, error);
    check(ins != nullptr &&
              exec::open_table_file(manager, *ins->schema, false, swap_file,
                                    error),
          "exec: swap file reopened for the drop guard");
    const exec::BoundDropTable* drop = bind_as<exec::BoundDropTable>(
        binder, "DROP TABLE exec_swap", bound, error);
    error.clear();
    check(drop != nullptr && !exec::execute_drop_table(*drop, manager, error) &&
              error.find("open") != std::string::npos &&
              std::filesystem::exists(dir + "/exec_swap.json"),
          "exec: DROP is refused while the data file is open");
    check(swap_file && swap_file->close(error),
          "exec: swap file closed for the retry");
    swap_file.reset();
    check(drop != nullptr && exec::execute_drop_table(*drop, manager, error),
          "exec: DROP succeeds once the file is closed");
    check(!std::filesystem::exists(dir + "/exec_swap.json") &&
              !std::filesystem::exists("exec_swap.bin"),
          "exec: DROP removed both the catalog and the data file");

    // A missing data file does not block the drop.
    check(file && file->close(error),
          "exec: person file closed before the drop test");
    file.reset();
    std::filesystem::remove("exec_person.bin"); // simulate external loss
    drop = bind_as<exec::BoundDropTable>(binder, "DROP TABLE exec_person",
                                         bound, error);
    check(drop != nullptr && exec::execute_drop_table(*drop, manager, error) &&
              !std::filesystem::exists(dir + "/exec_person.json"),
          "exec: DROP tolerates a missing data file and removes the catalog");

    // INSERT recreates a data file that is gone (the file-creation path).
    ins = bind_as<exec::BoundInsert>(binder,
                                     "INSERT INTO exec_corrupt VALUES "
                                     "(9, 'Gus', 61)",
                                     bound, error);
    check(ins != nullptr &&
              exec::open_table_file(manager, *ins->schema, true, file, error) &&
              file && exec::execute_insert(*ins, *file, inserted, error) &&
              inserted == 1,
          "exec: INSERT recreated the missing data file");
    check(file && file->close(error), "exec: recreated file closed");
    file.reset();

    // Clean the corrupt-test table; its drop also covers erase_file on a
    // file still registered with the manager after close().
    drop = bind_as<exec::BoundDropTable>(binder, "DROP TABLE exec_corrupt",
                                         bound, error);
    check(drop != nullptr && exec::execute_drop_table(*drop, manager, error),
          "exec: corrupt-test table dropped cleanly");

    manager.close_all(error);
}

// --- storage: hand-built list-surgery cases ----------------------------------
//
// A three-page file walked through every detach/append shape the lists can
// take — head, middle, tail, and only element — with the structural check
// after every step and the scan order verified against the expected page
// sequence (the free list's pages, then the full list's).

void list_case_checks() {
    std::filesystem::remove("listcase.bin");
    schema::TableSchema schema("");
    std::string error;
    check(schema::parse_table_schema(
              R"({"table_name": "listcase",
                  "columns": [
                    {"column_name": "sid", "column_type": "Integer", "size": 4},
                    {"column_name": "pad", "column_type": "String",  "size": 900}]})",
              schema, error),
          "listcase: schema fixture parses");
    const auto make_row = [](int id) {
        tuple::Tuple row;
        row.add(id);
        row.add(std::string(900, static_cast<char>('a' + id % 26)));
        return row;
    };
    const auto scan_ids = [&](heapfile::HeapFile& file) {
        std::vector<int> ids;
        heapfile::HeapFileIterator it = file.scan();
        tuple::Tuple row;
        while (it.next(row, error)) {
            ids.push_back(std::get<int>(row.column(0).column_value()));
        }
        return ids;
    };

    bufman::BufferManager manager;
    std::string init_error;
    manager.init(8, std::make_unique<bufman::LRUPolicy>(), init_error);
    heapfile::HeapFile file(manager);
    check(file.create("listcase.bin", schema, error) &&
              file.open("listcase.bin", schema, error),
          "listcase: file created and opened");

    // One page fills: the full list's only element. Erasing unlinks that
    // only element and appends it to an empty free list.
    bool placed = true;
    for (int id = 1; id <= 4; ++id) {
        heapfile::RowId rid;
        placed = placed && file.insert(make_row(id), rid, error) &&
                 rid.page_id == 1 && rid.slot == static_cast<std::uint32_t>(id - 1);
    }
    check(placed && file.check_structure(error),
          "listcase: four rows fill page 1");
    check(file.erase(heapfile::RowId{1, 0}, error) &&
              file.check_structure(error),
          "listcase: the full list's only element unlinks");
    {
        const std::vector<int> expected = {2, 3, 4};
        check(scan_ids(file) == expected && error.empty(),
              "listcase: the freed page scans before the full pages");
    }
    heapfile::RowId rid;
    check(file.insert(make_row(5), rid, error) && rid.page_id == 1 &&
              rid.slot == 0 && file.check_structure(error),
          "listcase: the page refills and rejoins the full list");

    // Fill pages 2 and 3 completely: page 1 came back full, so ids 6..9
    // land on page 2 and 10..13 on page 3. The full list becomes
    // 1 -> 2 -> 3 and the free list is empty.
    placed = true;
    for (int id = 6; id <= 13; ++id) {
        heapfile::RowId at;
        placed = placed && file.insert(make_row(id), at, error) &&
                 at.page_id == static_cast<std::uint32_t>(id <= 9 ? 2 : 3);
    }
    check(placed && file.check_structure(error),
          "listcase: twelve live rows fill pages 1..3 in order");

    // Middle of the full list: erasing from page 2 unlinks it between its
    // neighbors and leaves it as the free list's only element.
    check(file.erase(heapfile::RowId{2, 0}, error) &&
              file.check_structure(error),
          "listcase: a middle page unlinks from the full list");
    {
        const std::vector<int> expected = {7, 8, 9, 5, 2, 3, 4, 10, 11, 12, 13};
        check(scan_ids(file) == expected && error.empty(),
              "listcase: the scan walks the free list before the full list");
    }
    check(file.insert(make_row(14), rid, error) && rid.page_id == 2 &&
              rid.slot == 0 && file.check_structure(error),
          "listcase: the middle page refills onto the full list's tail");
    {
        const std::vector<int> expected = {5, 2, 3, 4, 10, 11, 12, 13, 14, 7, 8, 9};
        check(scan_ids(file) == expected && error.empty(),
              "listcase: the refilled page scans at the full list's tail");
    }

    // Head of the full list (page 1 leads 1 -> 3 -> 2 now): unlink, refill,
    // and the page comes back at the tail.
    check(file.erase(heapfile::RowId{1, 0}, error) &&
              file.check_structure(error),
          "listcase: the full list's head unlinks");
    {
        const std::vector<int> expected = {2, 3, 4, 10, 11, 12, 13, 14, 7, 8, 9};
        check(scan_ids(file) == expected && error.empty(),
              "listcase: the freed head page scans before the full pages");
    }
    check(file.insert(make_row(15), rid, error) && rid.page_id == 1 &&
              rid.slot == 0 && file.check_structure(error),
          "listcase: the head page refills at the full list's tail");
    {
        const std::vector<int> expected = {10, 11, 12, 13, 14, 7, 8, 9, 15, 2, 3, 4};
        check(scan_ids(file) == expected && error.empty(),
              "listcase: the full list's order follows the refills");
    }

    // Tail of the full list (page 1 closes 3 -> 2 -> 1 now).
    check(file.erase(heapfile::RowId{1, 3}, error) &&
              file.check_structure(error),
          "listcase: the full list's tail unlinks");
    {
        const std::vector<int> expected = {15, 2, 3, 10, 11, 12, 13, 14, 7, 8, 9};
        check(scan_ids(file) == expected && error.empty(),
              "listcase: the freed tail page scans before the full pages");
    }
    check(file.insert(make_row(16), rid, error) && rid.page_id == 1 &&
              rid.slot == 3 && file.check_structure(error),
          "listcase: the tail page refills onto the full list");

    // The previous format is refused by name, not misread.
    check(file.close(error), "listcase: file closed for the format step");
    {
        bufman::File raw;
        check(bufman::open_for_append("listcase.bin", raw, error),
              "listcase: raw block access for the format step");
        std::array<char, bufman::kBlockSize> block{};
        block[0] = 'T';
        block[1] = 'U';
        block[2] = 'P';
        block[3] = '1';
        check(bufman::write_block(raw, 0, block.data(), block.size(), error),
              "listcase: TUP1 magic written over the header");
        bufman::close(raw);
    }
    {
        heapfile::HeapFile old_format(manager);
        const bool refused =
            !old_format.open("listcase.bin", schema, error) &&
            error.find("not a tuple heap file") != std::string::npos;
        check(refused, "listcase: a TUP1 file is refused by name");
    }

    manager.close_all(error);
    std::filesystem::remove("listcase.bin");
}

// --- storage: the two-list heap file ----------------------------------------
//
// Exercises the free-space lists directly through the HeapFile API: the
// invariant checker runs after every single operation, so any relink
// mistake surfaces at the step that caused it, not as a mystery later.

void storage_checks() {
    // The suite may run repeatedly in one working directory.
    std::filesystem::remove("storage.bin");
    schema::TableSchema schema("");
    std::string error;
    // sid(4) + pad(900) = 904-byte records -> 4 slots per page: small
    // row counts exercise multi-page behavior.
    check(schema::parse_table_schema(
              R"({"table_name": "storage",
                  "columns": [
                    {"column_name": "sid", "column_type": "Integer", "size": 4},
                    {"column_name": "pad", "column_type": "String",  "size": 900}]})",
              schema, error),
          "storage: schema fixture parses");
    const auto make_row = [](int id) {
        tuple::Tuple row;
        row.add(id);
        row.add(std::string(900, static_cast<char>('a' + id % 26)));
        return row;
    };

    bufman::BufferManager manager;
    std::string init_error;
    manager.init(8, std::make_unique<bufman::LRUPolicy>(), init_error);
    heapfile::HeapFile file(manager);
    check(file.create("storage.bin", schema, error) &&
              file.open("storage.bin", schema, error),
          "storage: file created and opened");
    check(file.check_structure(error),
          "storage: an empty file passes the structural check");

    // Fill two pages completely. Tail-append keeps the full list in
    // allocation order, so the bulk load scans in insertion order.
    std::vector<heapfile::RowId> live;
    std::vector<int> keys; // parallel to live: the row's sid
    bool placement_ok = true;
    for (int id = 1; id <= 8; ++id) {
        heapfile::RowId rid;
        placement_ok = placement_ok && file.insert(make_row(id), rid, error) &&
                       rid.page_id == static_cast<std::uint32_t>((id - 1) / 4 + 1) &&
                       rid.slot == static_cast<std::uint32_t>((id - 1) % 4);
        live.push_back(rid);
        keys.push_back(id);
    }
    check(placement_ok && file.check_structure(error),
          "storage: eight rows fill pages 1 and 2 in order");
    {
        std::vector<int> scanned;
        heapfile::HeapFileIterator it = file.scan();
        tuple::Tuple row;
        while (it.next(row, error)) {
            scanned.push_back(std::get<int>(row.column(0).column_value()));
        }
        check(error.empty() && scanned == keys,
              "storage: a bulk load scans in allocation order");
    }

    // Fill/unfill transitions: erase a row of the full page 1 -> the page
    // moves to the free list; the next insert reuses exactly that slot and
    // the page fills back into the full list.
    check(file.erase(live[0], error) && file.check_structure(error),
          "storage: erasing from a full page frees it onto the free list");
    heapfile::RowId rid;
    check(file.insert(make_row(9), rid, error) && rid.page_id == 1 &&
              rid.slot == 0 && file.check_structure(error),
          "storage: an insert reuses the freed slot of the oldest page");
    live[0] = rid;
    keys[0] = 9;

    // Empty pages never die: erase everything and the structure keeps both
    // pages on the free list; the next insert reuses instead of allocating.
    // (The pages reach disk only at close, so the on-disk proof comes later.)
    bool erased_all = true;
    for (const heapfile::RowId& row_id : live) {
        erased_all = erased_all && file.erase(row_id, error);
    }
    check(erased_all && error.empty() && file.check_structure(error),
          "storage: erasing everything leaves both pages on the free list");
    live.clear();
    keys.clear();
    check(file.insert(make_row(10), rid, error) && rid.page_id == 1 &&
              rid.slot == 0,
          "storage: the emptied file reuses a page instead of allocating");

    // A randomized insert/erase sequence, checked after every operation.
    // The shadow model (live + keys) tracks exactly which rows must exist.
    std::mt19937 rng(42);
    int next_id = 11;
    live.push_back(rid);
    keys.push_back(10);
    bool sequence_ok = true;
    for (int step = 0; step < 200; ++step) {
        if (rng() % 100 < 55 || live.empty()) {
            tuple::Tuple row = make_row(next_id);
            heapfile::RowId inserted_at;
            sequence_ok = sequence_ok &&
                          file.insert(row, inserted_at, error) &&
                          file.check_structure(error);
            live.push_back(inserted_at);
            keys.push_back(next_id);
            ++next_id;
        } else {
            const std::size_t victim = rng() % live.size();
            sequence_ok = sequence_ok && file.erase(live[victim], error) &&
                          file.check_structure(error);
            live.erase(live.begin() + static_cast<long>(victim));
            keys.erase(keys.begin() + static_cast<long>(victim));
        }
        if (!sequence_ok) {
            break;
        }
    }
    check(sequence_ok,
          "storage: 200 random operations keep the structure consistent");
    {
        std::vector<int> scanned;
        heapfile::HeapFileIterator it = file.scan();
        tuple::Tuple row;
        while (it.next(row, error)) {
            scanned.push_back(std::get<int>(row.column(0).column_value()));
        }
        std::vector<int> expected = keys;
        std::sort(scanned.begin(), scanned.end());
        std::sort(expected.begin(), expected.end());
        check(error.empty() && scanned == expected,
              "storage: the final scan returns exactly the shadow model's rows");
    }

    // A corrupted link must be reported, not silently obeyed: point page
    // 1's list-next at itself (the raw block layer, after a flush).
    check(file.close(error), "storage: file closed for the damage step");
    {
        // The close flushed the dirty pages: the data pages are on disk,
        // and erases never shrank the file below its first two pages.
        bufman::File raw;
        check(bufman::open_for_read("storage.bin", raw, error) &&
                  bufman::block_count(raw, error) >= 3 && error.empty(),
              "storage: the closed file keeps its data pages on disk");
        bufman::close(raw);
    }
    {
        bufman::File raw;
        check(bufman::open_for_append("storage.bin", raw, error),
              "storage: raw block access for the damage step");
        std::array<char, bufman::kBlockSize> block{};
        check(bufman::read_block(raw, 1, block, error),
              "storage: data page read back for the damage step");
        block[bufman::kBlockSize - 4] = 1; // list-next := self -> a cycle
        check(bufman::write_block(raw, 1, block.data(), block.size(), error),
              "storage: damaged page written back");
        bufman::close(raw);
    }
    {
        heapfile::HeapFile damaged(manager);
        check(damaged.open("storage.bin", schema, error),
              "storage: the damaged file still opens");
        check(!damaged.check_structure(error) &&
                  error.find("cyclic") != std::string::npos,
              "storage: a cyclic list is reported by the structural check");
        damaged.close(error);
    }

    check(file.open("storage.bin", schema, error),
          "storage: file reopened for cleanup");
    manager.close_all(error);
    std::filesystem::remove("storage.bin");
}

} // namespace

int main() {
    catalog_checks();
    writer_checks();
    binder_checks();
    executor_checks();
    list_case_checks();
    storage_checks();
    // The catalog fixtures are per-pid scratch; take them down with us.
    std::error_code cleanup_error;
    std::filesystem::remove_all(catalog_dir(), cleanup_error);
    std::printf("%d passed, %d failed\n", passed_, failed_);
    if (failed_ == 0) {
        std::printf("All checks passed.\n");
        return 0;
    }
    std::printf("Some checks failed.\n");
    return 1;
}
