#ifndef EX11_HEAP_FILE_H
#define EX11_HEAP_FILE_H

// ============================================================================
// HeapFile — one table's rows, stored in a file of slotted pages.
// ============================================================================
//
// This is the module you implement. The declarations below are the complete
// contract: every public and private member method and field is documented.
// Your work is the member method bodies in heapfile/HeapFile.cpp — the
// file-local helpers already provided there (the header-page layout, its
// field offsets, and the identification read/write helpers) are yours to
// use but not to change.
//
// ---------------------------------------------------------------------------
// The file format (fixed by the helpers in HeapFile.cpp)
// ---------------------------------------------------------------------------
//
// A heap file is a sequence of 4096-byte blocks (bufman::kPageSize). Block 0
// is the header page; blocks 1, 2, ... are data pages. A page's id IS its
// block number, so page 0 is the header and data pages start at 1.
//
// Every page — header included — is a SlottedPage: the last 12 bytes of the
// block are a trailer holding the slot count and the two page links
// (`prev`, `next`), and the bytes before them split into slots of
// (record + occupancy byte). See heapfile/SlottedPage.h for the layout and
// its accessors.
//
// The header page's slots are never used. Its record area instead carries
// the identification record, laid out by the helpers in HeapFile.cpp:
//
//   offset  size  field
//   0       4     magic "TUP2"
//   4       4     free_head     first page with a free slot (kNoPage = empty)
//   8       4     free_tail     last page appended to the free list
//   12      4     full_head     first page whose every slot is used
//   16      4     full_tail     last page of the full list
//   20      4     next_page_id  the id the next allocation will use (starts 1)
//   24      4     name_length
//   28      ...   table name bytes
//
// ---------------------------------------------------------------------------
// The two free-space lists
// ---------------------------------------------------------------------------
//
// The data pages are organized as TWO doubly-linked lists addressed by the
// header, using each page's trailer links as the list links:
//
//   - the FREE list: every data page that has at least one free slot;
//   - the FULL list: every data page whose every slot is occupied.
//
// The invariants your implementation must maintain:
//
//   1. Every allocated data page (ids 1 .. next_page_id-1) sits on exactly
//      one list. Page 0 is on no list.
//   2. Membership is DERIVED, never stored: a page belongs on the full list
//      iff occupied_count() == capacity(). There is no membership bit — a
//      stored bit could drift silently, while the derived rule turns every
//      drift into a check_structure failure.
//   3. Both lists are appended at the TAIL, so each list's order reflects
//      allocation order: bulk loads scan in insertion order, and a page
//      emptied by erases is reused oldest-first.
//   4. Pages are allocated once and never removed. A page whose records
//      were all erased stays on the free list forever; the file never
//      shrinks. next_page_id only grows.
//
// ---------------------------------------------------------------------------
// The pin protocol
// ---------------------------------------------------------------------------
//
// Every disk access goes through the injected BufferManager. Plain row
// operations hold exactly ONE pinned page at a time: pin, act, unpin. List
// surgery (a page joining or moving between lists) transiently holds up to
// FIVE pins — the moving page plus the header, the page's two list
// neighbors, and the destination list's tail. The discipline:
//
//   1. Acquire ALL auxiliary pins before the first write, in ascending
//      page-id order. The set is always distinct (a page is on exactly one
//      list, so its neighbors and the destination tail cannot collide).
//   2. Once all pins are held, the surgery is pure memory writes — it
//      cannot fail mid-way, so a membership change is either fully applied
//      or not attempted.
//   3. Allocation happens only after the pins it needs are held.
//
// The session's pool of 8 frames is comfortable for this; a pool of 2 can
// fail where it used to succeed.
//
// ---------------------------------------------------------------------------
// Conventions
// ---------------------------------------------------------------------------
//
// Every method that can fail takes a trailing `std::string& error`, clears
// it on entry, and fills it with a named message exactly when it returns
// false. Never throw. On failure the file's contents are unchanged (either-
// or transitions, never half-applied ones). A closed file's methods fail
// with "heap file is not open".

