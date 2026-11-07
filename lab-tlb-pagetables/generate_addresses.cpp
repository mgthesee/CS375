// generate_addresses.cpp - writes addresses.txt (random accesses + a hot region with locality)
// Build: make generate      Run: ./generate
//
// Changes to the starter generator:
//  - The comment said "Hot region: 100 pages", but 0x70000000-0x700FFFFF is
//    256 pages. The hot region is now really 100 pages (0x70000000-0x70063FFF).
//  - The starter wrote all 700 hot accesses first, each to a random page, so
//    there was almost no locality for the TLB to use. Real programs touch the
//    same page several times in a row (a loop, an array). Now each hot access
//    stays on the current page with probability 0.8 and otherwise jumps to a
//    random hot page, and the 300 random accesses are mixed in throughout.
//  - The three example addresses from the handout are written first, so the
//    output shows how they are translated (0x7fff5fbff000 is a 48-bit address).
#include <fstream>
#include <random>
#include <cstdint>
using namespace std;

int main() {
    ofstream fout("addresses.txt");
    mt19937 rng(42);                                      // fixed seed: same file every time
    uniform_int_distribution<uint32_t> dist(0, 0xFFFFFFFF);
    uniform_int_distribution<uint32_t> hot_page(0, 99);  // hot region: 100 pages
    uniform_int_distribution<uint32_t> offset(0, 0xFFF);
    uniform_real_distribution<double> coin(0.0, 1.0);
    const uint32_t HOT_BASE = 0x70000000;

    fout << "0x00000000\n0x7fff5fbff000\n0x00001abc\n";   // examples from the handout

    uint32_t page = hot_page(rng);
    for (int i = 0; i < 1000; i++) {
        uint32_t va;
        if (coin(rng) < 0.7) {                            // 70% hot region
            if (coin(rng) >= 0.8) page = hot_page(rng);   // 20%: jump to another hot page
            va = HOT_BASE + (page << 12) + offset(rng);
        } else {                                          // 30% anywhere in 4 GB
            va = dist(rng);
        }
        fout << hex << "0x" << va << "\n";
    }
}
