/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "as.h"

#include <hwres.h>


static pthread_mutex_t rl_mux = PTHREAD_MUTEX_INITIALIZER;

/* Recursive mutex held during typed-mem mmap msg_receive; the roottask
 * re-enters this path while servicing its own IPC, so it must NOT set
 * RTMUX_SYNC_NONRECURSIVE (which PTHREAD_MUTEX_INITIALIZER would).
 */
static pthread_mutex_t tymem_lock = {
    .count = 0,
    .owner = RTMUX_SYNC_INITIALIZER,
};

/* Match a typed-memory name against an asinfo entry hierarchy.
 *
 * Compares the trailing path component of <name> against <as>'s name, then
 * recurses up the owner chain to match the remaining prefix. Parameters:
 *   base_as  : base of the asinfo array (for owner-offset resolution)
 *   as       : asinfo entry to match against
 *   name     : typed-memory name being looked up
 *   name_len : length of <name>
 * Return value: 1 if the name matches the asinfo hierarchy, 0 if not.
 */
static int match_name(struct asinfo_entry *base_as,
                      struct asinfo_entry *as,
                      const char *name,
                      size_t name_len)
{
    const char *str = SYSPAGE_ENTRY(strings)->data;
    const char *curr;
    const char *p;
    size_t len;

    p = &name[name_len];
    for (;;) {
        if (p == name)
            break;
        if (p[-1] == '/')
            break;
        --p;
    }
    curr = &str[as->name];
    {
        size_t prefix = (size_t) (p - name); /* always >= 0: p starts at
                                                name+name_len, moves back
                                                */
        len = name_len - prefix;
    }

    /* Check that this piece matches */
    if (strlen(curr) != len)
        return 0;
    if (memcmp(curr, p, len) != 0)
        return 0;

    /* See if that was the whole given name */
    if (p == name)
        return 1;

    /* First character in given name is a "/" => must be top level asinfo. */
    if ((p == &name[1]) && (as->owner == AS_NULL_OFF))
        return 1;

    /* More to given name, no owner for current asinfo */
    if (as->owner == AS_NULL_OFF)
        return 0;

    /* See if prefix also matches. */
    return match_name(base_as,
                      (struct asinfo_entry *) ((uint8_t *) base_as + as->owner),
                      name, (name_len - len) - 1);
}


/* Free a physical-allocation restriction list.
 *
 * Parameters:
 *   par : head of the pa_restrict list to free
 */
static void kill_restrict(struct pa_restrict *par)
{
    struct pa_restrict *next;

    while (par) {
        next = par->next;
        _sfree(par, sizeof(*par));
        par = next;
    }
}


/* Build a restriction list for a single typed-memory name.
 *
 * Walks the asinfo array, and for every entry matching <name> inserts a
 * pa_restrict node (ordered by asinfo priority) into the list. The asinfo
 * pointer is temporarily stashed in each node's checker field and replaced by
 * the real alloc_checker before returning. Parameters:
 *   name     : typed-memory name
 *   name_len : length of <name>
 *   rp       : OUT — receives the head of the restriction list
 * Return value: EOK on success, ENOENT if no entry matched, ENOMEM.
 */
static int one_restrict_create(const char *name,
                               size_t name_len,
                               struct pa_restrict **rp)
{
    struct asinfo_entry *base_as = SYSPAGE_ENTRY(asinfo);
    struct asinfo_entry *as;
    struct asinfo_entry *try_as;
    unsigned num;
    struct pa_restrict *par;
    struct pa_restrict *new;
    struct pa_restrict **owner;
    struct pa_restrict *try;

    /* We're hiding the asinfo pointer inside the checker field of the restrict
     * entry so we can remember both the priority & alloc_checker field in this
     * routine. It all gets fixed up at the end.
     */

