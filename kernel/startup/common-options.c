/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * \brief Handle common command-line options.
 *
 * \license Apache 2.0 (http://www.apache.org/licenses/LICENSE-2.0)
 */

#include <stdlib.h>
#include <startup.h>
#include <startup-api.h>
#include <confname.h>
#include <options.h>

static char *debug_opt[2];

static void get_freq(unsigned long *fp)
{
    unsigned long freq;
    char *end;

    freq = strtoul(optarg, &end, 10);
    if (optarg != end) {
        switch (*end) {
        case 'm':
        case 'M':
            freq *= 1000;
            /* fall through */
        case 'k':
        case 'K':
            freq *= 1000;
            /* fall through */
        case 'h':
        case 'H':
            ++end;
            break;
        default:
            if (freq < 1000)
                freq *= 1000000;
            break;
        }
        *fp = freq;
        optarg = end;
    }
    if (*optarg == ',')
        ++optarg;
}

void handle_common_option(int opt)
{
    unsigned long num;
    char *cp;

    if (!cpu_handle_common_option(opt)) {
        switch (opt) {
        case 'D':
            /* kprintf output channel */
            debug_opt[0] = optarg;
            break;
        case 'f':
            /* CPU and timer frequency */
            get_freq(&kopt.timebase_freq);
            get_freq(&kopt.timer_freq);
            break;
        case 'K':
            /* kdebug remote debug protocol channel */
            debug_opt[1] = optarg;
            break;
        case 'N':
            add_typed_string(_CS_HOSTNAME, optarg);
            break;
        case 'R':
            kopt.reserved_size = getsize(optarg, &cp);
            if (*cp == ',') {
                kopt.reserved_align = (unsigned) getsize(cp + 1, &cp);
            }
            break;
        case 'P':
            num = strtoul(optarg, &optarg, 10);
            if (num > 0) {
                if (num > CONFIG_PROCESSORS_MAX)
                    crash(
                        "Too many CPUs specified in '-P' option (%ld, but "
                        "maximum allowed is %d)\n",
                        num, CONFIG_PROCESSORS_MAX);
                kopt.max_cpus = (unsigned) num;
            }
            break;
        case 'p':
            kopt.nopreempt = true;
            break;
        case 'T':
            kopt.auto_trace = true;
            break;
        case 'v':
            kopt.loglevel++;
            break;
        }
    }
}

/**
 * \brief Set debug device.
 * \note TODO
 */
static void set_debug(unsigned channel,
                      const struct debug_device *dev,
                      const char *options)
{
    dev->init(channel, options, dev->defaults[channel]);

    if (channel == 0)
        set_print_char(dev->put);
}

/**
 * \brief Select debug device and channel.
 */
static void select_one(const struct debug_device *dev,
                       unsigned size,
                       unsigned channel)
{
    size_t len;
    const struct debug_device *selected;
    const struct debug_device *end;
    char *arg;

    end = (void *) ((uint8_t *) dev + size);
    arg = debug_opt[channel];
    if (!arg) {
        for (;;) {
            if (dev >= end)
                return;
            if (dev->defaults[channel])
                break;
            ++dev;
        }
        set_debug(channel, dev, "");
        return;
    }
    selected = NULL;
    for (;;) {
        const char *defaults;

        if (dev >= end)
            break;
        defaults = dev->defaults[channel];
        if (defaults) {
            char ch;

            len = strlen(dev->name);
            ch = arg[len];
            if ((memcmp(arg, dev->name, len) == 0) &&
                (ch == ' ' || ch == '\t' || ch == '\0' || ch == '.')) {
                selected = dev;
                arg += len;
                if (*arg == '.')
                    ++arg;
                break;
            }
            if (!selected && defaults[0] != '\0') {
                /* If we don't match any of the names, the first device in the
                 * list with actual options is the default device.
                 */
                selected = dev;
            }
        }
        ++dev;
    }
    if (selected)
        set_debug(channel, selected, arg);
}

void select_debug(const struct debug_device *dev, unsigned size)
{
    select_one(dev, size, 0);
    if (debug_opt[1])
        select_one(dev, size, 1);
}
