// memory_simulator_advanced.cpp
// CS375 Lab 9 - Advanced Segmented, Paged Memory Model Simulator
//               with TLB, Multi-Level Paging and Dynamic Memory Management
// Author: Mali Glemaud-Thesee
//
// Contributions (completed and fixed from the starter code):
//  - Two-level paging: each segment has a page directory; each directory entry
//    points to a page table of PAGES_PER_TABLE pages, created on first use.
//    dirNum and pageNum are validated ("Invalid dirNum 5, max 3").
//  - TLB: configurable size (--tlb), LRU order, hit/miss counters, hit rate,
//    stores page protection so a TLB hit cannot bypass a read-only page, and
//    entries are invalidated when their page is evicted or its segment removed.
//  - Page replacement: LRU (last-use timestamps) or FIFO (--replace lru|fifo).
//    The evicted page is marked not present and removed from the TLB.
//  - Exceptions: SegmentationFault, PageFault, ProtectionViolation, OffsetFault.
//    A PageFault is thrown by the page table and handled by loading the page.
//  - Dynamic segments: add/remove at runtime; remove frees every frame, flushes
//    the segment's TLB entries and deletes its page tables (no leaks).
//  - Batch file (batch_results.txt), 1000-address stress test with a valid
//    ratio (random_results.txt), translation timeline (memory_log.txt),
//    per-segment page-fault statistics with resize suggestions, memory map
//    with free/allocated frames, simulated latency (1-10 units + walk + fault),
//    configuration file, and an --experiment mode for the report.
//
// Bugs fixed in the starter: missing headers (<queue>, <algorithm>, <chrono>);
// out-of-bounds write for an invalid segment number; segment IDs were vector
// positions (add 3 after 0 and 1 became index 2); unlimited dirNum created page
// tables on demand (huge or negative dirNum crashed); --pagesize was ignored;
// initially present pages used random frames 0-99 never taken from physical
// memory; a page fault overwrote the page's protection with the access type;
// replaced pages stayed "present" (two pages shared a frame) and stayed in the
// TLB; removing a segment left its TLB entries (stale translations); TLB put()
// of an existing key duplicated it; gen() < ratio * RAND_MAX made the valid ratio
// meaningless; argument parsing assumed every flag had a value.
//
// Build:  g++ -std=c++17 -O2 -o memory_simulator_advanced memory_simulator_advanced.cpp
// Run:    ./memory_simulator_advanced                  (prompts, then commands)
//         ./memory_simulator_advanced --help

#include <iostream>
#include <iomanip>
#include <sstream>
#include <vector>
#include <map>
#include <list>
#include <queue>
#include <unordered_map>
#include <algorithm>
#include <cstdlib>
#include <ctime>
#include <chrono>
#include <fstream>
#include <string>
#include <random>
#include <stdexcept>
#include <memory>
#include <unistd.h>

enum Protection { READ_ONLY, READ_WRITE };
enum Access { READ, WRITE };

const char* protName(Protection p) { return p == READ_ONLY ? "RO" : "RW"; }

// Latency model (units): every translation costs random 1-10; a TLB miss adds a
// walk of both page-table levels; a page fault adds the cost of loading the page.
const int WALK_COST = 2 * 5;
const int FAULT_COST = 50;

// ============================================================================
// Exceptions
// ============================================================================
class MemoryException : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
    virtual std::string kind() const = 0;
};
class SegmentationFault : public MemoryException {
public:
    using MemoryException::MemoryException;
    std::string kind() const override { return "Segmentation fault"; }
};
class ProtectionViolation : public MemoryException {
public:
    using MemoryException::MemoryException;
    std::string kind() const override { return "Protection violation"; }
};
class OffsetFault : public MemoryException {
public:
    using MemoryException::MemoryException;
    std::string kind() const override { return "Offset fault"; }
};
// Thrown by the page table when the present bit is 0; translateAddress catches
// it and services the fault (like a hardware trap into the OS).
class PageFault : public MemoryException {
public:
    using MemoryException::MemoryException;
    std::string kind() const override { return "Page fault"; }
};

// ============================================================================
// Part 1: data structures
// ============================================================================
struct Page {
    int frame_number = -1;  // -1 if not in memory
    bool present = false;
    Protection protection = READ_WRITE;
    int last_access = 0;    // for LRU
};

struct Segment {
    int id = 0;
    int base_address = 0;   // where the segment's page directory lives
    int limit = 0;          // number of pages
    Protection protection = READ_WRITE;
    int fault_count = 0;    // page faults in this segment
    int error_count = 0;    // segmentation/offset/protection errors
    int accesses = 0;       // successful translations (for the page-fault rate)
};

struct FrameOwner { int seg = -1, dir = -1, page = -1; };

class PhysicalMemory {
public:
    int num_frames;
    std::vector<bool> free_frames;
    std::queue<int> fifo_queue;         // FIFO: load order
    std::vector<int> last_use;          // LRU: last access time of each frame
    std::vector<FrameOwner> owner;      // which page is in each frame
    bool use_lru;

    PhysicalMemory(int frames, bool lru)
        : num_frames(frames), free_frames(frames, true), last_use(frames, 0), owner(frames), use_lru(lru) {}

    // Returns a frame for `who`. If memory is full, a victim is chosen (LRU or
    // FIFO) and its previous owner is returned in `evicted`.
    int allocateFrame(FrameOwner who, int time, FrameOwner& evicted) {
        evicted = FrameOwner{};
        int frame = -1;
        for (int i = 0; i < num_frames; ++i)
            if (free_frames[i]) { frame = i; break; }
        if (frame == -1) {
            if (use_lru) {
                frame = std::min_element(last_use.begin(), last_use.end()) - last_use.begin();
                removeFromFifo(frame);
            } else {
                frame = fifo_queue.front();
                fifo_queue.pop();
            }
            evicted = owner[frame];
        }
        free_frames[frame] = false;
        fifo_queue.push(frame);
        owner[frame] = who;
        last_use[frame] = time;
        return frame;
    }

