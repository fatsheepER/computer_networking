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
#include <bits/types/struct_timeval.h>
#include <netinet/in.h>
#include <stdint.h>
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
#include "crc32c.h"

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

    // for rdt
    uint32_t client_id;
} client_thread_data_t;

// Structure of a frame header
typedef struct {
    uint32_t client_id;  // id for specific client
    uint16_t seq;  // sequence number
    uint16_t ack;  // acknowlegement number
    uint8_t flags;  // 0: ack; 1: data
    uint8_t wnd_size;  // sender's avaliable window size
    uint16_t len;  // length of payload, always 16 here
    uint32_t checksum;  // crc-32c
} header_t;

// Structure of a frame
typedef struct {
    header_t header;
    char payload[MESSAGE_SIZE];
} frame_t;

#define HEADER_SIZE sizeof(header_t)
#define FRAME_SIZE (HEADER_SIZE + MESSAGE_SIZE)
#define WND_SIZE 64  
#define MAX_SEQ 256  // enough for current window size
#define TIMEOUT_US 20000LL  // 20 ms

frame_t make_data_frame(int seq, uint32_t client_id) {
    frame_t frame;

    // set header
    frame.header.client_id = htonl(client_id);
    frame.header.seq = htons(seq);
    frame.header.ack = 0;  // it's not an ack frame
    frame.header.flags = 1;
    frame.header.wnd_size = WND_SIZE;
    frame.header.len = htons(MESSAGE_SIZE);

    // set payload
    memcpy(frame.payload, "ABCDEFGHIJKLMNOP", 16);

    // calculate checksum
    frame.header.checksum = 0;  // do the same in verification
    char buffer[FRAME_SIZE];
    memcpy(buffer, &frame.header, HEADER_SIZE);
    memcpy(buffer + HEADER_SIZE, frame.payload, MESSAGE_SIZE);
    frame.header.checksum = htonl(crc32c(buffer, FRAME_SIZE));

    return frame;
}

// send frame via udp socket
void send_frame(int sock, frame_t *f) {
    ssize_t sbytes = send(sock, f, FRAME_SIZE, 0);

    if (sbytes == -1) {
        perror("send_frame: send failed");
    }
    else if (sbytes != FRAME_SIZE) {
        perror("send_frame: sent bytes mismatch");
    }
}

// retransmit all frames within two SN from the ring-structured buffer
// return how many frames has been sent
int retransmit_window(int sock, frame_t *buf, int from_seq, int to_seq) {
    int num_sent = 0;

    for (int i = from_seq; i != to_seq; i = (i + 1) % MAX_SEQ) {
        int idx = i % WND_SIZE;
        send_frame(sock, &buf[idx]);    
        num_sent++;
    }

    return num_sent;
}

inline void start_timer(long long *time_us) {
    struct timeval now;
    gettimeofday(&now, NULL);
    *time_us = now.tv_sec * 1000000LL + now.tv_usec;
}

inline long long time_until_timeout_us(long long time_us) {
    if (time_us < 0) return -1LL;  // not started yet

    struct timeval now;
    gettimeofday(&now, NULL);
    long long elapse = now.tv_sec * 1000000LL + now.tv_usec - time_us;

    return (elapse >= TIMEOUT_US) ? 0 : (TIMEOUT_US - elapse);
}

/*
 * This function runs in a separate client thread to handle communication with the server
 */
void *client_thread_func(void *arg) {
    client_thread_data_t *data = (client_thread_data_t *)arg;
    struct epoll_event event;  // to register interested event in epoll_ctl
    struct epoll_event events[MAX_EVENTS];  // records after epoll_wait
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
    // ask epoll instance to watch this socket_fd for interested events
    if (epoll_ctl(data->epoll_fd, EPOLL_CTL_ADD, data->socket_fd, &event) == -1) {
        perror("epoll_ctl");
        pthread_exit(NULL);
    }

    int total_sent = 0;  //! can be replaced by tx_cnt
    int base = 0;  // the first sent-and-unacked seq
    int next_seq = 0;  // next seq to send
    frame_t send_buf[WND_SIZE];
    long long time_start_us = -1;  // -1: not started yet

    uint32_t client_id = data->client_id;

    while (total_sent < num_requests) {
        // fill the window
        while ((next_seq - base + MAX_SEQ) % MAX_SEQ < WND_SIZE && total_sent < num_requests) {
            frame_t f = make_data_frame(next_seq, client_id);
            send_frame(data->socket_fd, &f);

            send_buf[next_seq % WND_SIZE] = f;  // cache in buffer, used in retransmission
            if (base == next_seq) start_timer(&time_start_us);

            next_seq = (next_seq + 1) % MAX_SEQ;  // send next
            total_sent++; data->tx_cnt++;
        }

        // calculate rest time
        long long time_left_us = time_until_timeout_us(time_start_us);
        int time_left_ms = (time_left_us < 0) ? -1 : (int)(time_left_us / 1000);

        // wait for response
        int nfds = epoll_wait(data->epoll_fd, events, MAX_EVENTS, time_left_ms);
        if (nfds == -1) { perror("epoll_wait"); break; }

        // Timeout: retransmit all within window
        if (nfds == 0) {
            int num_resent = retransmit_window(data->socket_fd, send_buf, base, next_seq);
            // as asked to, no need to update tx_cnt after retransmission. anyway, it's accessible if needed
            start_timer(&time_start_us);
            continue;
        }

        // Receive: process acks
        for (int i = 0; i < nfds; i++) {
            if (events[i].data.fd != data->socket_fd) continue;

            // is it complete
            frame_t ack_frame;
            ssize_t rbytes = recv(data->socket_fd, &ack_frame, FRAME_SIZE, MSG_WAITALL);
            if (rbytes != FRAME_SIZE) { perror("recv"); break; }

            // is it an ack
            if (ack_frame.header.flags != 0) continue;

            // check checksum
            uint32_t cs_original = ntohl(ack_frame.header.checksum);
            ack_frame.header.checksum = 0;
            char check_buf[FRAME_SIZE];
            memcpy(check_buf, &ack_frame.header, HEADER_SIZE);
            memcpy(check_buf + HEADER_SIZE, ack_frame.payload, MESSAGE_SIZE);

            uint32_t cs_result = crc32c(check_buf, FRAME_SIZE);
            if (cs_result != cs_original) { 
                fprintf(stderr, "checksum mismatch!\n");
                continue;
            }

            // update ack, base and timer
            int ack = ntohs(ack_frame.header.seq);
            if ((ack - base + MAX_SEQ) % MAX_SEQ < WND_SIZE) {  // cumulativec ack
                int num_acked = (ack - base + MAX_SEQ) % MAX_SEQ + 1;
                data->rx_cnt += num_acked;

                base = (ack + 1) % MAX_SEQ;  // first unacked
                if (base == next_seq) time_start_us = -1;  // window's empty, stop the timer
                else start_timer(&time_start_us);  // restart the timer
            }
        }
    }

    // do some summary
    data->lost_pkt_cnt = data->tx_cnt - data->rx_cnt;

    // close and wave goodbye
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

    // init crc32c table
    crc32c_init();

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
            .rx_cnt = 0,
            .client_id = (uint32_t)i  // allocate id for each client
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
    int rcv_buf_size = 2048;
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