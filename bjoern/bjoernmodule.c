#include <Python.h>
#include "server.h"
#include "wsgi.h"
#include "bjoernmodule.h"
#include "filewrapper.h"

PyDoc_STRVAR(run_doc,
"run(application, host, port) -> None\n \
Calls listen(application, host, port) and starts the server mainloop.\n \
\n\
run() -> None\n \
Starts the server mainloop. listen(...) has to be called before calling \
run() without arguments.");
static PyObject*
run(PyObject* self, PyObject* args)
{
  if(PyTuple_GET_SIZE(args) == 0) {
    /* bjoern.run() */
    if(!wsgi_app) {
      PyErr_SetString(
        PyExc_RuntimeError,
        "Must call bjoern.listen(app, host, port) before "
        "calling bjoern.run() without arguments."
      );
      return NULL;
    }
  } else {
    /* bjoern.run(app, host, port) */
    if(!listen(self, args))
      return NULL;
  }

  server_run();
  wsgi_app = NULL;
  Py_RETURN_NONE;
}

static PyMethodDef Bjoern_FunctionTable[] = {
  {"run", run, METH_VARARGS, run_doc},
  {NULL, NULL, 0, NULL}
};

PyMODINIT_FUNC
initbjoern(void)
{
  PyObject *bjoern_module;
  _init_common();
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
  PyModule_AddObject(bjoern_module, "WsgiServer", (PyObject *) &WsgiServer_Type);
}
