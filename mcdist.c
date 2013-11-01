
/*
Copyright (c) 2013, GoldSpot Media, Inc.
All rights reserved.

Redistribution and use in source and binary forms, with or without modification,
are permitted provided that the following conditions are met:

* Redistributions of source code must retain the above copyright notice, this
  list of conditions and the following disclaimer.

* Redistributions in binary form must reproduce the above copyright notice, this
  list of conditions and the following disclaimer in the documentation and/or
  other materials provided with the distribution.

* Neither the name of the GoldSpot Media, Inc. nor the names of its
  contributors may be used to endorse or promote products derived from
  this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR
ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include <sys/socket.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <getopt.h>
#include <arpa/inet.h>
#include <string.h>
#include <netinet/ip.h>
#include <netdb.h>
#include <stdio.h>
#include <pthread.h>
#include <poll.h>
#include <unistd.h>
#include <stdlib.h>
#include <pthread.h>
#include <netinet/udp.h>

#ifdef linux
#define uh_sport source
#define uh_ulen len
#define uh_dport dest
#endif

typedef struct data_t {
    int size;
    struct data_t * next;
    struct data_t * prev;
} data_t;

#define REFS(k)     ((int*)(((void*)k)+sizeof(data_t)))
#define DATA(k)     ((void*)(((void*)k)+sizeof(data_t) + refs_size))

static void read_loop(int);
static void help(void);
static void* send_out_loop(void*);
static void* mclean(void*);

static int destinations = 0;
static int refs_size;
static in_addr_t multicast = INADDR_NONE;
static in_addr_t unicast = INADDR_NONE;

static pthread_mutex_t tail_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t tail_watch = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t head_lock = PTHREAD_MUTEX_INITIALIZER;

static data_t * data_tail;
static data_t * data_head;
static int debug = 0;
static int do_daemon = 0;
static int udp_proto;
#if 0
static int mport = 0;
#endif

struct sockaddr ** to;

int main(int argc, char ** argv) {

    int raws;
    int c;
    int i;

    while ((c = getopt(argc, argv, "u:m:t:fd")) != -1) {
        switch (c) {
            case 'u':
                unicast = inet_addr(optarg);
                if (unicast == INADDR_NONE) {
                    fprintf(stderr, "invalid unicast address %s\n", optarg);
                    return 1;
                }
                break;
            case 'm':
                multicast = inet_addr(optarg);
                if (multicast == INADDR_NONE) {
                    fprintf(stderr, "invalid multicast address %s\n", optarg);
                    return 1;
                }
                break;
#if 0
            case 'p':
                if (!(mport = atoi(optarg))) {
                    fprintf(stderr, "invalid multicast port value %s\n",
                            optarg);
                    return 1;
                }
                break;
#endif
            case 't':

                if (destinations) {
                    fprintf(stderr, "Use single -t argument!\n");
                    return 1;
                }

                {
                    char * aux = optarg;
                    while (aux) {
                        // $TODO: this can be way better.
                        destinations++;
                        aux = strchr(aux, ',');
                        if (aux) { aux++; }
                    }
                    if (!destinations) {
                        fprintf(stderr, "oops\n");
                        return 1;
                    }
                    refs_size = destinations * sizeof(int);
                    to = (struct sockaddr**)malloc(destinations * sizeof(void*));
                    aux = strdup(optarg);
                    i = 0;
                    while (aux) {
                        char * ip = aux;
                        struct sockaddr_in * sa;
                        in_addr_t sip;
                        aux = strchr(aux,',');
                        if (aux) {
                            *aux = 0;
                            aux++;
                        }
                        sa = (struct sockaddr_in*)malloc(sizeof(struct sockaddr_in));
                        to[i++] = (struct sockaddr*)sa;
                        sa->sin_family = AF_INET;
                        // $TODO: resolve first!!!!
                        sip = inet_addr(ip);
                        if (sip == INADDR_NONE) {
                            fprintf(stderr, "Invalid destination IP address"
                                    " %s\n", ip);
                            return 1;
                        }
                        memcpy(&sa->sin_addr, &sip, sizeof(sip));
                    }

                }
                break;

            case 'f':
                do_daemon = 1;
                break;
            case 'd':
                debug = 1;
                break;
            default:
                help();
        }
    }

    if (!destinations) {
        fprintf(stderr, "no destination specified\n");
        help();
    }

#if 0
    if (!mport) {
        fprintf(stderr, "no multicast port specified\n");
        help();
    }
#endif

    if (multicast == INADDR_NONE) {
        fprintf(stderr, "no multicast address specified\n");
        help();
    }

    if (unicast == INADDR_NONE) {
        fprintf(stderr, "no unicast address specified\n");
        help();
    }

    {
        static struct protoent * udp;
        udp = getprotobyname("udp");
        if (!udp) {
            fprintf(stderr, "your system doesn't know udp proto!");
            udp_proto = 17;
        } else {
            udp_proto = udp->p_proto;
        }
        if (debug) {
            fprintf(stdout, "udp is proto %d\n", udp_proto);
        }
    }

    raws = socket(AF_INET, SOCK_RAW, udp_proto);
    if (raws < 0) {
        perror("socket");
        return 2;
    }

    if (do_daemon) {

        pid_t p = fork();
        if (p < 0) {
            perror("fork");
            return 2;
        }
        if (p) {
            // I am parent
            return 0;
        }

        setsid();

    }

    for (i=0; i<destinations; i++) {
        int * arg = (int*)malloc(2*sizeof(int));
        int s = socket(AF_INET, SOCK_DGRAM, 0);
        if (s < 0) {
            perror("socket");
            return 2;
        }
        arg[0] = i;
        arg[1] = s;
        pthread_t p;
        if (pthread_create(&p, 0, send_out_loop, arg)) {
            perror("pthread_create");
            return 2;
        }
    }

    {
        pthread_t p;
        if (pthread_create(&p, 0, mclean, 0)) {
            perror("pthread create");
            return 2;
        }
    }


    read_loop(raws);

    return 0;

}

void * send_out_loop(void* arg) {

    int no = ((int*)arg)[0];
    int ds = ((int*)arg)[1];
    int slen = sizeof(struct sockaddr_in);
    int offset = sizeof(struct ip) + sizeof(struct udphdr);

    data_t * ptr = data_head;
    struct sockaddr * dest = to[no];

    while (1) {

        int rc;

        if (!ptr) {
            pthread_mutex_lock(&tail_lock);
            pthread_cond_wait(&tail_watch, &tail_lock);
            pthread_mutex_unlock(&tail_lock);

            ptr = data_tail;
            if (REFS(ptr)[no]) { continue; }

            if (ptr->prev) {
                pthread_mutex_lock(&head_lock);
                while (1) {
                    data_t * prev = ptr->prev;
                    if (prev && !REFS(prev)[no]) {
                        ptr = prev;
                    } else {
                        break;
                    }
                }
                pthread_mutex_unlock(&head_lock);
            }
            continue;
        }

        ((struct sockaddr_in*)dest)->sin_port =
            ((struct udphdr*)(DATA(ptr)+sizeof(struct ip)))->uh_dport;
        rc = sendto(ds, DATA(ptr)+offset, ptr->size-offset, 0, dest, slen);

        if (rc < 0) {
            perror("sendto");
        } else {
            if (debug) {
                fprintf(stdout, "%d sent %d bytes\n", no, rc);
            }
        }

        {
            data_t * aux = ptr;
            ptr = ptr->next;
            REFS(aux)[no] = 1;
        }

    }

    return 0;

}

void * mclean(void * arg) {

    while (1) {

        int can_clean;
        data_t * head = data_head;

        if (head) {

            int i;

            can_clean = 1;
            for (i=0; i<destinations; i++) {
                if (!REFS(head)[i]) {
                    can_clean = 0;
                    break;
                }
            }

        } else {
            can_clean = 0;
        }

        if (can_clean) {
            pthread_mutex_lock(&head_lock);
            if (!head->next) {
                pthread_mutex_lock(&tail_lock);
                if (!head->next) {
                    free(head);
                    data_head = 0;
                    data_tail = 0;
                    head = 0;
                    if (debug) {
                        printf("freed all\n");
                    }
                }
                pthread_mutex_unlock(&tail_lock);
            }

            if (head) {
                data_head = head->next;
                data_head->prev = 0;
                free(head);
                if (debug) {
                    printf("freed head\n");
                }
            }

            pthread_mutex_unlock(&head_lock);
        } else {
            struct timespec slp;
            slp.tv_sec = 0;
            slp.tv_nsec = 300000000;
            nanosleep(&slp, 0);
        }

    }

    return 0;

}

void read_loop(int s) {

    data_t * buffer = 0;
    int buffer_size = 0;

    struct pollfd pfd;

    pfd.fd = s;
    pfd.events = POLLIN | POLLPRI;

    int data_header_size = sizeof(data_t) + refs_size;

    while (1) {

        int rc = poll(&pfd, 1, -1);
        int psize;

        if (rc < 0) {
            perror("poll");
            _exit(2);
        }

        if (pfd.revents & (POLLERR|POLLHUP|POLLNVAL)) {
            fprintf(stderr, "error condition on raw sock %d\n", s);
            _exit(2);
        }

        if (ioctl(s, FIONREAD, &psize)) {
            perror("ioctl");
            _exit(2);
        }

        if (psize > buffer_size) {
            buffer_size = psize;
            if (buffer) {
                buffer = (data_t*)realloc(buffer, psize+data_header_size);
            } else {
                buffer = malloc(psize+data_header_size);
            }
            if (!buffer) {
                fprintf(stderr, "out of memory\n");
                _exit(2);
            }
        }

        rc = recv(s, DATA(buffer), psize, 0);
        if (!rc) {
            fprintf(stderr, "EOF from raw socket\n");
            _exit(2);
        }

        if (rc < 0) {
            perror("recv");
            _exit(2);
        }

        if (rc < sizeof(struct udphdr)) { continue; }

        {
            struct ip * h = (struct ip*) DATA(buffer);

            if (debug) {
                char * from = strdup(inet_ntoa(h->ip_src));
                char * to = strdup(inet_ntoa(h->ip_dst));
                printf("proto %d, %db from %s(%08x) to %s(%08x)\n",
                        h->ip_p, ntohs(h->ip_len),
                        from, ntohl(h->ip_src.s_addr),
                        to, ntohl(h->ip_dst.s_addr));
                free(to);
                free(from);
            }

            if (h->ip_p != udp_proto) { continue; }
            if (h->ip_src.s_addr != unicast) { continue; }
            if (h->ip_dst.s_addr != multicast) { continue; }

            struct udphdr * u = (struct udphdr*)
                (DATA(buffer) + sizeof(struct ip));

            if (debug) {
                printf("%d from %d to %d\n", ntohs(u->uh_ulen),
                        ntohs(u->uh_sport), ntohs(u->uh_dport));
            }

            {
                buffer->next = 0;
                buffer->size = rc;
                // data_unit->ref = destinations;
                memset(REFS(buffer), 0, refs_size);

                pthread_mutex_lock(&tail_lock);
                pthread_cond_broadcast(&tail_watch);
                if (data_tail) {
                    data_tail->next = buffer;
                    buffer->prev = data_tail;
                } else {
                    data_head = buffer;
                    buffer->prev = 0;
                }
                data_tail = buffer;
                pthread_mutex_unlock(&tail_lock);

                buffer = 0;
                buffer_size = 0;
            }

        }

    }

}



void help() {

    fprintf(stderr, 
"Hello. I am a multicast re-transmitter\n"
"I should only be used on network environments where there is no multicast\n"
"delivery (e.g. Amazon). If multicast delivery is ever enabled, you should\n"
"discontinue using me immedieately, or you may run into some (unknown)\n"
"unintended consequences.\n"
"\n"
"You may need to run me as root, as I need to create raw sockets\n"
"\n"
"I require and support the following options:\n"
"-u IP      required, unicast address (IP only)\n"
"-m IP      required, multicast address (IP only)\n"
#if 0
"-p port    required, multicast port\n"
#endif
"-t ip,...  required, list of the IP addresses to retransmit to\n"
"           (this is the list of nodes)\n"
"-f         daemonize\n"
"-d         print debug output\n"

);

    _exit(1);

}

