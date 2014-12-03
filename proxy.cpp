#include <cstring>
#include <climits>
#include <unistd.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <sys/epoll.h>
#include <algorithm>

#include "proxy.hpp"
#include "message.hpp"

using namespace cerb;

namespace {

    int const MAX_EVENTS = 1024;

    void set_nonblocking(int sockfd) {
        int opts;

        opts = fcntl(sockfd, F_GETFL);
        if (opts < 0) {
            perror("fcntl(F_GETFL)\n");
            exit(1);
        }
        opts = (opts | O_NONBLOCK);
        if (fcntl(sockfd, F_SETFL, opts) < 0) {
            perror("fcntl(F_SETFL)\n");
            exit(1);
        }
    }

    int set_tcpnodelay(int sockfd)
    {
        int nodelay = 1;
        socklen_t len = sizeof nodelay;
        return setsockopt(sockfd, IPPROTO_TCP, TCP_NODELAY, &nodelay, len);
    }

    template <typename T>
    class AutoReleaser {
        T* _p;
    public:
        explicit AutoReleaser(T* p)
            : _p(p)
        {}

        ~AutoReleaser()
        {
            delete _p;
        }

        T* operator->() const
        {
            return _p;
        }

        void detach()
        {
            _p = nullptr;
        }
    };

    void loop(cerb::Proxy* p)
    {
        struct epoll_event events[MAX_EVENTS];
        int nfds = epoll_wait(p->epfd, events, MAX_EVENTS, -1);
        if (nfds == -1) {
            if (errno == EINTR) {
                return;
            }
            perror("epoll_pwait");
            exit(EXIT_FAILURE);
        }

        for (int i = 0; i < nfds; ++i) {
            AutoReleaser<Connection> conn(
                static_cast<Connection*>(events[i].data.ptr));
            conn->triggered(p, events[i].events);
            conn.detach();
        }
    }

}

Connection::~Connection()
{
    // printf("*Shut %d\n", fd);
    close(fd);
}

void Acceptor::triggered(Proxy* p, int)
{
    p->accept_from(this->fd);
}

Server::~Server()
{
    _proxy->shut_server(this);
}

void Server::triggered(Proxy*, int events)
{
    if (events & EPOLLRDHUP) {
        delete this;
        return;
    }
    if (events & EPOLLIN) {
        this->_recv_from();
    }
    if (events & EPOLLOUT) {
        this->_send_to();
    }
}

void Server::_send_to()
{
    if (this->_commands.empty()) {
        return;
    }
    if (!this->_ready_commands.empty()) {
        // printf("+busy commands\n");
        return;
    }

    std::vector<struct iovec> iov;
    int n = 0, ntotal = 0, written_iov = 0;

    this->_ready_commands = std::move(this->_commands);
    std::for_each(this->_ready_commands.begin(), this->_ready_commands.end(),
                  [&](util::sref<Command>& cmd)
                  {
                      cmd->buffer.buffer_ready(iov);
                      n += cmd->buffer.size();
                  });
//    printf("+write %d in %lu vectors (%d)\n", n, iov.size(), this->fd);
    int rest_iov = iov.size();

    while (written_iov < int(iov.size())) {
        int iovcnt = std::min(rest_iov, IOV_MAX);
//        printf("+ * write %d vectors (%d)\n", iovcnt, this->fd);
        int nwrite = writev(this->fd, iov.data() + written_iov, iovcnt);
        if (nwrite < 0) {
            if (errno == EAGAIN) {
                continue;
            }
            perror("+writev error");
            exit(1);
        }
        ntotal += nwrite;
        rest_iov -= iovcnt;
        written_iov += iovcnt;
    }

    if (ntotal != n) {
        perror("+writev error (but could recover)");
        fprintf(stderr, "  need to write %d but write %d\n", n, ntotal);
        exit(1);
    }

    struct epoll_event ev;
    ev.events = EPOLLIN | EPOLLET;
    ev.data.ptr = this;
    if (epoll_ctl(_proxy->epfd, EPOLL_CTL_MOD, this->fd, &ev) == -1) {
        perror("epoll_ctl: mod (w#)");
        exit(1);
    }
}

