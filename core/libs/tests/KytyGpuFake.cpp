#include <cstdint>
#include <cstring>

extern "C" std::uint64_t kyty_gpu_resolve(const char* nid) {
    return nid != nullptr && std::strcmp(nid, "kytyHasThis") == 0 ? 0x1234500 : 0;
}
