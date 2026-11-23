# SPDX-License-Identifier: GPL-3.0-or-later
# A TCP echo server on 127.0.0.1 for run_checks.sh.
import socketserver
import sys


class Echo(socketserver.BaseRequestHandler):
    def handle(self):
        while data := self.request.recv(4096):
            self.request.sendall(data)


socketserver.ThreadingTCPServer.allow_reuse_address = True
with socketserver.ThreadingTCPServer(("127.0.0.1", int(sys.argv[1])), Echo) as server:
    server.serve_forever()