void Server::_recv_from()
{
    int n = this->_buffer.read(this->fd);
    // printf("+ read %d (%d)\n", n, this->fd);
    if (n == 0) {
        return;
    }
    try {
        auto messages(msg::split(this->_buffer.begin(), this->_buffer.end()));
        // printf("+msize %lld (%d) %c\n", messages.size(), this->fd, messages.size() == 0 ? 'X' : 'O');
        if (messages.size() > rint(this->_ready_commands.size())) {
            fprintf(stderr, " Error on split, expected %zu, actual %lld. Original message\n%s\n\n", this->_ready_commands.size(), messages.size(), this->_buffer.to_string().c_str());
            std::for_each(this->_ready_commands.begin(), this->_ready_commands.end(),
                          [&](util::sref<Command> cmd)
                          {
                              fprintf(stderr, " + Client <%lu> %s\n", cmd->buffer.size(), cmd->buffer.to_string().c_str());
                          });
            exit(1);
        }
        auto client_it = this->_ready_commands.begin();
        for (auto range = messages.begin(); range != messages.end();
             ++range, ++client_it)
        {
            if (client_it->nul()) {
                continue;
            }
            (*client_it)->copy_response(range.range_begin(), range.range_end());
        }

        this->_ready_commands.erase(this->_ready_commands.begin(), client_it);
        if (messages.finished()) {
            this->_buffer.clear();
        } else {
            this->_buffer.truncate_from_begin(messages.interrupt_point());
        }
    } catch (BadRedisMessage& e) {
        fprintf(stderr, "+Bad message %d\n%s\n\n", n, this->_buffer.to_string().c_str());
        exit(1);
    }
    struct epoll_event ev;
    ev.events = EPOLLIN | EPOLLOUT | EPOLLET;
    ev.data.ptr = this;
    if (epoll_ctl(_proxy->epfd, EPOLL_CTL_MOD, this->fd, &ev) == -1) {
        perror("epoll_ctl: mod output (sr)");
        exit(1);
    }
}

void Server::push_client_command(util::sref<Command> cmd)
{
    _commands.push_back(cmd);
}

void Server::pop_client(Client* cli)
{
    std::remove_if(this->_commands.begin(), this->_commands.end(),
                   [&](util::sref<Command>& cmd)
                   {
                       return cmd->group->client.is(cli);
                   });
    std::for_each(this->_ready_commands.begin(), this->_ready_commands.end(),
                  [&](util::sref<Command>& cmd)
                  {
                      if (cmd->group->client.is(cli)) {
                          cmd.reset();
                      }
                  });
}

Client::~Client()
{
    this->_proxy->shut_client(this);
}

void Client::triggered(Proxy*, int events)
{
    if (events & EPOLLRDHUP) {
        delete this;
        return;
    }
    if (events & EPOLLIN) {
        try {
            this->_recv_from();
        } catch (BadRedisMessage& e) {
            fprintf(stderr, "-Bad message %lu\n%s\n\n", this->buffer.size(),
                    this->buffer.to_string().c_str());
            exit(1);
        }
    }
    if (events & EPOLLOUT) {
        this->_send_to();
    }
}

void Client::_send_to()
{
    if (this->_awaiting_groups.empty()) {
        return;
    }
    if (!this->_ready_groups.empty()) {
        // printf("+busy commands\n");
        return;
    }

    std::vector<struct iovec> iov;
    int n = 0;

    this->_ready_groups = std::move(this->_awaiting_groups);
    std::for_each(this->_ready_groups.begin(), this->_ready_groups.end(),
                  [&](util::sptr<CommandGroup>& g)
                  {
                      g->append_buffer_to(iov);
                      n += g->total_buffer_size();
                  });

    // printf("-write %d (%d)\n", n, this->fd);
    while (true) {
        int nwrite = writev(this->fd, iov.data(), iov.size());
        if (nwrite <= 0 && errno == EAGAIN) {
            continue;
        }
        if (nwrite != n) {
            perror("-writev error (but should recover)");
            exit(1);
        }
        break;
    }
    this->_ready_groups.clear();

    struct epoll_event ev;
    ev.events = EPOLLIN | EPOLLET;
    ev.data.ptr = this;
    if (epoll_ctl(_proxy->epfd, EPOLL_CTL_MOD, this->fd, &ev) == -1) {
        perror("epoll_ctl: mod (w#)");
        exit(1);
    }
}

void Client::_recv_from()
{
    int n = this->buffer.read(this->fd);
    // printf("- read %d (%d)\n", n, this->fd);
    if (n == 0) {
        delete this;
        return;
    }

    auto messages(split_client_command(this->buffer, util::mkref(*this)));
    // printf("-msize %zu (%d)\n", messages.size(), this->fd);

    if (this->peer == nullptr) {
        this->peer = this->_proxy->connect_to("127.0.0.1", 6379);
    }
    Server* svr = this->peer;
    for (auto i = messages.begin(); i != messages.end(); ++i) {
        util::sptr<CommandGroup> g(std::move(*i));
        if (g->awaiting_count != 0) {
            // printf("-awaiting inc %d\n", g->awaiting_count);
            ++_awaiting_count;
            for (auto ci = g->commands.begin(); ci != g->commands.end(); ++ci) {
                svr->push_client_command(**ci);
                // printf(" to send %s\n", (*ci)->buffer.to_string().c_str());
            }
        }
        _awaiting_groups.push_back(std::move(g));
    }
    this->buffer.clear();
    // printf("-awaiting %d\n", _awaiting_count);
    if (0 < _awaiting_count) {
        struct epoll_event ev;
        ev.events = EPOLLIN | EPOLLOUT | EPOLLET;
        ev.data.ptr = svr;
        if (epoll_ctl(this->_proxy->epfd, EPOLL_CTL_MOD, svr->fd, &ev) == -1) {
            perror("epoll_ctl: mod output");
            exit(1);
        }
    } else {
        _response_ready();
    }
}