    as = base_as;
    par = NULL;
    num = _syspage_ptr->asinfo.entry_size / sizeof(*as);
    while (num != 0) {
        if (match_name(base_as, as, name, name_len)) {
            owner = &par;
            for (;;) {
                try = *owner;
                if (!try)
                    break;
                try_as = (struct asinfo_entry *) try->checker;
                if (try_as->priority < as->priority)
                    break;
                owner = &try->next;
            }
            new = _smalloc(sizeof(*new));
            if (!new) {
                kill_restrict(par);
                return ENOMEM;
            }
            new->start = as->start;
            new->end = as->end;
            new->checker = (void *) as;
            new->next = *owner;
            *owner = new;
        }
        ++as;
        --num;
    }
    if (!par)
        return ENOENT;
    /* RUSH3: coalese overlapping/adjacent entries where possible */
    for (try = par; try; try = try->next) {
        try_as = (void *) try->checker;
        try->checker = try_as->alloc_checker;
    }
    *rp = par;
    return EOK;
}


/* Build a restriction list for a typed-memory expression.
 *
 * Parses <name> as a sequence of names joined by '|' (union) and '&'
 * (intersection) operators, building each piece with one_restrict_create and
 * combining them accordingly. Fills <attr> with the aggregate size limits and
 * flags the object MM_MEM_RDB if no piece overlaps system RAM. Parameters:
 *   name : typed-memory expression
 *   obp  : object whose mem.mm.restriction is set on success
 *   attr : OUT — receives the memclass size attributes
 * Return value: EOK on success, or an errno on failure.
 */
static int multi_restrict_create(const char *name,
                                 path_object_t *obp,
                                 memclass_attr_t *attr)
{
    int is_sysram;
    struct pa_restrict *try;
    struct pa_restrict *new;
    struct pa_restrict *par;
    struct pa_restrict **owner;
    struct pa_restrict *check1;
    struct pa_restrict *check2;
    const char *curr;
    char last_op;
    int r;

    obp->mem.mm.restriction = NULL;
    par = NULL;
    curr = name;
    last_op = ' ';
    do {
        switch (*curr) {
        case '\0':
        case '|':
        case '&':
            if (curr == name) {
                kill_restrict(par);
                return EINVAL;
            }
            {
                size_t nlen =
                    (size_t) (curr -
                              name); /* always >= 0: curr starts at name */
                r = one_restrict_create(name, nlen, &new);
            }
            if (r != EOK) {
                kill_restrict(par);
                return r;
            }
            switch (last_op) {
            case ' ':
                /* first time, just set par to new */
                par = new;
                break;
            case '|':
                /* unioning the two names together, just tack the new
                 * restriction list to the end of the first
                 */
                owner = &par;
                for (;;) {
                    try = *owner;
                    if (!try)
                        break;
                    owner = &try->next;
                }
                *owner = new;
                break;
            case '&':
                /* want the intersection of the two restriction lists */
                try = NULL;
                owner = &try;
                for (check1 = par; check1; check1 = check1->next) {
                    /* walk the second list and look for overlapping entries */
                    for (check2 = new; check2; check2 = check2->next) {
                        paddr_t start = ~(paddr_t) 0;
                        paddr_t end = (paddr_t) 0;

                        if ((check2->start >= check1->start) &&
                            (check2->end <= check1->end)) {
                            /* 'check2' fully contined within 'check1' */
                            start = check2->start;
                            end = check2->end;
                        } else if ((check1->start >= check2->start) &&
                                   (check1->end <= check2->end)) {
                            /* 'check1' fully contained within 'check2' */
                            start = check1->start;
                            end = check1->end;
                        } else if ((check2->start < check1->start) &&
                                   (check2->end > check1->start)) {
                            /* start of 'check1' included in 'check2' */
                            start = check1->start;
                            end = check2->end;
                        } else if ((check2->start < check1->end) &&
                                   (check2->end > check1->end)) {
                            /* end of 'check1' included in 'check2' */
                            start = check2->start;
                            end = check1->end;
                        }
                        if (start < end) {
                            /* found an overlap */
                            struct pa_restrict *p;

                            p = _smalloc(sizeof(*p));
                            if (!p) {
                                kill_restrict(par);
                                kill_restrict(new);
                                return ENOMEM;
                            }
                            p->next = NULL;
                            p->checker = check1->checker;
                            p->start = start;
                            p->end = end;
                            *owner = p;
                            owner = &p->next;
                        }
                    }
                }
                kill_restrict(par);
                kill_restrict(new);
                par = try;
                break;
            default:
                break;
            }
            last_op = *curr;
            name = curr + 1;
            break;
        default:
            break;
        }
        ++curr;
    } while (last_op != '\0');
    if (!par) {
        /* empty intersection.... */
        return ENODEV;
    }
    attr->size = 0;
    attr->limits.alloc.size.min = __PAGESIZE;         /* FIX ME */
    attr->limits.alloc.size.max = memsize_t_INFINITY; /* FIX ME */
    is_sysram = 0;
    for (try = par; try; try = try->next) {
        /* If any piece of the restriction list overlaps with a system ram
         * entry, we're only going to allocate out of the system ram piece. If a
         * POSIX_TYPED_MEM_MAP_ALLOCATABLE request is used for that region, it's
         * the user's lookout.
         */
        is_sysram |= pa_sysram_overlap(try);

        /* RUSH2: This is actually wrong since there can be overlapping RUNS2:
         * entries in the restriction list
         */
        attr->size += (try->end - try->start) + 1;
    }
    if (!is_sysram)
        obp->mem.mm.flags |= MM_MEM_RDB;
    obp->mem.mm.restriction = par;
    return EOK;
}