    void freeFrame(int frame) {
        if (frame < 0 || frame >= num_frames || free_frames[frame]) return;
        free_frames[frame] = true;
        owner[frame] = FrameOwner{};
        last_use[frame] = 0;
        removeFromFifo(frame);
    }

    void touch(int frame, int time) { last_use[frame] = time; }

    int used() const { return std::count(free_frames.begin(), free_frames.end(), false); }
    double utilization() const { return (double)used() / num_frames * 100; }

private:
    void removeFromFifo(int frame) {
        std::queue<int> temp;
        while (!fifo_queue.empty()) {
            if (fifo_queue.front() != frame) temp.push(fifo_queue.front());
            fifo_queue.pop();
        }
        fifo_queue = temp;
    }
};

// Second level: one page table per directory entry.
class PageTable {
public:
    std::vector<Page> pages;
    int page_size;
    int directory_index;

    PageTable(int numPages, int pageSize, int dirIdx) : pages(numPages), page_size(pageSize), directory_index(dirIdx) {}

    // Throws PageFault when the page is not in memory.
    int getFrameNumber(int pageNum) const {
        const Page& p = pages.at(pageNum);
        if (!p.present)
            throw PageFault("page " + std::to_string(pageNum) + " of directory " + std::to_string(directory_index) +
                            " not present");
        return p.frame_number;
    }
    void setFrame(int pageNum, int frame) {   // protection is never changed here
        pages[pageNum].frame_number = frame;
        pages[pageNum].present = true;
    }
    // The page lost its frame to replacement (or its segment is going away).
    void replacePage(int pageNum) {
        pages[pageNum].frame_number = -1;
        pages[pageNum].present = false;
    }
};

// First level: directory index -> page table (allocated on demand).
class DirectoryTable {
public:
    std::vector<PageTable*> pageTables;     // nullptr = not allocated yet
    std::vector<Protection> page_protection;
    int pages_per_table, page_size, limit;

    DirectoryTable(int limitPages, int pagesPerTable, int pageSize)
        : pageTables((limitPages + pagesPerTable - 1) / pagesPerTable, nullptr),
          page_protection(limitPages, READ_WRITE), pages_per_table(pagesPerTable), page_size(pageSize),
          limit(limitPages) {}
    ~DirectoryTable() { freeTables(); }
    DirectoryTable(const DirectoryTable&) = delete;
    DirectoryTable& operator=(const DirectoryTable&) = delete;

    int numDirs() const { return pageTables.size(); }
    int pagesInDir(int dir) const { return std::min(pages_per_table, limit - dir * pages_per_table); }
    PageTable* getPageTable(int dir) const { return pageTables.at(dir); }

    PageTable* allocatePageTable(int dir) {
        if (!pageTables[dir]) {
            pageTables[dir] = new PageTable(pagesInDir(dir), page_size, dir);
            for (int p = 0; p < pagesInDir(dir); ++p)
                pageTables[dir]->pages[p].protection = page_protection[dir * pages_per_table + p];
        }
        return pageTables[dir];
    }
    int allocatedTables() const {
        return std::count_if(pageTables.begin(), pageTables.end(), [](PageTable* p) { return p != nullptr; });
    }
    void freeTables() {
        for (auto*& pt : pageTables) { delete pt; pt = nullptr; }
    }
};

class TLB {
public:
    struct Entry { int frame; Protection prot; };
private:
    std::unordered_map<std::string, Entry> cache;   // key "seg:dir:page"
    std::list<std::string> lruOrder;                // front = least recently used
    int maxSize;
    long hits = 0, misses = 0;
    static std::string key(int s, int d, int p) { return std::to_string(s) + ":" + std::to_string(d) + ":" + std::to_string(p); }

public:
    explicit TLB(int size) : maxSize(size) {}

    bool get(int segNum, int dirNum, int pageNum, Entry& out) {
        std::string k = key(segNum, dirNum, pageNum);
        auto it = cache.find(k);
        if (it == cache.end()) { misses++; return false; }
        hits++;
        lruOrder.remove(k);                 // most recently used -> back
        lruOrder.push_back(k);
        out = it->second;
        return true;
    }

    void put(int segNum, int dirNum, int pageNum, int frame, Protection prot) {
        std::string k = key(segNum, dirNum, pageNum);
        if (cache.count(k)) lruOrder.remove(k);           // update, no duplicate
        else if ((int)cache.size() >= maxSize) {          // evict least recently used
            cache.erase(lruOrder.front());
            lruOrder.pop_front();
        }
        cache[k] = {frame, prot};
        lruOrder.push_back(k);
    }

    void invalidate(int segNum, int dirNum, int pageNum) {
        std::string k = key(segNum, dirNum, pageNum);
        if (cache.erase(k)) lruOrder.remove(k);
    }
    int invalidateSegment(int segNum) {
        std::string prefix = std::to_string(segNum) + ":";
        int n = 0;
        for (auto it = lruOrder.begin(); it != lruOrder.end();) {
            if (it->rfind(prefix, 0) == 0) { cache.erase(*it); it = lruOrder.erase(it); n++; }
            else ++it;
        }
        return n;
    }

    long getHits() const { return hits; }
    long getMisses() const { return misses; }
    int size() const { return maxSize; }
    double hitRate() const { return hits + misses > 0 ? (double)hits / (hits + misses) * 100 : 0; }

    void displayCache(std::ostream& out) const {
        out << "TLB Contents (LRU first, " << cache.size() << "/" << maxSize << " entries):\n";
        for (const auto& k : lruOrder)
            out << "  " << k << " -> Frame " << cache.at(k).frame << " (" << protName(cache.at(k).prot) << ")\n";
        out << std::fixed << std::setprecision(2) << "TLB Hit Rate: " << hitRate() << "% (" << hits << " hits, "
            << misses << " misses)\n";
    }
};

