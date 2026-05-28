#include "Logger.h"
#include <memory>
struct Widget {};
void clean() {
    auto w = std::make_unique<Widget>();
    LOG_INFO("created widget");
    (void)w;
}
