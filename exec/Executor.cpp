#include "exec/Executor.h"

#include "schema/TableSchemaJson.h"

#include <cstdio>
#include <filesystem>

namespace exec {

namespace {

// Truth of a WHERE result: the binder guarantees an Integer expression.
bool predicate_holds(const BoundExpr& where, const tuple::Tuple& row,
                     std::string& error) {
    tuple::Column value;
    if (!evaluate(where, row, value, error)) {
        return false;
    }
    return std::get<int>(value.column_value()) != 0;
}

// Design D5's rebuild: the new row is the old row with SET positions
// replaced by their expressions evaluated against the old row — every
// assignment sees the row's old values, so simultaneous SET semantics
// (the swap case) come free.
bool rebuild_row(const BoundUpdate& plan, const tuple::Tuple& old,
                 tuple::Tuple& out, std::string& error) {
    for (std::size_t i = 0; i < old.column_count(); ++i) {
        const BoundAssignment* assigned = nullptr;
        for (const BoundAssignment& assignment : plan.assignments) {
            if (assignment.index == i) {
                assigned = &assignment;
                break;
            }
        }
        if (assigned == nullptr) {
            out.add_column(old.column(i));
            continue;
        }
        tuple::Column value;
        if (!evaluate(*assigned->value, old, value, error)) {
            return false;
        }
        out.add_column(value);
    }
    return true;
}

}

bool open_table_file(bufman::BufferManager& manager,
                     const schema::TableSchema& schema,
                     bool create_when_missing,
                     std::unique_ptr<heapfile::HeapFile>& file,
                     std::string& error) {
    error.clear();
    file.reset();
    if (!std::filesystem::exists(schema.data_file())) {
        if (!create_when_missing) {
            error = "no data file '" + schema.data_file() + "' for table '" +
                    schema.table_name() + "'";
            return false;
        }
        heapfile::HeapFile creator(manager);
        if (!creator.create(schema.data_file(), schema, error)) {
            return false;
        }
    }
    auto opened = std::make_unique<heapfile::HeapFile>(manager);
    if (!opened->open(schema.data_file(), schema, error)) {
        return false;
    }
    file = std::move(opened);
    return true;
}

bool execute_select(const BoundSelect& plan, heapfile::HeapFile& file,
                    const std::function<bool(const std::vector<tuple::Column>&)>& emit,
                    std::string& error) {
    error.clear();
    if (plan.has_limit && plan.limit == 0) {
        return true; // no rows wanted, nothing to read
    }
    heapfile::HeapFileIterator it = file.scan();
    tuple::Tuple row;
    std::vector<tuple::Column> output;
    std::uint64_t emitted = 0;
    while (it.next(row, error)) {
        if (plan.where && !predicate_holds(*plan.where, row, error)) {
            if (!error.empty()) {
                return false;
            }
            continue;
        }
        output.clear();
        output.reserve(plan.items.size());
        for (const BoundOutputItem& item : plan.items) {
            tuple::Column value;
            if (!evaluate(*item.expr, row, value, error)) {
                return false;
            }
            output.push_back(std::move(value));
        }
        if (!emit(output)) {
            return true; // the consumer stopped the stream; not a failure
        }
        ++emitted;
        if (plan.has_limit && emitted >= plan.limit) {
            return true;
        }
    }
    // next() reported false: empty error = exhausted, named = fail-fast.
    return error.empty();
}

bool execute_insert(const BoundInsert& plan, heapfile::HeapFile& file,
                    std::size_t& inserted, std::string& error) {
    error.clear();
    inserted = 0;
    // The binder refuses a statement with any invalid value list, so every
    // row in the plan is ready to store.
    for (const BoundInsertRow& row : plan.rows) {
        heapfile::RowId row_id;
        if (!file.insert(row.row, row_id, error)) {
            return false;
        }
        ++inserted;
    }
    return true;
}

bool execute_update(const BoundUpdate& plan, heapfile::HeapFile& file,
                    std::size_t& updated, std::string& error) {
    error.clear();
    updated = 0;
    // Collect first: (row id, old values) pairs. The iterator must not
    // see mutation, and the apply phase needs the old values anyway.
    std::vector<std::pair<heapfile::RowId, tuple::Tuple>> matches;
    heapfile::HeapFileIterator it = file.scan();
    tuple::Tuple row;
    while (it.next(row, error)) {
        if (plan.where && !predicate_holds(*plan.where, row, error)) {
            if (!error.empty()) {
                return false;
            }
            continue;
        }
        matches.emplace_back(it.row_id(), row);
    }
    if (!error.empty()) {
        return false; // fail-fast: a failed collection applies nothing
    }
    for (std::pair<heapfile::RowId, tuple::Tuple>& match : matches) {
        tuple::Tuple fresh;
        if (!rebuild_row(plan, match.second, fresh, error)) {
            return false;
        }
        if (!file.update(match.first, fresh, error)) {
            return false;
        }
        ++updated;
    }
    return true;
}

bool execute_delete(const BoundDelete& plan, heapfile::HeapFile& file,
                    std::size_t& deleted, std::string& error) {
    error.clear();
    deleted = 0;
    std::vector<heapfile::RowId> matches;
    heapfile::HeapFileIterator it = file.scan();
    tuple::Tuple row;
    while (it.next(row, error)) {
        if (plan.where && !predicate_holds(*plan.where, row, error)) {
            if (!error.empty()) {
                return false;
            }
            continue;
        }
        matches.push_back(it.row_id());
    }
    if (!error.empty()) {
        return false;
    }
    for (const heapfile::RowId& row_id : matches) {
        if (!file.erase(row_id, error)) {
            return false;
        }
        ++deleted;
    }
    return true;
}

bool execute_create_table(const BoundCreateTable& plan,
                          bufman::BufferManager& manager, std::string& error) {
    error.clear();
    if (!schema::write_table_schema(plan.catalog_path, plan.schema, error)) {
        return false;
    }
    heapfile::HeapFile file(manager);
    if (!file.create(plan.data_file, plan.schema, error)) {
        // Keep "create = catalog + data file, or neither".
        std::remove(plan.catalog_path.c_str());
        return false;
    }
    return true;
}

bool execute_drop_table(const BoundDropTable& plan,
                        bufman::BufferManager& manager, std::string& error) {
    error.clear();
    // The data file first: erase_file refuses while it is open through
    // the manager, and refusing must leave the table intact.
    if (std::filesystem::exists(plan.data_file)) {
        if (!heapfile::erase_file(manager, plan.data_file, error)) {
            return false;
        }
    }
    std::error_code fs_error;
    if (!std::filesystem::remove(plan.catalog_path, fs_error) || fs_error) {
        error = "cannot remove '" + plan.catalog_path + "'";
        return false;
    }
    return true;
}

}
