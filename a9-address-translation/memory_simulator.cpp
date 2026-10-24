// memory_simulator.cpp - CS375 A9: Segmented, Paged Memory Model Simulator
//
// Build:  g++ -std=c++17 -o memory_simulator memory_simulator.cpp
// Run:    ./memory_simulator                      (interactive, like the starter)
//         ./memory_simulator --help               (all options)
//
// Logical address = (segment, page directory, page, offset, access R/W)
//   segment table  -> segment: base, limit (pages), protection, page directory
//   page directory -> second-level page table (created only when first used)
//   page table     -> frame number, present bit, protection, last access
//   physical address = frame * page_size + offset
//
// Features: segment + page protection checks, page faults handled with FIFO or
// LRU replacement over a fixed PhysicalMemory frame pool, two-level page
// tables, runtime / file configuration with validation, 100-address stress
// test with a valid/invalid ratio (results.txt), batch file processing,
// simulated latency (1-5 units), detailed error log (errors.log), text memory
// map, translation timeline and metrics.
//
// Fixes to the starter code:
//  - <algorithm> and <chrono> were missing (std::count, std::chrono).
//  - Pages were marked present with a random frame 0-99 that was never taken
//    from PhysicalMemory, so two pages could share a frame and frames beyond
//    the pool size were used. Initial pages are now loaded through the pool.
//  - On replacement the victim page was never marked "not present", so two
//    pages ended up mapped to the same frame. The victim is now invalidated.
//  - The page-fault path called setFrame(page, frame, accessType), which
//    overwrote the page's protection with the access type (a read-only page
//    became writable after a write fault). Protection is now kept.
//  - The starter added segment.base_address to frame * page_size, mixing two
//    address spaces. Here base_address is where the segment's page directory
//    lives, and the physical address is frame * page_size + offset.
//  - generateRandomAddresses compared gen() (0..2^32) with RAND_MAX, so the
//    valid/invalid ratio did not work; it now uses a real probability.

#include <iostream>
#include <iomanip>
#include <sstream>
#include <vector>
#include <map>
#include <deque>
#include <memory>
#include <algorithm>
#include <cstdlib>
#include <ctime>
#include <chrono>
#include <fstream>
#include <string>
#include <random>
#include <unistd.h>

enum Protection { READ_ONLY, READ_WRITE };
enum Access { READ, WRITE };
enum Policy { FIFO, LRU };

static const char* protName(Protection p) { return p == READ_ONLY ? "RO" : "RW"; }

// ============================================================================
// Part 1: data structures
// ============================================================================
struct Page {
    int frame_number = -1;   // -1 if not in memory
    bool present = false;
    Protection protection = READ_WRITE;
    int last_access = 0;     // for LRU
};

// Second-level page table: one per used page-directory entry.
class PageTable {
public:
    std::vector<Page> pages;
    int page_size;
    PageTable(int numPages, int pageSize) : pages(numPages), page_size(pageSize) {}

    int getFrameNumber(int pageNum) const {
        if (pageNum < 0 || pageNum >= (int)pages.size() || !pages[pageNum].present) return -1;
        return pages[pageNum].frame_number;
    }
    void setFrame(int pageNum, int frame) {   // protection is NOT changed (starter bug)
        pages[pageNum].frame_number = frame;
        pages[pageNum].present = true;
    }
    void evict(int pageNum) {
        pages[pageNum].frame_number = -1;
        pages[pageNum].present = false;
    }
};

// First level: page directory index -> page table (allocated on demand).
class DirectoryTable {
public:
    int pages_per_table;
    int page_size;
    std::vector<std::unique_ptr<PageTable>> tables;
    std::vector<Protection> page_protection;   // protection of each page in the segment

    DirectoryTable(int numPages, int pagesPerTable, int pageSize)
        : pages_per_table(pagesPerTable), page_size(pageSize),
          tables((numPages + pagesPerTable - 1) / pagesPerTable), page_protection(numPages, READ_WRITE) {}