// Result of one translation (errors are thrown instead).
struct Translation {
    long physical = -1;
    bool tlb_hit = false;
    bool page_fault = false;
    std::string victim;          // "S1.D0.P2" if a page was evicted
    int latency = 0;
};

struct Metrics {
    long translations = 0, successes = 0, page_faults = 0, evictions = 0;
    long seg_faults = 0, offset_faults = 0, protection_violations = 0;
    long total_latency = 0, hit_latency = 0, miss_latency = 0, hit_count = 0, miss_count = 0;
};

class SegmentTable {
private:
    std::map<int, Segment> segments;                 // keyed by segment ID
    std::map<int, DirectoryTable*> directoryTables;
    TLB tlb;
    PhysicalMemory physMem;
    int page_size, pages_per_table;
    int time = 0;
    std::mt19937 rng;

public:
    Metrics m;

    SegmentTable(int tlbSize, int numFrames, bool use_lru, int pageSize, int pagesPerTable, unsigned seed)
        : tlb(tlbSize), physMem(numFrames, use_lru), page_size(pageSize), pages_per_table(pagesPerTable), rng(seed) {}

    ~SegmentTable() {
        for (auto& pair : directoryTables) delete pair.second;
    }
    SegmentTable(const SegmentTable&) = delete;
    SegmentTable& operator=(const SegmentTable&) = delete;

    int pageSize() const { return page_size; }
    int pagesPerTable() const { return pages_per_table; }
    bool usesLRU() const { return physMem.use_lru; }
    const TLB& getTLB() const { return tlb; }
    std::vector<int> segmentIds() const {
        std::vector<int> ids;
        for (auto& [id, s] : segments) ids.push_back(id);
        return ids;
    }
    const Segment& segment(int id) const { return segments.at(id); }
    int numDirs(int id) const { return directoryTables.at(id)->numDirs(); }

    void addSegment(int id, int base, int limit, Protection prot, const std::vector<Protection>& pageProt = {}) {
        if (id < 0) throw std::invalid_argument("Segment ID must be >= 0");
        if (segments.count(id)) throw std::invalid_argument("Segment " + std::to_string(id) + " already exists");
        if (limit < 1 || limit > 4096) throw std::invalid_argument("Segment limit must be 1-4096 pages");
        if (base < 0) throw std::invalid_argument("Base address must be >= 0");
        Segment s;
        s.id = id; s.base_address = base; s.limit = limit; s.protection = prot;
        segments[id] = s;
        directoryTables[id] = new DirectoryTable(limit, pages_per_table, page_size);
        for (size_t p = 0; p < pageProt.size() && (int)p < limit; ++p)
            directoryTables[id]->page_protection[p] = pageProt[p];
    }

    // Frees every frame of the segment, flushes its TLB entries, deletes its tables.
    int removeSegment(int id) {
        if (!segments.count(id)) throw std::invalid_argument("Cannot remove invalid segment " + std::to_string(id));
        int freed = 0;
        for (auto* pt : directoryTables[id]->pageTables) {
            if (!pt) continue;
            for (auto& page : pt->pages)
                if (page.present) { physMem.freeFrame(page.frame_number); freed++; }
        }
        tlb.invalidateSegment(id);
        delete directoryTables[id];
        directoryTables.erase(id);
        segments.erase(id);
        return freed;
    }

    // Load page (seg, global page index) into memory at startup.
    void preload(int id, int globalPage) {
        int dir = globalPage / pages_per_table, page = globalPage % pages_per_table;
        PageTable* pt = directoryTables.at(id)->allocatePageTable(dir);
        if (pt->pages[page].present) return;
        loadPage(id, dir, page, pt);
    }
    int freeFrames() const { return physMem.num_frames - physMem.used(); }
    double utilization() const { return physMem.utilization(); }

    Translation translateAddress(int segNum, int dirNum, int pageNum, int offset, Access access) {
        Translation t;
        t.latency = 1 + rng() % 10;
        time++;
        m.translations++;
        m.total_latency += t.latency;

        auto it = segments.find(segNum);
        if (it == segments.end()) {
            m.seg_faults++;
            throw SegmentationFault("Invalid segment " + std::to_string(segNum) + " (segments: " + idList() + ")");
        }
        Segment& seg = it->second;
        DirectoryTable* dt = directoryTables[segNum];
        auto error = [&](auto ex, long& counter) {
            counter++;
            seg.error_count++;
            throw ex;
        };
        if (dirNum < 0 || dirNum >= dt->numDirs())
            error(SegmentationFault("Invalid dirNum " + std::to_string(dirNum) + ", max " + std::to_string(dt->numDirs() - 1) +
                                    " for segment " + std::to_string(segNum)), m.seg_faults);
        if (pageNum < 0 || pageNum >= dt->pagesInDir(dirNum))
            error(SegmentationFault("Invalid pageNum " + std::to_string(pageNum) + " in directory " + std::to_string(dirNum) +
                                    ", max " + std::to_string(dt->pagesInDir(dirNum) - 1) + " (segment " +
                                    std::to_string(segNum) + " limit " + std::to_string(seg.limit) + " pages)"),
                  m.seg_faults);
        if (offset < 0 || offset >= page_size)
            error(OffsetFault("Offset " + std::to_string(offset) + " outside page size " + std::to_string(page_size)),
                  m.offset_faults);
        if (access == WRITE && seg.protection == READ_ONLY)
            error(ProtectionViolation("Write to read-only segment " + std::to_string(segNum) + " (page " +
                                      std::to_string(dirNum) + ":" + std::to_string(pageNum) + ")"),
                  m.protection_violations);

        int frame;
        TLB::Entry e;
        if (tlb.get(segNum, dirNum, pageNum, e)) {              // 1. TLB hit
            if (access == WRITE && e.prot == READ_ONLY)
                error(ProtectionViolation("Write to read-only page " + std::to_string(dirNum) + ":" + std::to_string(pageNum) +
                                          " of segment " + std::to_string(segNum) + " (found in TLB)"),
                      m.protection_violations);
            t.tlb_hit = true;
            frame = e.frame;
        } else {                                                // 2. walk both levels
            t.latency += WALK_COST;
            m.total_latency += WALK_COST;
            PageTable* pt = dt->allocatePageTable(dirNum);
            Page& page = pt->pages[pageNum];
            if (access == WRITE && page.protection == READ_ONLY)
                error(ProtectionViolation("Write to read-only page " + std::to_string(dirNum) + ":" + std::to_string(pageNum) +
                                          " of segment " + std::to_string(segNum)),
                      m.protection_violations);
            try {
                frame = pt->getFrameNumber(pageNum);
            } catch (const PageFault&) {                        // 3. service the page fault
                t.page_fault = true;
                t.victim = loadPage(segNum, dirNum, pageNum, pt);
                frame = page.frame_number;
                seg.fault_count++;
                m.page_faults++;
                t.latency += FAULT_COST;
                m.total_latency += FAULT_COST;
            }
            tlb.put(segNum, dirNum, pageNum, frame, page.protection);   // 4. update TLB
        }
        directoryTables[segNum]->getPageTable(dirNum)->pages[pageNum].last_access = time;
        physMem.touch(frame, time);
        seg.accesses++;                                         // valid accesses only
        if (t.tlb_hit) { m.hit_latency += t.latency; m.hit_count++; }
        else { m.miss_latency += t.latency; m.miss_count++; }
        m.successes++;
        t.physical = (long)frame * page_size + offset;
        return t;
    }

