# p2stdbeng

p2stdbeng is a small single-table SQL database engine written from scratch in
C++20, built layer by layer: block files, a buffer pool, slotted heap pages,
a free-space index, a tuple codec, a JSON catalog, and a SQL layer that
parses, binds, and executes statements against that storage.

**This is the student version: everything works except the heap file.** The
member methods declared in `heapfile/HeapFile.h` — and documented there in
full — are blank stubs in `heapfile/HeapFile.cpp`. Your task is to implement
them until the check suite passes. Everything else (block file, buffer
manager, slotted pages, catalog, serializer, parser, binder, executors) is
provided and must not change.

**Due date: October 23, 2026, 11:59 PM.**

## What it does

- **Storage**: a table is a heap file of fixed-width records in 4096-byte
  slotted pages. Block 0 is the header page; the rest are data pages.
- **Free-space lists**: the header carries two page lists — pages with free
  space and full pages — so INSERT picks its page in O(1) instead of walking
  a chain. A page's membership is derived from its occupancy, and a
  structural self-check (`HeapFile::check_structure`) can verify the whole
  structure at any time.
- **Buffering**: every disk access goes through an LRU buffer manager with a
  configurable frame count; dirty pages reach disk on eviction or close.
- **Types**: Integer, Double, and fixed-width String columns. The binder is
  type-strict: comparing an Integer column against a string literal is a
  named error before execution.
- **Catalogs**: each table's schema lives in a JSON file under
  `tablecatalog/`; the data file name comes from the catalog.

## Your task

Implement the blank member method bodies in `heapfile/HeapFile.cpp`:

- the lifecycle pair `create`/`open` (header formatting and validation) and
  `close`;
- the row operations `insert`, `find`, `update`, `erase` over the two
  free-space lists, plus the shared `pin_record_page` helper;
- the list-surgery primitives `append_fresh_page` and `move_between_lists`
  (all pins up front, in ascending page-id order, writes only after every
  pin is held);
- the structural self-check `check_structure`;
- the scan: `HeapFile::scan` seeding `HeapFileIterator::next`.

Start with `heapfile/HeapFile.h` — it documents the file format, the list
invariants, the pin protocol, and every method's contract. The header-page
layout and identification helpers in `HeapFile.cpp` are provided and fix the
on-disk format. Work with the check suite (`./build/p2stdbeng`): each check
names the behavior it verifies, and the storage checks run
`check_structure` after every operation, so a relink mistake surfaces at the
step that caused it.

## Repository structure

| Directory | Purpose |
|-----------|---------|
| `file/` | Block-level I/O: 4096-byte block read/write on plain files |
| `bufpool/` | Buffer pool and buffer manager: pin/unpin, eviction, write-back |
| `policy/` | Replacement policies (LRU) injected into the buffer manager |
| `heapfile/` | Slotted pages (provided) and the heap file (**your task**) |
| `tuple/` | In-memory row values (`Tuple`, `Column`) |
| `schema/` | Table schema, JSON catalog I/O, the tuple serializer, CSV bulk load, a row generator |
| `sql/` | SQL lexer, parser, AST (with a printer and a draw helper) |
| `exec/` | Binder (statements and expressions resolved against the catalog) and executors |

Two binaries share the library code:

- `p2stdbeng` — the check suite (`main.cpp`): catalog, parser, binder,
  executor, and storage checks, including the structural self-check run
  after every operation of the storage tests.
- `p2stdbeng_demo` — the SQL session (`p2stdbeng_demo.cpp`): a REPL over the
  engine, one buffer manager (LRU, 8 frames) per session.

## Building

CMake with a C++20 compiler:

```sh
cmake -S . -B build
cmake --build build
```

## Running

Run the check suite (also registered with CTest):

```sh
ctest --test-dir build --output-on-failure
# or directly:
./build/p2stdbeng
```

Start the SQL session (works once the heap file is implemented):

```sh
./build/p2stdbeng_demo
```

Every prompt line is one SQL statement, plus two REPL commands: `append
<table> <csv-file>` bulk-loads rows from a CSV file, and `quit` flushes and
exits. A session example:

```sql
CREATE TABLE person (pid INTEGER, name VARCHAR(10), age INTEGER)
INSERT INTO person VALUES (1, 'Ada', 30), (2, 'Bob', 25)
SELECT name FROM person WHERE age > 28
UPDATE person SET age = age + 1 WHERE pid = 1
DELETE FROM person WHERE pid = 2
DROP TABLE person
quit
```

Catalogs are read from and written to `tablecatalog/` in the working
directory; each table's data file is created on first use. All errors are
returned as named strings — the engine reports what failed (an unknown
column, a type mismatch, a corrupt record) instead of throwing.

## Checking storage health

The heap file exposes a structural self-check that walks both free-space
lists from the header and verifies reachability, single membership, no
cycles, membership-versus-occupancy, and on-disk coverage. The check suite
exercises it after every storage operation, including a randomized
insert/erase sequence cross-checked against a shadow model.
