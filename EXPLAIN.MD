# EXPLAIN — the modules and how to build the heap file

This document explains what each module of the engine does, the conventions
that hold across all of them, the behavior the finished engine must show, and
a strategy for implementing the one module that is left to you:
`heapfile/HeapFile.cpp`.

---

## 1. The big picture

The engine is a strict layer stack. Each layer only knows the layer below it,
and the meaning of bytes gets richer as you climb:

```
            p2stdbeng (check suite)     p2stdbeng_demo (SQL session)
                          |                      |
 exec/        Binder: AST -> bound plans   Executors: plans -> HeapFile calls
                          |                      |
 sql/         Lexer, Parser, AST (statements and expressions)
                          |
 schema/      TableSchema, JSON catalog, TupleSerializer, CSV load
                          |
 heapfile/    SlottedPage (page layout)  +  HeapFile (YOUR TASK)
                          |
 bufpool/     BufferManager: pin/unpin, page table, eviction, write-back
              BufferPool + DataFrame: the frames themselves
 policy/      ReplacementPolicy (LRU): which frame to evict
                          |
 file/        BlockFile: 4096-byte block read/write on plain files
```

Reading order for a newcomer: `file/BlockFile.h`, then `bufpool/` (smallest
first: `DataFrame.h`, `BufferPool.h`, then `BufferManager.h`), then
`heapfile/SlottedPage.h`, then `heapfile/HeapFile.h`, then `schema/`, then
`sql/`, then `exec/`.

---

## 2. The modules, bottom-up

### `file/` — BlockFile

The floor of the stack. A file is a sequence of 4096-byte blocks
(`bufman::kBlockSize`), and the layer offers exactly five operations:
`open_for_append` / `open_for_read`, `close`, `read_block`, `write_block`,
and `block_count`. It assigns no meaning to the bytes; everything above this
line decides what a block contains. A `bufman::File` is just an open file
descriptor the higher layers pass around.

### `bufpool/` — DataFrame, BufferPool, BufferManager

- **`DataFrame.h`** — one frame: a 4096-byte buffer plus a pin count and a
  dirty flag. `kPageSize = 4096` lives here.
- **`BufferPool`** — a fixed vector of frames. `pin`/`unpin` adjust the
  counts; `set_dirty` marks a frame as needing write-back. Nothing else.
- **`BufferManager`** — the actual machinery. It owns the pool, a page table
  mapping `(file, block) -> frame`, and an injected replacement policy.
  `pin(file, block)` returns a frame index: either the page is resident
  (a hit) or a victim frame is chosen by the policy, a dirty victim is
  written back first, and the block is read in. `alloc_page` is the
  counterpart for brand-new blocks: it takes a frame, zeroes it, registers
  it, and marks it dirty so it reaches disk on write-back. `unpin(frame,
  was_dirty)` releases one pin. `close_file` flushes one file's dirty pages,
  drops its table entries, and closes its descriptor — other files are
  untouched, and the call fails while any of the file's frames is still
  pinned.

The rule the whole engine lives by: **no layer above the buffer manager ever
touches a file directly**. Dirty pages become visible on disk only at
eviction or close, which is why a freshly written page can legitimately be
missing from the on-disk block count.

### `policy/` — ReplacementPolicy, LRUPolicy

The eviction decision, injected into the manager as a `unique_ptr`. LRU
evicts the least recently used frame. The seam exists so the storage layer
can be tested with different policies without changes.

### `heapfile/` — SlottedPage and HeapFile

- **`SlottedPage`** (provided) — a non-owning view over one page buffer.
  Layout: the last 12 bytes of the page are a trailer — slot count, `prev`
  page id, `next` page id — and the bytes before them split into slots of
  *(record + occupancy byte)*. `format()` prepares a fresh page; `attach()`
  binds to existing bytes and verifies the stored slot count against the
  value recomputed from the record size (a mismatch means corrupt or foreign
  bytes). Accessors cover the chain links (`prev_page`/`next_page`), the
  occupancy (`capacity`, `occupied_count`, `first_free_slot`,
  `is_occupied`), and the records themselves (`get_record`, `put_record`,
  `erase` — erasing clears the occupancy byte and leaves the bytes behind as
  garbage). `kNoPage = 0xFFFFFFFF` is the "no page" sentinel; 0 cannot be it
  because 0 is a legal page id.
- **`HeapFile`** — the module you implement; see sections 4–6.

### `tuple/` — Column and Tuple

The in-memory row. A `Column` is a typed value — `Integer` (int), `Double`
(double), or `String` (std::string) — and a `Tuple` is a vector of columns.
One tuple slot corresponds to one schema column, addressed by the column's
creation-order index.

### `schema/` — structure and codecs