    // ---------------- output ----------------
    void printMemoryMap(std::ostream& out) const {
        out << "---------------- Memory Map ----------------\n";
        for (auto& [id, s] : segments) {
            DirectoryTable* dt = directoryTables.at(id);
            out << "Segment " << id << ": base=" << s.base_address << " limit=" << s.limit << " pages " << protName(s.protection)
                << "  page faults=" << s.fault_count << "  directory: " << dt->allocatedTables() << "/" << dt->numDirs()
                << " page tables\n";
            for (int d = 0; d < dt->numDirs(); ++d) {
                out << "  Dir " << d << ": ";
                PageTable* pt = dt->getPageTable(d);
                if (!pt) { out << "(not allocated)\n"; continue; }
                for (int p = 0; p < (int)pt->pages.size(); ++p) {
                    const Page& pg = pt->pages[p];
                    out << "P" << p << "[" << (pg.present ? "F" + std::to_string(pg.frame_number) : "--") << " "
                        << protName(pg.protection) << "] ";
                }
                out << "\n";
            }
        }
        out << "Frames: ";
        for (int f = 0; f < physMem.num_frames; ++f) {
            const FrameOwner& o = physMem.owner[f];
            out << "|" << f << ":" << (physMem.free_frames[f] ? std::string("FREE")
                                                              : "S" + std::to_string(o.seg) + "D" + std::to_string(o.dir) +
                                                                    "P" + std::to_string(o.page));
            if (f % 8 == 7 && f + 1 < physMem.num_frames) out << "|\n        ";
        }
        out << "|\n" << std::fixed << std::setprecision(1) << "Allocated " << physMem.used() << "/" << physMem.num_frames
            << " frames (" << physMem.utilization() << "%), free " << freeFrames() << "\n";
    }

    void displayStats(std::ostream& out) const {
        out << "---------------- Statistics (" << (physMem.use_lru ? "LRU" : "FIFO") << ", TLB " << tlb.size()
            << ") ----------------\n" << std::fixed << std::setprecision(2);
        out << "Page faults per segment:\n";
        for (auto& [id, s] : segments) {
            double rate = s.accesses ? 100.0 * s.fault_count / s.accesses : 0;
            out << "  Segment " << id << ": " << s.fault_count << " page faults / " << s.accesses << " valid accesses ("
                << rate << "%), " << s.error_count << " errors\n";
            if (s.accesses >= 10 && rate > 20)
                out << "    Suggestion: page-fault rate > 20% -> increase limit/frame share of segment " << id << "\n";
            else if (s.accesses >= 10 && rate < 2 && s.limit > 1)
                out << "    Suggestion: page-fault rate < 2% -> segment " << id << " could give up frames\n";
        }
        long valid = m.successes;
        out << "Translations: " << m.translations << " (" << valid << " successful, " << m.seg_faults
            << " segmentation faults, " << m.offset_faults << " offset faults, " << m.protection_violations
            << " protection violations)\n";
        out << "Page Fault Rate: " << (m.translations ? 100.0 * m.page_faults / m.translations : 0) << "% ("
            << m.page_faults << " page faults, " << physMem.num_frames << " frames)\n";
        out << "TLB Hit Rate: " << tlb.hitRate() << "%\n";
        out << "Physical Memory Utilization: " << std::setprecision(1) << physMem.utilization() << "%\n";
        out << std::setprecision(2) << "Average Translation Latency: "
            << (m.translations ? (double)m.total_latency / m.translations : 0) << " units";
        if (m.hit_count && m.miss_count)
            out << " (TLB hit " << (double)m.hit_latency / m.hit_count << ", TLB miss "
                << (double)m.miss_latency / m.miss_count << ")";
        out << "\n";
    }

private:
    std::string idList() const {
        std::string s;
        for (auto& [id, seg] : segments) s += (s.empty() ? "" : ",") + std::to_string(id);
        return s.empty() ? "none" : s;
    }

