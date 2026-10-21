// Root certificates the runtime always trusts for https:// (runtime/net/http.c), whatever the system has.
#pragma once

// GTS Root R1 / R4, GlobalSign Root CA, ISRG Root X1 / X2 as one NUL-terminated PEM string (public data).
const char *net_root_pems(void);
