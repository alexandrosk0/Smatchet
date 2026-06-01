#include <thread>
void run() {
    std::thread t([]() {});
    // SMATCHET_DEVIATION(rule=no-detach; reason=test fixture; owner=alex; revisit=2099-12-31)
    t.detach();
}