- **`TableSchema`** — a table's metadata: name, columns in creation order,
  and a name-to-index map. `validate(tuple)` checks arity and types;
  `make_tuple()` builds a default-valued tuple shaped like the schema. The
  catalog loader sets `data_file`, the table's data-file name — the
  catalog's optional `data_file` key, resolved to `<table_name>.bin` when
  absent; always a bare file name.
- **`TableSchemaJson`** — reads and writes the per-table JSON catalog under
  `tablecatalog/`. The catalog is the complete registry of a table: its
  structure and where its data file lives.
- **`TupleSerializer`** — the one record codec, bound to a schema at
  construction. Every record is the same fixed width: 4 bytes per Integer,
  8 per Double, the declared width per String. `serialize()` validates first
  (arity, types, string width) and fails before touching the buffer;
  `deserialize()` verifies the String padding is all NUL and re-validates
  against the schema. `record_size()` is the width every record occupies.
- **`TableCsv`** — CSV bulk load: per-row validation where an invalid row is
  reported with its line number and *skipped*, never fatal.
- **`TableGenerator`** — pseudo-random schema-shaped rows for tests.

### `sql/` — Lexer, Parser, AST

A classic front end. The `Lexer` turns a line into tokens (errors carry the
line number); the `Parser` accepts a `;`-separated statement list and builds
an AST. Supported statements: `CREATE TABLE`, `DROP TABLE`, `INSERT ...
VALUES` (one or more value lists), `SELECT ... FROM ... [WHERE ...]
[LIMIT n]`, `UPDATE ... SET ... [WHERE ...]`, `DELETE FROM ... [WHERE ...]`.
Types in DDL: `INTEGER`/`INT` (4 bytes), `DOUBLE` (8 bytes), `VARCHAR(n)`.
The AST also has a printer and a draw helper for inspecting parse results.

### `exec/` — Binder and Executors

The binder resolves every statement against the catalog **before** anything
executes: it loads the table's schema (through a cache), resolves every
column name to a creation-order index, and type-checks every expression.
Its output is a *bound plan* (`BoundSelect`, `BoundInsert`, `BoundUpdate`,
`BoundDelete`, `BoundCreateTable`, `BoundDropTable`) that executors run
without ever seeing the AST. The binder is type-strict — comparing an
Integer column against a string literal is a named error before execution —
and it refuses out-of-scope SQL (`JOIN`, `GROUP BY`, `ORDER BY`) with an
error naming the unsupported feature.

The executors turn plans into `HeapFile` calls:

- `execute_select` scans the file, evaluates WHERE per row, projects, honors
  LIMIT, and hands each output row to a callback.
- `execute_insert` inserts each bound row — the binder already refused any
  invalid value list, so every row in the plan stores.
- `execute_update` and `execute_delete` are **collect-then-apply**: first
  scan and collect every matching row (with its row id and old values), then
  apply. A collection failure applies nothing.
- `execute_create_table` writes the catalog and creates the empty data file
  — both halves or neither; `execute_drop_table` removes the data file and
  the catalog, tolerating a missing data file.

### The two binaries

- **`p2stdbeng`** (`main.cpp`) — the check suite: catalog checks, writer
  checks, binder checks, executor checks, hand-built list-surgery cases, and
  storage checks that run `HeapFile::check_structure` after every operation,
  including a randomized 200-operation insert/erase sequence cross-checked
  against a shadow model.
- **`p2stdbeng_demo`** (`p2stdbeng_demo.cpp`) — the SQL session: a REPL over
  the engine with one buffer manager (LRU, 8 frames) per session, the
  `append <table> <csv-file>` bulk-load command, and a single flush of every
  open file at `quit`.

---

## 3. Conventions that hold everywhere

1. **Errors are values, never exceptions.** Every fallible operation takes a
   trailing `std::string& error`, clears it on entry, and fills it with a
   named message exactly when it returns false. An empty `error` after the
   call means success. (One deliberate exception: `HeapFileIterator::next`
   returns false with an *empty* error at normal end-of-scan — callers
   distinguish exhaustion from failure by testing the string.)
2. **Failures change nothing.** On any failure the file's contents are
   unchanged — transitions are either-or, never half-applied. Release every
   pin you took on every failure path.
3. **All disk access goes through the manager.** Pin, act on the frame's
   buffer, unpin (with the dirty flag when you wrote). Row operations hold
   exactly one pin at a time; list surgery transiently holds up to five.
4. **The catalog is the registry.** A table exists iff its catalog file
   exists; the data file name comes from the catalog (default
   `<table_name>.bin`); paths resolve relative to the working directory.

---

## 4. What the storage must satisfy

These are the behavioral requirements the heap file implementation is judged
against. Each one is exercised by a named check in the check suite.

### The two free-space lists

The header carries `free_head`, `free_tail`, `full_head`, `full_tail`, and
`next_page_id`. Every allocated data page is reachable from exactly one of
the two lists:

