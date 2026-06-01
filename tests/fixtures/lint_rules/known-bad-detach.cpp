#include <thread>
void run() {
    std::thread t([]() {});
    t.detach();
}
