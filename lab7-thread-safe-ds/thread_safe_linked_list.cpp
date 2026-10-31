// thread_safe_linked_list.cpp - Exercise 6: Thread-Safe Linked List
// Compile: g++ -o thread_safe_linked_list thread_safe_linked_list.cpp -pthread -std=c++11
//
// push_front and pop_front each change `head` in two steps (read head, then
// write head). Without the mutex two pushers could both read the same head and
// one node would be lost, or two poppers could both take the same node and
// delete it twice (crash / memory corruption). The mutex makes each operation
// a single indivisible step, so the list always stays a valid chain.
//
// Changes to the starter:
//  - Added missing <iostream>.
//  - The destructor called empty() and pop_front() separately (two locks);
//    it now frees the nodes directly - no other thread can use an object that
//    is being destroyed, so no lock is needed there.
//  - Copying the list would make two lists share (and double-delete) nodes, so
//    copying is disabled.
//  - The popper tried 10 times and skipped a try when the list was empty, so
//    items could be left in the list. It now pops until all 10 items are taken.
//  - Added a stress test: 4 threads x 10,000 push/pop pairs, then checks that
//    the list is empty and the count matches.
#include <iostream>
#include <mutex>
#include <thread>
#include <vector>
#include <chrono>
#include <atomic>

std::mutex print_mtx;
#define PRINT(x) do { std::lock_guard<std::mutex> pl(print_mtx); std::cout << x << "\n"; } while (0)

template <typename T>
class ThreadSafeLinkedList {
private:
    struct Node {
        T data;
        Node* next;
        Node(T value) : data(value), next(nullptr) {}
    };
    Node* head = nullptr;
    std::mutex mtx;

public:
    ThreadSafeLinkedList() = default;
    ThreadSafeLinkedList(const ThreadSafeLinkedList&) = delete;            // no shared nodes
    ThreadSafeLinkedList& operator=(const ThreadSafeLinkedList&) = delete;

    ~ThreadSafeLinkedList() {
        while (head) {
            Node* temp = head;
            head = head->next;
            delete temp;
        }
    }

    void push_front(T value) {
        Node* newNode = new Node(value);        // allocate outside the lock
        std::lock_guard<std::mutex> lock(mtx);
        newNode->next = head;
        head = newNode;
    }

    bool pop_front(T& value) {
        Node* temp;
        {
            std::lock_guard<std::mutex> lock(mtx);
            if (!head) return false;
            temp = head;
            value = temp->data;
            head = head->next;
        }
        delete temp;                            // node is unlinked: free it outside the lock
        return true;
    }

    bool empty() {
        std::lock_guard<std::mutex> lock(mtx);
        return head == nullptr;
    }

    size_t size() {
        std::lock_guard<std::mutex> lock(mtx);
        size_t count = 0;
        Node* current = head;
        while (current) {
            count++;
            current = current->next;
        }
        return count;
    }
};

void linkedListTest() {
    ThreadSafeLinkedList<int> list;
    std::vector<std::thread> threads;
    std::atomic<int> popped(0);

    auto pusher = [&list](int id) {
        for (int i = 0; i < 5; ++i) {
            list.push_front(id * 100 + i);
            PRINT("Thread " << id << " pushed: " << (id * 100 + i));
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    };

    auto popper = [&]() {
        while (popped < 10) {
            int value;
            if (list.pop_front(value)) {
                popped++;
                PRINT("    Popped: " << value);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
        }
    };

    threads.emplace_back(pusher, 1);
    threads.emplace_back(pusher, 2);
    threads.emplace_back(popper);

    for (auto& t : threads) t.join();
    std::cout << "Pushed 10, popped " << popped << ", list size now " << list.size() << "\n";

    // Stress test: each thread pushes then pops, 10,000 times
    ThreadSafeLinkedList<int> big;
    std::atomic<long> ok_pops(0);
    std::vector<std::thread> workers;
    for (int w = 0; w < 4; ++w)
        workers.emplace_back([&] {
            for (int i = 0; i < 10000; ++i) {
                int v;
                big.push_front(i);
                if (big.pop_front(v)) ok_pops++;
            }
        });
    for (auto& t : workers) t.join();
    std::cout << "Stress test (4 threads x 10000 push+pop): " << ok_pops << " successful pops, final size "
              << big.size() << (ok_pops == 40000 && big.empty() ? " (no nodes lost or freed twice)" : " (ERROR)") << "\n";
}

int main() {
    linkedListTest();
    return 0;
}
