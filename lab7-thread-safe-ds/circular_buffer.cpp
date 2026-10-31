// circular_buffer.cpp - Exercise 4: Thread-Safe Circular Buffer
// Compile: g++ -o circular_buffer circular_buffer.cpp -pthread -std=c++11
//
// A fixed array of BUFFER_SIZE slots used as a ring: `in` is where the next
// item is written, `out` where the next item is read, both wrap around with
// % BUFFER_SIZE, and `count` is how many items are stored.
//   push() waits on not_full while the buffer is full, then signals not_empty.
//   pop()  waits on not_empty while the buffer is empty, then signals not_full.
// wait(lock, predicate) releases the mutex while sleeping and re-checks the
// predicate when it wakes, so spurious wakeups are handled.
//
// Changes to the starter:
//  - Added missing headers <iostream> and <random>.
//  - pop() returned false right away when the buffer was empty, so a consumer
//    that tried too early lost one of its turns and finished having consumed
//    fewer items than were produced. pop() now waits on not_empty. A close()
//    method wakes the consumers when no more items will come.
//  - The test used one producer and one consumer; the handout asks for several.
//    It now runs 2 producers and 2 consumers. Producers are faster than the
//    consumers, so the buffer fills up and producers have to wait.
//  - rand() replaced with per-thread generators; output lines kept whole.
#include <iostream>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <vector>
#include <chrono>
#include <random>
#include <atomic>

#define BUFFER_SIZE 5

std::mutex print_mtx;
#define PRINT(x) do { std::lock_guard<std::mutex> pl(print_mtx); std::cout << x << "\n"; } while (0)

class ThreadSafeCircularBuffer {
private:
    int buffer[BUFFER_SIZE];
    int in = 0, out = 0, count = 0;
    bool closed = false;
    std::mutex mtx;
    std::condition_variable not_full, not_empty;

public:
    std::atomic<int> times_full{0};   // how often a producer found the buffer full

    void push(int value) {
        std::unique_lock<std::mutex> lock(mtx);
        if (count == BUFFER_SIZE) times_full++;
        not_full.wait(lock, [this] { return count < BUFFER_SIZE; });
        buffer[in] = value;
        in = (in + 1) % BUFFER_SIZE;
        count++;
        lock.unlock();
        not_empty.notify_one();
    }

    // Waits for an item. Returns false only when closed and empty.
    bool pop(int& value) {
        std::unique_lock<std::mutex> lock(mtx);
        not_empty.wait(lock, [this] { return count > 0 || closed; });
        if (count == 0) return false;
        value = buffer[out];
        out = (out + 1) % BUFFER_SIZE;
        count--;
        lock.unlock();
        not_full.notify_one();
        return true;
    }

    void close() {                      // no more items will be pushed
        std::lock_guard<std::mutex> lock(mtx);
        closed = true;
        not_empty.notify_all();
    }

    bool empty() {
        std::lock_guard<std::mutex> lock(mtx);
        return count == 0;
    }

    bool full() {
        std::lock_guard<std::mutex> lock(mtx);
        return count == BUFFER_SIZE;
    }

    int size() {
        std::lock_guard<std::mutex> lock(mtx);
        return count;
    }
};

void circularBufferTest() {
    ThreadSafeCircularBuffer cb;
    std::vector<std::thread> producers, consumers;
    const int NUM_ITEMS = 10;            // per producer
    const int NUM_PRODUCERS = 2, NUM_CONSUMERS = 2;
    std::atomic<long> sum_produced(0), sum_consumed(0);
    std::atomic<int> consumed(0);

    auto producer = [&](int id) {
        std::mt19937 gen(7 + id);
        for (int i = 0; i < NUM_ITEMS; ++i) {
            int value = gen() % 100;
            cb.push(value);
            sum_produced += value;
            PRINT("Producer " << id << " produced: " << value << "   (buffer " << cb.size() << "/" << BUFFER_SIZE << ")");
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    };

    auto consumer = [&](int id) {
        int value;
        while (cb.pop(value)) {
            sum_consumed += value;
            consumed++;
            PRINT("    Consumer " << id << " consumed: " << value);
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
        }
    };

    for (int i = 0; i < NUM_PRODUCERS; ++i) producers.emplace_back(producer, i);
    for (int i = 0; i < NUM_CONSUMERS; ++i) consumers.emplace_back(consumer, i);

    for (auto& t : producers) t.join();
    cb.close();                          // producers done: let consumers finish
    for (auto& t : consumers) t.join();

    std::cout << "Produced " << NUM_PRODUCERS * NUM_ITEMS << " items, consumed " << consumed
              << ". Sum produced = " << sum_produced << ", sum consumed = " << sum_consumed
              << (sum_produced == sum_consumed ? " (match)" : " (MISMATCH)") << ".\n"
              << "Producers found the buffer full " << cb.times_full << " times and waited.\n";
}

int main() {
    circularBufferTest();
    return 0;
}