    int entries() const { return tables.size(); }
    PageTable* table(int dir, bool create) {
        if (!tables[dir] && create) {
            tables[dir] = std::make_unique<PageTable>(pages_per_table, page_size);
            for (int p = 0; p < pages_per_table; ++p) {
                int global = dir * pages_per_table + p;
                if (global < (int)page_protection.size()) tables[dir]->pages[p].protection = page_protection[global];
            }
        }
        return tables[dir].get();
    }
    int allocatedTables() const {
        return std::count_if(tables.begin(), tables.end(), [](auto& t) { return t != nullptr; });
    }
};

struct Segment {
    int base_address;        // physical address of this segment's page directory
    int limit;               // number of pages
    Protection protection;
    std::unique_ptr<DirectoryTable> directory;
};

struct FrameOwner { int seg = -1, dir = -1, page = -1; };

// Fixed-size pool of physical frames with FIFO or LRU replacement.
class PhysicalMemory {
public:
    int num_frames;
    Policy policy;
    std::vector<FrameOwner> owner;      // who is in each frame (seg -1 = free)
    std::vector<int> last_used;         // for LRU
    std::deque<int> fifo_queue;         // load order, for FIFO

    PhysicalMemory(int frames, Policy p) : num_frames(frames), policy(p), owner(frames), last_used(frames, 0) {}

    int freeFrame() const {
        for (int i = 0; i < num_frames; ++i) if (owner[i].seg == -1) return i;
        return -1;
    }
    // Choose a victim frame when memory is full.
    int victim() const {
        if (policy == FIFO) return fifo_queue.front();
        int best = 0;
        for (int i = 1; i < num_frames; ++i) if (last_used[i] < last_used[best]) best = i;
        return best;
    }
    void load(int frame, FrameOwner who, int time) {
        if (owner[frame].seg != -1)   // replacing: remove from FIFO position
            fifo_queue.erase(std::find(fifo_queue.begin(), fifo_queue.end(), frame));
        owner[frame] = who;
        last_used[frame] = time;
        fifo_queue.push_back(frame);
    }
    int used() const { return std::count_if(owner.begin(), owner.end(), [](auto& o) { return o.seg != -1; }); }
    double utilization() const { return 100.0 * used() / num_frames; }
};

// ============================================================================
// Part 2: translation
// ============================================================================
struct Result {
    bool ok = false;
    long physical = -1;
    bool page_fault = false;
    int replaced_frame = -1;     // frame whose old page was evicted
    std::string victim;          // "S1.D0.P2" of the evicted page
    std::string error;           // empty when ok
    int latency = 0;
};

struct Metrics {
    int translations = 0, successes = 0, page_faults = 0, replacements = 0;
    int seg_faults = 0, offset_faults = 0, protection_violations = 0;
    long total_latency = 0;
    void add(const Result& r) {
        translations++;
        total_latency += r.latency;
        if (r.ok) successes++;
        if (r.page_fault) page_faults++;
        if (r.replaced_frame >= 0) replacements++;
        if (r.error.rfind("Segmentation", 0) == 0) seg_faults++;
        if (r.error.rfind("Offset", 0) == 0) offset_faults++;
        if (r.error.rfind("Protection", 0) == 0) protection_violations++;
    }
};

class SegmentTable {
public:
    std::vector<Segment> segments;
    PhysicalMemory physMem;
    int page_size, pages_per_table;
    int time = 0;
    std::mt19937 rng;
    Metrics metrics;
    std::ofstream error_log;

    SegmentTable(int frames, int pageSize, int pagesPerTable, Policy policy, unsigned seed)
        : physMem(frames, policy), page_size(pageSize), pages_per_table(pagesPerTable), rng(seed) {}

    void addSegment(int base, int limit, Protection prot) {
        Segment s{base, limit, prot, std::make_unique<DirectoryTable>(limit, pages_per_table, page_size)};
        segments.push_back(std::move(s));
    }

