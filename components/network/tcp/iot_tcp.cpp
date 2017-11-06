/*
  * ESPRESSIF MIT License
  *
  * Copyright (c) 2017 <ESPRESSIF SYSTEMS (SHANGHAI) PTE LTD>
  *
  * Permission is hereby granted for use on ESPRESSIF SYSTEMS products only, in which case,
  * it is free of charge, to any person obtaining a copy of this software and associated
  * documentation files (the "Software"), to deal in the Software without restriction, including
  * without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense,
  * and/or sell copies of the Software, and to permit persons to whom the Software is furnished
  * to do so, subject to the following conditions:
  *
  * The above copyright notice and this permission notice shall be included in all copies or
  * substantial portions of the Software.
  *
  * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
  * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
  * FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
  * COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
  * IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
  * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
  *
  */

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_log.h"

#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include "lwip/netdb.h"
#include "lwip/dns.h"
#include "iot_tcp.h"
static const char* TAG = "tcp_connection";
static const char* TAG_SERVER = "tcp_server";

static int cConnect(int s, const struct sockaddr *name, socklen_t namelen)
{
    return lwip_connect_r(s, name, namelen);
}

static int cListen(int s, int backlog)
{
    return lwip_listen_r(s, backlog);
}

static int cAccept(int s, struct sockaddr *addr, socklen_t *addrlen)
{
    return lwip_accept_r(s, addr, addrlen);
}

CTcpConn::CTcpConn(int sock)
{
    tout = 0;
    sockfd = sock;
}

CTcpConn::~CTcpConn()
{
    close(sockfd);
    sockfd = -1;
}

int CTcpConn::connect(const char* ip, uint16_t port)
{
    if (sockfd < 0) {
        sockfd = socket(AF_INET, SOCK_STREAM, 0);
        if (sockfd < 0) {
            ESP_LOGE(TAG, "... Failed to allocate socket.");
        }
        ESP_LOGI(TAG, "... allocated socket");
    }

    int ret = 0;
    struct sockaddr_in servaddr;
    memset(&servaddr, 0, sizeof(servaddr));
    servaddr.sin_family = AF_INET;
    servaddr.sin_port = htons(port);
    ret = inet_pton(AF_INET, ip, &servaddr.sin_addr);
    if( ret <= 0){
        ESP_LOGE(TAG, "inet_pton error for %s\n", ip);
        return ret;
    }

    ret = cConnect(sockfd, (struct sockaddr*)&servaddr, sizeof(servaddr));
    if( ret < 0){
        ESP_LOGE(TAG, "connect error: %s(%d)\n", ip, port);
        close(sockfd);
        sockfd = -1;
    }
    return ret;
}

int CTcpConn::connect(uint32_t ip, uint16_t port)
{
    if (sockfd < 0) {
        sockfd = socket(AF_INET, SOCK_STREAM, 0);
        if (sockfd < 0) {
            ESP_LOGE(TAG, "... Failed to allocate socket.");
        }
        ESP_LOGD(TAG, "... allocated socket");
    }
    int ret = 0;
    struct sockaddr_in servaddr;
    memset(&servaddr, 0, sizeof(servaddr));
    servaddr.sin_family = AF_INET;
    servaddr.sin_port = htons(port);
    struct in_addr ip_addr;
    ip_addr.s_addr = ip;
    servaddr.sin_addr = ip_addr;
    ret = cConnect(sockfd, (struct sockaddr*)&servaddr, sizeof(servaddr));
    if( ret < 0){
        ESP_LOGE(TAG, "connect error: %d.%d.%d.%d(%d)\n", ((uint8_t*)&ip)[0], ((uint8_t*)&ip)[1],
                                                          ((uint8_t*)&ip)[2], ((uint8_t*)&ip)[3], port);
        close(sockfd);
        sockfd = -1;
    }
    return ret;
}


int CTcpConn::set_timeout(int timeout)
{
    int ret = 0;
    if (sockfd < 0) {
        return -1;
    }
    tout = timeout;
    struct timeval receiving_timeout;
    receiving_timeout.tv_sec = timeout;
    receiving_timeout.tv_usec = 0;
    ret = setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &receiving_timeout,
            sizeof(receiving_timeout));
    if (ret < 0) {
        ESP_LOGE(TAG, "... failed to set socket receiving timeout");
        close(sockfd);
        sockfd = -1;
    }
    return ret;
}

int CTcpConn::read(uint8_t* data, int length, int timeout)
{
    int ret;
    if (sockfd < 0) {
        return -1;
    }

    if (timeout > 0) {
        ret = set_timeout(timeout);
        if (ret < 0) {
            return ret;
        }
    }
    ret = recv(sockfd, data, length, 0);
    return ret;
}

int CTcpConn::write(const uint8_t *data, int length)
{
    if (sockfd < 0) {
        ESP_LOGE(TAG, "... socket error");
        return -1;
    }
    int ret = send(sockfd, data, length, 0);
    if( ret < 0) {
        ESP_LOGE(TAG, "... socket send failed");
        close(sockfd);
        sockfd = -1;
    }
    return ret;
}

int CTcpConn::disconnect()
{
    int ret = close(sockfd);
    sockfd = -1;
    return ret;
}

//---------- TCP Server --------------
CTcpServer::CTcpServer()
{
    tout = 0;
    sockfd = -1;
}

CTcpServer::~CTcpServer()
{
    stop();
}

int CTcpServer::listen(uint16_t port, int max_connection)
{
    struct sockaddr_in server_addr;
    int ret = -1;

    /* Construct local address structure */
    memset(&server_addr, 0, sizeof(server_addr)); /* Zero out structure */
    server_addr.sin_family = AF_INET; /* Internet address family */
    server_addr.sin_addr.s_addr = INADDR_ANY; /* Any incoming interface */
    server_addr.sin_len = sizeof(server_addr);
    server_addr.sin_port = htons(port); /* Local port */

    /* Create socket for incoming connections */
    if (sockfd < 0) {
        sockfd = socket(AF_INET, SOCK_STREAM, 0);
        if (sockfd < 0) {
            ESP_LOGE(TAG_SERVER, "C > user_webserver_task failed to create sock!");
            return -1;
        }
    }
    /* Bind to the local address */
    ret = bind(sockfd, (struct sockaddr * )&server_addr, sizeof(server_addr));
    if (ret != 0) {
        ESP_LOGE(TAG_SERVER, "C > user_webserver_task failed to bind sock!");
        close(sockfd);
        sockfd = -1;
        return -1;
    }

    /* Listen to the local connection */
    ret = cListen(sockfd, max_connection);
    if (ret != 0) {
        ESP_LOGE(TAG_SERVER, "C > user_webserver_task failed to set listen queue!");
        close(sockfd);
        sockfd = -1;
        return -1;
    }
    return 0;
}

CTcpConn* CTcpServer::accept()
{
    if (sockfd < 0) {
        ESP_LOGE(TAG_SERVER, "TCP server socket error");
        return NULL;
    }
    int connect_fd = -1;
    connect_fd = cAccept(sockfd, (struct sockaddr*) NULL, NULL);
    if (connect_fd == -1) {
        ESP_LOGE(TAG_SERVER, "accept socket error: %s(errno: %d)", strerror(errno), errno);
        return NULL;
    }

    CTcpConn* tcp_conn = new CTcpConn(connect_fd);
    return tcp_conn;
}

void CTcpServer::stop()
{
    if (sockfd > 0) {
        close(sockfd);
    }
}

