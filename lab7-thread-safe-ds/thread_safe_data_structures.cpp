// thread_safe_data_structures.cpp - Exercise 1: Thread-safe queue and stack,
// Producer-Consumer and Undo-Redo
// Compile: g++ -o thread_safe_data_structures thread_safe_data_structures.cpp -pthread -std=c++11
//
// Changes to the starter:
//  - Added missing headers <iostream> and <atomic>.
//  - Consumer termination: each consumer counted only ITS OWN messages and
//    looped until that reached 15, so with 2 consumers neither could get there.
//    It then stopped on "pop failed AND all messages produced", which has a
//    race: pop can fail, a producer pushes the last message, and the consumer
//    then sees produced == 15 and quits, leaving that message unprocessed.
//    Now the consumers share an atomic counter of consumed messages and stop
//    only when all 15 are consumed, so no message can be left behind.
//  - Output from several threads went to std::cout at the same time, so lines
//    could be mixed together; a print mutex keeps each line whole.
//  - Undo-Redo: each stack was thread-safe, but an operation (e.g. undo = pop
//    one stack, push the other, change the text) was not atomic as a whole.
//    A mutex now protects each complete operation and currentText.
#include <iostream>
#include <queue>
#include <stack>
#include <mutex>
#include <thread>
#include <vector>
#include <string>
#include <chrono>
#include <atomic>

std::mutex print_mtx;   // keeps output lines from different threads separate
#define PRINT(x) do { std::lock_guard<std::mutex> pl(print_mtx); std::cout << x << std::endl; } while (0)

// Thread-safe Queue class
template <typename T>
class ThreadSafeQueue {
private:
    std::queue<T> queue;
    std::mutex mtx;

public:
    ThreadSafeQueue() {}

    void push(T value) {
        std::lock_guard<std::mutex> lock(mtx);   // locked until the end of the function
        queue.push(value);
    }

    bool pop(T& value) {
        std::lock_guard<std::mutex> lock(mtx);   // check + read + remove as ONE step
        if (queue.empty()) return false;
        value = queue.front();
        queue.pop();
        return true;
    }

    bool empty() {
        std::lock_guard<std::mutex> lock(mtx);
        return queue.empty();
    }

    size_t size() {
        std::lock_guard<std::mutex> lock(mtx);
        return queue.size();
    }
};

// Thread-safe Stack class
template <typename T>
class ThreadSafeStack {
private:
    std::stack<T> stack;
    std::mutex mtx;

public:
    ThreadSafeStack() {}

    void push(T value) {
        std::lock_guard<std::mutex> lock(mtx);
        stack.push(value);
    }

    bool pop(T& value) {
        std::lock_guard<std::mutex> lock(mtx);
        if (stack.empty()) return false;
        value = stack.top();
        stack.pop();
        return true;
    }

    bool empty() {
        std::lock_guard<std::mutex> lock(mtx);
        return stack.empty();
    }

    size_t size() {
        std::lock_guard<std::mutex> lock(mtx);
        return stack.size();
    }
};

// Problem 1: Producer-Consumer Simulation
void producerConsumerProblem() {
    ThreadSafeQueue<std::string> messageQueue;
    const int NUM_PRODUCERS = 3;
    const int NUM_CONSUMERS = 2;
    const int MESSAGES_PER_PRODUCER = 5;
    const int TOTAL = NUM_PRODUCERS * MESSAGES_PER_PRODUCER;
    std::atomic<int> messages_produced(0);
    std::atomic<int> messages_consumed(0);   // shared by all consumers
    std::vector<int> per_consumer(NUM_CONSUMERS, 0);
    std::vector<std::thread> producers;
    std::vector<std::thread> consumers;

    auto producer = [&](int id) {
        for (int i = 0; i < MESSAGES_PER_PRODUCER; ++i) {
            std::string message = "Producer " + std::to_string(id) + " Message " + std::to_string(i);
            messageQueue.push(message);
            messages_produced++;
            PRINT("Produced: " << message);
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    };

    auto consumer = [&](int id) {
        // Stop only when ALL messages have been consumed (by any consumer).
        while (messages_consumed < TOTAL) {
            std::string message;
            if (messageQueue.pop(message)) {
                messages_consumed++;
                per_consumer[id]++;
                PRINT("Consumer " << id << " processed: " << message);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
        }
    };

    for (int i = 0; i < NUM_PRODUCERS; ++i) {
        producers.emplace_back(producer, i);
    }
    for (int i = 0; i < NUM_CONSUMERS; ++i) {
        consumers.emplace_back(consumer, i);
    }

    for (auto& t : producers) t.join();
    for (auto& t : consumers) t.join();
    std::cout << "Produced " << messages_produced << ", consumed " << messages_consumed << " (consumer 0: "
              << per_consumer[0] << ", consumer 1: " << per_consumer[1] << "), left in queue: " << messageQueue.size()
              << std::endl;
}

// Problem 2: Undo-Redo System
void undoRedoProblem() {
    ThreadSafeStack<std::string> undoStack;
    ThreadSafeStack<std::string> redoStack;
    std::string currentText = "";
    std::mutex editor_mtx;   // makes each whole operation atomic

    auto editText = [&](const std::string& newText) {
        std::lock_guard<std::mutex> lock(editor_mtx);
        undoStack.push(currentText);
        while (!redoStack.empty()) {           // a new edit clears the redo history
            std::string temp;
            redoStack.pop(temp);
        }
        currentText = newText;
        std::cout << "Text updated to: " << currentText << std::endl;
    };

    auto undo = [&]() {
        std::lock_guard<std::mutex> lock(editor_mtx);
        std::string prevText;
        if (undoStack.pop(prevText)) {
            redoStack.push(currentText);
            currentText = prevText;
            std::cout << "Undo - Current text: " << currentText << std::endl;
        } else {
            std::cout << "Undo - nothing to undo" << std::endl;
        }
    };

    auto redo = [&]() {
        std::lock_guard<std::mutex> lock(editor_mtx);
        std::string nextText;
        if (redoStack.pop(nextText)) {
            undoStack.push(currentText);
            currentText = nextText;
            std::cout << "Redo - Current text: " << currentText << std::endl;
        } else {
            std::cout << "Redo - nothing to redo" << std::endl;
        }
    };

    editText("Hello");
    editText("Hello World");
    editText("Hello Universe");
    undo();
    undo();
    redo();
    editText("Hello Galaxy");
    redo();   // added: redo history was cleared by the edit above
    std::cout << "Final text: \"" << currentText << "\" (undo stack: " << undoStack.size()
              << ", redo stack: " << redoStack.size() << ")" << std::endl;
}

int main() {
    std::cout << "Problem 1: Producer-Consumer Simulation\n";
    producerConsumerProblem();

    std::cout << "\nProblem 2: Undo-Redo System\n";
    undoRedoProblem();
    return 0;
}