    // Load a page into a frame (free frame first, otherwise FIFO/LRU victim).
    Result loadPage(int seg, int dir, int page) {
        Result r;
        int frame = physMem.freeFrame();
        if (frame == -1) {
            frame = physMem.victim();
            FrameOwner v = physMem.owner[frame];
            segments[v.seg].directory->table(v.dir, false)->evict(v.page);   // invalidate victim
            r.replaced_frame = frame;
            r.victim = "S" + std::to_string(v.seg) + ".D" + std::to_string(v.dir) + ".P" + std::to_string(v.page);
        }
        segments[seg].directory->table(dir, true)->setFrame(page, frame);
        physMem.load(frame, {seg, dir, page}, time);
        return r;
    }

    Result fail(Result r, const std::string& why) {
        r.error = why;
        if (error_log) error_log << "t=" << time << " " << why << "\n";
        return r;
    }

    Result translateAddress(int segNum, int pageDir, int pageNum, int offset, Access access) {
        Result r;
        r.latency = 1 + rng() % 5;   // simulated latency 1-5 units
        time++;
        int nseg = segments.size();
        if (segNum < 0 || segNum >= nseg)
            return fail(r, "Segmentation fault: invalid segment " + std::to_string(segNum) + ", max " + std::to_string(nseg - 1));
        Segment& s = segments[segNum];
        if (pageDir < 0 || pageDir >= s.directory->entries())
            return fail(r, "Segmentation fault: page directory " + std::to_string(pageDir) + " invalid for segment " +
                               std::to_string(segNum) + ", max " + std::to_string(s.directory->entries() - 1));
        int global = pageDir * pages_per_table + pageNum;
        if (pageNum < 0 || pageNum >= pages_per_table || global >= s.limit)
            return fail(r, "Segmentation fault: page " + std::to_string(pageNum) + " in directory " + std::to_string(pageDir) +
                               " exceeds segment " + std::to_string(segNum) + " limit of " + std::to_string(s.limit) + " pages");
        if (offset < 0 || offset >= page_size)
            return fail(r, "Offset fault: offset " + std::to_string(offset) + " outside page size " + std::to_string(page_size));
        if (access == WRITE && s.protection == READ_ONLY)
            return fail(r, "Protection violation: write to read-only segment " + std::to_string(segNum));
        PageTable* pt = s.directory->table(pageDir, true);
        if (access == WRITE && pt->pages[pageNum].protection == READ_ONLY)
            return fail(r, "Protection violation: write to read-only page " + std::to_string(pageNum) + " of segment " +
                               std::to_string(segNum));

        int frame = pt->getFrameNumber(pageNum);
        if (frame == -1) {                              // present bit is 0 -> page fault
            Result load = loadPage(segNum, pageDir, pageNum);
            r.page_fault = true;
            r.replaced_frame = load.replaced_frame;
            r.victim = load.victim;
            frame = pt->getFrameNumber(pageNum);
            if (error_log) error_log << "t=" << time << " Page fault: S" << segNum << ".D" << pageDir << ".P" << pageNum
                                     << " loaded into frame " << frame
                                     << (r.victim.empty() ? "" : " (replaced " + r.victim + ")") << "\n";
        }
        pt->pages[pageNum].last_access = time;
        physMem.last_used[frame] = time;
        r.ok = true;
        r.physical = (long)frame * page_size + offset;
        return r;
    }