#include "bufpool/BufferManager.h"
#include "heapfile/SlottedPage.h"
#include "schema/TableSchema.h"
#include "schema/TupleSerializer.h"
#include "tuple/Tuple.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace heapfile {

/// The address of one record in a heap file. A row id is a (page, slot)
/// pair: the page id is the block number (block 0 is the header page, so
/// record pages start at 1) and the slot is the record's index inside that
/// page's slot array. A row id is valid only while its record exists:
/// erasing frees the slot, and a later insert may hand out the same row id
/// again.
struct RowId {
    /// The block number of the page holding the record. 0 would name the
    /// header page and is never a valid record location.
    std::uint32_t page_id = 0;
    /// The record's slot index within its page (0-based, below the page's
    /// capacity).
    std::uint32_t slot = 0;
};

inline bool operator==(const RowId& a, const RowId& b) {
    return a.page_id == b.page_id && a.slot == b.slot;
}

inline bool operator!=(const RowId& a, const RowId& b) {
    return !(a == b);
}

class HeapFile;

/// Scan iterator over the records of one open HeapFile. Created by
/// HeapFile::scan(), it snapshots the two list heads at creation time and
/// afterwards walks the free list's pages in list order, then the full
/// list's pages in list order; within a page, slots are visited in slot
/// order and only occupied slots yield records. Once both lists are
/// exhausted, every further next() call keeps reporting exhaustion (false
/// with an empty error). The order is unspecified while the file is being
/// mutated.
class HeapFileIterator {
public:
    /// Reports the next record of the scan. On success returns true and
    /// fills `tuple` with the decoded row and row_id() with its address.
    /// On normal exhaustion returns false with `error` left EMPTY — the
    /// caller distinguishes end-of-scan from failure by testing `error`.
    /// On failure (a page that cannot be pinned or attached, a record that
    /// violates the schema, a cyclic list) returns false with `error`
    /// filled, and the iterator will keep reporting the same failure.
    bool next(tuple::Tuple& tuple, std::string& error);

    /// The row id of the record most recently reported by next(); valid
    /// only after a true return.
    const RowId& row_id() const { return current_; }

private:
    friend class HeapFile;
    HeapFileIterator(HeapFile& file, std::uint32_t free_head,
                     std::uint32_t full_head, std::uint32_t allocated);

    HeapFile* file_;          ///< the owning HeapFile (its manager, schema,
                              ///< serializer and file handle are used here)
    std::uint32_t page_id_;   ///< page currently being walked, or kNoPage
                              ///< when the current list is finished
    std::uint32_t next_list_; ///< head of the list still to be walked after
                              ///< the current one (kNoPage = none left)
    std::uint32_t slot_;      ///< next slot index to inspect on page_id_
    /// Cycle guard: seen_[p] marks pages the walk has already left. A page
    /// may be re-pinned while its slots are iterated, but once the walk
    /// advances past a page it can never legitimately return; seeing it
    /// again means the lists are cyclic and the scan must fail.
    std::vector<char> seen_;
    /// The page whose slot scan is in progress (kNoPage between pages), so
    /// re-entering a left page can be told apart from staying on one.
    std::uint32_t iterating_ = kNoPage;
    /// Row id of the last yielded record, returned by row_id().
    RowId current_;
};

/// A file of fixed-width tuple records stored in slotted pages organized
/// as two header-addressed free-space lists: pages with free space, and
/// pages whose every slot is used. See the file-level comment above for
/// the format, the list invariants, and the pin protocol.
///
/// The record layout is chosen by the schema handed to create/open: every
/// column occupies a fixed width (natural size for numerics, the
/// catalog-declared column_size for Strings), so one record is always
/// record_size(schema) bytes and a page holds slot_capacity(record_size)
/// of them.
///
/// Lifecycle: construct with the session's BufferManager, then create() or
/// open() one file at a time — a HeapFile manages one open file, and the
/// schema passed to open must outlive it. close() flushes and closes only
/// this file, leaving every other file the manager holds untouched. After
/// close (or before open) every operation fails with "heap file is not
/// open". The file reaches the disk only through the manager: dirty pages
/// become visible on disk at eviction or close, which is why a freshly
/// written page can be missing from the on-disk block count.
class HeapFile {
public:
    /// Binds the (initially closed) HeapFile to the session's buffer
    /// manager. Does not touch the disk; nothing is open until create()
    /// or open() succeeds.
    explicit HeapFile(bufman::BufferManager& manager);

