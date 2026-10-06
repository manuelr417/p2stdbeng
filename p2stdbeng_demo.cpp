// p2dbeng_demo — the SQL session over the p2 execution layer. One buffer
// manager (LRU, 8 frames) for the whole session; catalogs resolve through
// the binder's cache; data files open on first use, bind to their table,
// and flush once at quit. Every prompt line is one SQL statement except
// `append <table> <csv-file>` (the CSV bulk-load seam) and `quit`.

#include "bufpool/BufferManager.h"
#include "exec/Binder.h"
#include "exec/BoundExpr.h"
#include "exec/Executor.h"
#include "heapfile/HeapFile.h"
#include "policy/LRUPolicy.h"
#include "schema/TableCsv.h"
#include "schema/TableSchema.h"
#include "sql/Ast.h"
#include "sql/Parser.h"
#include "tuple/Column.h"
#include "tuple/Tuple.h"

#include <cstddef>
#include <iostream>
#include <memory>
#include <string>
#include <unordered_map>
#include <unistd.h>
#include <vector>

namespace {

constexpr const char* kCatalogDirectory = "tablecatalog";
constexpr std::size_t kPoolFrames = 8;

// One value, in the CLI's `name=value` style.
std::string print_value(const tuple::Column& value) {
    switch (value.column_type()) {
        case tuple::ColumnType::Integer:
            return std::to_string(std::get<int>(value.column_value()));
        case tuple::ColumnType::Double:
            return std::to_string(std::get<double>(value.column_value()));
        case tuple::ColumnType::String:
            return std::get<std::string>(value.column_value());
    }
    return "?";
}

// --- Session state: one manager, one binder, the file table. ---------------

struct SessionFile {
    std::unique_ptr<heapfile::HeapFile> file;
    const schema::TableSchema* schema; // owned by the binder's catalog cache
};

class Session {
public:
    Session() : manager_(std::make_unique<bufman::BufferManager>()) {
        std::string init_error;
        manager_->init(kPoolFrames, std::make_unique<bufman::LRUPolicy>(),
                       init_error);
    }

    // The session's data files, keyed by data-file path. A file binds to
    // the table it was first used with; cross-table reuse is refused.
    SessionFile* file_for(const schema::TableSchema* schema,
                          bool create_when_missing, std::string& error) {
        error.clear();
        const std::string& path = schema->data_file();
        const auto found = files_.find(path);
        if (found != files_.end()) {
            if (found->second.schema->table_name() != schema->table_name()) {
                error = "'" + path + "' is bound to table '" +
                        found->second.schema->table_name() + "'";
                return nullptr;
            }
            return &found->second;
        }
        std::unique_ptr<heapfile::HeapFile> file;
        if (!exec::open_table_file(*manager_, *schema, create_when_missing,
                                   file, error)) {
            return nullptr;
        }
        const auto inserted =
            files_.emplace(path, SessionFile{std::move(file), schema}).first;
        return &inserted->second;
    }

    // Closes every session file exactly once; reports the first failure.
    bool close_all(std::string& error) {
        error.clear();
        bool ok = true;
        for (auto& [path, entry] : files_) {
            std::string close_error;
            if (!entry.file->close(close_error)) {
                if (ok) {
                    error = close_error;
                    ok = false;
                }
            }
            entry.file.reset();
        }
        files_.clear();
        return ok;
    }

    // Closes the session's own handle for `table`'s data file, if one is
    // open — the DROP path: the engine refuses to erase an open file, but
    // the session's handle is the session's to release. Returns false
    // (with the error) when the close itself fails.
    bool close_table_file(const std::string& table, std::string& error) {
        for (auto it = files_.begin(); it != files_.end(); ++it) {
            if (it->second.schema->table_name() == table) {
                const bool ok = it->second.file->close(error);
                files_.erase(it);
                return ok;
            }
        }
        return true;
    }

    bufman::BufferManager& manager() { return *manager_; }

