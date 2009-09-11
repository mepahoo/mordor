// Copyright (c) 2009 - Decho Corp.

#include "mordor/common/pch.h"

#include <iostream>

#include <boost/bind.hpp>
#include <boost/shared_ptr.hpp>

#include "mordor/common/config.h"
#include "mordor/common/exception.h"
#include "mordor/common/http/basic.h"
#include "mordor/common/http/client.h"
#include "mordor/common/iomanager.h"
#include "mordor/common/socket.h"
#include "mordor/common/streams/socket.h"
#include "mordor/common/streams/ssl.h"
#include "mordor/common/streams/std.h"
#include "mordor/common/streams/transfer.h"

HTTP::ClientConnection::ptr establishConn(IOManager &ioManager, Address::ptr address, bool ssl)
{
    Socket::ptr s(address->createSocket(ioManager));
    s->connect(address);
    Stream::ptr stream(new SocketStream(s));
    if (ssl)
        stream.reset(new SSLStream(stream));

    HTTP::ClientConnection::ptr conn(new HTTP::ClientConnection(stream));
    return conn;
}

int main(int argc, const char *argv[])
{
    Config::loadFromEnvironment();
    StdoutStream stdoutStream;
    Fiber::ptr mainfiber(new Fiber());
    IOManager ioManager;
    try {
        URI uri = argv[1];
        ASSERT(uri.authority.hostDefined());
        ASSERT(!uri.schemeDefined() || uri.scheme() == "http" || uri.scheme() == "https");

        std::string username, password, proxy, proxyUsername, proxyPassword;

        if (argc == 3 || argc == 5 || argc == 7) {
            proxy = argv[2];
            --argc;
            ++argv;
        }

        if (argc == 4 || argc == 6) {
            username = argv[2];
            password = argv[3];
        }
        if (argc == 6) {
            proxyUsername = argv[4];
            proxyPassword = argv[5];
        }

        std::vector<Address::ptr> addresses =
        Address::lookup(proxy.empty() ? uri.authority.host() : proxy, AF_UNSPEC, SOCK_STREAM);
        if (proxy.empty()) {
            IPAddress *addr = dynamic_cast<IPAddress *>(addresses[0].get());
            ASSERT(addr);
            if (uri.authority.portDefined()) {
                addr->port(uri.authority.port());
            } else if (uri.schemeDefined() && uri.scheme() == "https") {
                addr->port(443);
            } else {
                addr->port(80);
            }
        }
        
        HTTP::ClientConnection::ptr conn = establishConn(ioManager, addresses[0], proxy.empty() && uri.schemeDefined() && uri.scheme() == "https");
        HTTP::Request requestHeaders;
        if (proxy.empty())
            requestHeaders.requestLine.uri.path = uri.path;
        else
            requestHeaders.requestLine.uri = uri;
        requestHeaders.request.host = uri.authority.host();
        HTTP::ClientRequest::ptr request = conn->request(requestHeaders);
        if (request->response().status.status == HTTP::PROXY_AUTHENTICATION_REQUIRED &&
            (!proxyUsername.empty() || !proxyPassword.empty())) {
            request->finish();
            HTTP::BasicAuth::authorize(requestHeaders, proxyUsername, proxyPassword, true);
            try {
                request = conn->request(requestHeaders);
                request->ensureResponse();
            } catch (SocketException) {
                conn = establishConn(ioManager, addresses[0], proxy.empty() && uri.schemeDefined() && uri.scheme() == "https");
                request = conn->request(requestHeaders);
            } catch (HTTP::IncompleteMessageHeaderException) {
                conn = establishConn(ioManager, addresses[0], proxy.empty() && uri.schemeDefined() && uri.scheme() == "https");
                request = conn->request(requestHeaders);
            }
        }
        if (request->response().status.status == HTTP::UNAUTHORIZED &&
            (!username.empty() || !password.empty())) {
            request->finish();
            HTTP::BasicAuth::authorize(requestHeaders, username, password);
            request = conn->request(requestHeaders);
        }
        if (request->hasResponseBody()) {
            try {
                if (request->response().entity.contentType.type != "multipart") {
                    transferStream(request->responseStream(), stdoutStream);
                } else {
                    Multipart::ptr responseMultipart = request->responseMultipart();
                    for (BodyPart::ptr bodyPart = responseMultipart->nextPart(); bodyPart;
                        bodyPart = responseMultipart->nextPart()) {
                        transferStream(bodyPart->stream(), stdoutStream);
                    }                        
                }
            } catch(...) {
                request->cancel();
                throw;
            }
        }
    } catch (std::exception& ex) {
        std::cerr << "Caught " << typeid(ex).name() << ": "
                  << ex.what( ) << std::endl;
    }
    ioManager.stop();
    return 0;
}
