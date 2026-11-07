// main.cpp - Lab 8: Caching, TLB and Multi-level Page Tables
//
// Build:  make            Run: make run   (or ./generate && ./lab)
//
//   ./lab [file]            Parts 2-4: 2-level page table + 16-entry LRU TLB + page faults
//   ./lab --part1 [file]    Part 1: direct-mapped page table (1M entries)
//   ./lab --experiment      TLB size, working set, with/without TLB, frame policy
//   options: --tlb N  --frames N  --seed N  --invalidate P  --lru  --no-tlb  --quiet
//
// Virtual address (32 bits):  | L1 index (10) | L2 index (10) | offset (12) |
// Translation:  VA -> TLB hit? -> done
//                  -> miss: walk page directory + page table (fault if not valid)
//                  -> insert VPN->PFN into TLB (LRU) -> PA = PFN << 12 | offset
//
// Cost model used for "cycles" (simple, like a textbook):
//   TLB lookup = 1 cycle, each page-table memory reference = 100 cycles
//   -> TLB hit = 1 cycle, TLB miss = 1 + 2 x 100 = 201, no TLB = 200 every time
//
// Fixes to the starter skeleton:
//  - allocate_frame() looped forever once all 256 frames were used (it kept
//    picking random frames until it found a free one). Now, when memory is
//    full, a victim frame is chosen (random, or LRU with --lru), its page is
//    marked invalid in the page table, and the frame is reused.
//  - The TLB was never told about evictions, so it could keep a stale
//    VPN -> frame mapping that now belongs to another page. Evicting or
//    invalidating a page now also removes it from the TLB.
//  - After printing a fault with `hex`, the summary counts were printed in
//    hexadecimal (and padded with 0s). The stream state is restored.
//  - `fin >> hex >> addr` into a uint32_t stops reading at a 48-bit address
//    such as 0x7fff5fbff000 (failbit), silently dropping the rest of the file.
//    Addresses are read as text; wider ones are truncated to 32 bits with a warning.
//  - The time measured included printing every line; translation time is now
//    measured separately without output.
//  - random_device made every run different; a fixed --seed makes runs repeatable.
//  - print_stats() divided by zero when there were no lookups.

#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <string>
#include <cstdint>
#include <iomanip>
#include <chrono>
#include <random>
#include <algorithm>

using namespace std;
using namespace std::chrono;

// === Config ===
const int PAGE_SIZE = 4096;
const int PAGE_OFFSET_BITS = 12;
const int VPN_BITS = 20;
const int L1_BITS = 10, L2_BITS = 10;
const int PD_ENTRIES = 1 << L1_BITS;
const int PT_ENTRIES = 1 << L2_BITS;
const int TLB_CYCLES = 1;          // cost model
const int MEM_CYCLES = 100;

struct Options {
    int tlb_size = 16;
    int frames = 256;              // only 256 physical frames (Part 4)
    bool use_tlb = true;
    bool lru_frames = false;       // victim frame: random (default) or LRU
    double invalidate = 0.01;      // chance per access that the OS swaps out a random page
    unsigned seed = 42;
    bool quiet = false;
};

// === Helper Functions ===
uint32_t get_l1_index(uint32_t va) { return (va >> 22) & 0x3FF; }
uint32_t get_l2_index(uint32_t va) { return (va >> 12) & 0x3FF; }
uint32_t get_offset(uint32_t va)   { return va & 0xFFF; }
uint32_t get_vpn(uint32_t va)      { return va >> 12; }

string hex8(uint64_t v, int width = 8) {
    ostringstream s;
    s << "0x" << hex << setw(width) << setfill('0') << v;
    return s.str();
}

// ============================================================================
// Part 1: direct-mapped (single-level) page table: 2^20 entries, one per VPN
// ============================================================================
struct PageTableEntry {
    bool valid = false;
    uint32_t frame = 0;
};