void Client::_response_ready()
{
    struct epoll_event ev;
    ev.events = EPOLLIN | EPOLLOUT | EPOLLET;
    ev.data.ptr = this;
    if (epoll_ctl(_proxy->epfd, EPOLL_CTL_MOD, this->fd, &ev) == -1) {
        perror("epoll_ctl: mod (w#)");
        exit(1);
    }
}

void Client::group_responsed()
{
    if (--_awaiting_count == 0) {
        _response_ready();
    }
    // printf("-await %d (%d) %c\n", _awaiting_count, this->fd, _awaiting_count == 0 ? 'O' : 'X');
}

Proxy::Proxy()
    : epfd(epoll_create(MAX_EVENTS))
    , server_conn(nullptr)
{
    if (epfd == -1) {
        throw std::runtime_error("epoll_create");
    }
}

Proxy::~Proxy()
{
    close(epfd);
}

void Proxy::run(int port)
{
    struct epoll_event ev;
    struct sockaddr_in local;

    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        perror("sockfd\n");
        exit(1);
    }
    cerb::Acceptor listen_conn(listen_fd);
    set_nonblocking(listen_conn.fd);
    int option = 1;
    if (setsockopt(listen_conn.fd, SOL_SOCKET, SO_REUSEPORT | SO_REUSEADDR,
                   &option, sizeof option) < 0)
    {
        perror("setsockopt");
        exit(1);
    }

    bzero(&local, sizeof(local));
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_ANY);;
    local.sin_port = htons(port);
    if (bind(listen_conn.fd, (struct sockaddr*)&local, sizeof local) < 0) {
        perror("bind\n");
        exit(1);
    }
    listen(listen_conn.fd, 20);

    ev.events = EPOLLIN | EPOLLET;
    ev.data.ptr = &listen_conn;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, listen_conn.fd, &ev) == -1) {
        perror("epoll_ctl: listen_sock");
        exit(EXIT_FAILURE);
    }

    while (true) {
        loop(this);
    }
}

Server* Proxy::connect_to(char const* host, int port)
{
    if (this->server_conn != nullptr) {
        return this->server_conn;
    }

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("Create socket");
        exit(1);
    }

    set_nonblocking(fd);
    set_tcpnodelay(fd);

    struct hostent* server = gethostbyname(host);
    if (server == nullptr) {
        perror("ERROR, no such host");
        exit(1);
    }
    struct sockaddr_in serv_addr;
    bzero(&serv_addr, sizeof serv_addr);
    serv_addr.sin_family = AF_INET;
    memcpy(&serv_addr.sin_addr.s_addr, server->h_addr, server->h_length);
    serv_addr.sin_port = htons(port);

    this->server_conn = new Server(fd, this);

    Server* c = this->server_conn;

    struct epoll_event ev;
    ev.events = EPOLLIN | EPOLLOUT | EPOLLET;
    ev.data.ptr = c;
    if (epoll_ctl(this->epfd, EPOLL_CTL_ADD, fd, &ev) == -1) {
        perror("epoll_ctl: mod output");
        exit(1);
    }

    if (connect(fd, (struct sockaddr*)&serv_addr, sizeof serv_addr) < 0) {
        if (errno == EINPROGRESS) {
            return c;
        }
        perror("ERROR connecting");
        exit(1);
    }

    return c;
}

void Proxy::accept_from(int listen_fd)
{
    int conn_sock;
    struct sockaddr_in remote;
    socklen_t addrlen = sizeof remote;
    while ((conn_sock = accept(listen_fd, (struct sockaddr*)&remote,
                               &addrlen)) > 0)
    {
        // printf("*Accepted %d\n", conn_sock);
        set_nonblocking(conn_sock);
        set_tcpnodelay(conn_sock);
        Connection* c = new Client(conn_sock, this);
        struct epoll_event ev;
        ev.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
        ev.data.ptr = c;
        if (epoll_ctl(this->epfd, EPOLL_CTL_ADD, conn_sock, &ev) == -1) {
            perror("epoll_ctl: add");
            exit(EXIT_FAILURE);
        }
    }
    if (conn_sock == -1) {
        if (errno != EAGAIN && errno != ECONNABORTED
            && errno != EPROTO && errno != EINTR)
        {
            perror("accept");
            exit(1);
        }
    }
}

void Proxy::shut_client(Client* cli)
{
    if (this->server_conn != nullptr) {
        this->server_conn->pop_client(cli);
    }
    epoll_ctl(this->epfd, EPOLL_CTL_DEL, cli->fd, NULL);
}

void Proxy::shut_server(Server*)
{
    this->server_conn = nullptr;
}