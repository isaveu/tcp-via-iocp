#include "context.h"
#include "acceptor.h"

namespace Networking
{

Context::Context() : _acceptor(nullptr)
{
}

void Context::Init(const char* address, unsigned port)
{
    WSA_CHECK
    (
        _winsock.IsInitialized(),
        "Failed to initialize WinSock, maybe wrong version"
    );

    // Create a handle for the completion port
    _completion_port = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
    WSA_CHECK(!!_completion_port, "Failed to create IO Completion port");

    // Init the socket
    _socket.Init(address, port);

    // Associate the socket with the completion port
    WSA_CHECK
    (
        !!CreateIoCompletionPort(reinterpret_cast<HANDLE>(_socket.Native()), _completion_port, 0, 0),
        "Failed to associate listening socket with the IO Completion port"
    );
}

void Context::AsyncRead(const Connection* conn)
{
    auto overlapped = conn->GetReadOverlapped();
    overlapped->wsa_buf.len = overlapped->connection->ReadBufferSize;
    overlapped->wsa_buf.buf = reinterpret_cast<CHAR*>(overlapped->connection->GetReadBuffer());

    DWORD flags = 0;
    DWORD bytes_transferred = 0;

    auto recv_result = WSARecv(overlapped->connection->GetSocket(), &overlapped->wsa_buf, 1, &bytes_transferred, &flags, reinterpret_cast<LPWSAOVERLAPPED>(overlapped), NULL);
    CHECK
    (
        recv_result == NULL || (recv_result == SOCKET_ERROR && WSAGetLastError() == WSA_IO_PENDING),
        "Failed to receive data"
    );
}

void Context::AsyncWrite(const Connection* conn, void* data, std::size_t size)
{
    auto mutable_conn = const_cast<Connection*>(conn);

    if (mutable_conn->GetWriteBufferSize() < size)
        mutable_conn->ResizeWriteBuffer(size);

    memcpy_s(mutable_conn->GetWriteBuffer(), mutable_conn->GetWriteBufferSize(), data, size);

    mutable_conn->SetSentBytes(0);
    mutable_conn->SetTotalBytes(size);

    auto overlapped = mutable_conn->GetWriteOverlapped();
    overlapped->wsa_buf.len = size;
    overlapped->wsa_buf.buf = reinterpret_cast<CHAR*>(mutable_conn->GetWriteBuffer());

    DWORD bytes;
    auto send_result = WSASend(mutable_conn->GetSocket(), &overlapped->wsa_buf, 1, &bytes, 0, reinterpret_cast<LPWSAOVERLAPPED>(overlapped), NULL);

    CHECK
    (
        send_result == NULL || (send_result == SOCKET_ERROR && WSAGetLastError() == WSA_IO_PENDING),
        "Failed to send data"
    );
}

void Context::MainLoop()
{
    DWORD bytes_transferred;
    ULONG_PTR completion_key;
    DWORD Flags = 0;
    Overlapped* overlapped = nullptr;

    while (GetQueuedCompletionStatus(_completion_port, &bytes_transferred, &completion_key, reinterpret_cast<LPOVERLAPPED*>(&overlapped), INFINITE))
    {
        if (!overlapped)
            continue;

        if (overlapped->type == Overlapped::Type::Accept)
        {
            // server accepted new connection from client
            if (_acceptor)
                _acceptor->Start();

            if (OnConnected)
                OnConnected(overlapped->connection);

            continue;
        }

        if (overlapped->type == Overlapped::Type::Connect)
        {
            // client connected to the server
            if (OnConnected)
                OnConnected(overlapped->connection);

            continue;
        }

        if (bytes_transferred == 0)
        {
            // remote side disconnected
            if (OnDisconnected)
                OnDisconnected(overlapped->connection);

            delete overlapped->connection;
            overlapped = nullptr;

            continue;
        }

        if (overlapped->type == Overlapped::Type::Read)
        {
            // async read operation fihished
            if (OnRead)
                OnRead(overlapped->connection, overlapped->connection->GetReadBuffer(), bytes_transferred);

            continue;
        }

        if (overlapped->type == Overlapped::Type::Write)
        {
            // async write operation fihished or only the part
            auto conn = overlapped->connection;

            conn->SetSentBytes(conn->GetSentBytes() + bytes_transferred);

            if (conn->GetSentBytes() < conn->GetTotalBytes())
            {
                // need to write more
                overlapped->wsa_buf.len = conn->GetTotalBytes() - conn->GetSentBytes();
                overlapped->wsa_buf.buf = reinterpret_cast<CHAR*>(conn->GetWriteBuffer()) + conn->GetSentBytes();

                auto send_result = WSASend(conn->GetSocket(), &overlapped->wsa_buf, 1, &bytes_transferred, 0, reinterpret_cast<LPWSAOVERLAPPED>(overlapped), NULL);
                CHECK
                (
                    send_result == NULL || (send_result == SOCKET_ERROR && WSAGetLastError() == WSA_IO_PENDING),
                    "Failed to send data"
                );
            }
            else
            {
                // async write operation fully completed
                if (OnWrite)
                    OnWrite(overlapped->connection, bytes_transferred);
            }
        }
    }
}

Socket& Context::GetSocket()
{
    return _socket;
}

HANDLE Context::GetCompletionPort()
{
    return _completion_port;
}
}