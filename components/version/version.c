#include "version.h"

#ifndef FW_VERSION_STR
#define FW_VERSION_STR "0.0.0-stub"
#endif

const char* version_get_string(void) {
    return FW_VERSION_STR;
}
