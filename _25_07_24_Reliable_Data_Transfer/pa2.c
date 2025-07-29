/*
# Copyright 2025 University of Kentucky
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#
# SPDX-License-Identifier: Apache-2.0
*/

/* 
Please specify the group members here

# Student #1: 
# Student #2:
# Student #3: 

*/

#include <asm-generic/errno-base.h>
#include <asm-generic/errno.h>
#include <asm-generic/socket.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <pthread.h>
#include <errno.h>
#include <fcntl.h>

#define MAX_EVENTS 64
#define MESSAGE_SIZE 16
#define DEFAULT_CLIENT_THREADS 4

char *server_ip = "127.0.0.1";
int server_port = 12345;
int num_client_threads = DEFAULT_CLIENT_THREADS;
int num_requests = 1000000;

/*
 * This structure is used to store per-thread data in the client
 */
typedef struct {
    // original
    int epoll_fd;        /* File descriptor for the epoll instance, used for monitoring events on the socket. */
    int socket_fd;       /* File descriptor for the client socket connected to the server. */
    long long total_rtt; /* Accumulated Round-Trip Time (RTT) for all messages sent and received (in microseconds). */
    long total_messages; /* Total number of messages sent and received. */
    float request_rate;  /* Computed request rate (requests per second) based on RTT and total messages. */

    // for packet loss measurement
    long tx_cnt;  // number of packets sent by each client thread
    long rx_cnt;  // number of returned packets successfully received by the client thread
    long lost_pkt_cnt;
} client_thread_data_t;

/*
 * This function runs in a separate client thread to handle communication with the server
 */
void *client_thread_func(void *arg) {
    client_thread_data_t *data = (client_thread_data_t *)arg;
    struct epoll_event event;  // to register interested event in epoll_ctl
    struct epoll_event events[MAX_EVENTS];  // records after epoll_wait
    char send_buf[MESSAGE_SIZE] = "ABCDEFGHIJKMLNOP"; /* Send 16-Bytes message every time */
    char recv_buf[MESSAGE_SIZE];
    struct timeval start, end;

    // Hint 1: register the "connected" client_thread's socket in the its epoll instance
    // Hint 2: use gettimeofday() and "struct timeval start, end" to record timestamp, which can be used to calculated RTT.

    /* TODO:
     * It sends messages to the server, waits for a response using epoll,
     * and measures the round-trip time (RTT) of this request-response.
     */
    event.events = EPOLLIN;
    event.data.fd = data->socket_fd;  // save some data, just for reference
    // ask epoll instance to watch this fd for interested events
    if (epoll_ctl(data->epoll_fd, EPOLL_CTL_ADD, data->socket_fd, &event) == -1) {
        perror("epoll_ctl");
        pthread_exit(NULL);
    }
 
    for (int i = 0; i < num_requests; i++) {
        gettimeofday(&start, NULL);

        ssize_t n = send(data->socket_fd, send_buf, MESSAGE_SIZE, 0);
        if (n != MESSAGE_SIZE) {
            perror("send");
            break;
        }
        data->tx_cnt++;

        // after wait, have [nfds] events ready
        int nfds = epoll_wait(data->epoll_fd, events, MAX_EVENTS, 100);  //? this timeout parameter
        if (nfds == -1) {
            perror("epoll_wait");
            break;
        }
        if (nfds == 0) { continue; }  // timeout and packet lost

        // handle each of them
        for (int j = 0; j < nfds; j++) {
            if (events[j].data.fd != data->socket_fd) continue;  // retreive that data and identify

            ssize_t bytes = recv(data->socket_fd, recv_buf, MESSAGE_SIZE, MSG_WAITALL);  // WAITALL no need for udp?
            if (bytes != MESSAGE_SIZE) {
                perror("recv");
                break;
            }
            data->rx_cnt++;  // packet received successfully

            gettimeofday(&end, NULL);
            long long rtt = (end.tv_sec - start.tv_sec) * 1000000 + (end.tv_usec - start.tv_usec);
            data->total_rtt += rtt;
            data->total_messages += 1;
        }
    }

    /* TODO:
     * The function exits after sending and receiving a predefined number of messages (num_requests). 
     * It calculates the request rate based on total messages and RTT
     */
    if (data->total_rtt > 0) {
        data->request_rate = (float)data->total_messages / (data->total_rtt / 1000000.0f);
    }
    data->lost_pkt_cnt = data->tx_cnt - data->rx_cnt;

    close(data->socket_fd);
    close(data->epoll_fd);

    return NULL;
}

/*
 * This function orchestrates multiple client threads to send requests to a server,
 * collect performance data of each threads, and compute aggregated metrics of all threads.
 */