class DirectPageTable {
    vector<PageTableEntry> table;    // 1,048,576 entries, allocated up front
    uint32_t next_frame = 0;
public:
    DirectPageTable() : table(1u << VPN_BITS) {}
    uint32_t translate(uint32_t va) {
        PageTableEntry& pte = table[get_vpn(va)];      // ONE memory reference
        if (!pte.valid) {                              // first touch: give it the next frame
            pte.valid = true;
            pte.frame = next_frame++;
        }
        return (pte.frame << PAGE_OFFSET_BITS) | get_offset(va);
    }
    size_t bytes() const { return table.size() * sizeof(PageTableEntry); }
};

// ============================================================================
// Part 3: TLB - fully associative, LRU replacement by timestamp
// ============================================================================
struct TLBEntry {
    uint32_t vpn = 0, pfn = 0;
    uint64_t timestamp = 0;          // last use; smallest = least recently used
    bool valid = false;
};

class TLB {
    vector<TLBEntry> entries;        // 16 by default (size is a parameter for the experiment)
    uint64_t clock = 0;
    long hits = 0, misses = 0;
public:
    explicit TLB(int size = 16) : entries(size) {}

    bool lookup(uint32_t vpn, uint32_t& pfn) {
        for (auto& e : entries) {    // fully associative: compare with every entry
            if (e.valid && e.vpn == vpn) {
                e.timestamp = ++clock;   // used now -> most recently used
                pfn = e.pfn;
                hits++;
                return true;
            }
        }
        misses++;
        return false;
    }

    void insert(uint32_t vpn, uint32_t pfn) {
        int victim = 0;              // an invalid entry if there is one, else the LRU entry
        for (int i = 0; i < (int)entries.size(); i++) {
            if (!entries[i].valid) { victim = i; break; }
            if (entries[i].timestamp < entries[victim].timestamp) victim = i;
        }
        entries[victim] = {vpn, pfn, ++clock, true};
    }

    void invalidate(uint32_t vpn) {  // page was evicted: drop its translation
        for (auto& e : entries)
            if (e.valid && e.vpn == vpn) e.valid = false;
    }

    long get_hits() const { return hits; }
    long get_misses() const { return misses; }
    double hit_rate() const { return hits + misses ? hits * 100.0 / (hits + misses) : 0.0; }

    void print_stats() const {
        cout << "TLB Hits: " << dec << hits << ", Misses: " << misses << ", Hit Rate: " << fixed
             << setprecision(2) << hit_rate() << "%\n";
    }
};

// ============================================================================
// Parts 2 + 4: two-level page table, limited physical memory, page faults
// ============================================================================
struct PageDirectoryEntry {
    bool present = false;
    PageTableEntry* pt = nullptr;    // second-level table, created on first use
};

struct Result {
    uint32_t pa = 0;
    bool tlb_hit = false;
    bool fault = false;
    int evicted_frame = -1;          // frame taken from another page (-1 = free frame used)
    uint32_t evicted_vpn = 0;
    int cycles = 0;
};

class MMU {
    PageDirectoryEntry page_directory[PD_ENTRIES];
    TLB tlb;
    Options opt;
    vector<int64_t> frame_owner;     // VPN stored in each frame, -1 = free
    vector<uint64_t> frame_last_use; // for LRU victim selection
    vector<uint32_t> free_frames;
    mt19937 rng;
    uint64_t time = 0;
public:
    long faults = 0, evictions = 0, invalidations = 0, walks = 0;
    long long cycles = 0;

    explicit MMU(const Options& o)
        : tlb(o.tlb_size), opt(o), frame_owner(o.frames, -1), frame_last_use(o.frames, 0), rng(o.seed) {
        for (int f = o.frames - 1; f >= 0; f--) free_frames.push_back(f);
    }
    ~MMU() {
        for (auto& pde : page_directory) delete[] pde.pt;
    }
    MMU(const MMU&) = delete;
    MMU& operator=(const MMU&) = delete;

    const TLB& get_tlb() const { return tlb; }
    int page_tables_allocated() const {
        return count_if(begin(page_directory), end(page_directory), [](const PageDirectoryEntry& d) { return d.present; });
    }
    size_t bytes() const {
        return sizeof(page_directory) + page_tables_allocated() * PT_ENTRIES * sizeof(PageTableEntry);
    }

