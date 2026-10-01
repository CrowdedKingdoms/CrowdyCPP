// <windows.h> defines `far` and `near` as empty macros, and a Windows consumer often
// includes it before this SDK (CrowdyPy's extension does, for its wake socket). Every
// public header has to compile after them.
#define far
#define near

#include "crowdy/replication/connection.hpp"
#include "crowdy/session/chunk_store.hpp"
#include "crowdy/session/world_session.hpp"

int main() { return 0; }