void run_client() {
    pthread_t threads[num_client_threads];
    client_thread_data_t thread_data[num_client_threads];
    struct sockaddr_in server_addr;

    // set server address
    memset(&server_addr, 0, sizeof(server_addr));  // init memory
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(server_port);  // host to network short
    inet_pton(AF_INET, server_ip, &server_addr.sin_addr);  // ip presentation to network

    /* TODO:
     * Create sockets and epoll instances for client threads
     * and connect these sockets of client threads to the server
     */
    
    // Hint: use thread_data to save the created socket and epoll instance for each thread
    // You will pass the thread_data to pthread_create() as below
    for (int i = 0; i < num_client_threads; i++) {
        // create socket instance
        int sock = socket(AF_INET, SOCK_DGRAM, 0);
        if (sock == -1) {
            perror("socket");
            exit(EXIT_FAILURE);
        }

        // we can keep the connect() to keep using send & recv in client
        if (connect(sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) == -1) {
            perror("connect");
            exit(EXIT_FAILURE);
        }

        // create epoll instance
        int epfd = epoll_create1(0);
        if (epfd == -1) {
            perror("epoll_create");
            exit(EXIT_FAILURE);
        }

        // save instances, init metrics
        thread_data[i] = (client_thread_data_t){
            .epoll_fd = epfd,
            .socket_fd = sock,
            .total_rtt = 0,
            .total_messages = 0,
            .request_rate = 0.0f,
            .tx_cnt = 0,
            .rx_cnt = 0
        };

        // create thread with thread_data
        pthread_create(&threads[i], NULL, client_thread_func, &thread_data[i]);
    }

    /* TODO:
     * Wait for client threads to complete and aggregate metrics of all client threads
     */
    long long total_rtt = 0;
    long total_messages = 0;
    float total_request_rate = 0.0f;
    long total_lost_pkts = 0;

    for (int i = 0; i < num_client_threads; i++) {
        pthread_join(threads[i], NULL);
        total_rtt += thread_data[i].total_rtt;
        total_messages += thread_data[i].total_messages;
        total_request_rate += thread_data[i].request_rate;
        total_lost_pkts += thread_data[i].lost_pkt_cnt;
    }

    printf("Average RTT: %lld us\n", total_rtt / total_messages);
    printf("Total Request Rate: %f messages/s\n", total_request_rate);
    printf("Total Packet Loss: %ld messages\n", total_lost_pkts);
}

void run_server() {
    int sock, epfd;
    struct sockaddr_in server_addr;
    struct epoll_event event, events[MAX_EVENTS];
    char buf[MESSAGE_SIZE];

    /* TODO:
     * Server creates listening socket and epoll instance.
     * Server registers the listening socket to epoll
     */
    sock = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0);
    if (sock == -1) {
        perror("socket");
        exit(EXIT_FAILURE);
    }

    // reduce socket buffer, might help observing packet loss
    int rcv_buf_size = 4096 * 16;
    if (setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &rcv_buf_size, sizeof(rcv_buf_size)) == -1) {
        perror("setsockopt");
        exit(EXIT_FAILURE);
    }

    int opt = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;  // server listens to all interfaces for that port number
    server_addr.sin_port = htons(server_port);

    if (bind(sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) == -1) {
        perror("bind");
        exit(EXIT_FAILURE);
    }

    // No use for udp
    // if (listen(sock, 3) == -1) {
    //     perror("listen");
    //     exit(EXIT_FAILURE);
    // }

    epfd = epoll_create1(EPOLL_CLOEXEC);
    if (epfd == -1) {
        perror("epoll_create");
        exit(EXIT_FAILURE);
    }

    event.events = EPOLLIN;
    event.data.fd = sock;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, sock, &event) == -1) {
        perror("epoll_ctl");
        exit(EXIT_FAILURE);
    }

    /* Server's run-to-completion event loop */
    while (1) {
        /* TODO:
         * Server uses epoll to handle connection establishment with clients
         * or receive the message from clients and echo the message back
         */
        int nfds = epoll_wait(epfd, events, MAX_EVENTS, -1);
        if (nfds == -1) {
            if (errno == EINTR) continue;
            perror("epoll_wait");
            break;
        }

        for (int i = 0; i < nfds; i++) {
            int fd = events[i].data.fd;
            uint32_t mask = events[i].events;

            // new data sent, read and send back
            if (mask & EPOLLIN) {
                struct sockaddr_in client_addr;
                socklen_t len = sizeof(client_addr);
                ssize_t n = recvfrom(fd, buf, MESSAGE_SIZE, 0, (struct sockaddr*)&client_addr, &len);

                if (n > 0) {
                    sendto(fd, buf, n, 0, (struct sockaddr*)&client_addr, len);
                }
            }
        }
    }

    close(sock);
    close(epfd);
}

int main(int argc, char *argv[]) {
    if (argc > 1 && strcmp(argv[1], "server") == 0) {
        if (argc > 2) server_ip = argv[2];
        if (argc > 3) server_port = atoi(argv[3]);

        run_server();
    } else if (argc > 1 && strcmp(argv[1], "client") == 0) {
        if (argc > 2) server_ip = argv[2];
        if (argc > 3) server_port = atoi(argv[3]);
        if (argc > 4) num_client_threads = atoi(argv[4]);
        if (argc > 5) num_requests = atoi(argv[5]);

        run_client();
    } else {
        printf("Usage: %s <server|client> [server_ip server_port num_client_threads num_requests]\n", argv[0]);
    }

    return 0;
}