    // Find the PTE for a VA: L1 index -> page directory, L2 index -> page table.
    PageTableEntry& walk(uint32_t va) {
        PageDirectoryEntry& pde = page_directory[get_l1_index(va)];
        if (!pde.present) {
            pde.present = true;
            pde.pt = new PageTableEntry[PT_ENTRIES];
        }
        return pde.pt[get_l2_index(va)];
    }

    // Remove a page from memory: PTE invalid, TLB entry dropped, frame freed.
    void unmap_frame(int frame) {
        uint32_t vpn = (uint32_t)frame_owner[frame];
        page_directory[vpn >> L2_BITS].pt[vpn & (PT_ENTRIES - 1)].valid = false;
        tlb.invalidate(vpn);
        frame_owner[frame] = -1;
    }

    // Get a frame for a faulting page: a free one, otherwise evict a victim.
    uint32_t allocate_frame(Result& r) {
        if (!free_frames.empty()) {
            uint32_t f = free_frames.back();
            free_frames.pop_back();
            return f;
        }
        int victim;
        if (opt.lru_frames)
            victim = min_element(frame_last_use.begin(), frame_last_use.end()) - frame_last_use.begin();
        else
            victim = rng() % opt.frames;
        r.evicted_frame = victim;
        r.evicted_vpn = (uint32_t)frame_owner[victim];
        unmap_frame(victim);
        evictions++;
        return victim;
    }

    // Part 4: the OS swaps out a random resident page (simulates other processes
    // needing memory); the next access to that page will page-fault.
    bool maybe_invalidate(uint32_t& vpn_out, int& frame_out) {
        if (opt.invalidate <= 0 || uniform_real_distribution<double>(0, 1)(rng) >= opt.invalidate) return false;
        vector<int> used;
        for (int f = 0; f < opt.frames; f++)
            if (frame_owner[f] != -1) used.push_back(f);
        if (used.empty()) return false;
        frame_out = used[rng() % used.size()];
        vpn_out = (uint32_t)frame_owner[frame_out];
        unmap_frame(frame_out);
        free_frames.push_back(frame_out);
        invalidations++;
        return true;
    }

    // Part 2: translation through the 2-level page table only (no TLB).
    uint32_t translate_2level(uint32_t va, Result& r) {
        walks++;
        r.cycles += 2 * MEM_CYCLES;                 // page directory + page table
        PageTableEntry& pte = walk(va);
        if (!pte.valid) {                           // page fault
            r.fault = true;
            faults++;
            pte.frame = allocate_frame(r);
            pte.valid = true;
            frame_owner[pte.frame] = get_vpn(va);
        }
        return pte.frame;
    }

    // Parts 3 + 4: VA -> (TLB) -> 2-level walk -> TLB insert -> PA
    Result translate(uint32_t va) {
        Result r;
        uint32_t vpn = get_vpn(va), pfn;
        time++;
        if (opt.use_tlb) {
            r.cycles += TLB_CYCLES;
            if (tlb.lookup(vpn, pfn)) {             // 1. TLB hit -> done
                r.tlb_hit = true;
            } else {
                pfn = translate_2level(va, r);      // 2. walk the page table
                tlb.insert(vpn, pfn);               // 3. update TLB (LRU)
            }
        } else {
            pfn = translate_2level(va, r);
        }
        frame_last_use[pfn] = time;
        cycles += r.cycles;
        r.pa = (pfn << PAGE_OFFSET_BITS) | get_offset(va);   // 4. physical address
        return r;
    }
};

// ============================================================================
// Input
// ============================================================================
vector<uint32_t> load_addresses(const string& file, bool warn) {
    vector<uint32_t> out;
    ifstream fin(file);
    if (!fin) {
        cerr << "Cannot open " << file << " (run ./generate first)\n";
        exit(1);
    }
    string tok;
    while (fin >> tok) {
        try {
            size_t used;
            unsigned long long v = stoull(tok, &used, 16);
            if (used != tok.size()) throw invalid_argument(tok);
            if (v > 0xFFFFFFFFull && warn)
                cout << "WARNING: " << tok << " is wider than 32 bits; using low 32 bits " << hex8(v & 0xFFFFFFFFull)
                     << "\n";
            out.push_back((uint32_t)(v & 0xFFFFFFFFull));
        } catch (...) {
            cerr << "Skipping invalid address: " << tok << "\n";
        }
    }
    return out;
}