- the **free list** — pages with at least one free slot;
- the **full list** — pages whose every slot is occupied.

Membership is **derived, never stored**: a page belongs on the full list iff
`occupied_count() == capacity()`. There is no membership bit — a stored bit
could drift silently, while the derived rule turns every drift into a
`check_structure` failure. Both lists are appended at the **tail**, so each
list's order reflects allocation (and re-allocation) order. Pages are
allocated once and never removed: a page whose records were all erased stays
on the free list forever, and the file never shrinks.

*Examples:* a file with page 1 full and page 2 half-full has `full_head`
reaching page 1 and `free_head` reaching page 2, with no page reachable from
both. Filling a page's last slot moves it to the full list; erasing a slot
from a full page moves it back to the free list.

### INSERT picks a page without walking

INSERT stores each row into the free list's **first** page — the first page
in free-list order with a free slot — without visiting any other page. Only
when the free list is empty does INSERT allocate a fresh page (id taken from
`next_page_id`, which then advances), append it to the free list, and store
the row there.

Consequences: a bulk load scans back in insertion order (page 1's rows
before page 2's, and so on), and pages freed by erases are reused before new
pages are allocated — delete rows until an earlier page has space, and the
next inserts land there without the file growing.

### Pages move only on fill or unfill

A page moves from the free list to the full list exactly when an insert
occupies its last free slot, and back exactly when an erase frees a slot on
a previously full page. **UPDATE never moves a page** — records are
fixed-width, so occupancy never changes — and find/update/erase address
records directly by `(page, slot)`.

### Scans walk the free list, then the full list

A scan visits the free list's pages in list order, then the full list's
pages in list order, returning every occupied slot's tuple in slot order.
Within one list the order is the list's order; the order across lists and
under interleaved mutation is unspecified. Every row appears exactly once —
no page and no slot is skipped or visited twice. A single-page table scans
in plain slot order.

### The structure can verify itself

`check_structure` walks both lists from the header and must report a named
error when any of these holds:

- a head or tail pointer that disagrees with the walk (empty iff `kNoPage`
  at both ends);
- a cycle, an unreachable page, or a page visited twice;
- a page on a list contradicting its occupancy;
- the header page appearing on a list;
- the union of the two lists not covering exactly pages 1 .. `next_page_id`
  − 1;
- an on-disk data page the walk cannot reach (a dirty page may not be on
  disk yet, so not the other way around).

An empty file (header only) passes. A severed link is reported, not silently
absorbed.

### The TUP2 format refuses foreign files

A heap file identifies itself with the magic `TUP2` in block 0; opening a
file with any other magic fails with a named error and leaves the file
untouched. There is no migration path from older formats. A freshly created
file opens with empty lists and `next_page_id` 1.

---

## 5. What the SQL layer guarantees

The layers above the heap file are provided, but their behavior shapes how
your methods get called — worth knowing what to rely on:

- **SELECT** scans and filters; `*` returns all columns in catalog order, an
  explicit list returns them in the order written; an unmatched WHERE is an
  empty result, not an error; LIMIT caps the output.
- **INSERT** is all-or-nothing per statement: every value list is validated
  at bind time (value count, types, string width), and a statement with any
  invalid list stores nothing. The data file is created on first insert if
  missing.
- **UPDATE** evaluates every SET expression against the row's **old**
  values, so assignments never observe each other — `SET a = b, b = a`
  swaps. Non-matching rows stay untouched.
- **DELETE** removes the collected matches; freed space becomes reusable by
  later inserts. DELETE without WHERE removes every row.
- **Fail-fast scans**: a record that violates the schema fails the whole
  statement with a named error — no partial SELECT results, no partial
  UPDATE or DELETE application.
- **DDL**: `CREATE TABLE` writes the catalog and creates the empty data
  file, refusing a duplicate name with both files untouched; `DROP TABLE`
  removes both files, refusing an unknown name. A data file the session
  holds open is closed by the session as part of the drop; a file open
  through something else refuses the erase.
- **The session** keeps each data file open from first use until `quit`,
  binds each open file to the table it was first used with (cross-table
  reuse is refused), prints errors and continues on any failed statement,
  and flushes every file exactly once at quit (exit code 1 only if a flush
  fails). The `append` command reports each invalid CSV row as
  `line N: reason` and skips it.

---

## 6. Implementation strategy

Work in this order. Each step builds on the previous one, and every step has
checks waiting for it in the suite — build and run `./build/p2stdbeng` (or
`ctest --test-dir build --output-on-failure`) after each step and let the
failing check names tell you what is still wrong.

### Step 0 — read the contract

Read `heapfile/HeapFile.h` top to bottom: the file format comment, the list
invariants, the pin protocol, and every method's doc. Then read the provided
helpers at the top of `HeapFile.cpp` (`HeaderFields`, the offsets,
`write_identification`, `check_identification`, `header_fields_in_range`,
and the `release` helper). They fix the on-disk format; your methods build
on them. Skim `heapfile/SlottedPage.h` and `bufpool/BufferManager.h` so the
pin/unpin calls feel natural.

### Step 1 — the trivial state helpers

`is_open` and `require_open` are two-liners, but implement them first:
every other method opens with `require_open`, so having them real makes
everything after testable. `resident` is a one-liner over the manager's
pool observation.

### Step 2 — lifecycle: `create`, `open`, `close`

- **`create`**: refuse an open HeapFile and a non-empty path (an existing
  *empty* file is formatted in place); compute `record_size_` from the
  serializer and refuse a record size that cannot fit a page; `alloc_page`
  page 0; format it as a `SlottedPage`; write the identification record;
  close (which flushes) and leave the HeapFile closed.
- **`open`**: probe that the path exists first (the manager's open would
  silently create it); register with the manager; pin and validate block 0 —
  identification, `SlottedPage::attach`, `header_fields_in_range` — then
  bind `schema_`, `serializer_`, `record_size_` and mark the file open. On
  any failure unwind completely (close through the manager, reset the
  handle) and return false.
- **`close`**: close through the manager (`close_file`); reset this
  HeapFile's state only when that succeeded, so a failed close (a page
  still pinned) can be retried.

