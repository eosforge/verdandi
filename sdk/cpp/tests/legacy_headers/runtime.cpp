#include "verdandi/legacy/runtime.hpp"

bool verdandi_legacy_runtime_header_test() {
    return verdandi::legacy::has_capability("client");
}