// ============================================================================
// Runs
// ============================================================================
struct Summary {
    double hit_rate = 0, avg_cycles = 0, ns_per_translation = 0;
    long faults = 0, evictions = 0, invalidations = 0;
};

Summary run_trace(const vector<uint32_t>& addrs, const Options& opt, bool print, vector<uint32_t>* pas = nullptr) {
    MMU mmu(opt);
    for (uint32_t va : addrs) {
        uint32_t ivpn;
        int iframe;
        bool lines = print && !opt.quiet;          // --quiet: summary only
        if (mmu.maybe_invalidate(ivpn, iframe) && lines)
            cout << "INVALIDATE: VPN " << hex8(ivpn, 5) << " swapped out of frame " << dec << iframe << "\n";
        Result r = mmu.translate(va);
        if (pas) pas->push_back(r.pa);
        if (!lines) continue;
        if (r.fault) {
            cout << "PAGE FAULT: VA " << hex8(va) << " → ";
            if (r.evicted_frame >= 0)
                cout << "evicted frame " << dec << r.evicted_frame << " (VPN " << hex8(r.evicted_vpn, 5) << ")\n";
            else
                cout << "loaded into free frame " << dec << (r.pa >> PAGE_OFFSET_BITS) << "\n";
        }
        cout << "VA: " << hex8(va) << " → PA: " << hex8(r.pa) << (r.tlb_hit ? "  TLB hit" : "  TLB miss") << "\n";
    }
    Summary s;
    s.hit_rate = mmu.get_tlb().hit_rate();
    s.avg_cycles = addrs.empty() ? 0 : (double)mmu.cycles / addrs.size();
    s.faults = mmu.faults;
    s.evictions = mmu.evictions;
    s.invalidations = mmu.invalidations;
    if (print) {
        cout << "\n=== SUMMARY ===\n" << dec;
        cout << "Total Addresses: " << addrs.size() << "\n";
        cout << "Page Faults: " << mmu.faults << " (" << mmu.evictions << " evictions, " << mmu.invalidations
             << " random invalidations)\n";
        if (opt.use_tlb) mmu.get_tlb().print_stats();
        else cout << "TLB: disabled\n";
        cout << fixed << setprecision(2) << "Avg Translation Cost: " << s.avg_cycles << " cycles"
             << " (TLB hit 1, page walk 2 x " << MEM_CYCLES << ")\n";
        cout << "Page tables allocated: " << mmu.page_tables_allocated() << " of " << PD_ENTRIES << " ("
             << mmu.bytes() / 1024 << " KB vs " << (1u << VPN_BITS) * sizeof(PageTableEntry) / 1024
             << " KB for a direct-mapped table)\n";
    }
    return s;
}

// Wall-clock time per translation (no printing), averaged over many passes.
double time_ns(const vector<uint32_t>& addrs, Options opt, int reps) {
    MMU mmu(opt);
    volatile uint32_t sink = 0;
    auto start = high_resolution_clock::now();
    for (int r = 0; r < reps; r++)
        for (uint32_t va : addrs) sink = sink + mmu.translate(va).pa;
    auto end = high_resolution_clock::now();
    return duration_cast<nanoseconds>(end - start).count() / (double)(reps * addrs.size());
}

void part1(const vector<uint32_t>& addrs) {
    DirectPageTable dpt;
    cout << "Part 1: direct-mapped page table (" << (1u << VPN_BITS) << " entries, " << dpt.bytes() / (1024 * 1024)
         << " MB)\n";
    for (size_t i = 0; i < addrs.size() && i < 12; i++) {
        uint32_t pa = dpt.translate(addrs[i]);
        cout << "VA: " << hex8(addrs[i]) << " → PA: " << hex8(pa) << " (page " << hex8(get_vpn(addrs[i]), 5) << ")\n";
    }
    for (size_t i = 12; i < addrs.size(); i++) dpt.translate(addrs[i]);
    cout << "... translated all " << dec << addrs.size() << " addresses (1 memory reference each)\n";
}