    // Text memory map: segment table, page tables, and the frame pool.
    void printMemoryMap(std::ostream& out) {
        out << "---------------- Memory Map ----------------\n";
        for (size_t i = 0; i < segments.size(); ++i) {
            Segment& s = segments[i];
            out << "Segment " << i << ": base=" << s.base_address << " limit=" << s.limit << " pages  " << protName(s.protection)
                << "  directory: " << s.directory->allocatedTables() << "/" << s.directory->entries() << " page tables allocated\n";
            for (int d = 0; d < s.directory->entries(); ++d) {
                PageTable* pt = s.directory->table(d, false);
                out << "  Dir " << d << ": ";
                if (!pt) { out << "(no page table yet)\n"; continue; }
                for (int p = 0; p < pages_per_table && d * pages_per_table + p < s.limit; ++p) {
                    Page& pg = pt->pages[p];
                    out << "P" << p << "[" << (pg.present ? "F" + std::to_string(pg.frame_number) : "--") << " "
                        << protName(pg.protection) << "] ";
                }
                out << "\n";
            }
        }
        out << "Frames: ";
        for (int f = 0; f < physMem.num_frames; ++f) {
            FrameOwner o = physMem.owner[f];
            out << "|" << f << ":" << (o.seg == -1 ? "free" : "S" + std::to_string(o.seg) + "D" + std::to_string(o.dir) + "P" + std::to_string(o.page));
            if (f % 6 == 5 && f + 1 < physMem.num_frames) out << "|\n        ";
        }
        out << "|\n";
        out << std::fixed << std::setprecision(1) << "Physical Memory Utilization: " << physMem.utilization() << "% ("
            << physMem.used() << "/" << physMem.num_frames << " frames)\n";
    }

    void printMetrics(std::ostream& out, const std::string& title) {
        Metrics& m = metrics;
        out << "---------------- " << title << " ----------------\n" << std::fixed << std::setprecision(2);
        out << "Translations: " << m.translations << "  successful: " << m.successes << "  failed: "
            << m.translations - m.successes << "\n";
        out << "Page faults: " << m.page_faults << " (rate " << (m.translations ? 100.0 * m.page_faults / m.translations : 0)
            << "%)  replacements: " << m.replacements << " [" << (physMem.policy == FIFO ? "FIFO" : "LRU") << "]\n";
        out << "Segmentation faults: " << m.seg_faults << "  offset faults: " << m.offset_faults
            << "  protection violations: " << m.protection_violations << "\n";
        out << "Average latency: " << (m.translations ? (double)m.total_latency / m.translations : 0) << " units\n";
        out << std::setprecision(1) << "Memory utilization: " << physMem.utilization() << "%\n";
        // two-level vs single-level page table size
        int used_entries = 0, flat_entries = 0;
        for (auto& s : segments) {
            used_entries += s.directory->entries() + s.directory->allocatedTables() * pages_per_table;
            flat_entries += s.directory->entries() * pages_per_table;
        }
        out << "Page table entries: two-level " << used_entries << " vs single-level " << flat_entries << "\n";
    }
};

// ============================================================================
// Part 3: configuration
// ============================================================================
struct Config {
    int frames = 8, page_size = 1000, segments = 3, pages_per_table = 4;
    Policy policy = FIFO;
    unsigned seed = 0;
    bool seeded = false;
    int seg_bytes = 0;     // if > 0, every segment is this many bytes (limit = ceil(bytes / page_size))
};

// Validates a configuration; returns an empty string when it is fine.
std::string validate(const Config& c) {
    if (c.frames < 1 || c.frames > 4096) return "number of frames must be 1-4096";
    if (c.page_size < 16 || c.page_size > 65536) return "page size must be 16-65536";
    if (c.segments < 1 || c.segments > 64) return "number of segments must be 1-64";
    if (c.pages_per_table < 1 || c.pages_per_table > 1024) return "pages per table must be 1-1024";
    return "";
}