/* Create a typed-memory object (delegates to the shared-memory path).
 *
 * Parameters:
 *   obp   : object being created
 *   extra : creation argument passed through to shmem_create
 * Return value: result of shmem_create.
 */
int tymem_create(path_object_t *obp, void *extra)
{
    return shmem_create(obp, extra);
}


/* Finalize a typed-memory object once it has no more references.
 *
 * Returns without freeing if references remain; otherwise frees the object's
 * name. Parameters:
 *   obp : typed-memory object
 * Return value: 0 if references remain, 1 if the object may be freed.
 */
int tymem_done(path_object_t *obp)
{
    if (obp->mem.mm.refs)
        return 0;

    /* FIX ME - following required as per comments on per open() path_object_t
     * creation
     */
    if (obp->tymem.name) {
        _sfree(obp->tymem.name, strlen(obp->tymem.name + 1));
    }

    return 1;
}

/* Open a typed-memory object by name.
 *
 * Creates an OBJECT_MEM_TYPED object, copies the name, and builds its
 * restriction list from <name>. Parameters:
 *   name        : typed-memory name to open
 *   oflags      : open flags (unused)
 *   obpp        : OUT — receives the created object on success
 *   creator_pid : pid charged for the object's memory
 * Return value: EOK on success, or an errno on failure.
 */
/* RUSH1: permission checking? */
int vm_tymem_open(const char *name,
                  int oflags,
                  path_object_t **obpp,
                  pid_t creator_pid)
{
    path_object_t *obp;
    size_t name_len;
    int r;
    memclass_attr_t dummy_attr;

    pthread_mutex_lock(&rl_mux);
    r = ENOMEM;

    /* although this path_object_t will be created in order to represent a
     * partition of a different class of memory, the path_object_t itself will
     * be allocated from the callers sysram memory partition
     */
    obp = object_create(OBJECT_MEM_TYPED, NULL, creator_pid);
    if (!obp)
        goto fail1;

    name_len = strlen(name) + 1;
    obp->tymem.name = _smalloc(name_len);
    if (!obp->tymem.name) {
        goto fail2;
    }
    memcpy(obp->tymem.name, name, name_len);
    r = multi_restrict_create(name, obp, &dummy_attr);
    if (r != EOK)
        goto fail2;

    *obpp = obp;
    pthread_mutex_unlock(&rl_mux);
    return EOK;

fail2:
    object_done(obp);

fail1:
    pthread_mutex_unlock(&rl_mux);
    return r;
}