    exec::Binder binder{kCatalogDirectory};
    std::ostream& diagnostics = std::cerr;

private:
    std::unique_ptr<bufman::BufferManager> manager_;
    std::unordered_map<std::string, SessionFile> files_;
};

// --- One statement of each kind. -------------------------------------------

bool run_select(Session& session, const exec::BoundSelect& plan,
                std::string& error) {
    SessionFile* entry =
        session.file_for(plan.schema, false, error);
    if (entry == nullptr) {
        return false;
    }
    return exec::execute_select(
        plan, *entry->file,
        [&](const std::vector<tuple::Column>& row) {
            std::string line;
            for (std::size_t i = 0; i < plan.items.size(); ++i) {
                if (i != 0) {
                    line += ", ";
                }
                line += plan.items[i].name + "=" +
                        print_value(row[i]);
            }
            std::cout << line << "\n";
            return true;
        },
        error);
}

bool run_insert(Session& session, const exec::BoundInsert& plan,
                std::string& error) {
    SessionFile* entry = session.file_for(plan.schema, true, error);
    if (entry == nullptr) {
        return false;
    }
    std::size_t inserted = 0;
    if (!exec::execute_insert(plan, *entry->file, inserted, error)) {
        return false;
    }
    std::cout << "inserted " << inserted << " row(s)\n";
    return true;
}

bool run_update(Session& session, const exec::BoundUpdate& plan,
                std::string& error) {
    SessionFile* entry = session.file_for(plan.schema, false, error);
    if (entry == nullptr) {
        return false;
    }
    std::size_t updated = 0;
    if (!exec::execute_update(plan, *entry->file, updated, error)) {
        return false;
    }
    std::cout << "updated " << updated << " row(s)\n";
    return true;
}

bool run_delete(Session& session, const exec::BoundDelete& plan,
                std::string& error) {
    SessionFile* entry = session.file_for(plan.schema, false, error);
    if (entry == nullptr) {
        return false;
    }
    std::size_t deleted = 0;
    if (!exec::execute_delete(plan, *entry->file, deleted, error)) {
        return false;
    }
    std::cout << "deleted " << deleted << " row(s)\n";
    return true;
}

bool run_create(Session& session, const exec::BoundCreateTable& plan,
                std::string& error) {
    if (!exec::execute_create_table(plan, session.manager(), error)) {
        return false;
    }
    std::cout << "created table '" << plan.schema.table_name() << "'\n";
    return true;
}

bool run_drop(Session& session, const exec::BoundDropTable& plan,
              std::string& error) {
    // The engine refuses to erase an open file; the session's own handle
    // is released first, so a table used in this session can be dropped
    // in it. A handle held outside the session still refuses the drop.
    // The schema lives in the binder's cache, so the name is captured
    // before the cache entry is invalidated.
    const std::string name = plan.schema->table_name();
    if (!session.close_table_file(name, error)) {
        return false;
    }
    if (!exec::execute_drop_table(plan, session.manager(), error)) {
        return false;
    }
    session.binder.invalidate(name);
    std::cout << "dropped table '" << name << "'\n";
    return true;
}

// --- The `append` command: CSV bulk load through TableCsv. ------------------

bool run_append(Session& session, const std::string& line,
                std::string& error) {
    // append <table> <csv-file>
    const std::size_t first_space = line.find(' ');
    if (first_space == std::string::npos) {
        error = "usage: append <table> <csv-file>";
        return false;
    }
    const std::size_t second_space = line.find(' ', first_space + 1);
    if (second_space == std::string::npos) {
        error = "usage: append <table> <csv-file>";
        return false;
    }
    const std::string name = line.substr(first_space + 1,
                                         second_space - first_space - 1);
    const std::string csv = line.substr(second_space + 1);
    const schema::TableSchema* schema = session.binder.table_schema(name, error);
    if (schema == nullptr) {
        return false;
    }
    schema::CsvLoadResult loaded;
    if (!schema::load_csv(*schema, csv, loaded, session.diagnostics, error)) {
        return false;
    }
    SessionFile* entry = session.file_for(schema, true, error);
    if (entry == nullptr) {
        return false;
    }
    std::size_t appended = 0;
    for (const tuple::Tuple& row : loaded.tuples) {
        heapfile::RowId row_id;
        if (!entry->file->insert(row, row_id, error)) {
            return false;
        }
        ++appended;
    }
    std::cout << "appended " << appended << " record(s), skipped "
              << loaded.skipped << " row(s)\n";
    return true;
}

// Executes one parsed statement; false with a named error.
bool run_statement(Session& session, const sql::Statement& statement,
                   std::string& error) {
    exec::BoundStatement bound;
    if (!session.binder.bind(statement, bound, error)) {
        return false;
    }
    if (auto* plan = std::get_if<exec::BoundSelect>(&bound)) {
        return run_select(session, *plan, error);
    }
    if (auto* plan = std::get_if<exec::BoundInsert>(&bound)) {
        return run_insert(session, *plan, error);
    }
    if (auto* plan = std::get_if<exec::BoundUpdate>(&bound)) {
        return run_update(session, *plan, error);
    }
    if (auto* plan = std::get_if<exec::BoundDelete>(&bound)) {
        return run_delete(session, *plan, error);
    }
    if (auto* plan = std::get_if<exec::BoundCreateTable>(&bound)) {
        return run_create(session, *plan, error);
    }
    if (auto* plan = std::get_if<exec::BoundDropTable>(&bound)) {
        return run_drop(session, *plan, error);
    }
    error = "unsupported statement";
    return false;
}

}

int main() {
    Session session;
    const bool interactive = ::isatty(0);
    const std::string prompt = "> ";
    std::string line;
    while (true) {
        if (interactive) {
            std::cout << prompt << std::flush;
        }
        if (!std::getline(std::cin, line)) {
            break; // end of input: quit
        }
        if (line.empty()) {
            continue;
        }
        if (line == "quit") {
            break;
        }
        if (line.rfind("append ", 0) == 0) {
            std::string error;
            if (!run_append(session, line, error)) {
                session.diagnostics << "error: " << error << "\n";
            }
            continue;
        }
        sql::ParseResult parsed;
        std::string error;
        if (!sql::parse(line, parsed, error)) {
            session.diagnostics << "error: " << error << "\n";
            continue;
        }
        for (const auto& statement : parsed.statements) {
            error.clear();
            if (!run_statement(session, *statement, error)) {
                session.diagnostics << "error: " << error << "\n";
                break; // the rest of the line waits for the next prompt
            }
        }
    }
    std::string flush_error;
    const bool flushed = session.close_all(flush_error);
    if (!flushed) {
        session.diagnostics << "error: " << flush_error << "\n";
        return 1;
    }
    return 0;
}
