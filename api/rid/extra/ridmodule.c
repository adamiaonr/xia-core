#include <Python.h>
#include "rid.h"

static PyObject * rid_name_to_rid(PyObject * self, PyObject * args)
{
    char * name;
    char * rid_string;

    if ( ! PyArg_ParseTuple(args, "s", &name))
        return NULL;

    rid_string = name_to_rid((char *) name);

    return Py_BuildValue("s", rid_string);
}

static PyMethodDef rid_methods[] = {
    {"name_to_rid",  rid_name_to_rid, METH_VARARGS, 
    	"get a correctly formatted RID out of a URL-like prefix."},
    {NULL, NULL, 0, NULL}        /* Sentinel */
};

PyMODINIT_FUNC
initrid(void)
{
    (void) Py_InitModule("rid", rid_methods);
}
