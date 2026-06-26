//
//  net.c
//  mtx
//
//  Created by Pavel Morozkin on 07.12.13.
//  Copyright (c) 2013 Pavel Morozkin. All rights reserved.
//

#include "../include/api.h"
#include "../include/net.h"
#include "../include/buf.h"

#include <string.h>
#include <sys/types.h>
#include <stdio.h>
#include <strings.h>

#ifdef __GNUC__
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/select.h>
#include <sys/time.h>
#include <unistd.h>

static int net_connect_with_timeout(
                                    int sockfd,
                                    const struct sockaddr *address,
                                    socklen_t address_length,
                                    int timeout_seconds)
{
    int original_flags;
    int result;
    int socket_error;
    socklen_t socket_error_length;
    fd_set write_set;
    struct timeval timeout;
    
    original_flags = fcntl(sockfd, F_GETFL, 0);
    
    if (original_flags < 0) {
        return -1;
    }
    
    if (fcntl(sockfd, F_SETFL, original_flags | O_NONBLOCK) < 0) {
        return -1;
    }
    
    result = connect(sockfd, address, address_length);
    
    if (result == 0) {
        fcntl(sockfd, F_SETFL, original_flags);
        return 0;
    }
    
    if (errno != EINPROGRESS) {
        fcntl(sockfd, F_SETFL, original_flags);
        return -1;
    }
    
    FD_ZERO(&write_set);
    FD_SET(sockfd, &write_set);
    
    timeout.tv_sec = timeout_seconds;
    timeout.tv_usec = 0;
    
    result = select(
                    sockfd + 1,
                    NULL,
                    &write_set,
                    NULL,
                    &timeout);
    
    if (result == 0) {
        errno = ETIMEDOUT;
        fcntl(sockfd, F_SETFL, original_flags);
        return -1;
    }
    
    if (result < 0) {
        fcntl(sockfd, F_SETFL, original_flags);
        return -1;
    }
    
    socket_error = 0;
    socket_error_length = sizeof(socket_error);
    
    if (getsockopt(
                   sockfd,
                   SOL_SOCKET,
                   SO_ERROR,
                   &socket_error,
                   &socket_error_length) != 0)
    {
        fcntl(sockfd, F_SETFL, original_flags);
        return -1;
    }
    
    fcntl(sockfd, F_SETFL, original_flags);
    
    if (socket_error != 0) {
        errno = socket_error;
        return -1;
    }
    
    return 0;
}

static int net_send_all(
                        int sockfd,
                        const void *data,
                        size_t length)
{
    const unsigned char *bytes;
    size_t total_sent;
    
    bytes = (const unsigned char *)data;
    total_sent = 0;
    
    while (total_sent < length) {
        ssize_t sent = send(
                            sockfd,
                            bytes + total_sent,
                            length - total_sent,
                            0);
        
        if (sent > 0) {
            total_sent += (size_t)sent;
            continue;
        }
        
        if (sent < 0 && errno == EINTR) {
            continue;
        }
        
        return -1;
    }
    
    return 0;
}

