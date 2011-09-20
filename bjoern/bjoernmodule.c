#include <Python.h>
#include "request.h"
#include "server.h"
#include "wsgi.h"
#include "filewrapper.h"

PyDoc_STRVAR(run_doc,
"run() -> None\n \
Run the applications");
static PyObject*
run(PyObject* self)
{
  /* This is a METH_NOARGS, so ensure self is NULL */
  if (self != NULL) {
    return NULL;
  }
  server_run();
  Py_RETURN_NONE;
}

static PyMethodDef Bjoern_FunctionTable[] = {
  {"run", run, METH_NOARGS, run_doc},
  {NULL, NULL, 0, NULL}
};

PyMODINIT_FUNC
initbjoern(void)
{
  PyObject *bjoern_module;
  _init_common();
  _init_request();
  _init_server();
  _init_filewrapper();

  if (PyType_Ready(&WsgiServer_Type) < 0)
    return;
  if (PyType_Ready(&FileWrapper_Type) < 0)
    return;
  if (PyType_Ready(&StartResponse_Type) < 0)
    return;

  bjoern_module = Py_InitModule3("bjoern", Bjoern_FunctionTable, "bjoern WSGI/HTTP gateway");

  Py_INCREF(&WsgiServer_Type);
  Py_INCREF(&FileWrapper_Type);
  Py_INCREF(&StartResponse_Type);

  PyModule_AddObject(bjoern_module, "version", Py_BuildValue("(ii)", 1, 2));
  PyModule_AddObject(bjoern_module, "WSGIServer", (PyObject *) &WsgiServer_Type);
}