    // Puts a page in a frame; if a victim was evicted, invalidates it everywhere.
    std::string loadPage(int segNum, int dirNum, int pageNum, PageTable* pt) {
        FrameOwner evicted;
        int frame = physMem.allocateFrame({segNum, dirNum, pageNum}, time, evicted);
        std::string victim;
        if (evicted.seg != -1) {
            directoryTables[evicted.seg]->getPageTable(evicted.dir)->replacePage(evicted.page);
            tlb.invalidate(evicted.seg, evicted.dir, evicted.page);
            m.evictions++;
            victim = "S" + std::to_string(evicted.seg) + ".D" + std::to_string(evicted.dir) + ".P" + std::to_string(evicted.page);
        }
        pt->setFrame(pageNum, frame);
        return victim;
    }
};

// ============================================================================
// Translation with output/logging
// ============================================================================
std::string addrText(int s, int d, int p, int o, Access a) {
    std::ostringstream ss;
    ss << "(" << s << "," << d << "," << p << "," << o << "," << (a == READ ? "R" : "W") << ")";
    return ss.str();
}

// Translates one address and writes a timeline line ("Time X: Address Y -> Z").
// Returns true if the translation succeeded.
bool translateAndReport(SegmentTable& st, int s, int d, int p, int o, Access a, std::ostream& out, std::ostream* log) {
    std::ostringstream line;
    bool ok = false;
    try {
        Translation t = st.translateAddress(s, d, p, o, a);
        line << "-> physical " << t.physical << (t.tlb_hit ? "  TLB hit" : "  TLB miss");
        if (t.page_fault) line << "  PAGE FAULT" << (t.victim.empty() ? " (free frame)" : " (replaced " + t.victim + ")");
        line << "  latency " << t.latency;
        ok = true;
    } catch (const MemoryException& e) {
        line << "ERROR " << e.kind() << ": " << e.what();
    }
    std::ostringstream full;
    full << "Time " << std::setw(4) << st.m.translations << ": " << std::left << std::setw(18) << addrText(s, d, p, o, a)
         << std::right << line.str() << "\n";
    out << full.str();
    if (log) *log << full.str();
    return ok;
}

// ============================================================================
// Batch, stress test
// ============================================================================
void processBatchFile(SegmentTable& st, const std::string& filename, std::ostream* log) {
    std::ifstream file(filename);
    if (!file) throw std::invalid_argument("Cannot open batch file " + filename);
    std::ofstream out("batch_results.txt");
    std::string line;
    int ok = 0, total = 0;
    long faults_before = st.m.page_faults;
    while (std::getline(file, line)) {
        line = line.substr(0, line.find('#'));
        std::istringstream ss(line);
        int s, d, p, o;
        std::string a;
        if (!(ss >> s >> d >> p >> o >> a)) continue;
        Access acc = (a == "1" || a == "W" || a == "w") ? WRITE : READ;
        if (translateAndReport(st, s, d, p, o, acc, out, log)) ok++;
        total++;
    }
    out << "Batch summary: " << ok << "/" << total << " successful, " << total - ok << " errors, "
        << st.m.page_faults - faults_before << " page faults\n";
}

// num random addresses; with probability validRatio every field is valid,
// otherwise exactly one field is made invalid.
void generateRandomAddresses(SegmentTable& st, int num, double validRatio, const std::string& logFile, std::mt19937& gen,
                             std::ostream* log) {
    std::ofstream out(logFile);
    std::vector<int> ids = st.segmentIds();
    if (ids.empty()) throw std::invalid_argument("No segments");
    std::uniform_real_distribution<double> coin(0, 1);
    Metrics before = st.m;
    long hits_before = st.getTLB().getHits(), lookups_before = st.getTLB().getHits() + st.getTLB().getMisses();
    for (int i = 0; i < num; ++i) {
        int s = ids[gen() % ids.size()];
        const Segment& seg = st.segment(s);
        int global = gen() % seg.limit;
        int d = global / st.pagesPerTable(), p = global % st.pagesPerTable(), o = gen() % st.pageSize();
        Access a = coin(gen) < 0.7 ? READ : WRITE;
        if (coin(gen) >= validRatio) {
            switch (gen() % 4) {
                case 0: s = *std::max_element(ids.begin(), ids.end()) + 1 + gen() % 3; break;
                case 1: d = st.numDirs(s) + gen() % 3; break;
                case 2: p = st.pagesPerTable() + gen() % 3; break;
                default: o = st.pageSize() + gen() % 500; break;
            }
        }
        translateAndReport(st, s, d, p, o, a, out, log);
    }
    Metrics& m = st.m;
    long t = m.translations - before.translations;
    long lookups = st.getTLB().getHits() + st.getTLB().getMisses() - lookups_before;
    out << std::fixed << std::setprecision(2) << "Summary: " << num << " addresses, valid ratio " << validRatio << ": "
        << m.successes - before.successes << " successful, " << m.seg_faults - before.seg_faults << " segmentation faults, "
        << m.offset_faults - before.offset_faults << " offset faults, "
        << m.protection_violations - before.protection_violations << " protection violations\n"
        << "Page Fault Rate: " << 100.0 * (m.page_faults - before.page_faults) / t << "% ("
        << m.page_faults - before.page_faults << " page faults)  TLB Hit Rate: "
        << (lookups ? 100.0 * (st.getTLB().getHits() - hits_before) / lookups : 0) << "%  Avg latency: "
        << (double)(m.total_latency - before.total_latency) / t << "\n";
}

// ============================================================================
// Setup
// ============================================================================
struct Config {
    int frames = 10, tlb = 4, page_size = 1000, ppt = 4, segments = 3;
    bool lru = true;
    unsigned seed = 0;
    bool seeded = false;
};

