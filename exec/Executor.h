#ifndef P2_EXECUTOR_H
#define P2_EXECUTOR_H

#include "bufpool/BufferManager.h"
#include "exec/Binder.h"
#include "heapfile/HeapFile.h"
#include "tuple/Tuple.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace exec {

/// Opens the table's data file as a HeapFile over `manager`, creating the
/// (header-only) file first when `create_when_missing` is set and the file
/// does not exist. The caller owns the file and must close it (or close
/// the whole session) when done.
bool open_table_file(bufman::BufferManager& manager,
                     const schema::TableSchema& schema,
                     bool create_when_missing,
                     std::unique_ptr<heapfile::HeapFile>& file,
                     std::string& error);

/// Runs a bound SELECT over `file`, passing every matching output row (one
/// Column per bound output item, in item order) to `emit` in scan order.
/// LIMIT stops the scan once enough rows were emitted (`limit == 0` emits
/// nothing); `emit` returning false also stops it early (both are success).
/// A WHERE expression that fails to evaluate (division or modulo by zero,
/// integer overflow) fails the statement with a named error, as does an
/// unreadable record — fail-fast, no partial results.
bool execute_select(const BoundSelect& plan, heapfile::HeapFile& file,
                    const std::function<bool(const std::vector<tuple::Column>&)>& emit,
                    std::string& error);

/// Inserts every bound row, one call to the heap file per row. The binder
/// has already refused any statement with an invalid value list, so every
/// row in the plan stores. Reports the count.
bool execute_insert(const BoundInsert& plan, heapfile::HeapFile& file,
                    std::size_t& inserted, std::string& error);

/// Collect-then-apply: scans for matching rows first, keeping each match's
/// row id and old values; only when the whole collection succeeded are the
/// new rows written back, each rebuilt from its old values (every SET
/// expression sees the row's old values). A collection failure (unreadable
/// record) applies nothing; an apply failure aborts the rest and is named.
bool execute_update(const BoundUpdate& plan, heapfile::HeapFile& file,
                    std::size_t& updated, std::string& error);

/// Collect-then-apply with erase: matches are collected first, then
/// removed one call per row. A collection failure applies nothing.
bool execute_delete(const BoundDelete& plan, heapfile::HeapFile& file,
                    std::size_t& deleted, std::string& error);

/// Writes the table's catalog and creates its empty data file — the two
/// halves of a table, or neither: a failed file creation removes the
/// catalog again. The new file is left closed; the first statement naming
/// the table opens it.
bool execute_create_table(const BoundCreateTable& plan,
                          bufman::BufferManager& manager, std::string& error);

/// Removes the table's data file (refused while it is open through
/// `manager`) and then its catalog. A missing data file is tolerated —
/// DROP removes what exists — but a failing removal abandons the catalog.
bool execute_drop_table(const BoundDropTable& plan,
                        bufman::BufferManager& manager, std::string& error);

}

#endif // P2_EXECUTOR_H
