#ifndef __server_h_
#define __server_h_

#include "request.h"

PyTypeObject WsgiServer_Type;

bool server_init(const char* hostaddr, const int port);
bool server_run(void);
bool _init_server(void);


#endif