std::string validate(const Config& c) {
    if (c.frames < 1 || c.frames > 4096) return "frames must be 1-4096";
    if (c.tlb < 1 || c.tlb > 1024) return "TLB size must be 1-1024";
    if (c.page_size < 16 || c.page_size > 65536) return "page size must be 16-65536";
    if (c.ppt < 1 || c.ppt > 1024) return "pages per table must be 1-1024";
    if (c.segments < 0 || c.segments > 64) return "segments must be 0-64";
    return "";
}

// Random limits (3-8 pages) and protections; about half the pages start in memory.
void randomSegments(SegmentTable& st, int count, std::mt19937& gen) {
    for (int id = 0; id < count; ++id) {
        int limit = 3 + gen() % 6;
        Protection prot = gen() % 4 == 0 ? READ_ONLY : READ_WRITE;
        std::vector<Protection> pp(limit);
        for (auto& p : pp) p = gen() % 4 == 0 ? READ_ONLY : READ_WRITE;
        st.addSegment(id, id * 10000, limit, prot, pp);
    }
    for (int id = 0; id < count; ++id)
        for (int p = 0; p < st.segment(id).limit; ++p)
            if (gen() % 2 && st.freeFrames() > 0) st.preload(id, p);
}

// Config file: "frames N", "pagesize N", "tlb N", "ppt N", "replace lru|fifo", then
// one segment per line: segID base limit RO|RW [page protections; '+' = loaded at start]
std::unique_ptr<SegmentTable> loadConfig(const std::string& file, Config& c) {
    std::ifstream in(file);
    if (!in) throw std::invalid_argument("Cannot open config file " + file);
    std::vector<std::string> segLines;
    std::string line;
    while (std::getline(in, line)) {
        line = line.substr(0, line.find('#'));
        std::istringstream ss(line);
        std::string key;
        if (!(ss >> key)) continue;
        if (key == "frames") ss >> c.frames;
        else if (key == "pagesize") ss >> c.page_size;
        else if (key == "tlb") ss >> c.tlb;
        else if (key == "ppt") ss >> c.ppt;
        else if (key == "replace") { std::string p; ss >> p; c.lru = (p == "lru"); }
        else segLines.push_back(line);
    }
    std::string err = validate(c);
    if (!err.empty()) throw std::invalid_argument("Invalid configuration: " + err);
    auto st = std::make_unique<SegmentTable>(c.tlb, c.frames, c.lru, c.page_size, c.ppt, c.seed);
    int preload = 0;
    for (auto& l : segLines) {
        std::istringstream ss(l);
        int id, base, limit;
        std::string prot, p;
        if (!(ss >> id >> base >> limit >> prot) || (prot != "RO" && prot != "RW"))
            throw std::invalid_argument("Invalid segment line: " + l);
        std::vector<Protection> pp;
        std::vector<int> load;
        while (ss >> p) {
            if (p.substr(0, 2) != "RO" && p.substr(0, 2) != "RW") throw std::invalid_argument("Invalid page protection '" + p + "'");
            if (p.back() == '+') load.push_back(pp.size());
            pp.push_back(p.substr(0, 2) == "RO" ? READ_ONLY : READ_WRITE);
        }
        if ((int)pp.size() > limit) throw std::invalid_argument("More page entries than the limit in: " + l);
        st->addSegment(id, base, limit, prot == "RO" ? READ_ONLY : READ_WRITE, pp);
        for (int g : load) {
            if (++preload > c.frames) throw std::invalid_argument("Invalid configuration: more preloaded pages than frames");
            st->preload(id, g);
        }
    }
    return st;
}

// ============================================================================
// Experiments (for the report)
// ============================================================================
struct Access4 { int seg, dir, page, off; Access acc; };

// Valid accesses with locality: 80% go to the first 30% of each segment's pages.
std::vector<Access4> localityWorkload(const SegmentTable& st, int n, unsigned seed) {
    std::mt19937 g(seed);
    std::vector<Access4> w;
    auto ids = st.segmentIds();
    for (int i = 0; i < n; ++i) {
        int s = ids[g() % ids.size()];
        int limit = st.segment(s).limit;
        int hot = std::max(1, limit * 3 / 10);
        int page = (g() % 10 < 8) ? g() % hot : g() % limit;
        w.push_back({s, page / st.pagesPerTable(), page % st.pagesPerTable(), (int)(g() % st.pageSize()), READ});
    }
    return w;
}

std::unique_ptr<SegmentTable> experimentTable(const Config& c, int frames, int tlb, bool lru) {
    auto st = std::make_unique<SegmentTable>(tlb, frames, lru, c.page_size, c.ppt, c.seed);
    st->addSegment(0, 0, 8, READ_WRITE);
    st->addSegment(1, 10000, 6, READ_ONLY);
    st->addSegment(2, 20000, 10, READ_WRITE);
    return st;
}

void runWorkload(SegmentTable& st, const std::vector<Access4>& w) {
    for (auto& a : w) {
        try { st.translateAddress(a.seg, a.dir, a.page, a.off, a.acc); } catch (const MemoryException&) {}
    }
}