    /// True exactly while a file is open through this HeapFile — from a
    /// successful create/open until a successful close.
    bool is_open() const;

    /// Creates a new one-block heap file (just the header page, carrying
    /// the identification record and the two empty lists) at `path`.
    /// Refuses a path that already holds a non-empty file, a schema with
    /// no record layout, and a record size that cannot fit a page; an
    /// existing EMPTY path is formatted in place. The file is left CLOSED
    /// (fully written back to disk); open() it to use it.
    bool create(const std::string& path, const schema::TableSchema& schema,
                std::string& error);

    /// Opens an existing heap file read-write, validating that block 0 is
    /// a tuple heap file whose identification names `schema`'s table, that
    /// the page layout matches the schema's record size, and that the
    /// header's list pointers are in range. A missing path is reported as
    /// such (the manager's open would otherwise create one). The schema
    /// must outlive the open file — only a pointer is kept.
    bool open(const std::string& path, const schema::TableSchema& schema,
              std::string& error);

    /// Starts a scan over all records: the free list's pages in list
    /// order, then the full list's pages in list order. The header page
    /// never holds occupied slots. The returned iterator snapshots the
    /// list heads now; the order is unspecified while the file is being
    /// mutated. A closed file yields an exhausted iterator whose next()
    /// reports "heap file is not open".
    HeapFileIterator scan();

    /// Validates `tuple` against the schema and stores it as one new
    /// record, reporting its address in `row_id`. Placement: the free
    /// list's HEAD page — the oldest page with a free slot, found without
    /// walking — and when the free list is empty, a freshly allocated page
    /// (id taken from the header's next_page_id, which then advances)
    /// appended to the free list. When the target page's LAST free slot is
    /// taken, the page moves to the full list. Serialization failures
    /// (wrong arity, type, or string width) fail before any page is
    /// pinned and change nothing.
    bool insert(const tuple::Tuple& tuple, RowId& row_id, std::string& error);

    /// Copies the record at `row_id` and unpacks it into `tuple`. Fails
    /// with a named error when the row id names the header page, a page
    /// beyond the end of the file, an out-of-range slot, a free slot, or a
    /// record that violates the schema.
    bool find(RowId row_id, tuple::Tuple& tuple, std::string& error);

    /// Packs `tuple` (validating it exactly like insert does) and
    /// overwrites the record at `row_id` in place. Records are all the
    /// same size and occupancy stays 1, so an update never changes how
    /// full the page is and NEVER moves the page between lists.
    bool update(RowId row_id, const tuple::Tuple& tuple, std::string& error);

    /// Clears the occupancy byte of `row_id`'s slot; the record bytes stay
    /// behind as garbage and the slot becomes reusable by later inserts.
    /// When the erase frees a slot on a page that WAS full, the page moves
    /// to the tail of the free list — so the oldest-emptied page is reused
    /// first. Fails (leaving the file unchanged) when the row id does not
    /// address an occupied record.
    bool erase(RowId row_id, std::string& error);

    /// True when `page_id` is currently resident in the file's pool — the
    /// introspection the CLI needs to report pool hits. This only
    /// observes; it never pins or reads.
    bool resident(std::uint32_t page_id) const;

    /// The structural self-check: walks both lists from the header, one
    /// pin at a time, and verifies the invariants — head/tail pointers
    /// agree with the walk (empty ⇔ kNoPage at both ends), every allocated
    /// page (1 .. next_page_id-1) is reachable exactly once (no cycles, no
    /// orphans, no page on both lists), no page contradicts its list's
    /// occupancy rule, page 0 is on no list, and every on-disk data page is
    /// reachable (a dirty page may not be on disk yet, so not the other
    /// way around). O(pages). A diagnostic only — never called by the
    /// storage operations themselves; the check suite runs it after every
    /// operation of the storage tests.
    bool check_structure(std::string& error);