// Random initialization like the starter: 3-7 pages per segment, random
// protection, about half of the pages loaded at start (while frames last).
void randomInit(SegmentTable& st, const Config& c, std::mt19937& gen) {
    for (int i = 0; i < c.segments; ++i) {
        int limit = c.seg_bytes > 0 ? (c.seg_bytes + c.page_size - 1) / c.page_size : 3 + gen() % 5;
        Protection prot = (gen() % 4 == 0) ? READ_ONLY : READ_WRITE;
        st.addSegment(i * 10000, limit, prot);
        for (int p = 0; p < limit; ++p) st.segments[i].directory->page_protection[p] = (gen() % 4 == 0) ? READ_ONLY : READ_WRITE;
    }
    for (int i = 0; i < c.segments; ++i)
        for (int p = 0; p < st.segments[i].limit; ++p)
            if (gen() % 2 && st.physMem.freeFrame() != -1)
                st.loadPage(i, p / c.pages_per_table, p % c.pages_per_table);
}

// File format (one item per line, '#' comments):
//   frames 8 | page_size 1000 | pages_per_table 4 | policy fifo|lru
//   segID base limit RO|RW [page protections...]   e.g.  1 10000 5 RW RW RO RW+ RW RW
//   a page written with '+' (RW+) is loaded into memory at start
bool loadConfig(const std::string& file, Config& c, std::unique_ptr<SegmentTable>& st) {
    std::ifstream in(file);
    if (!in) { std::cerr << "Cannot open config file " << file << "\n"; return false; }
    std::vector<std::string> seglines;
    std::string line;
    while (std::getline(in, line)) {
        line = line.substr(0, line.find('#'));
        std::istringstream ss(line);
        std::string key;
        if (!(ss >> key)) continue;
        if (key == "frames") ss >> c.frames;
        else if (key == "page_size") ss >> c.page_size;
        else if (key == "pages_per_table") ss >> c.pages_per_table;
        else if (key == "policy") { std::string p; ss >> p; c.policy = (p == "lru") ? LRU : FIFO; }
        else seglines.push_back(line);
    }
    c.segments = seglines.size();
    std::string err = validate(c);
    if (!err.empty()) { std::cerr << "Invalid configuration: " << err << "\n"; return false; }
    st = std::make_unique<SegmentTable>(c.frames, c.page_size, c.pages_per_table, c.policy, c.seed);
    int expected = 0, preload = 0;
    for (auto& l : seglines) {
        std::istringstream ss(l);
        int id, base, limit;
        std::string prot;
        if (!(ss >> id >> base >> limit >> prot) || id != expected || limit < 1 || (prot != "RO" && prot != "RW")) {
            std::cerr << "Invalid segment line (expected id " << expected << "): " << l << "\n";
            return false;
        }
        st->addSegment(base, limit, prot == "RO" ? READ_ONLY : READ_WRITE);
        std::string p;
        std::vector<int> load;
        for (int k = 0; k < limit && ss >> p; ++k) {       // set every protection first ...
            if (p.substr(0, 2) != "RO" && p.substr(0, 2) != "RW") { std::cerr << "Invalid page protection '" << p << "' in: " << l << "\n"; return false; }
            st->segments[id].directory->page_protection[k] = (p.substr(0, 2) == "RO") ? READ_ONLY : READ_WRITE;
            if (p.back() == '+') load.push_back(k);
        }
        for (int k : load) {                                // ... then load the '+' pages
            if (++preload > c.frames) { std::cerr << "Invalid configuration: more preloaded pages than frames\n"; return false; }
            st->loadPage(id, k / c.pages_per_table, k % c.pages_per_table);
        }
        expected++;
    }
    return true;
}

// ============================================================================
// Part 4: stress test, batch, output
// ============================================================================
std::string addrText(int s, int d, int p, int o, Access a) {
    std::ostringstream ss;
    ss << "(" << s << "," << d << "," << p << "," << o << "," << (a == READ ? "R" : "W") << ")";
    return ss.str();
}

std::string describe(const Result& r) {
    if (!r.ok) return "ERROR  " + r.error;
    std::string s = "-> physical " + std::to_string(r.physical);
    if (r.page_fault) s += "  PAGE FAULT" + (r.victim.empty() ? std::string(" (free frame)") : " (replaced " + r.victim + ")");
    else s += "  hit";
    return s + "  latency " + std::to_string(r.latency);
}