/* Close a typed-memory object, freeing its restriction list and pmem.
 *
 * Parameters:
 *   obp : typed-memory object to close
 */
void vm_tymem_close(path_object_t *obp)
{
    memobj_lock(obp);
    kill_restrict(obp->mem.mm.restriction);
    (void) shmem_done(obp); /* Free up pmem list if possible */
    memobj_unlock(obp);
}


/* Allocate typed memory backed by the resource database.
 *
 * Walks the object's restriction list, attaching ranges via the hardware
 * resource database and wrapping each as a fake pa_quantum. On allocation
 * pressure the request size is halved and retried unless a contiguous
 * allocation was demanded. Parameters:
 *   obp   : typed-memory object whose restriction list to allocate from
 *   size  : number of bytes to allocate
 *   flags : PAA_FLAG_* allocation flags
 * Return value: head of the allocated pa_quantum list, or NULL.
 */
struct pa_quantum *tymem_rdb_alloc(path_object_t *obp,
                                   size_t size,
                                   unsigned flags)
{
    hwres_claim_t req;
    int r;
    struct pa_quantum *head;
    struct pa_quantum **owner;
    struct pa_quantum *pq;
    size_t requested_size;
    struct pa_restrict *rl;
    memsize_t resv_size = 0;

    head = NULL;
    owner = &head;
    requested_size = size;

    pthread_mutex_lock(&tymem_lock);

    do {
    restart:
        rl = obp->mem.mm.restriction;

        for (;;) {
            req.length = size;
            req.start = rl->start;
            req.end = rl->end;
            req.align = __PAGESIZE;
            req.flags = HWRES_MEMORY | HWRES_FLAG_RANGE | HWRES_FLAG_ALIGN;

            /* can the allocator satisfy the request ? */
            if (pa_free_size() < (size - resv_size)) {
                goto fail1;
            }
            if ((r = hwres_proc_interface(&req, 1, HWRES_REQ_CLAIM)) == EOK) {
                pq = pa_alloc_fake(req.start, size);
                if (!pq) {
                    pthread_mutex_unlock(&tymem_lock);
                    goto fail2;
                }
                pq->u.inuse.next = NULL;
                pq->flags |= PAQ_FLAG_RDB;
                *owner = pq;
                owner = &pq->u.inuse.next;
                break;
            }
            rl = rl->next;
            if (!rl) {
                if (flags & PAA_FLAG_CONTIG)
                    goto fail1;
                if (size <= __PAGESIZE)
                    goto fail1;
                size >>= 1;
                goto restart;
            }
        }
        requested_size -= size;
    } while (requested_size != 0);

    pthread_mutex_unlock(&tymem_lock);
    return head;

fail2:
    tymem_rdb_free(obp, req.start, size);

fail1:
    while (head) {
        pq = head->u.inuse.next;
        tymem_rdb_free(obp, pa_quantum_to_paddr(head),
                       NQUANTUM_TO_LEN(head->run));
        pa_free_fake(head);
        head = pq;
    }

    pthread_mutex_unlock(&tymem_lock);
    return NULL;
}


/* Allocate a specific physical range of typed memory from the resource DB.
 *
 * Attaches [paddr, paddr+size) via the hardware resource database and wraps it
 * as a fake pa_quantum. Parameters:
 *   obp   : typed-memory object
 *   paddr : physical start address
 *   size  : number of bytes
 *   rp    : OUT — receives the errno on failure
 * Return value: the allocated pa_quantum, or NULL on failure (*rp set).
 */
