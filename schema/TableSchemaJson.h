#ifndef EX11_TABLE_SCHEMA_JSON_H
#define EX11_TABLE_SCHEMA_JSON_H

#include "schema/TableSchema.h"

#include <string>

namespace schema {

/// Parse catalog text — the JSON shape of tablecatalog/person.json: an
/// object with "table_name" and a "columns" array of objects, each with
/// "column_name", "column_type" (Integer/String/Double), and "size" —
/// into a fresh schema. On failure: false, a named error ("line N: ..."),
/// `schema` left empty. On success the caller's schema is replaced
/// wholesale: the table name, one add_column per entry in file order,
/// each column's declared size set.
bool parse_table_schema(const std::string& text, TableSchema& schema,
                        std::string& error);

/// Read the file at `path` and parse_table_schema it. File errors
/// (missing, unreadable) are named errors like any other.
bool load_table_schema(const std::string& path, TableSchema& schema,
                       std::string& error);

/// Write `schema` to `path` as catalog JSON in exactly the shape
/// load_table_schema parses: "table_name", "data_file" (only when it
/// differs from the "<table_name>.bin" default), and one "columns" entry
/// per column with column_name, column_type (Integer/String/Double), and
/// size — the natural byte width (4/8) for Integer/Double columns, the
/// declared width for String columns, which must be declared. Refuses an
/// unnamed table and names or sizes the loader would reject. The parent
/// directory must exist; an existing file at `path` is replaced.
bool write_table_schema(const std::string& path, const TableSchema& schema,
                        std::string& error);

}

#endif // EX11_TABLE_SCHEMA_JSON_H
