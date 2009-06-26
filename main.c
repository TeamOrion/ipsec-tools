/*
 * Copyright (C) 2009 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <signal.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>

#ifdef ANDROID_CHANGES
#include <fcntl.h>
#include <android/log.h>
#include <cutils/sockets.h>
#endif

#include "config.h"
#include "libpfkey.h"
#include "ipsec_strerror.h"
#include "gcmalloc.h"
#include "vmbuf.h"
#include "crypto_openssl.h"
#include "oakley.h"
#include "vendorid.h"
#include "pfkey.h"
#include "schedule.h"
#include "isakmp_var.h"
#include "nattraversal.h"
#include "plog.h"
#include "grabmyaddr.h"
#include "localconf.h"
#include "sockmisc.h"
#include "admin.h"
#include "privsep.h"
#include "misc.h"

extern int setup(int argc, char **argv);
int f_local = 0;

static void interrupt(int signal)
{
    exit(1);
}

#ifdef ANDROID_CHANGES

static int get_control_and_arguments(int *argc, char ***argv)
{
    static char *args[256];
    int control;
    int i;

    if ((i = android_get_control_socket("racoon")) == -1) {
        return -1;
    }
    do_plog(LLV_DEBUG, "Waiting for control socket");
    if (listen(i, 1) == -1 || (control = accept(i, NULL, 0)) == -1) {
        do_plog(LLV_ERROR, "Cannot get control socket");
        exit(-1);
    }
    close(i);
    fcntl(control, F_SETFD, FD_CLOEXEC);

    args[0] = (*argv)[0];
    for (i = 1; i < 256; ++i) {
        unsigned char length;
        if (recv(control, &length, 1, 0) != 1) {
            do_plog(LLV_ERROR, "Cannot get argument length");
            exit(-1);
        }
        if (length == 0xFF) {
            break;
        } else {
            int offset = 0;
            args[i] = malloc(length + 1);
            while (offset < length) {
                int n = recv(control, &args[i][offset], length - offset, 0);
                if (n > 0) {
                    offset += n;
                } else {
                    do_plog(LLV_ERROR, "Cannot get argument value");
                    exit(-1);
                }
            }
            args[i][length] = 0;
        }
    }
    do_plog(LLV_DEBUG, "Received %d arguments", i - 1);

    *argc = i;
    *argv = args;
    return control;
}

#endif


int main(int argc, char **argv)
{
    fd_set fdset;
    int fdset_size;
    struct myaddrs *p;
#ifdef ANDROID_CHANGES
    unsigned char code;
    int control = get_control_and_arguments(&argc, &argv);
#endif

    do_plog(LLV_INFO, "ipsec-tools 0.7.2 (http://ipsec-tools.sf.net)\n");

    signal(SIGHUP, interrupt);
    signal(SIGINT, interrupt);
    signal(SIGTERM, interrupt);
    signal(SIGCHLD, interrupt);
    signal(SIGPIPE, SIG_IGN);

    eay_init();
    oakley_dhinit();
    compute_vendorids();
    sched_init();

    if (setup(argc, argv) < 0 || pfkey_init() < 0 || isakmp_init() < 0) {
        exit(1);
    }

#ifdef ANDROID_CHANGES
    code = argc - 1;
    send(control, &code, 1, 0);
#endif

#ifdef ENABLE_NATT
    natt_keepalive_init();
#endif

    FD_ZERO(&fdset);
    FD_SET(lcconf->sock_pfkey, &fdset);
    fdset_size = lcconf->sock_pfkey;
    for (p = lcconf->myaddrs; p; p = p->next) {
        FD_SET(p->sock, &fdset);
        if (fdset_size < p->sock) {
            fdset_size = p->sock;
        }
    }
    ++fdset_size;

    while (1) {
        fd_set readset = fdset;
        struct timeval *timeout = schedular();
        if (select(fdset_size, &readset, NULL, NULL, timeout) < 0) {
            exit(1);
        }
        if (FD_ISSET(lcconf->sock_pfkey, &readset)) {
            pfkey_handler();
        }
        for (p = lcconf->myaddrs; p; p = p->next) {
            if (FD_ISSET(p->sock, &readset)) {
                isakmp_handler(p->sock);
            }
        }
    }
    return 0;
}

/* plog.h */

void do_plog(int level, char *format, ...)
{
    if (level >= 0 && level <= 5) {
#ifdef ANDROID_CHANGES
        static int levels[6] = {
            ANDROID_LOG_ERROR, ANDROID_LOG_WARN, ANDROID_LOG_INFO,
            ANDROID_LOG_INFO, ANDROID_LOG_DEBUG, ANDROID_LOG_VERBOSE
        };
        va_list ap;
        va_start(ap, format);
        __android_log_vprint(levels[level], "racoon", format, ap);
        va_end(ap);
#else
        static char *levels = "EWNIDV";
        fprintf(stderr, "%c: ", levels[level]);
        va_list ap;
        va_start(ap, format);
        vfprintf(stderr, format, ap);
        va_end(ap);
#endif
    }
}

char *binsanitize(char *data, size_t length)
{
    char *output = racoon_malloc(length + 1);
    if (output) {
        size_t i;
        for (i = 0; i < length; ++i) {
            output[i] = isprint(data[i]) ? data[i] : '?';
        }
        output[length] = '\0';
    }
    return output;
}

/* libpfkey.h */

ipsec_policy_t ipsec_set_policy(__ipsec_const char *message, int length)
{
    struct sadb_x_policy *p;
    int direction;

    if (!strcmp("in bypass", message)) {
        direction = IPSEC_DIR_INBOUND;
    } else if (!strcmp("out bypass", message)) {
        direction = IPSEC_DIR_OUTBOUND;
    } else {
        __ipsec_errcode = EIPSEC_INVAL_POLICY;
        return NULL;
    }

    p = calloc(1, sizeof(struct sadb_x_policy));
    p->sadb_x_policy_len = PFKEY_UNIT64(sizeof(struct sadb_x_policy));
    p->sadb_x_policy_exttype = SADB_X_EXT_POLICY;
    p->sadb_x_policy_type = IPSEC_POLICY_BYPASS;
    p->sadb_x_policy_dir = direction;
#ifdef HAVE_PFKEY_POLICY_PRIORITY
    p->sadb_x_policy_priority = PRIORITY_DEFAULT;
#endif
    __ipsec_errcode = EIPSEC_NO_ERROR;
    return (ipsec_policy_t)p;
}

int ipsec_get_policylen(ipsec_policy_t policy)
{
    return policy ? PFKEY_EXTLEN(policy) : -1;
}

/* grabmyaddr.h */

int getsockmyaddr(struct sockaddr *addr)
{
    struct myaddrs *p;
    for (p = lcconf->myaddrs; p; p = p->next) {
        if (cmpsaddrstrict(addr, p->addr) == 0) {
            return p->sock;
        }
    }
    return -1;
}

/* privsep.h */

int privsep_pfkey_open()
{
    return pfkey_open();
}

void privsep_pfkey_close(int key)
{
    pfkey_close(key);
}

vchar_t *privsep_eay_get_pkcs1privkey(char *file)
{
    return eay_get_pkcs1privkey(file);
}

int privsep_script_exec(char *script, int name, char * const *environ)
{
    return 0;
}

/* misc.h */

int racoon_hexdump(void *data, size_t length)
{
    return 0;
}