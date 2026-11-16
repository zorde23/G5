#include "prx/libc/include/GuestArena.hpp"
#include <cstdlib>
#include <cstring>

static void Require(bool value) { if (!value) std::abort(); }

int main() {
    Require(!GuestArena::GuestArenaDonatedAny_nid_no_patch(0, 0));
    const char* fromIni = std::getenv("G5_INI_TEST");
    Require(fromIni != nullptr && std::strcmp(fromIni, "from ini") == 0);
    const char* kept = std::getenv("G5_INI_TEST_KEEP");
    Require(kept != nullptr && std::strcmp(kept, "environment") == 0);
    Require(std::getenv("G5_INI_TEST_COMMENT") == nullptr);
    return 0;
}
