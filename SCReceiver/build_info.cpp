#ifndef SC2015_RELEASE_VERSION
#define SC2015_RELEASE_VERSION "development"
#endif

// Forced into the final image by the linker and checked by the binary audit.
// The date version is separate from the byte-sized diagnostic capability revision.
extern "C" const char sc2015_build_version[] = SC2015_RELEASE_VERSION;