// Timeline line: "Time X: (s,d,p,o,R) -> physical Y ..."
void report(std::ostream& out, SegmentTable& st, int s, int d, int p, int o, Access a, const Result& r) {
    out << "Time " << std::setw(3) << st.time << ": " << std::left << std::setw(20) << addrText(s, d, p, o, a) << std::right
        << describe(r) << "\n";
}

// Random logical address; with probability validRatio every field is in range.
void randomAddress(SegmentTable& st, std::mt19937& gen, double validRatio, int& s, int& d, int& p, int& o, Access& a) {
    std::uniform_real_distribution<double> coin(0, 1);
    a = coin(gen) < 0.7 ? READ : WRITE;
    s = gen() % st.segments.size();
    Segment& seg = st.segments[s];
    int global = gen() % seg.limit;
    d = global / st.pages_per_table;
    p = global % st.pages_per_table;
    o = gen() % st.page_size;
    if (coin(gen) >= validRatio) {            // make exactly one field invalid
        switch (gen() % 4) {
            case 0: s = st.segments.size() + gen() % 3; break;
            case 1: d = seg.directory->entries() + gen() % 2; break;
            case 2: p = st.pages_per_table + gen() % 3; break;
            default: o = st.page_size + gen() % 500; break;
        }
    }
}

void stressTest(SegmentTable& st, int num, double validRatio, std::mt19937& gen, const std::string& file, bool echo) {
    std::ofstream log(file);
    Metrics before = st.metrics;
    log << "Stress test: " << num << " random addresses, valid ratio " << validRatio << ", policy "
        << (st.physMem.policy == FIFO ? "FIFO" : "LRU") << "\n";
    for (int i = 0; i < num; ++i) {
        int s, d, p, o;
        Access a;
        randomAddress(st, gen, validRatio, s, d, p, o, a);
        Result r = st.translateAddress(s, d, p, o, a);
        st.metrics.add(r);
        report(log, st, s, d, p, o, a, r);
        if (echo) report(std::cout, st, s, d, p, o, a, r);
    }
    Metrics& m = st.metrics;
    int t = m.translations - before.translations, ok = m.successes - before.successes, pf = m.page_faults - before.page_faults;
    log << std::fixed << std::setprecision(2) << "Summary: " << ok << " successful, " << t - ok << " failed, " << pf
        << " page faults (rate " << 100.0 * pf / t << "%), avg latency "
        << (double)(m.total_latency - before.total_latency) / t << ", memory utilization " << std::setprecision(1)
        << st.physMem.utilization() << "%\n";
}

// Batch file: one address per line "seg dir page offset R|W"
bool batchFile(SegmentTable& st, const std::string& file) {
    std::ifstream in(file);
    if (!in) { std::cerr << "Cannot open batch file " << file << "\n"; return false; }
    std::string line;
    int ok = 0, total = 0;
    while (std::getline(in, line)) {
        line = line.substr(0, line.find('#'));
        std::istringstream ss(line);
        int s, d, p, o;
        std::string a;
        if (!(ss >> s >> d >> p >> o >> a)) continue;
        Access acc = (a == "W" || a == "w" || a == "1") ? WRITE : READ;
        Result r = st.translateAddress(s, d, p, o, acc);
        st.metrics.add(r);
        report(std::cout, st, s, d, p, o, acc, r);
        total++;
        if (r.ok) ok++;
    }
    std::cout << "Batch summary: " << ok << "/" << total << " successful, " << total - ok << " failed\n";
    return true;
}