void experiments(Config c) {
    std::cout << std::fixed << std::setprecision(2);
    std::cout << "Segments: 0 (8 pages), 1 (6 pages, RO), 2 (10 pages) = 24 pages; page size " << c.page_size
              << ", 4 pages per table, seed " << c.seed << "\n\n";

    std::cout << "Experiment 1: LRU vs FIFO, same 2000-access workload, TLB 4\n";
    std::cout << "  Workload    Frames  Policy   Page faults  Fault rate  TLB hit  Avg latency\n";
    for (int loc = 1; loc >= 0; --loc)
        for (int frames : {6, 10, 16})
            for (bool lru : {true, false}) {
                auto st = experimentTable(c, frames, 4, lru);
                std::vector<Access4> w;
                if (loc) w = localityWorkload(*st, 2000, c.seed + 1);
                else {
                    std::mt19937 g(c.seed + 2);
                    for (int i = 0; i < 2000; ++i) {
                        int s = g() % 3, lim = st->segment(s).limit, p = g() % lim;
                        w.push_back({s, p / c.ppt, p % c.ppt, (int)(g() % c.page_size), READ});
                    }
                }
                runWorkload(*st, w);
                std::cout << "  " << std::left << std::setw(11) << (loc ? "locality" : "uniform") << std::right << std::setw(6)
                          << frames << "  " << std::left << std::setw(6) << (lru ? "LRU" : "FIFO") << std::right
                          << std::setw(13) << st->m.page_faults << std::setw(11) << 100.0 * st->m.page_faults / 2000 << "%"
                          << std::setw(8) << st->getTLB().hitRate() << "%" << std::setw(13)
                          << (double)st->m.total_latency / st->m.translations << "\n";
            }

    std::cout << "\nExperiment 2: TLB size (locality workload, 10 frames, LRU)\n";
    std::cout << "  TLB entries  Hit rate  Avg latency  Hit latency  Miss latency  TLB storage\n";
    for (int tlb : {4, 8, 16, 32}) {
        auto st = experimentTable(c, 10, tlb, true);
        runWorkload(*st, localityWorkload(*st, 2000, c.seed + 1));
        std::cout << std::setw(13) << tlb << std::setw(9) << st->getTLB().hitRate() << "%" << std::setw(13)
                  << (double)st->m.total_latency / st->m.translations << std::setw(13)
                  << (double)st->m.hit_latency / std::max(1L, st->m.hit_count) << std::setw(14)
                  << (double)st->m.miss_latency / std::max(1L, st->m.miss_count) << std::setw(10) << tlb * 8 << " B\n";
    }

    std::cout << "\nExperiment 3: page size 1000 vs 4000 (3 segments of 25,500 bytes, 48,000 bytes of physical memory,\n"
              << "              same 4000 byte-address accesses with locality, TLB 4, LRU)\n";
    std::cout << "  Page size  Pages/seg  Frames  Dirs/seg  Page faults  Fault rate  TLB hit  Util.  Wasted bytes\n";
    std::vector<std::pair<int, int>> bytes;   // (segment, byte offset)
    {
        std::mt19937 g(c.seed + 3);
        for (int i = 0; i < 4000; ++i) {
            int s = g() % 3;
            int b = (g() % 10 < 8) ? g() % 7650 : g() % 25500;   // 80% in the first 30% of the segment
            bytes.push_back({s, b});
        }
    }
    for (int ps : {1000, 4000}) {
        int pages = (25500 + ps - 1) / ps, frames = 48000 / ps;
        SegmentTable st(4, frames, true, ps, c.ppt, c.seed);
        for (int s = 0; s < 3; ++s) st.addSegment(s, s * 100000, pages, READ_WRITE);
        for (auto& [s, b] : bytes) {
            int page = b / ps;
            st.translateAddress(s, page / c.ppt, page % c.ppt, b % ps, READ);
        }
        std::cout << std::setw(11) << ps << std::setw(11) << pages << std::setw(8) << frames << std::setw(10)
                  << st.numDirs(0) << std::setw(13) << st.m.page_faults << std::setw(11)
                  << 100.0 * st.m.page_faults / 4000 << "%" << std::setw(8) << st.getTLB().hitRate() << "%"
                  << std::setw(6) << std::setprecision(0) << st.utilization() << "%" << std::setprecision(2) << std::setw(14)
                  << 3 * (pages * ps - 25500) << "\n";
    }

    std::cout << "\nExperiment 4: valid/invalid ratio (1000 random addresses, default random segments, 10 frames, TLB 4)\n";
    std::cout << "  Valid ratio  Successful  Seg faults  Offset faults  Protection  Page faults  TLB hit\n";
    for (double r : {1.0, 0.7, 0.3}) {
        std::mt19937 gen(c.seed);
        SegmentTable st(4, 10, true, c.page_size, c.ppt, c.seed);
        randomSegments(st, 3, gen);
        std::mt19937 g2(c.seed + 5);
        generateRandomAddresses(st, 1000, r, "/dev/null", g2, nullptr);
        std::cout << std::setw(13) << r << std::setw(12) << st.m.successes << std::setw(12) << st.m.seg_faults
                  << std::setw(15) << st.m.offset_faults << std::setw(12) << st.m.protection_violations << std::setw(13)
                  << st.m.page_faults << std::setw(8) << st.getTLB().hitRate() << "%\n";
    }
}

// ============================================================================
// Main
// ============================================================================
void usage() {
    std::cout << R"(Usage: ./memory_simulator_advanced [options]
  (no options)        prompts for frames, page size, TLB size, policy; then reads commands
  --frames N          physical frames (default 10)       --tlb N        TLB entries (default 4)
  --pagesize N        bytes per page (default 1000)      --ppt N        pages per page table (default 4)
  --replace lru|fifo  page replacement (default lru)     --segments N   random segments at start (default 3)
  --seed N            repeatable run                     --config FILE  segments from a file
  --batch FILE        translate FILE -> batch_results.txt, then print stats
  --experiment        LRU vs FIFO, TLB size, page size, valid ratio tables
Commands: add <id> <base> <limit> <RO|RW>, remove <id>, translate <seg> <dir> <page> <offset> <0=read|1=write>,
          random <num> [validRatio], batch <file>, map, stats, tlb, help, quit
)";
}

