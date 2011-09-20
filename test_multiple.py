import bjoern

def connect_callback():
    print 'connected'

def application(environ, start_response):
    start_response('200 OK', [('Content-Type', 'text/plain')])
    yield 'Hello, world!\n'

if __name__ == '__main__':
    workers = []
    for x in xrange(10):
        w = bjoern.WSGIServer(application, {'connect_callback': connect_callback})
        w.listen('', 9000 + x)
    bjoern.run()