struct pa_quantum *tymem_rdb_alloc_given(path_object_t *obp,
                                         paddr_t paddr,
                                         size_t size,
                                         int *rp)
{
    hwres_claim_t req;
    int r;
    struct pa_quantum *pq;

    req.length = size;
    req.start = paddr;
    req.end = paddr + size - 1;
    req.flags = HWRES_MEMORY | HWRES_FLAG_RANGE;
    r = hwres_proc_interface(&req, 1, HWRES_REQ_CLAIM);
    if (r != EOK) {
        goto fail1;
    }
    r = ENOMEM;
    pq = pa_alloc_fake(paddr, size);
    if (!pq)
        goto fail2;
    pq->flags |= PAQ_FLAG_RDB;
    return pq;

fail2:
    tymem_rdb_free(obp, paddr, size);

fail1:
    *rp = r;
    return NULL;
}


/* Detach a physical range of typed memory from the resource database.
 *
 * Parameters:
 *   obp   : typed-memory object (unused)
 *   paddr : physical start address
 *   size  : number of bytes
 */
void tymem_rdb_free(path_object_t *obp, paddr_t paddr, size_t size)
{
    hwres_claim_t req;

    /* RUSH2: handle partial frees */
    req.start = paddr;
    req.end = paddr + size - 1;
    req.length = size;
    req.flags = HWRES_MEMORY;
    (void) hwres_proc_interface(&req, 1, HWRES_REQ_RELEASE);
}

/* Compute free/contiguous typed memory available within a restriction list.
 *
 * Queries the resource database for unused memory ranges and, for each range
 * overlapping the restriction list, accumulates the total free bytes and tracks
 * the largest contiguous run. Parameters:
 *   rlp     : restriction list to intersect against
 *   totalp  : OUT — total free bytes within the restriction (may be NULL)
 *   contigp : OUT — largest contiguous free run (may be NULL)
 */
static void tymem_rdb_free_info(struct pa_restrict *rlp,
                                paddr_t *totalp,
                                paddr_t *contigp)
{
    struct pa_restrict *rp;
    int num;
    int idx;
    int i;
    hwres_range_t list[20];
    paddr_t start;
    paddr_t end;
    paddr_t total;
    paddr_t contig;

    total = 0;
    contig = 0;

    idx = 0;
    for (;;) {
        num = hwres_proc_query(list, NUM_ELTS(list), idx, HWRES_MEMORY);
        if (num <= 0)
            break;
        idx += num;
        for (i = 0; i < num; ++i) {
            if (!(list[i].flags & HWRES_FLAG_USED)) {
                for (rp = rlp; rp; rp = rp->next) {
                    /* RUSH3: If the list is in ascending order, kick out early
                     * RUSH3: if list[i].end < rp->start
                     */
                    start = max(list[i].start, rp->start);
                    end = min(list[i].end, rp->end);
                    if (start < end) {
                        paddr_t size;

                        size = (end - start) + 1;
                        total += size;
                        if (size > contig)
                            contig = size;
                    }
                }
            }
        }
    }
    if (totalp)
        *totalp = total;
    if (contigp)
        *contigp = contig;
}


/* Report free/contiguous typed memory for an object.
 *
 * Routes to the resource-database query for MM_MEM_RDB objects, or to the
 * physical-allocator query otherwise. Parameters:
 *   obp    : typed-memory object
 *   total  : OUT — total free bytes (may be NULL)
 *   contig : OUT — largest contiguous free run (may be NULL)
 */
void tymem_free_info(path_object_t *obp, paddr_t *total, paddr_t *contig)
{
    if (obp->mem.mm.flags & MM_MEM_RDB) {
        tymem_rdb_free_info(obp->mem.mm.restriction, total, contig);
    } else {
        /* Physical-system typed memory still uses the VM physical allocator
         * accounting path.
         */
        pa_free_info(obp->mem.mm.restriction, total, contig);
    }
}