### Step 3 — list surgery: `append_fresh_page`, `move_between_lists`

These are the only writers of list pointers. Follow the pin protocol
exactly:

1. Acquire **all** auxiliary pins before the first write, in **ascending
   page-id order** — the header, the moving page's two source neighbors,
   and the destination list's tail. The set is always distinct (a page sits
   on exactly one list), so ordering by id is well-defined.
2. Once all pins are held, the surgery is pure memory writes and cannot fail
   mid-way: a membership change is either fully applied or not attempted.
3. Release every pin you took on every failure path (the provided
   `release` helper keeps the real error intact).

`append_fresh_page` is the degenerate case: the free list is empty by
construction, so the header is the only auxiliary pin — make the new page
the list's only element and advance `next_page_id`.

### Step 4 — row operations

Implement `pin_record_page` first: it is the shared validate-pin-attach path
for find/update/erase. It rejects the header page and pages beyond the
file's extent (resident pages count as existing even before write-back),
pins, attaches, and requires the slot occupied; the caller owns the pin on
success and the helper cleans up on failure.

Then the four operations, each a short composition of pieces you already
have:

- **`insert`**: serialize (validates first — failures change nothing);
  target = free list head, or allocate + `append_fresh_page` when empty;
  `put_record` into the first free slot; `move_between_lists(to_full=true)`
  when the last free slot was taken; report the RowId.
- **`find`**: pin the record page, `get_record`, unpin, deserialize.
- **`update`**: serialize, pin, `put_record` over the same slot, unpin. No
  list move — occupancy did not change.
- **`erase`**: pin, note whether the page *was* full, clear the occupancy
  byte, `move_between_lists(to_full=false)` only when it was. The record
  bytes stay behind as garbage.

### Step 5 — the scan

`scan()` pins the header once, reads the two list heads and
`next_page_id`, unpins, and returns an iterator seeded with them. The
iterator then walks free-list pages, then full-list pages, in list order,
scanning each page's slots in order and yielding occupied ones; it
remembers its slot to resume across pins. Exhaustion is `false` with an
EMPTY error; the `seen_` array is the cycle guard — once the walk leaves a
page it can never legitimately return, so seeing it again is a named
failure.

### Step 6 — `check_structure` last

It is the largest single method but pure reading: walk each list one pin at
a time, count page visits, verify each page's occupancy against its list,
and compare the visited set against 1 .. `next_page_id` − 1 and the on-disk
block count (allowing dirty pages not yet on disk — the provided
`header_fields_in_range` shows the slack convention). Implement it last
because by then everything it verifies exists, and keep using it while
developing: the storage checks run it after every operation, so a relink
mistake surfaces at the step that caused it.

### Working habits that pay off

- **Keep the error convention mechanically.** Clear `error` on entry; fill
  it exactly when returning false; return the `release()` helper's pattern
  on failure paths so unpinning never clobbers the real cause.
- **Either-or transitions.** If a failure can happen after you started
  changing a page, restructure: validate everything, take all pins, *then*
  write.
- **Mind the pool size.** The session's 8 frames are comfortable for the
  five-pin surgery; a pool of 2 can fail where 8 succeeds. The pin
  discipline in `HeapFile.h` exists precisely so the small pools work.
- **Do not change the provided files.** Everything outside the member
  method bodies of `HeapFile.cpp` is fixed — the helpers fix the on-disk
  format, and the check suite depends on it.