// Same random address stream through FIFO and LRU, for uniform and "locality"
// (80% of accesses go to 20% of the pages) workloads.
void comparePolicies(Config c, int num) {
    std::cout << "FIFO vs LRU on the same address stream (" << c.frames << " frames, " << num << " valid addresses)\n";
    std::cout << std::left << std::setw(12) << "Workload" << std::setw(8) << "Policy" << std::right << std::setw(10)
              << "Faults" << std::setw(12) << "Fault rate" << "\n";
    for (int locality = 0; locality < 2; ++locality)
        for (Policy pol : {FIFO, LRU}) {
            std::mt19937 gen(c.seed);
            c.policy = pol;
            SegmentTable st(c.frames, c.page_size, c.pages_per_table, pol, c.seed);
            randomInit(st, c, gen);
            int total_pages = 0;
            for (auto& s : st.segments) total_pages += s.limit;
            std::mt19937 addr(c.seed + 99);
            int hot = std::max(1, total_pages / 5);
            for (int i = 0; i < num; ++i) {
                int k = (locality && addr() % 10 < 8) ? addr() % hot : addr() % total_pages;
                int s = 0;
                while (k >= st.segments[s].limit) k -= st.segments[s++].limit;
                Result r = st.translateAddress(s, k / c.pages_per_table, k % c.pages_per_table, 0, READ);
                st.metrics.add(r);
            }
            std::cout << std::left << std::setw(12) << (locality ? "locality" : "uniform") << std::setw(8)
                      << (pol == FIFO ? "FIFO" : "LRU") << std::right << std::setw(10) << st.metrics.page_faults
                      << std::fixed << std::setprecision(1) << std::setw(11) << 100.0 * st.metrics.page_faults / num << "%\n";
        }
}

void usage() {
    std::cout << R"(Usage: ./memory_simulator [options]
  (no options)          interactive: prompts for frames, page size, segments, then addresses
  --frames N            physical frames            --page-size N     bytes per page
  --segments N          number of segments         --pages-per-table N  second-level table size (default 4)
  --policy fifo|lru     page replacement           --seed N          repeatable random run
  --seg-bytes N         every segment is N bytes (for page-size comparisons)
  --config FILE         load segments/pages from FILE instead of random
  --batch FILE          translate addresses from FILE (seg dir page offset R|W)
  --random N            N random addresses -> results.txt      --valid R   valid ratio (default 0.7)
  --compare N           FIFO vs LRU on N addresses (uniform and locality workloads)
  --map                 print the memory map at the end
)";
}

