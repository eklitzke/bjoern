#include "request.h"

PyTypeObject WsgiServer_Type;
bool server_init(const char* hostaddr, const int port);
void server_run(void);
void _init_server(void);