// Random trace over exactly `pages` distinct pages (for the working-set experiment).
vector<uint32_t> working_set_trace(int pages, int n, unsigned seed) {
    mt19937 rng(seed);
    vector<uint32_t> base(pages);
    for (int i = 0; i < pages; i++) base[i] = 0x10000000u + (uint32_t)i * 0x1000u;
    vector<uint32_t> t(n);
    for (int i = 0; i < n; i++) t[i] = base[rng() % pages] + (rng() & 0xFFF);
    return t;
}

void experiment(const vector<uint32_t>& addrs, Options base) {
    base.quiet = true;
    const int RUNS = 5;
    cout << dec << fixed << setprecision(2);

    // 1. TLB size (averaged over 5 runs: the seed changes random evictions/invalidations).
    //    Also on a trace whose working set is 24 pages, so the effect of size is visible.
    cout << "Experiment 1: TLB size (average of " << RUNS << " runs)\n";
    cout << " TLB entries   addresses.txt hit rate   Avg cycles   Page faults   24-page working set hit rate\n";
    ofstream csv1("tlb_size.csv");
    csv1 << "tlb_entries,hit_rate,avg_cycles,faults,hit_rate_24_pages\n";
    for (int size : {4, 8, 16, 32, 64}) {
        double hr = 0, cyc = 0, f = 0, hr24 = 0;
        for (int run = 0; run < RUNS; run++) {
            Options o = base;
            o.tlb_size = size;
            o.seed = base.seed + run;
            Summary s = run_trace(addrs, o, false);
            hr += s.hit_rate; cyc += s.avg_cycles; f += s.faults;
            Options o24 = o;
            o24.invalidate = 0;
            hr24 += run_trace(working_set_trace(24, 20000, o.seed), o24, false).hit_rate;
        }
        cout << setw(12) << size << setw(24) << hr / RUNS << "%" << setw(13) << cyc / RUNS << setw(14) << f / RUNS
             << setw(30) << hr24 / RUNS << "%\n";
        csv1 << size << "," << hr / RUNS << "," << cyc / RUNS << "," << f / RUNS << "," << hr24 / RUNS << "\n";
    }

    // 2. Working set size (16-entry TLB, 256 frames, no random invalidation)
    cout << "\nExperiment 2: working set size (20000 random accesses over N pages, 16-entry TLB, "
         << base.frames << " frames)\n";
    cout << "    N pages   Hit rate   Page faults   Faults after warm-up\n";
    ofstream csv2("working_set.csv");
    csv2 << "pages,hit_rate,faults\n";
    for (int pages : {4, 8, 16, 32, 64, 128, 256, 512, 1024}) {
        Options o = base;
        o.invalidate = 0;
        vector<uint32_t> t = working_set_trace(pages, 20000, base.seed);
        Summary s = run_trace(t, o, false);
        long warm = max(0L, s.faults - min(pages, o.frames));   // faults beyond the first touch of each page
        cout << setw(11) << pages << setw(10) << s.hit_rate << "%" << setw(14) << s.faults << setw(23) << warm << "\n";
        csv2 << pages << "," << s.hit_rate << "," << s.faults << "\n";
    }

    // 3. With vs without TLB
    cout << "\nExperiment 3: translation time with and without the TLB (addresses.txt)\n";
    Options with = base, without = base;
    without.use_tlb = false;
    vector<uint32_t> pa1, pa2;
    Summary sw = run_trace(addrs, with, false, &pa1);
    Summary so = run_trace(addrs, without, false, &pa2);
    Options tw = with, to = without;
    tw.invalidate = to.invalidate = 0;            // timing: same work in both runs
    double nw = time_ns(addrs, tw, 2000), no = time_ns(addrs, to, 2000);
    cout << "                   Avg cycles (model)   Measured time\n";
    cout << " With TLB     " << setw(18) << sw.avg_cycles << setw(14) << nw << " ns\n";
    cout << " Without TLB  " << setw(18) << so.avg_cycles << setw(14) << no << " ns\n";
    cout << " (Measured time is for this software simulation: the TLB is a loop over 16 entries,\n"
         << "  while real hardware compares all entries at once.)\n";
    cout << " Speedup (cycle model): " << so.avg_cycles / sw.avg_cycles << "x\n";
    cout << " Same physical address for every access with and without TLB: " << (pa1 == pa2 ? "YES" : "NO") << "\n";
    ofstream csv3("with_without.csv");
    csv3 << "mode,avg_cycles,ns\nwith_tlb," << sw.avg_cycles << "," << nw << "\nwithout_tlb," << so.avg_cycles << ","
         << no << "\n";

    // 4. Victim frame: random vs LRU (512-page working set does not fit in 256 frames)
    cout << "\nExperiment 4: victim frame policy, 20000 accesses with locality over 512 pages\n";
    mt19937 rng(base.seed);
    vector<uint32_t> loc;
    for (int i = 0; i < 20000; i++) {
        int page = (rng() % 10 < 8) ? rng() % 200 : rng() % 512;   // 80% of accesses to 200 pages
        loc.push_back(0x20000000u + page * 0x1000u + (rng() & 0xFFF));
    }
    for (bool lru : {false, true}) {
        Options o = base;
        o.invalidate = 0;
        o.lru_frames = lru;
        Summary s = run_trace(loc, o, false);
        cout << (lru ? " LRU   " : " Random") << " victim: " << s.faults << " page faults\n";
    }
    cout << "\n(CSV files written: tlb_size.csv, working_set.csv, with_without.csv)\n";
}

