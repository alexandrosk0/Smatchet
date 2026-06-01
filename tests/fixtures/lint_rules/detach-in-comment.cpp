// A comment mentioning std::thread().detach() must NOT trip the no-detach rule —
// migration notes routinely reference the forbidden form they replaced.
#include <string>
void run() {
    // We use LaunchBackgroundTask instead of std::thread().detach() here.
    std::string s = ".detach()";  // string-literal mention must NOT fire either
    (void)s;
}