net_t net_open(const _string_t ip, ui32_t port)
{
    net_t net;
    struct sockaddr_in serv_addr;
    struct hostent *server;
    struct timeval io_timeout;
    int no_sigpipe;
    const unsigned char init[] = {
        0xee, 0xee, 0xee, 0xee
    };
    
    net.sockfd = -1;
    
    printf("MTX NET: opening connection to %s:%u\n",
           ip ? ip : "(null)",
           (unsigned int)port);
    fflush(stdout);
    
    if (ip == NULL) {
        api.log.error("net_open: IP address is NULL");
        return net;
    }
    
    net.sockfd = socket(AF_INET, SOCK_STREAM, 0);
    
    if (net.sockfd < 0) {
        printf("MTX NET: socket failed: errno=%d (%s)\n",
               errno,
               strerror(errno));
        fflush(stdout);
        
        api.log.error("net_open: can't open socket");
        return net;
    }
    
    /*
     * Prevent SIGPIPE from terminating the application.
     */
    no_sigpipe = 1;
    
    if (setsockopt(
                   net.sockfd,
                   SOL_SOCKET,
                   SO_NOSIGPIPE,
                   &no_sigpipe,
                   sizeof(no_sigpipe)) != 0)
    {
        printf("MTX NET: SO_NOSIGPIPE failed: errno=%d (%s)\n",
               errno,
               strerror(errno));
        fflush(stdout);
        
        close(net.sockfd);
        net.sockfd = -1;
        return net;
    }
    
    /*
     * Cap blocking send() and recv() calls.
     */
    io_timeout.tv_sec = 15;
    io_timeout.tv_usec = 0;
    
    if (setsockopt(
                   net.sockfd,
                   SOL_SOCKET,
                   SO_RCVTIMEO,
                   &io_timeout,
                   sizeof(io_timeout)) != 0)
    {
        printf("MTX NET: SO_RCVTIMEO failed: errno=%d (%s)\n",
               errno,
               strerror(errno));
        fflush(stdout);
        
        close(net.sockfd);
        net.sockfd = -1;
        return net;
    }
    
    if (setsockopt(
                   net.sockfd,
                   SOL_SOCKET,
                   SO_SNDTIMEO,
                   &io_timeout,
                   sizeof(io_timeout)) != 0)
    {
        printf("MTX NET: SO_SNDTIMEO failed: errno=%d (%s)\n",
               errno,
               strerror(errno));
        fflush(stdout);
        
        close(net.sockfd);
        net.sockfd = -1;
        return net;
    }
    
    server = gethostbyname(ip);
    
    if (server == NULL) {
        printf("MTX NET: host lookup failed for %s\n", ip);
        fflush(stdout);
        
        api.log.error("net_open: no such host");
        
        close(net.sockfd);
        net.sockfd = -1;
        return net;
    }
    
    bzero((char *)&serv_addr, sizeof(serv_addr));
    
    serv_addr.sin_family = AF_INET;
    
    bcopy(
          (char *)server->h_addr_list[0],
          (char *)&serv_addr.sin_addr.s_addr,
          server->h_length);
    
    serv_addr.sin_port = htons(port);
    
    printf("MTX NET: attempting TCP connection\n");
    fflush(stdout);
    
    if (net_connect_with_timeout(
                                 net.sockfd,
                                 (struct sockaddr *)&serv_addr,
                                 sizeof(serv_addr),
                                 15) != 0)
    {
        printf("MTX NET: connect failed: errno=%d (%s)\n",
               errno,
               strerror(errno));
        fflush(stdout);
        
        api.log.error("net_open: can't connect");
        
        close(net.sockfd);
        net.sockfd = -1;
        return net;
    }
    
    printf("MTX NET: TCP connected, fd=%d\n", net.sockfd);
    fflush(stdout);
    
    /*
     * Select Telegram's intermediate transport.
     */
    if (net_send_all(net.sockfd, init, sizeof(init)) != 0) {
        printf("MTX NET: transport marker failed: errno=%d (%s)\n",
               errno,
               strerror(errno));
        fflush(stdout);
        
        api.log.error("net_open: can't initialize transport");
        
        close(net.sockfd);
        net.sockfd = -1;
        return net;
    }
    
    printf("MTX NET: intermediate transport marker sent\n");
    fflush(stdout);
    
    /*
     * Publish the descriptor only after the complete connection
     * and transport initialization have succeeded.
     */
    shared_rc.net.sockfd = net.sockfd;
    
    return net;
}

void net_close(net_t net)
{
    int sockfd = net.sockfd;
    
    if (sockfd < 0) {
        return;
    }
    
    shutdown(sockfd, SHUT_RDWR);
    close(sockfd);
    
    if (shared_rc.net.sockfd == sockfd) {
        shared_rc.net.sockfd = -1;
    }
}

void net_send(const buf_t_ buf)
{
  net_t net = shared_rc_get_net();
  i32_t n = (i32_t)send(net.sockfd, buf.data, buf.size, 0);

  if (n < 0) {
    api.log.error("ERROR writing to socket");
  }
}

buf_t_ net_receive()
{
  buf_t_ buf;
	/*buf_init(&buf);*/
  net_t net = shared_rc_get_net();
  i32_t n = (i32_t)recv(net.sockfd, buf.data, max_buf_size, 0);

  if (n < 0) {
    api.log.error("ERROR reading from socket");
  }

  buf.size = n;

  return buf;
}

#elif defined _MSC_VER /*WIN32*/
#include <io.h>
#include <winsock.h>

SOCKET s;
net_t net_open(const string_t ip, ui32_t port)
{
  net_t n = {};
  WSADATA wsa;
  struct sockaddr_in server;

  if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
    //printf("Failed. Error Code : %d",WSAGetLastError());
  }

  if ((s = socket(AF_INET , SOCK_STREAM , 0)) == INVALID_SOCKET) {
    //printf("Could not create socket : %d" , WSAGetLastError());
  }

  server.sin_addr.s_addr = inet_addr(ip);
  server.sin_family = AF_INET;
  server.sin_port = htons(port);

  if (connect(s , (struct sockaddr *)&server , sizeof(server)) < 0) {
    //puts("connect error");
  }

  return n;
}

void net_close(const net_t net)
{
  closesocket(s);
}

void net_send(const buf_t_ buf)
{
  if (send(s , (const char *)buf.data , buf.size , 0) < 0) {
    //puts("Send failed");
  }
}

buf_t_ net_receive()
{
  buf_t_ buf;
  i32_t n = (i32_t)recv(s, (char *)buf.data, max_buf_size, 0);

  if (n < 0) {
    api.log.error("ERROR reading from socket");
  }

  buf.size = n;

  return buf;
}

#else
#error Do not know which socket model we will use
#endif

buf_t_ net_drive(const buf_t_ buf, stk_mode_t m)
{
  net_send(buf);
  buf_t_ r = {};
	/*buf_init(&r);*/

  switch (m) {
    case SEND_RECEIVE:
    {
      r = net_receive();

      break;
    }
    case SEND:
    {
      break;
    }
    default:
    {
      break;
    }
  }

  return r;
}
