#pragma once
#ifdef _WIN32
#include <winsock2.h>

extern "C" void RefuseClosedLoopbackAtOnce_nid_no_patch(SOCKET socket, const sockaddr* address);
extern "C" bool NoteOffHostSocket_nid_no_patch(const char* call, const sockaddr* address);
extern "C" bool NetLocalOnly_nid_no_patch();
extern "C" bool KeepBindLocal_nid_no_patch(sockaddr* address);
extern "C" bool RefuseOffHost_nid_no_patch(const sockaddr* address);
extern "C" bool RefuseHostLookup_nid_no_patch();
#endif