int main(int argc, char* argv[]) {
    Config c;
    std::string batchFile, configFile;
    bool experiment = false, prompt = true;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) { std::cerr << "Missing value after " << arg << "\n"; std::exit(1); }
            return argv[++i];
        };
        try {
            if (arg == "--frames") c.frames = std::stoi(next());
            else if (arg == "--tlb") c.tlb = std::stoi(next());
            else if (arg == "--pagesize") c.page_size = std::stoi(next());
            else if (arg == "--ppt") c.ppt = std::stoi(next());
            else if (arg == "--segments") c.segments = std::stoi(next());
            else if (arg == "--replace") {
                std::string p = next();
                if (p != "lru" && p != "fifo") throw std::invalid_argument(p);
                c.lru = (p == "lru");
            }
            else if (arg == "--seed") { c.seed = std::stoul(next()); c.seeded = true; continue; }
            else if (arg == "--batch") batchFile = next();
            else if (arg == "--config") configFile = next();
            else if (arg == "--experiment") experiment = true;
            else if (arg == "--help") { usage(); return 0; }
            else { std::cerr << "Unknown option " << arg << "\n"; usage(); return 1; }
            prompt = false;
        } catch (...) { std::cerr << "Invalid value for " << arg << "\n"; return 1; }
    }
    if (!c.seeded) c.seed = std::chrono::system_clock::now().time_since_epoch().count() % 100000;
    srand(c.seed);
    bool echo = !isatty(0);   // input from a file: show what was read

    if (experiment) { experiments(c); return 0; }

    if (prompt) {
        std::string pol;
        auto ask = [&](const char* q, auto& v) {
            std::cout << q;
            std::cin >> v;
            if (echo) std::cout << v << "\n";
        };
        ask("Enter number of physical frames: ", c.frames);
        ask("Enter page size: ", c.page_size);
        ask("Enter TLB size: ", c.tlb);
        ask("Enter replacement policy (lru/fifo): ", pol);
        if (!std::cin || (pol != "lru" && pol != "fifo")) { std::cerr << "Invalid input\n"; return 1; }
        c.lru = (pol == "lru");
    }
    std::string err = validate(c);
    if (!err.empty()) { std::cerr << "Invalid configuration: " << err << "\n"; return 1; }

    std::unique_ptr<SegmentTable> st;
    std::mt19937 gen(c.seed);
    try {
        if (!configFile.empty()) st = loadConfig(configFile, c);
        else {
            st = std::make_unique<SegmentTable>(c.tlb, c.frames, c.lru, c.page_size, c.ppt, c.seed);
            randomSegments(*st, c.segments, gen);
        }
    } catch (const std::exception& e) {
        std::cerr << e.what() << "\n";
        return 1;
    }
    std::ofstream log("memory_log.txt");
    std::cout << "Config: " << c.frames << " frames, page size " << c.page_size << ", TLB " << c.tlb << ", "
              << (c.lru ? "LRU" : "FIFO") << ", " << c.ppt << " pages per table, seed " << c.seed << "\n";

    if (!batchFile.empty()) {
        try { processBatchFile(*st, batchFile, &log); }
        catch (const std::exception& e) { std::cerr << e.what() << "\n"; return 1; }
        std::cout << "Batch results logged to batch_results.txt\n";
        st->displayStats(std::cout);
        return 0;
    }

    st->printMemoryMap(std::cout);
    std::cout << "Commands: add <id> <base> <limit> <RO|RW>, remove <id>, translate <seg> <dir> <page> <offset> <access>, "
                 "random <num> [ratio], batch <file>, map, stats, tlb, quit\n";
    std::string line;
    std::getline(std::cin, line);   // rest of the last prompt line
    std::cout << "> ";
    while (std::getline(std::cin, line)) {
        std::istringstream in(line);
        std::string cmd;
        if (!(in >> cmd)) { std::cout << "> "; continue; }
        if (echo) std::cout << line << "\n";
        try {
            if (cmd == "add") {
                int id, base, limit;
                std::string prot;
                if (!(in >> id >> base >> limit >> prot) || (prot != "RO" && prot != "RW"))
                    throw std::invalid_argument("usage: add <id> <base> <limit> <RO|RW>");
                st->addSegment(id, base, limit, prot == "RO" ? READ_ONLY : READ_WRITE);
                std::cout << "Segment " << id << " added (" << limit << " pages, " << prot << ", "
                          << st->numDirs(id) << " directory entries)\n";
                st->printMemoryMap(std::cout);
            } else if (cmd == "remove") {
                int id;
                if (!(in >> id)) throw std::invalid_argument("usage: remove <id>");
                int freed = st->removeSegment(id);
                std::cout << "Segment " << id << " removed, " << freed << " frames freed, TLB entries flushed\n";
                st->printMemoryMap(std::cout);
            } else if (cmd == "translate") {
                int s, d, p, o, a;
                if (!(in >> s >> d >> p >> o >> a)) throw std::invalid_argument("usage: translate <seg> <dir> <page> <offset> <0|1>");
                translateAndReport(*st, s, d, p, o, a ? WRITE : READ, std::cout, &log);
            } else if (cmd == "random") {
                int num;
                double ratio = 0.7;
                if (!(in >> num) || num < 1) throw std::invalid_argument("usage: random <num> [validRatio]");
                in >> ratio;
                if (ratio < 0 || ratio > 1) throw std::invalid_argument("valid ratio must be 0-1");
                generateRandomAddresses(*st, num, ratio, "random_results.txt", gen, &log);
                std::cout << num << " random addresses (valid ratio " << ratio << ") logged to random_results.txt\n";
            } else if (cmd == "batch") {
                std::string f;
                in >> f;
                processBatchFile(*st, f, &log);
                std::cout << "Batch results logged to batch_results.txt\n";
            } else if (cmd == "map") st->printMemoryMap(std::cout);
            else if (cmd == "stats") st->displayStats(std::cout);
            else if (cmd == "tlb") st->getTLB().displayCache(std::cout);
            else if (cmd == "help") usage();
            else if (cmd == "quit") break;
            else std::cout << "Unknown command " << cmd << " (type help)\n";
        } catch (const std::exception& e) {
            std::cout << "Error: " << e.what() << "\n";
        }
        std::cout << "> ";
    }
    std::cout << "\n";
    st->displayStats(log);
    return 0;
}
