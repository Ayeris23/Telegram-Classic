/**
 * File              : net.c
 * Author            : Igor V. Sementsov <ig.kuzm@gmail.com>
 * Date              : 21.11.2024
 * Last Modified Date: 15.01.2025
 * Last Modified By  : Igor V. Sementsov <ig.kuzm@gmail.com>
 */

#include "../tg/tg.h"
#include <stdint.h>
#include <stdio.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>

#include "../tg/list.h"
#include "../tl/alloc.h"
#include "queue.h"
#include "transport.h"
#include "net.h"

static int tg_connect_with_timeout(
                                   int sockfd,
                                   const struct sockaddr *address,
                                   socklen_t address_length,
                                   int timeout_seconds)
{
    int original_flags;
    int result;
    int socket_error;
    int saved_errno;
    socklen_t socket_error_length;
    fd_set write_set;
    struct timeval timeout;
    
    original_flags = fcntl(sockfd, F_GETFL, 0);
    
    if (original_flags < 0)
        return -1;
    
    if (fcntl(sockfd,
              F_SETFL,
              original_flags | O_NONBLOCK) < 0)
    {
        return -1;
    }
    
    result = connect(
                     sockfd,
                     address,
                     address_length);
    
    if (result == 0) {
        fcntl(sockfd, F_SETFL, original_flags);
        return 0;
    }
    
    if (errno != EINPROGRESS) {
        saved_errno = errno;
        fcntl(sockfd, F_SETFL, original_flags);
        errno = saved_errno;
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
        fcntl(sockfd, F_SETFL, original_flags);
        errno = ETIMEDOUT;
        return -1;
    }
    
    if (result < 0) {
        saved_errno = errno;
        fcntl(sockfd, F_SETFL, original_flags);
        errno = saved_errno;
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
        saved_errno = errno;
        fcntl(sockfd, F_SETFL, original_flags);
        errno = saved_errno;
        return -1;
    }
    
    if (fcntl(sockfd, F_SETFL, original_flags) < 0)
        return -1;
    
    if (socket_error != 0) {
        errno = socket_error;
        return -1;
    }
    
    return 0;
}

static int tg_send_all(
                       int sockfd,
                       const void *data,
                       size_t length)
{
    const unsigned char *bytes;
    size_t total_sent;
    
    bytes = (const unsigned char *)data;
    total_sent = 0;
    
    while (total_sent < length) {
        ssize_t sent;
        
        sent = send(
                    sockfd,
                    bytes + total_sent,
                    length - total_sent,
                    0);
        
        if (sent > 0) {
            total_sent += (size_t)sent;
            continue;
        }
        
        if (sent < 0 && errno == EINTR)
            continue;
        
        return -1;
    }
    
    return 0;
}

int tg_net_open(tg_t *tg, const char *ip, int port)
{
    struct sockaddr_in serv_addr;
    struct hostent *server;
    struct timeval io_timeout;
    int sockfd;
    int no_sigpipe;
    const unsigned char transport_init[] = {
        0xee, 0xee, 0xee, 0xee
    };
    
    if (ip == NULL) {
        ON_ERR(tg, "%s: IP address is NULL", __func__);
        return -1;
    }
    
    printf("LIBTG NET: opening %s:%d\n", ip, port);
    fflush(stdout);
    
    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    
    if (sockfd < 0) {
        ON_ERR(tg,
               "%s: can't open socket: errno=%d (%s)",
               __func__,
               errno,
               strerror(errno));
        
        return -1;
    }
    
    /*
     * Prevent send() from terminating the process with SIGPIPE.
     */
    no_sigpipe = 1;
    
    if (setsockopt(
                   sockfd,
                   SOL_SOCKET,
                   SO_NOSIGPIPE,
                   &no_sigpipe,
                   sizeof(no_sigpipe)) != 0)
    {
        ON_ERR(tg,
               "%s: SO_NOSIGPIPE failed: errno=%d (%s)",
               __func__,
               errno,
               strerror(errno));
        
        close(sockfd);
        return -1;
    }
    
    /*
     * Prevent later send() and recv() calls from blocking forever.
     */
    io_timeout.tv_sec = 15;
    io_timeout.tv_usec = 0;
    
    if (setsockopt(
                   sockfd,
                   SOL_SOCKET,
                   SO_RCVTIMEO,
                   &io_timeout,
                   sizeof(io_timeout)) != 0)
    {
        ON_ERR(tg,
               "%s: SO_RCVTIMEO failed: errno=%d (%s)",
               __func__,
               errno,
               strerror(errno));
        
        close(sockfd);
        return -1;
    }
    
    if (setsockopt(
                   sockfd,
                   SOL_SOCKET,
                   SO_SNDTIMEO,
                   &io_timeout,
                   sizeof(io_timeout)) != 0)
    {
        ON_ERR(tg,
               "%s: SO_SNDTIMEO failed: errno=%d (%s)",
               __func__,
               errno,
               strerror(errno));
        
        close(sockfd);
        return -1;
    }
    
    server = gethostbyname(ip);
    
    if (server == NULL) {
        ON_ERR(tg,
               "%s: no host with ip: '%s'",
               __func__,
               ip);
        
        close(sockfd);
        return -1;
    }
    
    bzero(
          (char *)&serv_addr,
          sizeof(serv_addr));
    
    serv_addr.sin_family = AF_INET;
    
    bcopy(
          (char *)server->h_addr_list[0],
          (char *)&serv_addr.sin_addr.s_addr,
          server->h_length);
    
    serv_addr.sin_port = htons(port);
    
    printf("LIBTG NET: attempting TCP connection\n");
    fflush(stdout);
    
    if (tg_connect_with_timeout(
                                sockfd,
                                (struct sockaddr *)&serv_addr,
                                sizeof(serv_addr),
                                15) != 0)
    {
        ON_ERR(tg,
               "%s: connect failed: errno=%d (%s)",
               __func__,
               errno,
               strerror(errno));
        
        printf("LIBTG NET: connect failed: errno=%d (%s)\n",
               errno,
               strerror(errno));
        fflush(stdout);
        
        close(sockfd);
        return -1;
    }
    
    printf("LIBTG NET: TCP connected, fd=%d\n", sockfd);
    fflush(stdout);
    
    /*
     * Select Telegram's intermediate transport.
     */
    if (tg_send_all(
                    sockfd,
                    transport_init,
                    sizeof(transport_init)) != 0)
    {
        ON_ERR(tg,
               "%s: transport marker failed: errno=%d (%s)",
               __func__,
               errno,
               strerror(errno));
        
        printf("LIBTG NET: transport marker failed: "
               "errno=%d (%s)\n",
               errno,
               strerror(errno));
        fflush(stdout);
        
        shutdown(sockfd, SHUT_RDWR);
        close(sockfd);
        
        return -1;
    }
    
    printf("LIBTG NET: intermediate transport marker sent\n");
    fflush(stdout);
    
    return sockfd;
}

void tg_net_close(tg_t *tg, int sockfd)
{
    if (sockfd < 0)
        return;
    
    /*
     * shutdown() wakes a worker currently blocked in recv().
     * The code that owns the descriptor must ensure this function
     * is called only once for that descriptor.
     */
    shutdown(sockfd, SHUT_RDWR);
    
    if (close(sockfd) != 0) {
        ON_ERR(tg,
               "%s: close failed for fd=%d: errno=%d (%s)",
               __func__,
               sockfd,
               errno,
               strerror(errno));
    }
}