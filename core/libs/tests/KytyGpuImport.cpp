#include "prx/libc/include/CallTrace.hpp"
#include "prx/libc/include/general/VabiMacros.hpp"
#include <cstdint>
#include <cstdio>

namespace {

int failures = 0;

void Expect(bool condition, const char* what) {
    if (!condition) {
        std::fprintf(stderr, "FAILED: %s\n", what);
        ++failures;
    }
}

int Original() {
    return 7;
}

}

int main() {
    void* const original = reinterpret_cast<void*>(&Original);
    Expect(TraceImport_nid_no_patch(original, "kytyHasThis", "libSceAgcDriver.prx") == reinterpret_cast<void*>(std::uintptr_t{0x1234500}), "a NID Kyty has binds to Kyty");
    Expect(TraceImport_nid_no_patch(original, "kytyHasThis#C#D", "libScePad") == reinterpret_cast<void*>(std::uintptr_t{0x1234500}), "a routed library without .prx, and a NID with its library suffix, bind to Kyty");
    void* const missing = TraceImport_nid_no_patch(original, "kytyLacksThis", "libSceVideoOut.prx");
    Expect(missing != original && missing != nullptr, "a NID Kyty lacks binds to the stub, not to the g5 export");
    using Stub = std::uint64_t(APS5_VABI*)();
    Expect(reinterpret_cast<Stub>(missing)() == 0, "the stub returns 0");
    Expect(TraceImport_nid_no_patch(original, "kytyHasThis", "libSceAudioOut.prx") == original, "a library that is not routed keeps the g5 export");
    std::puts(failures == 0 ? "kyty gpu import: ok" : "kyty gpu import: failed");
    return failures == 0 ? 0 : 1;
}