    /// Flushes and closes only this file (BufferManager::close_file);
    /// every other file the manager holds stays open. The HeapFile's state
    /// is reset only when the manager actually released the file, so a
    /// failed close (a page still pinned) leaves the HeapFile open and the
    /// caller can retry. After a successful close every later operation
    /// fails with "heap file is not open".
    bool close(std::string& error);

private:
    friend class HeapFileIterator;

    /// Shared pin-and-validate path for find/update/erase: checks the row
    /// id against the header-page rule and the file's extent (a page
    /// exists iff it is resident in the pool or already written back),
    /// pins the page, attaches a SlottedPage view, and requires the slot
    /// occupied. On success the caller owns ONE pin and must unpin it; on
    /// failure the pin, if taken, is released here and false is returned
    /// with `error` filled.
    bool pin_record_page(RowId row_id, SlottedPage& page, std::size_t& frame,
                         std::string& error);

    // --- List surgery (the only writers of list pointers) -------------------
    // Every primitive acquires ALL auxiliary pins before its first write, in
    // ascending page-id order, so once the pins are held the surgery is pure
    // memory writes and cannot fail mid-way: a page's membership change is
    // either fully applied or not attempted. The moving page arrives pinned
    // (the caller keeps its pin through the call and releases it after).

    /// Appends a freshly allocated page (already pinned at `page_frame`,
    /// formatted with kNoPage links) to the free list as its only element,
    /// and advances the header's next_page_id past it. Only called right
    /// after an allocation, when the free list is empty by construction —
    /// so the header is the only auxiliary pin. Fails if the free list is
    /// not empty (an internal-consistency refusal, not a disk error).
    bool append_fresh_page(std::uint32_t page_id, std::size_t page_frame,
                           std::string& error);

    /// Moves page_id (pinned at `page_frame`) from one list to the other:
    /// to_full=true sends a just-filled page from the free list to the
    /// full list, to_full=false sends a just-unfilled page back. The page
    /// is detached from its source list (its prev/next name the source
    /// neighbors) and appended at the destination list's tail, under one
    /// pin acquisition (header + the two source neighbors + the
    /// destination tail, in ascending page-id order), so the transition
    /// never leaves the page on no list or on both. Reports every
    /// pin-acquisition failure and releases what it took.
    bool move_between_lists(bool to_full, std::uint32_t page_id,
                            std::size_t page_frame, std::string& error);

    /// Fills `error` with "heap file is not open" and returns false when
    /// the HeapFile is closed; returns true otherwise. The first line of
    /// every operation that requires an open file.
    bool require_open(std::string& error) const;

    bufman::BufferManager& manager_; ///< the session's buffer manager (every
                                     ///< disk access goes through it)
    bufman::File file_;              ///< the manager's handle for the open
                                     ///< file; reset when closed
    const schema::TableSchema* schema_ = nullptr; ///< the opened table's
                                     ///< schema; caller-owned and must
                                     ///< outlive the open file
    /// The record codec, bound to *schema_ at open; empty while closed.
    /// serialize() validates and packs a Tuple into a record, deserialize()
    /// unpacks a record back into a Tuple, record_size() is the fixed
    /// width every record occupies.
    std::optional<schema::TupleSerializer> serializer_;
    std::size_t record_size_ = 0;    ///< one record's width in bytes, from
                                     ///< the serializer; 0 while closed
    bool open_ = false;              ///< true from a successful create/open
                                     ///< until a successful close
};

/// Deletes the heap file at `path` from disk with the standard file-remove
/// call. Refuses while the file is open through `manager` — the registry
/// every HeapFile of the single-manager pattern reports to. Not a member:
/// it works on paths, so it is safe to call for files this HeapFile never
/// opened.
bool erase_file(bufman::BufferManager& manager, const std::string& path,
                std::string& error);

}

#endif // EX11_HEAP_FILE_H