int main(int argc, char* argv[]) {
    Options opt;
    string file = "addresses.txt", mode = "run";
    for (int i = 1; i < argc; i++) {
        string a = argv[i];
        auto next = [&]() -> string {
            if (i + 1 >= argc) { cerr << "Missing value after " << a << "\n"; exit(1); }
            return argv[++i];
        };
        try {
            if (a == "--part1") mode = "part1";
            else if (a == "--experiment") mode = "experiment";
            else if (a == "--tlb") opt.tlb_size = stoi(next());
            else if (a == "--frames") opt.frames = stoi(next());
            else if (a == "--seed") opt.seed = stoul(next());
            else if (a == "--invalidate") opt.invalidate = stod(next());
            else if (a == "--lru") opt.lru_frames = true;
            else if (a == "--no-tlb") opt.use_tlb = false;
            else if (a == "--quiet") opt.quiet = true;
            else if (a[0] == '-') { cerr << "Unknown option " << a << "\n"; return 1; }
            else file = a;
        } catch (...) { cerr << "Invalid value for " << a << "\n"; return 1; }
    }
    if (opt.tlb_size < 1 || opt.tlb_size > 1024 || opt.frames < 1 || opt.frames > (1 << 20) || opt.invalidate < 0 ||
        opt.invalidate > 1) {
        cerr << "Invalid options: TLB 1-1024 entries, frames 1-1048576, invalidate 0-1\n";
        return 1;
    }

    vector<uint32_t> addresses = load_addresses(file, mode != "experiment");
    if (mode == "part1") { part1(addresses); return 0; }
    if (mode == "experiment") { experiment(addresses, opt); return 0; }

    cout << "Starting translation with 2-level PT + "
         << (opt.use_tlb ? "TLB (" + to_string(opt.tlb_size) + "-entry LRU)" : string("no TLB")) << ", "
         << opt.frames << " frames, " << (opt.lru_frames ? "LRU" : "random") << " victim, seed " << opt.seed << "\n\n";
    run_trace(addresses, opt, true);
    Options t = opt;
    t.invalidate = 0;
    cout << "Translation Time (measured, no output): " << fixed << setprecision(1) << time_ns(addresses, t, 500)
         << " ns per address\n";
    return 0;
}