int main(int argc, char* argv[]) {
    Config c;
    std::string configFile, batch;
    int randomN = 0, compareN = 0;
    double valid = 0.7;
    bool showMap = false, modeGiven = false;   // interactive unless a mode/config option is given
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) { std::cerr << "Missing value after " << a << "\n"; std::exit(1); }
            return argv[++i];
        };
        if (a != "--seed" && a != "--valid" && a != "--policy") modeGiven = true;
        try {
            if (a == "--frames") c.frames = std::stoi(next());
            else if (a == "--page-size") c.page_size = std::stoi(next());
            else if (a == "--segments") c.segments = std::stoi(next());
            else if (a == "--pages-per-table") c.pages_per_table = std::stoi(next());
            else if (a == "--policy") { std::string p = next(); if (p != "fifo" && p != "lru") throw 1; c.policy = p == "lru" ? LRU : FIFO; }
            else if (a == "--seed") { c.seed = std::stoul(next()); c.seeded = true; }
            else if (a == "--seg-bytes") c.seg_bytes = std::stoi(next());
            else if (a == "--config") configFile = next();
            else if (a == "--batch") batch = next();
            else if (a == "--random") randomN = std::stoi(next());
            else if (a == "--valid") valid = std::stod(next());
            else if (a == "--compare") compareN = std::stoi(next());
            else if (a == "--map") showMap = true;
            else if (a == "--help") { usage(); return 0; }
            else { std::cerr << "Unknown option " << a << "\n"; usage(); return 1; }
        } catch (...) { std::cerr << "Invalid value for " << a << "\n"; return 1; }
    }
    bool interactive = !modeGiven;
    if (!c.seeded) c.seed = std::chrono::system_clock::now().time_since_epoch().count() % 100000;
    if (valid < 0 || valid > 1) { std::cerr << "--valid must be between 0 and 1\n"; return 1; }

    if (interactive) {
        bool echo = !isatty(0);   // when input comes from a file, show what was read
        std::string pol;
        auto ask = [&](const char* prompt, auto& var) {
            std::cout << prompt;
            std::cin >> var;
            if (echo) std::cout << var << "\n";
        };
        ask("Enter number of physical frames: ", c.frames);
        ask("Enter page size: ", c.page_size);
        ask("Enter number of segments: ", c.segments);
        ask("Enter replacement policy (fifo/lru): ", pol);
        if (pol != "fifo" && pol != "lru") { std::cerr << "Policy must be fifo or lru\n"; return 1; }
        c.policy = pol == "lru" ? LRU : FIFO;
        if (!std::cin) { std::cerr << "Invalid input\n"; return 1; }
        std::string err = validate(c);
        if (!err.empty()) { std::cerr << "Invalid configuration: " << err << "\n"; return 1; }
        std::mt19937 gen(c.seed);
        SegmentTable st(c.frames, c.page_size, c.pages_per_table, c.policy, c.seed);
        st.error_log.open("errors.log");
        randomInit(st, c, gen);
        st.printMemoryMap(std::cout);
        std::cout << "Enter logical address (seg dir page offset access[0=read,1=write]) or -1 to stop:\n";
        int s, d, p, o, acc;
        while (std::cin >> s && s != -1) {
            if (!(std::cin >> d >> p >> o >> acc)) break;
            Result r = st.translateAddress(s, d, p, o, acc ? WRITE : READ);
            st.metrics.add(r);
            if (echo) std::cout << "> " << s << " " << d << " " << p << " " << o << " " << acc << "\n";
            report(std::cout, st, s, d, p, o, acc ? WRITE : READ, r);
        }
        std::cout << "\n";
        st.printMemoryMap(std::cout);
        st.printMetrics(std::cout, "Manual translations");
        std::cout << "Generate random addresses? (y/n): ";
        char yn = 'n';
        std::cin >> yn;
        if (echo) std::cout << yn << "\n";
        if (yn == 'y') {
            stressTest(st, 100, valid, gen, "results.txt", false);
            std::cout << "100 random addresses (valid ratio " << valid << ") logged to results.txt; errors in errors.log\n";
            st.printMetrics(std::cout, "After stress test");
        }
        return 0;
    }

    if (compareN > 0) { comparePolicies(c, compareN); return 0; }

    std::string err = validate(c);
    if (!err.empty()) { std::cerr << "Invalid configuration: " << err << "\n"; return 1; }
    std::unique_ptr<SegmentTable> st;
    std::mt19937 gen(c.seed);
    if (!configFile.empty()) {
        if (!loadConfig(configFile, c, st)) return 1;
    } else {
        st = std::make_unique<SegmentTable>(c.frames, c.page_size, c.pages_per_table, c.policy, c.seed);
        randomInit(*st, c, gen);
    }
    st->error_log.open("errors.log");
    std::cout << "Config: " << c.frames << " frames, page size " << c.page_size << ", " << st->segments.size()
              << " segments, " << c.pages_per_table << " pages per table, " << (c.policy == FIFO ? "FIFO" : "LRU")
              << ", seed " << c.seed << "\n";
    if (!batch.empty() && !batchFile(*st, batch)) return 1;
    if (randomN > 0) {
        stressTest(*st, randomN, valid, gen, "results.txt", false);
        std::cout << randomN << " random addresses (valid ratio " << valid << ") logged to results.txt\n";
    }
    if (showMap || (batch.empty() && randomN == 0)) st->printMemoryMap(std::cout);
    st->printMetrics(std::cout, "Metrics");
    return 0;
}
