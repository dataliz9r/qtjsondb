/*      $OpenBSD: btree.c,v 1.30 2010/09/01 12:13:21 martinh Exp $ */

/*
 * Copyright (c) 2009, 2010 Martin Hedenfalk <martin@bzero.se>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include "btree.h"
#include "btree_p.h"

//#define ENABLE_BIG_KEYS

#ifdef ENABLE_BIG_KEYS
#warning "Big keys may cause unforseen circumstances. Avoid for now."
#endif

#undef DEBUG

#ifdef DEBUG
# define DPRINTF(...)   do { fprintf(stderr, "%s:%d: ", __func__, __LINE__); \
                             fprintf(stderr, __VA_ARGS__); \
                             fprintf(stderr, "\n"); } while (0)
#else
# define DPRINTF(...) do { } while (0)
#endif

#ifndef NO_ERROR_MESSAGES
# define EPRINTF(...)   do { fprintf(stderr, "%s:%d: ", __func__, __LINE__); \
                             fprintf(stderr, __VA_ARGS__); \
                             fprintf(stderr, "\n"); } while (0)
#else
# define EPRINTF(...) do { } while (0)
#endif

static struct mpage     *mpage_lookup(struct btree *bt, pgno_t pgno);
static void              mpage_add(struct btree *bt, struct mpage *mp);
static void              mpage_free(struct mpage *mp);
static void              mpage_del(struct btree *bt, struct mpage *mp);
static void              mpage_flush(struct btree *bt);
static struct mpage     *mpage_copy(struct btree *bt, struct mpage *mp);
static void              mpage_prune(struct btree *bt);
static void              mpage_dirty(struct btree *bt, struct mpage *mp);
static struct mpage     *mpage_touch(struct btree *bt, struct mpage *mp);
static int               mpage_cmp(struct mpage *a, struct mpage *b);

RB_PROTOTYPE(page_cache, mpage, entry, mpage_cmp);
RB_GENERATE(page_cache, mpage, entry, mpage_cmp);

static int               btree_read_page(struct btree *bt, pgno_t pgno,
                            struct page *page);
static struct mpage     *btree_get_mpage(struct btree *bt, pgno_t pgno);
enum SearchType {
        SearchKey=0,
        SearchFirst=1,
        SearchLast=2,
};
static int               btree_search_page_root(struct btree *bt,
                            struct mpage *root, struct btval *key,
                            struct cursor *cursor, enum SearchType searchType, int modify,
                            struct mpage **mpp);
static int               btree_search_page(struct btree *bt,
                            struct btree_txn *txn, struct btval *key,
                            struct cursor *cursor, enum SearchType searchType, int modify,
                            struct mpage **mpp);

static int               btree_write_header(struct btree *bt, int fd);
static int               btree_read_header(struct btree *bt);
static int               btree_is_meta_page(struct btree *bt, struct page *p);
static int               btree_read_meta(struct btree *bt, pgno_t *p_next);
static int               btree_read_meta_with_tag(struct btree *bt, unsigned int tag, pgno_t *p_root);
static int               btree_write_meta(struct btree *bt, pgno_t root,
                                          unsigned int flags, uint32_t tag);
static void              btree_ref(struct btree *bt);

static struct node      *btree_search_node(struct btree *bt, struct mpage *mp,
                            struct btval *key, int *exactp, unsigned int *kip);
static int               btree_add_node(struct btree *bt, struct mpage *mp,
                            indx_t indx, struct btval *key, struct btval *data,
                            pgno_t pgno, uint8_t flags);
static void              btree_del_node(struct btree *bt, struct mpage *mp,
                            indx_t indx);
static int               btree_read_data(struct btree *bt, struct mpage *mp,
                            struct node *leaf, struct btval *data);

static int               btree_rebalance(struct btree *bt, struct mpage *mp);
static int               btree_update_key(struct btree *bt, struct mpage *mp,
                            indx_t indx, struct btval *key);
static int               btree_adjust_prefix(struct btree *bt,
                            struct mpage *src, int delta);
static int               btree_move_node(struct btree *bt, struct mpage *src,
                            indx_t srcindx, struct mpage *dst, indx_t dstindx);
static int               btree_merge(struct btree *bt, struct mpage *src,
                            struct mpage *dst);
static int               btree_split(struct btree *bt, struct mpage **mpp,
                            unsigned int *newindxp, struct btval *newkey,
                            struct btval *newdata, pgno_t newpgno);
static struct mpage     *btree_new_page(struct btree *bt, uint32_t flags);
static int               btree_write_overflow_data(struct btree *bt,
                            struct page *p, struct btval *data);

static void              cursor_pop_page(struct cursor *cursor);
static struct ppage     *cursor_push_page(struct cursor *cursor,
                            struct mpage *mp);

static int               bt_set_key(struct btree *bt, struct mpage *mp,
                            struct node *node, struct btval *key);
static int               btree_sibling(struct cursor *cursor, int move_right, int rightmost);
static int               btree_cursor_next(struct cursor *cursor,
                            struct btval *key, struct btval *data);
static int               btree_cursor_prev(struct cursor *cursor,
                            struct btval *key, struct btval *data);
static int               btree_cursor_set(struct cursor *cursor,
                            struct btval *key, struct btval *data, int *exactp);
static int               btree_cursor_first(struct cursor *cursor,
                            struct btval *key, struct btval *data);

static void              bt_reduce_separator(struct btree *bt, struct node *min,
                            struct btval *sep);
static void              remove_prefix(struct btree *bt, struct btval *key,
                            size_t pfxlen);
static void              expand_prefix(struct btree *bt, struct mpage *mp,
                            indx_t indx, struct btkey *expkey);
static void              concat_prefix(struct btree *bt, char *pfxstr, size_t pfxlen,
                            char *keystr, size_t keylen,char *dest, size_t *size);
static void              common_prefix(struct btree *bt, struct btkey *min,
                            struct btkey *max, struct btkey *pfx);
static void              find_common_prefix(struct btree *bt, struct mpage *mp);

static size_t            bt_leaf_size(struct btree *bt, struct mpage *mp, struct btval *key,
                            struct btval *data);
static int               bt_is_overflow(struct btree *bt, struct mpage *mp, size_t ksize,
                            size_t dsize);
static size_t            bt_branch_size(struct btree *bt, struct btval *key);

static pgno_t            btree_compact_tree(struct btree *bt, pgno_t pgno,
                            struct btree *btc);

static int               memncmp(const void *s1, size_t n1,
                            const void *s2, size_t n2, void *);
static int               memnrcmp(const void *s1, size_t n1,
                            const void *s2, size_t n2, void *);

static uint32_t          calculate_crc32(const char *begin, const char *end);
static uint32_t          calculate_checksum(struct btree *bt, const struct page *p);
static int               verify_checksum(struct btree *bt, const struct page *page);

struct btree            *btree_open_empty_copy(struct btree *bt);
int                      btree_clear(btree **bt);
int                      btree_replace(struct btree *bt, struct btree *btw);

static uint32_t
calculate_crc32(const char *begin, const char *end)
{
        const uint32_t *begin32 = (const uint32_t*)begin;
        const uint32_t *end32 = (const uint32_t*)(end - ((end - begin) % 4));
        if (begin32 >= end32)
                return 0;
        /* code derived from 32-bit CRC calculation by Gary S. Brown - Copyright (C) 1986. */
        static const uint32_t crctable[256] = {
                0x00000000, 0x77073096, 0xee0e612c, 0x990951ba, 0x076dc419, 0x706af48f, 0xe963a535, 0x9e6495a3,
                0x0edb8832, 0x79dcb8a4, 0xe0d5e91e, 0x97d2d988, 0x09b64c2b, 0x7eb17cbd, 0xe7b82d07, 0x90bf1d91,
                0x1db71064, 0x6ab020f2, 0xf3b97148, 0x84be41de, 0x1adad47d, 0x6ddde4eb, 0xf4d4b551, 0x83d385c7,
                0x136c9856, 0x646ba8c0, 0xfd62f97a, 0x8a65c9ec, 0x14015c4f, 0x63066cd9, 0xfa0f3d63, 0x8d080df5,
                0x3b6e20c8, 0x4c69105e, 0xd56041e4, 0xa2677172, 0x3c03e4d1, 0x4b04d447, 0xd20d85fd, 0xa50ab56b,
                0x35b5a8fa, 0x42b2986c, 0xdbbbc9d6, 0xacbcf940, 0x32d86ce3, 0x45df5c75, 0xdcd60dcf, 0xabd13d59,
                0x26d930ac, 0x51de003a, 0xc8d75180, 0xbfd06116, 0x21b4f4b5, 0x56b3c423, 0xcfba9599, 0xb8bda50f,
                0x2802b89e, 0x5f058808, 0xc60cd9b2, 0xb10be924, 0x2f6f7c87, 0x58684c11, 0xc1611dab, 0xb6662d3d,
                0x76dc4190, 0x01db7106, 0x98d220bc, 0xefd5102a, 0x71b18589, 0x06b6b51f, 0x9fbfe4a5, 0xe8b8d433,
                0x7807c9a2, 0x0f00f934, 0x9609a88e, 0xe10e9818, 0x7f6a0dbb, 0x086d3d2d, 0x91646c97, 0xe6635c01,
                0x6b6b51f4, 0x1c6c6162, 0x856530d8, 0xf262004e, 0x6c0695ed, 0x1b01a57b, 0x8208f4c1, 0xf50fc457,
                0x65b0d9c6, 0x12b7e950, 0x8bbeb8ea, 0xfcb9887c, 0x62dd1ddf, 0x15da2d49, 0x8cd37cf3, 0xfbd44c65,
                0x4db26158, 0x3ab551ce, 0xa3bc0074, 0xd4bb30e2, 0x4adfa541, 0x3dd895d7, 0xa4d1c46d, 0xd3d6f4fb,
                0x4369e96a, 0x346ed9fc, 0xad678846, 0xda60b8d0, 0x44042d73, 0x33031de5, 0xaa0a4c5f, 0xdd0d7cc9,
                0x5005713c, 0x270241aa, 0xbe0b1010, 0xc90c2086, 0x5768b525, 0x206f85b3, 0xb966d409, 0xce61e49f,
                0x5edef90e, 0x29d9c998, 0xb0d09822, 0xc7d7a8b4, 0x59b33d17, 0x2eb40d81, 0xb7bd5c3b, 0xc0ba6cad,
                0xedb88320, 0x9abfb3b6, 0x03b6e20c, 0x74b1d29a, 0xead54739, 0x9dd277af, 0x04db2615, 0x73dc1683,
                0xe3630b12, 0x94643b84, 0x0d6d6a3e, 0x7a6a5aa8, 0xe40ecf0b, 0x9309ff9d, 0x0a00ae27, 0x7d079eb1,
                0xf00f9344, 0x8708a3d2, 0x1e01f268, 0x6906c2fe, 0xf762575d, 0x806567cb, 0x196c3671, 0x6e6b06e7,
                0xfed41b76, 0x89d32be0, 0x10da7a5a, 0x67dd4acc, 0xf9b9df6f, 0x8ebeeff9, 0x17b7be43, 0x60b08ed5,
                0xd6d6a3e8, 0xa1d1937e, 0x38d8c2c4, 0x4fdff252, 0xd1bb67f1, 0xa6bc5767, 0x3fb506dd, 0x48b2364b,
                0xd80d2bda, 0xaf0a1b4c, 0x36034af6, 0x41047a60, 0xdf60efc3, 0xa867df55, 0x316e8eef, 0x4669be79,
                0xcb61b38c, 0xbc66831a, 0x256fd2a0, 0x5268e236, 0xcc0c7795, 0xbb0b4703, 0x220216b9, 0x5505262f,
                0xc5ba3bbe, 0xb2bd0b28, 0x2bb45a92, 0x5cb36a04, 0xc2d7ffa7, 0xb5d0cf31, 0x2cd99e8b, 0x5bdeae1d,
                0x9b64c2b0, 0xec63f226, 0x756aa39c, 0x026d930a, 0x9c0906a9, 0xeb0e363f, 0x72076785, 0x05005713,
                0x95bf4a82, 0xe2b87a14, 0x7bb12bae, 0x0cb61b38, 0x92d28e9b, 0xe5d5be0d, 0x7cdcefb7, 0x0bdbdf21,
                0x86d3d2d4, 0xf1d4e242, 0x68ddb3f8, 0x1fda836e, 0x81be16cd, 0xf6b9265b, 0x6fb077e1, 0x18b74777,
                0x88085ae6, 0xff0f6a70, 0x66063bca, 0x11010b5c, 0x8f659eff, 0xf862ae69, 0x616bffd3, 0x166ccf45,
                0xa00ae278, 0xd70dd2ee, 0x4e048354, 0x3903b3c2, 0xa7672661, 0xd06016f7, 0x4969474d, 0x3e6e77db,
                0xaed16a4a, 0xd9d65adc, 0x40df0b66, 0x37d83bf0, 0xa9bcae53, 0xdebb9ec5, 0x47b2cf7f, 0x30b5ffe9,
                0xbdbdf21c, 0xcabac28a, 0x53b39330, 0x24b4a3a6, 0xbad03605, 0xcdd70693, 0x54de5729, 0x23d967bf,
                0xb3667a2e, 0xc4614ab8, 0x5d681b02, 0x2a6f2b94, 0xb40bbe37, 0xc30c8ea1, 0x5a05df1b, 0x2d02ef8d
        };

        uint32_t crc = ~(*begin32++);
        while (begin32 < end32) {
                begin = (const char*)begin32++;
                crc = (crc >> 8) ^ crctable[(crc ^ *(begin + 0)) & 0x000000ff];
                crc = (crc >> 8) ^ crctable[(crc ^ *(begin + 1)) & 0x000000ff];
                crc = (crc >> 8) ^ crctable[(crc ^ *(begin + 2)) & 0x000000ff];
                crc = (crc >> 8) ^ crctable[(crc ^ *(begin + 3)) & 0x000000ff];
        }

        // Hash up remaining bytes
        if ((const char *)end32 < end) {
                begin = (const char *)end32;
                while (begin != end)
                    crc = (crc >> 8) ^ crctable[(crc ^ *begin++) & 0x000000ff];
        }

        return ~crc;
}

static uint32_t
calculate_checksum(struct btree *bt, const struct page *p)
{
        assert(p && bt);

        const uint32_t       offset = offsetof(page, checksum) + sizeof(p->checksum);
        const char          *begin = (const char *)p;
        const char          *end = (const char *)p + bt->head.psize;

        if (F_ISSET(bt->flags, BT_NOPGCHECKSUM))
                return 0;

        DPRINTF("calculating checksum for page %u, flags %x", p->pgno, p->flags);

        if (F_ISSET(p->flags, P_HEAD)) {
                return calculate_crc32(begin + offset, begin + PAGEHDRSZ + sizeof(struct bt_head));
        } else if (F_ISSET(p->flags, P_META)) {
                return calculate_crc32(begin + offset, begin + PAGEHDRSZ + sizeof(struct bt_meta));
        } else if (F_ISSET(p->flags, P_BRANCH) || F_ISSET(p->flags, P_LEAF)) {
                indx_t l = MAX(PAGEHDRSZ, p->lower);
                indx_t u = MIN(bt->head.psize, p->upper);
                if (l > u)
                    l = u;
                uint32_t c1 = calculate_crc32(begin + offset, begin + l);
                uint32_t c2 = calculate_crc32(begin + u, end);
                return c1 ^ c2;
        } else if (F_ISSET(p->flags, P_OVERFLOW)) {
                return calculate_crc32(begin + offset, end);
        }

        EPRINTF("unknown page type, flags = %x", p->flags);
        return 0;
}

static int
verify_checksum(struct btree *bt, const struct page *p)
{
        assert(bt && p);

        uint32_t         c;

        if (F_ISSET(bt->flags, BT_NOPGCHECKSUM))
                return BT_SUCCESS;

        DPRINTF("verifying checksum for page %u", p->pgno);

        c = calculate_checksum(bt, p);
        if (c != p->checksum) {
                DPRINTF("checksum for page %u doesn't match: expected %x got %x", p->pgno, p->checksum, c);
                return BT_FAIL;
        }
        return BT_SUCCESS;
}

static int
memncmp(const void *s1, size_t n1, const void *s2, size_t n2, void *)
{
        if (n1 < n2) {
            int ret = memcmp(s1, s2, n1);
                if (ret == 0)
                        return -1;
                else return ret;
        }
        else if (n1 > n2) {
            int ret = memcmp(s1, s2, n2);
                if (ret == 0)
                        return 1;
                else return ret;
        }
        return memcmp(s1, s2, n1);
}

static int
memnrcmp(const void *s1, size_t n1, const void *s2, size_t n2, void *)
{
        const unsigned char     *p1;
        const unsigned char     *p2;

        if (n1 == 0)
                return n2 == 0 ? 0 : -1;

        if (n2 == 0)
                return n1 == 0 ? 0 : 1;

        p1 = (const unsigned char *)s1 + n1 - 1;
        p2 = (const unsigned char *)s2 + n2 - 1;

        while (*p1 == *p2) {
                if (p1 == s1)
                        return (p2 == s2) ? 0 : -1;
                if (p2 == s2)
                        return (p1 == p2) ? 0 : 1;
                p1--;
                p2--;
        }
        return *p1 - *p2;
}

void
btree_set_cmp(struct btree *bt, bt_cmp_func cmp)
{
        bt->cmp = cmp;
}

int
btree_cmp(struct btree *bt, const struct btval *a, const struct btval *b)
{
        return bt->cmp((const char *)a->data, a->size, (const char *)b->data, b->size, 0);
}

static void
common_prefix(struct btree *bt, struct btkey *min, struct btkey *max,
    struct btkey *pfx)
{
        size_t           n = 0;
        char            *p1;
        char            *p2;

        if (min->len == 0 || max->len == 0 || bt->cmp) {
                pfx->len = 0;
                return;
        }

        if (F_ISSET(bt->flags, BT_REVERSEKEY)) {
                p1 = min->str + min->len - 1;
                p2 = max->str + max->len - 1;

                while (*p1 == *p2) {
                        p1--;
                        p2--;
                        n++;
                        if (p1 < min->str || p2 < max->str)
                                break;
                }

                assert(n <= (int)sizeof(pfx->str));
                pfx->len = n;
                bcopy(p2 + 1, pfx->str, n);
        } else {
                p1 = min->str;
                p2 = max->str;

                while (*p1 == *p2) {
                        p1++;
                        p2++;
                        n++;
                        if (n == min->len || n == max->len)
                                break;
                }

                assert(n <= (int)sizeof(pfx->str));
                pfx->len = n;
                bcopy(max->str, pfx->str, n);
        }
}

static void
remove_prefix(struct btree *bt, struct btval *key, size_t pfxlen)
{
        assert(bt);
        if (pfxlen == 0 || bt->cmp != NULL)
                return;

        DPRINTF("removing %zu bytes of prefix from key [%.*s]", pfxlen,
            (int)key->size, (char *)key->data);
        assert(pfxlen <= key->size);
        key->size -= pfxlen;
        if (!F_ISSET(bt->flags, BT_REVERSEKEY))
                key->data = (char *)key->data + pfxlen;
}

static void
expand_prefix(struct btree *bt, struct mpage *mp, indx_t indx,
    struct btkey *expkey)
{
        struct node     *node;
        size_t           sz;

        node = NODEPTR(mp, indx);
        sz = (node->ksize + mp->prefix.len) > MAXPFXSIZE ? (MAXPFXSIZE - mp->prefix.len) : node->ksize;
        expkey->len = sizeof(expkey->str);
        concat_prefix(bt, mp->prefix.str, mp->prefix.len,
            NODEKEY(node), sz, expkey->str, &expkey->len);
}

static int
bt_cmp(struct btree *bt, const struct btval *key1, const struct btval *key2,
    struct btkey *pfx)
{
        if (bt->cmp) {
                return bt->cmp((const char*)key1->data, key1->size, (const char*)key2->data, key2->size, 0);
        } else {
                if (F_ISSET(bt->flags, BT_REVERSEKEY)) {
                        return memnrcmp(key1->data, key1->size - pfx->len,
                                        key2->data, key2->size, 0);
                } else {
                        return memncmp((char *)key1->data + pfx->len, key1->size - pfx->len,
                                                key2->data, key2->size, 0);

                }
        }
}

void
btval_reset(struct btval *btv)
{
        if (btv) {
                if (btv->mp)
                        btv->mp->ref--;
                if (btv->free_data) {
                        assert(btv->data);
                        free(btv->data);
                }
                bzero(btv, sizeof(*btv));
        }
}

int btval_ref(struct btval *btv)
{
        assert(btv);
        assert(btv->mp);
        return ++btv->mp->ref;
}

int btval_deref(struct btval *btv)
{
        assert(btv);
        assert(btv->mp);
        return --btv->mp->ref;
}

static int
mpage_cmp(struct mpage *a, struct mpage *b)
{
        if (a->pgno > b->pgno)
                return 1;
        if (a->pgno < b->pgno)
                return -1;
        return 0;
}

static struct mpage *
mpage_lookup(struct btree *bt, pgno_t pgno)
{
        struct mpage find;
        struct mpage *mp = 0;

        find.pgno = pgno;
        mp = RB_FIND(page_cache, bt->page_cache, &find);
        if (mp) {
                bt->stat.hits++;
                /* Update LRU queue. Move page to the end. */
                TAILQ_REMOVE(bt->lru_queue, mp, lru_next);
                TAILQ_INSERT_TAIL(bt->lru_queue, mp, lru_next);
        }
        return mp;
}

static void
mpage_add(struct btree *bt, struct mpage *mp)
{
        assert(RB_INSERT(page_cache, bt->page_cache, mp) == NULL);
        DPRINTF("mpage_add: mp=%p pgno=%d", mp, mp->pgno);
        bt->stat.cache_size++;
        TAILQ_INSERT_TAIL(bt->lru_queue, mp, lru_next);
}

static void
mpage_free(struct mpage *mp)
{
        if (mp != NULL) {
                free(mp->page);
                free(mp);
        }
}

static void
mpage_del(struct btree *bt, struct mpage *mp)
{
        assert(RB_REMOVE(page_cache, bt->page_cache, mp) == mp);
        DPRINTF("mpage_del: mp=%p pgno=%d", mp, mp->pgno);
        assert(bt->stat.cache_size > 0);
        bt->stat.cache_size--;
        TAILQ_REMOVE(bt->lru_queue, mp, lru_next);
}

static void
mpage_flush(struct btree *bt)
{
    struct mpage	*mp;

    while ((mp = RB_MIN(page_cache, bt->page_cache)) != NULL) {
            mpage_del(bt, mp);
            mpage_free(mp);
    }
}

static struct mpage *
mpage_copy(struct btree *bt, struct mpage *mp)
{
        struct mpage    *copy;

        if ((copy = (mpage *)calloc(1, sizeof(*copy))) == NULL)
                return NULL;
        if ((copy->page = (page *)malloc(bt->head.psize)) == NULL) {
                free(copy);
                return NULL;
        }
        bcopy(mp->page, copy->page, bt->head.psize);
        bcopy(&mp->prefix, &copy->prefix, sizeof(mp->prefix));
        copy->parent = mp->parent;
        copy->parent_index = mp->parent_index;
        copy->pgno = mp->pgno;

        return copy;
}

/* Remove the least recently used memory pages until the cache size is
 * within the configured bounds. Pages referenced by cursors or returned
 * key/data are not pruned.
 */
static void
mpage_prune(struct btree *bt)
{
        struct mpage    *mp, *next;

        for (mp = TAILQ_FIRST(bt->lru_queue); mp; mp = next) {
                if (bt->stat.cache_size <= bt->stat.max_cache)
                        break;
                next = TAILQ_NEXT(mp, lru_next);
                if (!mp->dirty && mp->ref <= 0) {
                        mpage_del(bt, mp);
                        mpage_free(mp);
                }
        }
}

/* Mark a page as dirty and push it on the dirty queue.
 */
static void
mpage_dirty(struct btree *bt, struct mpage *mp)
{
        assert(bt != NULL);
        assert(bt->txn != NULL);

        if (!mp->dirty) {
                mp->dirty = 1;
                SIMPLEQ_INSERT_TAIL(bt->txn->dirty_queue, mp, next);
        }
}

/* Touch a page: make it dirty and re-insert into tree with updated pgno.
 */
static struct mpage *
mpage_touch(struct btree *bt, struct mpage *mp)
{
        assert(bt != NULL);
        assert(bt->txn != NULL);
        assert(mp != NULL);

        if (!mp->dirty) {
                DPRINTF("touching page %u -> %u", mp->pgno, bt->txn->next_pgno);
                if (mp->ref == 0)
                        mpage_del(bt, mp);
                else {
                        if ((mp = mpage_copy(bt, mp)) == NULL)
                                return NULL;
                }
                mp->pgno = mp->page->pgno = bt->txn->next_pgno++;
                mpage_dirty(bt, mp);
                mpage_add(bt, mp);

                /* Update the page number to new touched page. */
                if (mp->parent != NULL)
                        NODEPGNO(NODEPTR(mp->parent,
                            mp->parent_index)) = mp->pgno;
        }

        return mp;
}

static int
btree_read_page(struct btree *bt, pgno_t pgno, struct page *page)
{
        ssize_t          rc;

        DPRINTF("reading page %u", pgno);
        bt->stat.reads++;
        if ((rc = pread(bt->fd, page, bt->head.psize, (off_t)pgno*bt->head.psize)) == 0) {
                DPRINTF("page %u doesn't exist", pgno);
                errno = ENOENT;
                return BT_FAIL;
        } else if (rc != (ssize_t)bt->head.psize) {
                if (rc > 0)
                        errno = EINVAL;
                fprintf(stderr, "%s:%d: short pread rc=%Zd psize=%d\n",
                        __FUNCTION__, __LINE__, rc, bt->head.psize);
                DPRINTF("read: %s", strerror(errno));
                return BT_FAIL;
        }

        if (page->pgno != pgno) {
                EPRINTF("page numbers don't match: %u != %u", pgno, page->pgno);
                errno = EINVAL;
                return BT_FAIL;
        }

        if (verify_checksum(bt, page) != 0) {
                EPRINTF("checksum error for page %d", pgno);
                errno = EINVAL;
                return BT_FAIL;
        }

        DPRINTF("page %u has flags 0x%X", pgno, page->flags);

        return BT_SUCCESS;
}

int
btree_sync(struct btree *bt)
{
        unsigned int         flags = BT_MARKER;
        if (!F_ISSET(bt->flags, BT_NOSYNC))
                return fsync(bt->fd);
        if (F_ISSET(bt->flags, BT_USEMARKER) && !F_ISSET(bt->meta.flags, BT_MARKER)) {
            /* If we're closing a dead btree then add the tombstone flag */
            if (F_ISSET(bt->meta.flags, BT_TOMBSTONE))
                    flags |= BT_TOMBSTONE;
            /* we want to use marker and the last meta page doesn't have it */
            /* put a copy of the last meta page but this time with a marker */
            if (bt->txn) {
                EPRINTF("btree_sync while in transaction is not a good idea");
                return BT_FAIL;
            }
            if (fsync(bt->fd) != 0)
                    return BT_FAIL;
            bt->txn = btree_txn_begin(bt, 0);
            if (bt->txn == 0)
                    return BT_FAIL;
            if (btree_write_meta(bt, bt->meta.root, flags, bt->meta.tag) == BT_FAIL) {
                    btree_txn_abort(bt->txn);
                    return BT_FAIL;
            }
            btree_txn_abort(bt->txn);
            return BT_SUCCESS;
        }
        return 0;
}


struct btree_txn *
btree_txn_begin(struct btree *bt, int rdonly)
{
        struct btree_txn        *txn;

        if (!rdonly && bt->txn != NULL) {
                DPRINTF("write transaction already begun");
                errno = EBUSY;
                return NULL;
        }

        if ((txn = (btree_txn *)calloc(1, sizeof(*txn))) == NULL) {
                DPRINTF("calloc: %s", strerror(errno));
                return NULL;
        }

        if (rdonly) {
                txn->flags |= BT_TXN_RDONLY;                
                DPRINTF("taking read lock on txn %p, bt %p", txn, bt);
        } else {
                txn->dirty_queue = (dirty_queue *)calloc(1, sizeof(*txn->dirty_queue));
                if (txn->dirty_queue == NULL) {
                        free(txn);
                        return NULL;
                }
                SIMPLEQ_INIT(txn->dirty_queue);

                DPRINTF("taking write lock on txn %p, bt %p", txn, bt);
                if (flock(bt->fd, LOCK_EX | LOCK_NB) != 0) {
                        EPRINTF("flock: %s", strerror(errno));
                        errno = EBUSY;
                        free(txn->dirty_queue);
                        free(txn);
                        return NULL;
                }
                bt->txn = txn;
        }

        txn->bt = bt;
        btree_ref(bt);

        if (btree_read_meta(bt, &txn->next_pgno) != BT_SUCCESS) {
                btree_txn_abort(txn);
                return NULL;
        }

        txn->root = bt->meta.root;
        txn->tag = bt->meta.tag;
        DPRINTF("begin transaction on btree %p, root page %u (tag %d)", bt, txn->root, txn->tag);

        return txn;
}

struct btree_txn *
btree_txn_begin_with_tag(struct btree *bt, unsigned int tag)
{
    struct btree_txn *txn;
    pgno_t root_page;

    if (btree_read_meta_with_tag(bt, tag, &root_page) != BT_SUCCESS) {
            return NULL;
    }

    if ((txn = (btree_txn *)calloc(1, sizeof(*txn))) == NULL) {
            DPRINTF("calloc: %s", strerror(errno));
            return NULL;
    }

    txn->root  = root_page;
    txn->bt    = bt;
    txn->flags = BT_TXN_RDONLY;
    txn->tag   = tag;
    btree_ref(bt);

    DPRINTF("begin transaction on btree %p, root page %u (tag %d)", bt, txn->root, txn->tag);

    return txn;
}

void
btree_txn_abort(struct btree_txn *txn)
{
        struct mpage    *mp;
        struct btree    *bt;

        if (txn == NULL)
                return;

        bt = txn->bt;
        DPRINTF("abort transaction on btree %p, root page %u", bt, txn->root);

        if (!F_ISSET(txn->flags, BT_TXN_RDONLY)) {
                /* Discard all dirty pages.
                 */
                while (!SIMPLEQ_EMPTY(txn->dirty_queue)) {
                        mp = SIMPLEQ_FIRST(txn->dirty_queue);
                        assert(mp->ref == 0);   /* cursors should be closed */
                        mpage_del(bt, mp);
                        SIMPLEQ_REMOVE_HEAD(txn->dirty_queue, next);
                        mpage_free(mp);
                }

                DPRINTF("releasing write lock on txn %p", txn);
                txn->bt->txn = NULL;
                if (flock(txn->bt->fd, LOCK_UN) != 0) {
                        DPRINTF("failed to unlock fd %d: %s",
                            txn->bt->fd, strerror(errno));
                }
                free(txn->dirty_queue);
        }

        btree_close(txn->bt);
        free(txn);
}

int
btree_txn_is_read(struct btree_txn *txn)
{
        assert(txn);
        return txn->flags & BT_TXN_RDONLY ? 1 : 0;
}

int
btree_txn_is_error(struct btree_txn *txn)
{
        assert(txn);
        return txn->flags & BT_TXN_ERROR ? 1 : 0;
}

unsigned int
btree_txn_get_tag(struct btree_txn *txn)
{
    assert(txn != 0);
    return txn->tag;
}

int
btree_txn_commit(struct btree_txn *txn, unsigned int tag, unsigned int flags)
{
        int              n, done;
        ssize_t          rc;
        off_t            size;
        struct mpage    *mp;
        struct btree    *bt;
        struct iovec     iov[BT_COMMIT_PAGES];
        const int        needfsync = !F_ISSET(txn->bt->flags, BT_NOSYNC) || F_ISSET(flags, BT_FORCE_MARKER);

        assert(txn != NULL);
        assert(txn->bt != NULL);

        bt = txn->bt;

        if (F_ISSET(txn->flags, BT_TXN_RDONLY)) {
                DPRINTF("attempt to commit read-only transaction");
                btree_txn_abort(txn);
                errno = EPERM;
                return BT_FAIL;
        }

        if (txn != bt->txn) {
                EPRINTF("attempt to commit unknown transaction");
                btree_txn_abort(txn);
                errno = EINVAL;
                return BT_FAIL;
        }

        if (F_ISSET(txn->flags, BT_TXN_ERROR)) {
                EPRINTF("error flag is set, can't commit");
                btree_txn_abort(txn);
                errno = EINVAL;
                return BT_FAIL;
        }

        if (SIMPLEQ_EMPTY(txn->dirty_queue)) {
                if (bt->stat.tag != tag) {
                        goto done;
                } else {
                        mpage_prune(bt);
                        btree_txn_abort(txn);
                        return BT_SUCCESS;
                }
        }

        if (F_ISSET(bt->flags, BT_FIXPADDING)) {
                size = lseek(bt->fd, 0, SEEK_END);
                size += bt->head.psize - (size % bt->head.psize);
                DPRINTF("extending to multiple of page size: %llu", (long long unsigned)size);
                if (ftruncate(bt->fd, size) != 0) {
                        DPRINTF("ftruncate: %s", strerror(errno));
                        btree_txn_abort(txn);
                        return BT_FAIL;
                }
                bt->flags &= ~BT_FIXPADDING;
        }

        DPRINTF("committing transaction on btree %p, root page %u",
            bt, txn->root);

        /* Commit up to BT_COMMIT_PAGES dirty pages to disk until done.
         */
        do {
                n = 0;
                done = 1;
                SIMPLEQ_FOREACH(mp, txn->dirty_queue, next) {
                        mp->page->checksum = calculate_checksum(bt, mp->page);
                        iov[n].iov_len = bt->head.psize;
                        iov[n].iov_base = mp->page;
                        DPRINTF("commiting page %u == %u with checksum %x", mp->pgno, mp->page->pgno, mp->page->checksum);
                        if (++n >= BT_COMMIT_PAGES) {
                                done = 0;
                                break;
                        }
                }

                if (n == 0)
                        break;

                DPRINTF("commiting %u dirty pages", n);
                rc = writev(bt->fd, iov, n);
                if (rc != (ssize_t)bt->head.psize*n) {
                        if (rc > 0) {
                                DPRINTF("short write, filesystem full?");
                        } else {
                                DPRINTF("writev: %s", strerror(errno));
                        }
                        btree_txn_abort(txn);
                        return BT_FAIL;
                }

                /* Remove the dirty flag from the written pages.
                 */
                while (!SIMPLEQ_EMPTY(txn->dirty_queue)) {
                        mp = SIMPLEQ_FIRST(txn->dirty_queue);
                        mp->dirty = 0;
                        SIMPLEQ_REMOVE_HEAD(txn->dirty_queue, next);
                        if (--n == 0)
                                break;
                }
        } while (!done);

done:
        if (needfsync) {
                if (fsync(bt->fd) != 0) {
                        btree_txn_abort(txn);
                        return BT_FAIL;
                }
        }
        if (btree_write_meta(bt, txn->root,
                             needfsync ? BT_MARKER : 0,
                             tag) != BT_SUCCESS) {
                btree_txn_abort(txn);
                return BT_FAIL;
        }

        mpage_prune(bt);
        btree_txn_abort(txn);

        return BT_SUCCESS;
}

static int
btree_write_header(struct btree *bt, int fd)
{
        struct stat      sb;
        struct bt_head  *h;
        struct page     *p;
        ssize_t          rc;
        unsigned int     psize;

        DPRINTF("writing header page");
        assert(bt != NULL);

        /* Ask stat for optimal blocksize for I/O but
           don't use smaller than the initial page size */
        psize = PAGESIZE;
        if (fstat(fd, &sb) == 0 && sb.st_blksize > PAGESIZE)
            psize = sb.st_blksize;

        if ((p = (page *)calloc(1, psize)) == NULL)
                return -1;
        p->flags = P_HEAD;

        h = (bt_head *)METADATA(p);
        h->magic = BT_MAGIC;
        h->version = BT_VERSION;
        h->psize = psize;
        h->ksize = MAXKEYSIZE;
        bcopy(h, &bt->head, sizeof(*h));

        p->checksum = calculate_checksum(bt, p);
        DPRINTF("writing page %u with checksum %x", p->pgno, p->checksum);
        rc = write(fd, p, bt->head.psize);
        free(p);
        if (rc != (ssize_t)bt->head.psize) {
                if (rc > 0)
                        DPRINTF("short write, filesystem full?");
                return BT_FAIL;
        }

        return BT_SUCCESS;
}

static int
btree_read_header(struct btree *bt)
{
        char             page[PAGESIZE];
        struct page     *p = 0;
        struct page     *pcheck = 0;
        struct bt_head  *h = 0;
        ssize_t          rc;
        assert(bt != NULL);

        /* We don't know the page size yet, so use a minimum value.
         */

        if ((rc = pread(bt->fd, page, PAGESIZE, 0)) == 0) {
                errno = ENOENT;
                goto fail;
        } else if ((size_t)rc != PAGESIZE) {
                EPRINTF("read: %s", strerror(errno));
                if (rc > 0)
                        errno = EINVAL;
                goto fail;
        }

        p = (struct page *)page;

        if (!F_ISSET(p->flags, P_HEAD)) {
                EPRINTF("page %d not a header page", p->pgno);
                errno = EINVAL;
                goto fail;
        }

        h = (bt_head *)METADATA(p);
        if (h->magic != BT_MAGIC) {
                EPRINTF("header has invalid magic");
                errno = EINVAL;
                goto fail;
        }

        if (h->version != BT_VERSION) {
                EPRINTF("database is version %u, expected version %u",
                    bt->head.version, BT_VERSION);
                errno = EINVAL;
                goto fail;
        }

        if (h->ksize != MAXKEYSIZE) {
                EPRINTF("database uses max key size %u, expected max key size %u",
                    bt->head.ksize, MAXKEYSIZE);
                errno = EINVAL;
                goto fail;
        }

        bcopy(h, &bt->head, sizeof(*h));

        if (bt->head.psize == PAGESIZE) {
                pcheck = p;
        } else {
                const size_t pheadsz = PAGEHDRSZ + sizeof(bt_head);
                pcheck = (struct page *)malloc(pheadsz);
                if (pread(bt->fd, page, pheadsz, 0) <= 0) {
                    EPRINTF("pread failed to get data to verify checksum");
                    goto fail;
                }
        }

        if (verify_checksum(bt, pcheck) != 0) {
                EPRINTF("checksum fail");
                goto fail;
        } else {
                if (pcheck != p)
                    free(pcheck);
        }

        DPRINTF("btree_read_header: magic = %x", bt->head.magic);
        DPRINTF("btree_read_header: version = %d", bt->head.version);
        DPRINTF("btree_read_header: flags = %d", bt->head.flags);
        DPRINTF("btree_read_header: psize = %d", bt->head.psize);
        DPRINTF("btree_read_header: ksize = %d", bt->head.ksize);

        return 0;
fail:
        if (pcheck && pcheck != p)
            free(pcheck);
        return -1;
}

static int
btree_write_meta(struct btree *bt, pgno_t root, unsigned int flags, uint32_t tag)
{
        struct mpage    *mp;
        struct bt_meta  *meta;
        ssize_t          rc;

        DPRINTF("writing meta page for root page %u with flags %d and tag %d", root, flags, tag);

        assert(bt != NULL);
        assert(bt->txn != NULL);

        if ((mp = btree_new_page(bt, P_META)) == NULL)
                return -1;

        bt->meta.prev_meta = bt->meta.pgno;
        bt->meta.pgno = mp->pgno;
        bt->meta.root = root;
        bt->meta.flags = flags;
        bt->meta.created_at = time(0);
        bt->meta.revisions++;
        bt->meta.tag = tag;

        if (F_ISSET(bt->flags, BT_NOPGCHECKSUM))
                SHA1((unsigned char *)&bt->meta, METAHASHLEN, bt->meta.hash);

        /* Copy the meta data changes to the new meta page. */
        meta = METADATA(mp->page);
        bcopy(&bt->meta, meta, sizeof(*meta));

        mp->page->checksum = calculate_checksum(bt, mp->page);
        DPRINTF("writing page %u with checksum %x, digest %.*s", mp->page->pgno, mp->page->checksum, SHA_DIGEST_LENGTH, meta->hash);

        rc = write(bt->fd, mp->page, bt->head.psize);
        mp->dirty = 0;
        SIMPLEQ_REMOVE_HEAD(bt->txn->dirty_queue, next);
        if (rc != (ssize_t)bt->head.psize) {
                if (rc > 0)
                        DPRINTF("short write, filesystem full?");
                return BT_FAIL;
        }

        if ((bt->size = lseek(bt->fd, 0, SEEK_END)) == -1) {
                DPRINTF("failed to update file size: %s", strerror(errno));
                bt->size = 0;
        }
        return BT_SUCCESS;
}

/* Returns true if page p is a valid meta page, false otherwise.
 */
static int
btree_is_meta_page(struct btree *bt, struct page *p)
{
        struct bt_meta  *m;
        unsigned char    hash[SHA_DIGEST_LENGTH];

        m = METADATA(p);
        if (!F_ISSET(p->flags, P_META)) {
                DPRINTF("page %d not a meta page", p->pgno);
                errno = EINVAL;
                return 0;
        }

        if (m->root >= p->pgno && m->root != P_INVALID) {
                EPRINTF("page %d points to an invalid root page", p->pgno);
                errno = EINVAL;
                return 0;
        }

        if (F_ISSET(bt->flags, BT_NOPGCHECKSUM)) {
                SHA1((unsigned char *)m, METAHASHLEN, hash);

                if (bcmp(hash, m->hash, SHA_DIGEST_LENGTH) != 0) {
                        EPRINTF("page %d has an invalid digest %.*s", p->pgno, SHA_DIGEST_LENGTH, m->hash);
                        errno = EINVAL;
                        return 0;
                }
        }

        return 1;
}

static int
btree_read_meta(struct btree *bt, pgno_t *p_next)
{
        struct mpage    *mp;
        struct bt_meta  *meta;
        pgno_t           meta_pgno, next_pgno, rest_pgno;
        off_t            size;
        off_t            bt_prev_sz = bt->size;

        assert(bt != NULL);

        if ((size = lseek(bt->fd, 0, SEEK_END)) == -1) {
                fprintf(stderr, "failed to lseek errno=%d\n", errno);
                goto fail;
        }

        DPRINTF("btree_read_meta: size = %llu", (long long unsigned)size);

        if (size < bt->size) {
                EPRINTF("file has shrunk!");
                errno = EIO;
                goto fail;
        }

        if ((uint32_t)size == bt->head.psize) {           /* there is only the header */
                if (p_next != NULL)
                        *p_next = 1;
                return BT_SUCCESS;              /* new file */
        }

        next_pgno = size / bt->head.psize;
        if (next_pgno == 0) {
                DPRINTF("corrupt file");
                fprintf(stderr, "corrupt file\n");
                errno = EIO;
                goto fail;
        }

        meta_pgno = next_pgno - 1;

        if (size % bt->head.psize != 0) {
                DPRINTF("filesize not a multiple of the page size!");
                bt->flags |= BT_FIXPADDING;
                next_pgno++;
        }

        if (p_next != NULL)
                *p_next = next_pgno;

        if (size == bt->size) {
                DPRINTF("size unchanged, keeping current meta page");
                if (F_ISSET(bt->meta.flags, BT_TOMBSTONE)) {
                        DPRINTF("file is dead");
                        errno = ESTALE;
                        return BT_FAIL;
                } else
                        return BT_SUCCESS;
        }
        bt->size = size;

        while (meta_pgno > 0) {
                mp = btree_get_mpage(bt, meta_pgno); // TODO: Add page type to get_mpage, early out (avoid checksum checks)
                if (mp && btree_is_meta_page(bt, mp->page)) {
                        meta = METADATA(mp->page);
                        DPRINTF("flags = 0x%x", meta->flags);
                        if (F_ISSET(meta->flags, BT_TOMBSTONE)) {
                                DPRINTF("file is dead");
                                errno = ESTALE;
                                return BT_FAIL;
                        } else if (F_ISSET(bt->flags, BT_USEMARKER) && !F_ISSET(meta->flags, BT_MARKER)) {
                                DPRINTF("found a meta page %d but without marker, skipping...", meta_pgno);
                                /* dont skip if pages up to last marked meta are ok */
                                if (!F_ISSET(bt->flags, BT_NOPGCHECKSUM)) {
                                        rest_pgno = meta_pgno - 1;
                                        while ((mp = btree_get_mpage(bt, rest_pgno)) != NULL) {
                                                if (rest_pgno == 0 || (btree_is_meta_page(bt, mp->page) && F_ISSET(meta->flags, BT_MARKER))) {
                                                        bcopy(meta, &bt->meta, sizeof(bt->meta));
                                                        return BT_SUCCESS;
                                                }
                                                rest_pgno--;
                                        }
                                }
                        } else {
                                /* Make copy of last meta page. */
                                bcopy(meta, &bt->meta, sizeof(bt->meta));
                                return BT_SUCCESS;
                        }
                }
                --meta_pgno;    /* scan backwards to first valid meta page */
        }

        errno = EIO;
        if (bt_prev_sz)
                EPRINTF("failed somehow errno=%d\n", errno);
fail:
        if (p_next != NULL)
                *p_next = P_INVALID;
        return BT_FAIL;
}

static int
btree_read_meta_with_tag(struct btree *bt, unsigned int tag, pgno_t *p_root)
{
        pgno_t pgno;
        struct page *p;
        struct bt_meta *meta;

        assert(bt != NULL);
        assert(p_root != NULL);

        if (btree_read_meta(bt, NULL) != BT_SUCCESS)
            return BT_FAIL;

        if (bt->meta.tag == tag) {
                *p_root = bt->meta.root;
                return BT_SUCCESS;
        }

        if ((p = (page *)malloc(bt->head.psize)) == NULL) {
                free(p);
                return BT_FAIL;
        }

        pgno = bt->meta.prev_meta;
        while (pgno != P_INVALID) {
                if (btree_read_page(bt, pgno, p) != BT_SUCCESS) {
                        free(p);
                        return BT_FAIL;
                }
                if (!F_ISSET(p->flags, P_META)) {
                        EPRINTF("corrupted meta page chain detected (page %d flags %d)", pgno, p->flags);
                        free(p);
                        return BT_FAIL;
                }
                if (!btree_is_meta_page(bt, p)) {
                        EPRINTF("corrupted meta page found (page %d flags %d)", pgno, p->flags);
                        free(p);
                        return BT_FAIL;
                }
                meta = METADATA(p);
                if (meta->tag == tag) {
                        *p_root = meta->root;
                        free(p);
                        return BT_SUCCESS;
                }
                pgno = meta->prev_meta;
        }
        free(p);
        return BT_FAIL;
}

struct btree *
btree_open_fd(const char *path, int fd, unsigned int flags)
{
        struct btree    *bt;
        int              fl;

        fl = fcntl(fd, F_GETFL, 0);
        int rc;
        if ((rc = fcntl(fd, F_SETFL, fl | O_APPEND)) == -1) {
                EPRINTF( "fcntl fd=%d rc=%d errno=%d\n", fd, rc, errno);
                return NULL;
        }

        bt = (struct btree *)calloc(1, sizeof(btree));

        if (!bt) {
            EPRINTF("failed to allocate memory for btree");
            goto fail;
        }

        bt->fd = fd;
        bt->flags = flags;
        bt->flags &= ~BT_FIXPADDING;
        bt->ref = 1;
        bt->meta.pgno = P_INVALID;
        bt->meta.root = P_INVALID;
        bt->meta.prev_meta = P_INVALID;
        bt->path = (char*)malloc(strlen(path) + 1);
        strcpy(bt->path, path);

        if ((bt->page_cache = (struct page_cache *)calloc(1, sizeof(*bt->page_cache))) == NULL)
              goto fail;
        bt->stat.max_cache = BT_MAXCACHE_DEF;
        RB_INIT(bt->page_cache);

        if ((bt->lru_queue = (lru_queue *)calloc(1, sizeof(*bt->lru_queue))) == NULL) {
                EPRINTF("failed to allocate lru_queue");
                goto fail;
        }
        TAILQ_INIT(bt->lru_queue);

        if (btree_read_header(bt) != 0) {
                if (errno != ENOENT) {
                        EPRINTF("failed to read header");
                        goto fail;
                }
                DPRINTF("new database");
                btree_write_header(bt, bt->fd);
        }

        if (btree_read_meta(bt, NULL) != 0) {
                DPRINTF("valid meta not found. Clearing file");
                if (F_ISSET(bt->flags, BT_RDONLY) || btree_clear(&bt) != BT_SUCCESS) {
                        EPRINTF("failed to read meta");
                        goto fail;
                }
        }

        DPRINTF("opened database version %u, pagesize %u",
            bt->head.version, bt->head.psize);
        DPRINTF("timestamp: %s", ctime((const time_t *)&bt->meta.created_at));
        DPRINTF("depth: %u", bt->meta.depth);
        DPRINTF("entries: %llu", (long long unsigned)bt->meta.entries);
        DPRINTF("revisions: %u", bt->meta.revisions);
        DPRINTF("branch pages: %u", bt->meta.branch_pages);
        DPRINTF("leaf pages: %u", bt->meta.leaf_pages);
        DPRINTF("overflow pages: %u", bt->meta.overflow_pages);
        DPRINTF("root: %u", bt->meta.root);
        DPRINTF("previous meta page: %u", bt->meta.prev_meta);

        return bt;

fail:
        EPRINTF("%s: fail errno=%d\n", path, errno);
        if (bt) {
            free(bt->lru_queue);
            free(bt->page_cache);
            free(bt->path);
        }
        free(bt);
        return NULL;
}

int
btree_clear(btree **bt)
{
        struct btree *btc;

        assert(bt && *bt);

        btc = btree_open_empty_copy(*bt);

        if (!btc) {
                EPRINTF("failed to open new file");
                return BT_FAIL;
        }

        if (btree_replace(*bt, btc) != BT_SUCCESS) {
                EPRINTF("failed to replace");
                return BT_FAIL;
        }

        strcpy(btc->path, (*bt)->path);
        btree_close(*bt);
        *bt = btc;
        return BT_SUCCESS;
}

int
btree_replace(btree *bt, btree *btw)
{
        struct btree_txn *txn;

        assert(bt && btw);

        if ((txn = btree_txn_begin(bt, 0)) == NULL)
                return BT_FAIL;

        DPRINTF("replacing %s with %s", bt->path, btw->path);
        if (rename(btw->path, bt->path) != 0)
                goto fail;

        /* Write a "tombstone" meta page so other processes can pick up
         * the change and re-open the file.
         */
        if (btree_write_meta(bt, P_INVALID, BT_TOMBSTONE, 0) != BT_SUCCESS)
                goto fail;

        btree_txn_abort(txn);
        return BT_SUCCESS;
fail:
        btree_txn_abort(txn);
        return BT_FAIL;
}

struct btree*
btree_open_empty_copy(struct btree *bt)
{
        char                    *copy_path = NULL;
        const char               copy_ext[] = ".copy.XXXXXX";
        struct btree            *btc;
        int                      fd;

        assert(bt != NULL);

        DPRINTF("creating empty copy of btree %p with path %s", bt, bt->path);

        if (bt->path == NULL) {
                errno = EINVAL;
                return 0;
        }

        copy_path = (char*)malloc(strlen(bt->path) + strlen(copy_ext) + 1);
        strcpy(copy_path, bt->path);
        strcat(copy_path, copy_ext);

        fd = mkstemp(copy_path);
        if (fd == -1) {
                EPRINTF("failed to get fd for empty copy");
                goto failed;
        }

        if ((btc = btree_open_fd(copy_path, fd, bt->flags)) == NULL)
                goto failed;
        DPRINTF("opened empty btree %p", btc);

        free(copy_path);
        return btc;

failed:
        unlink(copy_path);
        free(copy_path);
        btree_close(btc);
        return 0;
}


struct btree *
btree_open(const char *path, unsigned int flags, mode_t mode)
{
        int              fd, oflags;
        struct btree    *bt;

        if (F_ISSET(flags, BT_RDONLY))
                oflags = O_RDONLY;
        else
                oflags = O_RDWR | O_CREAT | O_APPEND;

        fd = open(path, oflags, mode);
        if (fd == -1)
                return NULL;
        if ((bt = btree_open_fd(path, fd, flags)) == NULL)
                close(fd);
        else {
                DPRINTF("opened btree %p", bt);
        }

        return bt;
}

int btree_get_fd(struct btree *bt)
{
        return bt->fd;
}

static void
btree_ref(struct btree *bt)
{
        bt->ref++;
        DPRINTF("ref is now %d on btree %p", bt->ref, bt);
}

void
btree_close(struct btree *bt)
{
        if (bt == NULL)
                return;

        if (bt->ref == 1)
                btree_sync(bt);

        if (--bt->ref == 0) {
                DPRINTF("ref is zero, closing btree %p:%s", bt, bt->path);
                close(bt->fd);
                mpage_flush(bt);
                free(bt->page_cache);
                free(bt->lru_queue);
                free(bt->path);
                free(bt);
        } else
                DPRINTF("ref is now %d on btree %p", bt->ref, bt);
}

void
btree_close_nosync(struct btree *bt)
{
        if (bt == NULL)
                return;

        if (--bt->ref == 0) {
                DPRINTF("ref is zero, closing btree %p:%s", bt, bt->path);
                close(bt->fd);
                mpage_flush(bt);
                free(bt->page_cache);
                free(bt->lru_queue);
                free(bt->path);
                free(bt);
        } else
                DPRINTF("ref is now %d on btree %p", bt->ref, bt);
}

struct btree_txn *
btree_get_txn(struct btree *bt)
{
        assert(bt);
        return bt->txn;
}

/* Search for key within a leaf page, using binary search.
 * Returns the smallest entry larger or equal to the key.
 * If exactp is non-null, stores whether the found entry was an exact match
 * in *exactp (1 or 0).
 * If kip is non-null, stores the index of the found entry in *kip.
 * If no entry larger of equal to the key is found, returns NULL.
 */
static struct node *
btree_search_node(struct btree *bt, struct mpage *mp, struct btval *key,
    int *exactp, unsigned int *kip)
{
        unsigned int     i = 0;
        int              low, high;
        int              rc = 0;
        struct node     *node;
        struct btval     nodekey;

        DPRINTF("searching for [%.*s] in %lu keys in %s page %u with prefix [%.*s]",
            key->size, (const char*)key->data,
            NUMKEYS(mp),
            IS_LEAF(mp) ? "leaf" : "branch",
            mp->pgno, (int)mp->prefix.len, (char *)mp->prefix.str);

        assert(NUMKEYS(mp) > 0);

        bzero(&nodekey, sizeof(nodekey));

        low = IS_LEAF(mp) ? 0 : 1;
        high = NUMKEYS(mp) - 1;
        while (low <= high) {
                i = (low + high) >> 1;
                node = NODEPTR(mp, i);

                nodekey.size = node->ksize;
                nodekey.data = NODEKEY(node);

                rc = bt_cmp(bt, key, &nodekey, &mp->prefix);

                if (IS_LEAF(mp))
                        DPRINTF("found leaf index %u [%.*s], rc = %i",
                            i, (int)nodekey.size, (char *)nodekey.data, rc);
                else
                        DPRINTF("found branch index %u [%.*s -> %u], rc = %i",
                            i, (int)node->ksize, (char *)NODEKEY(node),
                            node->n_pgno, rc);

                if (rc == 0)
                        break;
                if (rc > 0)
                        low = i + 1;
                else
                        high = i - 1;
        }

        if (rc > 0) {   /* Found entry is less than the key. */
                i++;    /* Skip to get the smallest entry larger than key. */
                if (i >= NUMKEYS(mp))
                        /* There is no entry larger or equal to the key. */
                        return NULL;
        }
        if (exactp)
                *exactp = (rc == 0);
        if (kip)        /* Store the key index if requested. */
                *kip = i;

        return NODEPTR(mp, i);
}

static void
cursor_pop_page(struct cursor *cursor)
{
        struct ppage    *top;

        top = CURSOR_TOP(cursor);
        CURSOR_POP(cursor);
        top->mpage->ref--;

        DPRINTF("popped page %u off cursor %p", top->mpage->pgno, cursor);

        free(top);
}

static struct ppage *
cursor_push_page(struct cursor *cursor, struct mpage *mp)
{
        struct ppage    *ppage;

        DPRINTF("pushing page %u on cursor %p", mp->pgno, cursor);

        if ((ppage = (struct ppage *)calloc(1, sizeof(struct ppage))) == NULL)
                return NULL;
        ppage->mpage = mp;
        mp->ref++;
        CURSOR_PUSH(cursor, ppage);
        return ppage;
}

static struct mpage *
btree_get_mpage(struct btree *bt, pgno_t pgno)
{
        struct mpage    *mp;

        mp = mpage_lookup(bt, pgno);
        if (mp == NULL) {
                if ((mp = (mpage *)calloc(1, sizeof(*mp))) == NULL)
                        return NULL;
                if ((mp->page = (page *)malloc(bt->head.psize)) == NULL) {
                        free(mp);
                        return NULL;
                }
                if (btree_read_page(bt, pgno, mp->page) != BT_SUCCESS) {
                        mpage_free(mp);
                        return NULL;
                }
                mp->pgno = pgno;
                mpage_add(bt, mp);
        } else
                DPRINTF("returning page %u from cache", pgno);

        DPRINTF("btree_get_mpage %p", mp);
        return mp;
}

static void
concat_prefix(struct btree *bt, char *s1, size_t n1, char *s2, size_t n2,
              char *cs, size_t *cn)
{
        assert(*cn >= n1 + n2);
        if (F_ISSET(bt->flags, BT_REVERSEKEY)) {
                bcopy(s2, cs, n2);
                bcopy(s1, cs + n2, n1);
        } else {
                bcopy(s1, cs, n1);
                bcopy(s2, cs + n1, n2);
        }
        *cn = n1 + n2;
}

static void
find_common_prefix(struct btree *bt, struct mpage *mp)
{
        if (bt->cmp != NULL)
                return;

        indx_t                   lbound = 0, ubound = 0;
        struct mpage            *lp, *up;
        struct btkey             lprefix, uprefix;

        mp->prefix.len = 0;

        lp = mp;
        while (lp->parent != NULL) {
                if (lp->parent_index > 0) {
                        lbound = lp->parent_index;
                        break;
                }
                lp = lp->parent;
        }

        up = mp;
        while (up->parent != NULL) {
                if (up->parent_index + 1 < (indx_t)NUMKEYS(up->parent)) {
                        ubound = up->parent_index + 1;
                        break;
                }
                up = up->parent;
        }

        if (lp->parent != NULL && up->parent != NULL) {
                expand_prefix(bt, lp->parent, lbound, &lprefix);
                expand_prefix(bt, up->parent, ubound, &uprefix);
                common_prefix(bt, &lprefix, &uprefix, &mp->prefix);
        }
        else if (mp->parent)
                bcopy(&mp->parent->prefix, &mp->prefix, sizeof(mp->prefix));

        DPRINTF("found common prefix [%.*s] (len %zu) for page %u",
            (int)mp->prefix.len, mp->prefix.str, mp->prefix.len, mp->pgno);
}

static int
btree_search_page_root(struct btree *bt, struct mpage *root, struct btval *key,
                       struct cursor *cursor, enum SearchType searchType, int modify, struct mpage **mpp)
{
        struct mpage    *mp, *parent;

        if (cursor && cursor_push_page(cursor, root) == NULL)
                return BT_FAIL;

        mp = root;
        DPRINTF("searchType=%d isBranch=%d", searchType, IS_BRANCH(mp));
        while (IS_BRANCH(mp)) {
                unsigned int     i = 0;
                struct node     *node;

                DPRINTF("branch page %u has %lu keys", mp->pgno, NUMKEYS(mp));
                assert(NUMKEYS(mp) > 1);
                DPRINTF("found index 0 to page %u", NODEPGNO(NODEPTR(mp, 0)));

                if (searchType == SearchFirst)  /* Initialize cursor to first page. */
                        i = 0;
                else if (searchType == SearchLast) {    /* Initialize cursor to last page. */
                        i = NUMKEYS(mp) - 1;
                        DPRINTF("SearchLast i=%d", i);
                } else {
                        int      exact;
                        node = btree_search_node(bt, mp, key, &exact, &i);
                        if (node == NULL)
                                i = NUMKEYS(mp) - 1;
                        else if (!exact) {
                                assert(i > 0);
                                i--;
                        }
                }

                if (key)
                        DPRINTF("following index %u for key %.*s",
                            i, (int)key->size, (char *)key->data);
                assert(i < NUMKEYS(mp));
                node = NODEPTR(mp, i);

                if (cursor)
                        CURSOR_TOP(cursor)->ki = i;

                parent = mp;
                if ((mp = btree_get_mpage(bt, NODEPGNO(node))) == NULL)
                        return BT_FAIL;
                mp->parent = parent;
                mp->parent_index = i;
                find_common_prefix(bt, mp);

                if (cursor && cursor_push_page(cursor, mp) == NULL)
                        return BT_FAIL;

                if (modify && (mp = mpage_touch(bt, mp)) == NULL)
                        return BT_FAIL;
        }

        if (!IS_LEAF(mp)) {
                DPRINTF("internal error, index points to a %02X page!?",
                    mp->page->flags);
                return BT_FAIL;
        }

        DPRINTF("found leaf page %u for key %.*s", mp->pgno,
            key ? (int)key->size : 0, key ? (char *)key->data : NULL);

        *mpp = mp;
        return BT_SUCCESS;
}

/* Search for the page a given key should be in.
 * Stores a pointer to the found page in *mpp.
 * Searches for key if searchType is SearchKey
 * Searches for the lowest page if searchType is SearchFirst
 * Searches for the highest page if searchType is SearchLast
 * If cursor is non-null, pushes parent pages on the cursor stack.
 * If modify is true, visited pages are updated with new page numbers.
 */
static int
btree_search_page(struct btree *bt, struct btree_txn *txn, struct btval *key,
                  struct cursor *cursor, enum SearchType searchType, int modify, struct mpage **mpp)
{
        int              rc;
        pgno_t           root;
        struct mpage    *mp = 0;

        /* Can't modify pages outside a transaction. */
        if (txn == NULL && modify) {
                EPRINTF("cannot modify pages outside a transaction");
                errno = EINVAL;
                return BT_FAIL;
        }

        /* Choose which root page to start with. If a transaction is given
         * use the root page from the transaction, otherwise read the last
         * committed root page.
         */
        if (txn == NULL) {
                if ((rc = btree_read_meta(bt, NULL)) != BT_SUCCESS)
                        return rc;
                root = bt->meta.root;
        } else if (F_ISSET(txn->flags, BT_TXN_ERROR)) {
                EPRINTF("transaction has failed, must abort");
                errno = EINVAL;
                return BT_FAIL;
        } else
                root = txn->root;

        if (root == P_INVALID) {                /* Tree is empty. */
                DPRINTF("tree is empty");
                errno = ENOENT;
                return BT_FAIL;
        }

        if ((mp = btree_get_mpage(bt, root)) == NULL)
                return BT_FAIL;

        DPRINTF("root page has flags 0x%X mp=%p", mp->page->flags, mp);

        assert(mp->parent == NULL);
        assert(mp->prefix.len == 0);

        if (modify && !mp->dirty) {
                if ((mp = mpage_touch(bt, mp)) == NULL)
                        return BT_FAIL;
                txn->root = mp->pgno;
        }

        return btree_search_page_root(bt, mp, key, cursor, searchType, modify, mpp);
}

static int
btree_read_data(struct btree *bt, struct mpage *mp, struct node *leaf,
    struct btval *data)
{
        struct mpage    *omp;           /* overflow mpage */
        size_t           psz;
        size_t           max;
        size_t           sz = 0;
        pgno_t           pgno;

        bzero(data, sizeof(*data));
        max = bt->head.psize - PAGEHDRSZ;

        if (!F_ISSET(leaf->flags, F_BIGDATA)) {
                data->size = leaf->n_dsize;
                if (data->size > 0) {
                        if (mp == NULL) {
                                if ((data->data = malloc(data->size)) == NULL)
                                        return BT_FAIL;
                                bcopy(NODEDATA(leaf), data->data, data->size);
                                data->free_data = 1;
                                data->mp = NULL;
                        } else {
                                data->data = NODEDATA(leaf);
                                data->free_data = 0;
                                data->mp = mp;
                                mp->ref++;
                        }
                }
                return BT_SUCCESS;
        }

        /* Read overflow data.
         */
        DPRINTF("allocating %u byte for overflow data", leaf->n_dsize);
        if ((data->data = malloc(leaf->n_dsize)) == NULL)
                return BT_FAIL;
        data->size = leaf->n_dsize;
        data->free_data = 1;
        data->mp = NULL;
        bcopy(NODEDATA(leaf), &pgno, sizeof(pgno));
        for (sz = 0; sz < data->size; ) {
                if ((omp = btree_get_mpage(bt, pgno)) == NULL ||
                    !F_ISSET(omp->page->flags, P_OVERFLOW)) {
                        DPRINTF("read overflow page %u failed", pgno);
                        free(data->data);
                        mpage_free(omp);
                        return BT_FAIL;
                }
                psz = data->size - sz;
                if (psz > max)
                        psz = max;
                bcopy(omp->page->ptrs, (char *)data->data + sz, psz);
                sz += psz;
                pgno = omp->page->p_next_pgno;
        }

        return BT_SUCCESS;
}

int
btree_txn_get(struct btree *bt, struct btree_txn *txn,
    struct btval *key, struct btval *data)
{
        int              rc, exact;
        struct node     *leaf;
        struct mpage    *mp;

        assert(key);
        assert(data);
        DPRINTF("===> get key [%.*s]", (int)key->size, (char *)key->data);

        if (bt != NULL && txn != NULL && bt != txn->bt) {
                errno = EINVAL;
                return BT_FAIL;
        }

        if (bt == NULL) {
                if (txn == NULL) {
                        errno = EINVAL;
                        return BT_FAIL;
                }
                bt = txn->bt;
        }

        if (key->size == 0 || key->size > MAXKEYSIZE) {
                errno = EINVAL;
                return BT_FAIL;
        }

        if ((rc = btree_search_page(bt, txn, key, NULL, SearchKey, 0, &mp)) != BT_SUCCESS)
                return rc;

        leaf = btree_search_node(bt, mp, key, &exact, NULL);
        if (leaf && exact)
                rc = btree_read_data(bt, mp, leaf, data);
        else {
                errno = ENOENT;
                rc = BT_FAIL;
        }

        mpage_prune(bt);
        return rc;
}

static int
btree_sibling(struct cursor *cursor, int move_right, int rightmost)
{
        int              rc;
        struct node     *indx;
        struct ppage    *parent, *top;
        struct mpage    *mp;

        top = CURSOR_TOP(cursor);
        if ((parent = SLIST_NEXT(top, entry)) == NULL) {
                errno = ENOENT;
                return BT_FAIL;                 /* root has no siblings */
        }

        DPRINTF("parent page is page %u, index %u",
            parent->mpage->pgno, parent->ki);

        cursor_pop_page(cursor);
        if (move_right ? (parent->ki + 1 >= NUMKEYS(parent->mpage))
                       : (parent->ki == 0)) {
                DPRINTF("no more keys left, moving to %s node of %s sibling",
                        rightmost ? "rightmost" : "leftmost",
                        move_right ? "right" : "left");
                if ((rc = btree_sibling(cursor, move_right, rightmost)) != BT_SUCCESS)
                        return rc;
                parent = CURSOR_TOP(cursor);
        } else {
                if (move_right)
                        parent->ki++;
                else
                        parent->ki--;
                DPRINTF("just moving to %s index key %u",
                    move_right ? "right" : "left", parent->ki);
        }
        assert(IS_BRANCH(parent->mpage));

        indx = NODEPTR(parent->mpage, parent->ki);
        if ((mp = btree_get_mpage(cursor->bt, indx->n_pgno)) == NULL)
                return BT_FAIL;
        mp->parent = parent->mpage;
        mp->parent_index = parent->ki;

        top = cursor_push_page(cursor, mp);
        find_common_prefix(cursor->bt, mp);
        if (rightmost)
            top->ki = NUMKEYS(mp)-1;

        return BT_SUCCESS;
}

static int
bt_set_key(struct btree *bt, struct mpage *mp, struct node *node,
    struct btval *key)
{
        if (key == NULL)
                return 0;

        if (mp->prefix.len > 0) {
                key->size = node->ksize + mp->prefix.len;
                key->data = malloc(key->size);
                if (key->data == NULL)
                        return -1;
                concat_prefix(bt,
                              mp->prefix.str, mp->prefix.len,
                              NODEKEY(node), node->ksize,
                              (char *)key->data, &key->size);
                key->free_data = 1;
        } else {
                key->size = node->ksize;
                key->data = NODEKEY(node);
                key->free_data = 0;
                key->mp = mp;
                mp->ref++;
        }

        return 0;
}

static int
btree_cursor_next(struct cursor *cursor, struct btval *key, struct btval *data)
{
        struct ppage    *top;
        struct mpage    *mp;
        struct node     *leaf;

        if (cursor->eof) {
                errno = ENOENT;
                return BT_FAIL;
        }

        assert(cursor->initialized);

        top = CURSOR_TOP(cursor);
        mp = top->mpage;

        DPRINTF("cursor_next: top page is %u in cursor %p", mp->pgno, cursor);

        if (top->ki + 1 >= NUMKEYS(mp)) {
                DPRINTF("=====> move to next sibling page");
                if (btree_sibling(cursor, 1, 0) != BT_SUCCESS) {
                        cursor->eof = 1;
                        return BT_FAIL;
                }
                top = CURSOR_TOP(cursor);
                mp = top->mpage;
                DPRINTF("next page is %u, key index %u", mp->pgno, top->ki);
        } else
                top->ki++;

        DPRINTF("==> cursor points to page %u with %lu keys, key index %u",
            mp->pgno, NUMKEYS(mp), top->ki);

        assert(IS_LEAF(mp));
        leaf = NODEPTR(mp, top->ki);

        if (data && btree_read_data(cursor->bt, mp, leaf, data) != BT_SUCCESS)
                return BT_FAIL;

        if (bt_set_key(cursor->bt, mp, leaf, key) != 0)
                return BT_FAIL;

        return BT_SUCCESS;
}

static int
btree_cursor_prev(struct cursor *cursor, struct btval *key, struct btval *data)
{
        struct ppage    *top;
        struct mpage    *mp;
        struct node     *leaf;

        if (cursor->eof) {
                errno = ENOENT;
                return BT_FAIL;
        }

        assert(cursor->initialized);

        top = CURSOR_TOP(cursor);
        mp = top->mpage;

        DPRINTF("top page is %u in cursor %p", mp->pgno, cursor);

        if (top->ki - 1 == -1u) {
                DPRINTF("=====> move to prev sibling page");
                if (btree_sibling(cursor, 0, 1) != BT_SUCCESS) {
                        cursor->eof = 1;
                        return BT_FAIL;
                }
                top = CURSOR_TOP(cursor);
                mp = top->mpage;
                DPRINTF("next page is %u, key index %u", mp->pgno, top->ki);
        } else
                top->ki--;

        DPRINTF("==> cursor points to page %u with %lu keys, key index %u",
                mp->pgno, NUMKEYS(mp), top->ki);

        assert(IS_LEAF(mp));
        leaf = NODEPTR(mp, top->ki);

        if (data && btree_read_data(cursor->bt, mp, leaf, data) != BT_SUCCESS)
                return BT_FAIL;

        if (bt_set_key(cursor->bt, mp, leaf, key) != 0)
                return BT_FAIL;

        return BT_SUCCESS;
}

static int
btree_cursor_set(struct cursor *cursor, struct btval *key, struct btval *data,
    int *exactp)
{
        int              rc;
        struct node     *leaf;
        struct mpage    *mp;
        struct ppage    *top;

        assert(cursor);
        assert(key);
        assert(key->size > 0);

        rc = btree_search_page(cursor->bt, cursor->txn, key, cursor, SearchKey, 0, &mp);
        if (rc != BT_SUCCESS)
                return rc;
        assert(IS_LEAF(mp));

        top = CURSOR_TOP(cursor);
        leaf = btree_search_node(cursor->bt, mp, key, exactp, &top->ki);
        if (exactp != NULL && !*exactp) {
                /* BT_CURSOR_EXACT specified and not an exact match. */
                errno = ENOENT;
                return BT_FAIL;
        }

        if (leaf == NULL) {
                DPRINTF("===> inexact leaf not found, goto sibling");
                if (btree_sibling(cursor, 1, 0) != BT_SUCCESS)
                        return BT_FAIL;         /* no entries matched */
                top = CURSOR_TOP(cursor);
                top->ki = 0;
                mp = top->mpage;
                assert(IS_LEAF(mp));
                leaf = NODEPTR(mp, 0);
        }

        cursor->initialized = 1;
        cursor->eof = 0;

        if (data && btree_read_data(cursor->bt, mp, leaf, data) != BT_SUCCESS)
                return BT_FAIL;

        if (bt_set_key(cursor->bt, mp, leaf, key) != 0)
                return BT_FAIL;
        DPRINTF("==> cursor placed on key %.*s",
            (int)key->size, (char *)key->data);

        return BT_SUCCESS;
}

static int
btree_cursor_first(struct cursor *cursor, struct btval *key, struct btval *data)
{
        int              rc;
        struct mpage    *mp;
        struct node     *leaf;

        rc = btree_search_page(cursor->bt, cursor->txn, NULL, cursor, SearchFirst, 0, &mp);
        if (rc != BT_SUCCESS)
                return rc;
        assert(IS_LEAF(mp));

        leaf = NODEPTR(mp, 0);
        cursor->initialized = 1;
        cursor->eof = 0;

        if (data && btree_read_data(cursor->bt, mp, leaf, data) != BT_SUCCESS)
                return BT_FAIL;

        if (bt_set_key(cursor->bt, mp, leaf, key) != 0)
                return BT_FAIL;

        return BT_SUCCESS;
}

static int
btree_cursor_last(struct cursor *cursor, struct btval *key, struct btval *data)
{
        int              rc;
        struct mpage    *mp;
        struct node     *leaf;
        struct ppage    *top;

        rc = btree_search_page(cursor->bt, cursor->txn, NULL, cursor, SearchLast, 0, &mp);
        if (rc != BT_SUCCESS)
                return rc;
        assert(IS_LEAF(mp));

        top = CURSOR_TOP(cursor);
        // get the last leaf in the page
        top->ki = NUMKEYS(mp)-1;
        leaf = NODEPTR(mp, top->ki);
        cursor->initialized = 1;
        cursor->eof = 0;

        if (data && btree_read_data(cursor->bt, mp, leaf, data) != BT_SUCCESS)
                return BT_FAIL;

        if (bt_set_key(cursor->bt, mp, leaf, key) != 0)
                return BT_FAIL;

        return BT_SUCCESS;
}

int
btree_cursor_get(struct cursor *cursor, struct btval *key, struct btval *data,
    enum cursor_op op)
{
        int              rc;
        int              exact = 0;

        assert(cursor);

        switch (op) {
        case BT_CURSOR:
        case BT_CURSOR_EXACT:
                while (CURSOR_TOP(cursor) != NULL)
                        cursor_pop_page(cursor);
                if (key == NULL || key->size == 0 || key->size > MAXKEYSIZE) {
                        errno = EINVAL;
                        rc = BT_FAIL;
                } else if (op == BT_CURSOR_EXACT)
                        rc = btree_cursor_set(cursor, key, data, &exact);
                else
                        rc = btree_cursor_set(cursor, key, data, NULL);
                break;
        case BT_NEXT:
                if (!cursor->initialized)
                        rc = btree_cursor_first(cursor, key, data);
                else
                        rc = btree_cursor_next(cursor, key, data);
                break;
        case BT_PREV:
                if (!cursor->initialized)
                        rc = btree_cursor_last(cursor, key, data);
                else
                        rc = btree_cursor_prev(cursor, key, data);
                break;
        case BT_FIRST:
                while (CURSOR_TOP(cursor) != NULL)
                        cursor_pop_page(cursor);
                rc = btree_cursor_first(cursor, key, data);
                break;
        case BT_LAST:
                while (CURSOR_TOP(cursor) != NULL)
                        cursor_pop_page(cursor);
                rc = btree_cursor_last(cursor, key, data);
                break;
        default:
                DPRINTF("unhandled/unimplemented cursor operation %u", op);
                rc = BT_FAIL;
                break;
        }

        mpage_prune(cursor->bt);

        return rc;
}

struct btree *
btree_cursor_bt(struct cursor *cursor)
{
        assert(cursor);
        return cursor->bt;
}

struct btree_txn *
btree_cursor_txn(struct cursor *cursor)
{
        assert(cursor);
        return cursor->txn;
}

static struct mpage *
btree_new_page(struct btree *bt, uint32_t flags)
{
        struct mpage    *mp;

        assert(bt != NULL);
        assert(bt->txn != NULL);

        DPRINTF("allocating new mpage %u, page size %u, flags %0X",
            bt->txn->next_pgno, bt->head.psize, flags);
        if ((mp = (mpage *)calloc(1, sizeof(*mp))) == NULL)
                return NULL;
        if ((mp->page = (page *)malloc(bt->head.psize)) == NULL) {
                free(mp);
                return NULL;
        }
        memset(mp->page, 0, bt->head.psize);
        mp->pgno = mp->page->pgno = bt->txn->next_pgno++;
        mp->page->flags = flags;
        mp->page->lower = PAGEHDRSZ;
        mp->page->upper = bt->head.psize;

        if (IS_BRANCH(mp))
                bt->meta.branch_pages++;
        else if (IS_LEAF(mp))
                bt->meta.leaf_pages++;
        else if (IS_OVERFLOW(mp))
                bt->meta.overflow_pages++;

        mpage_add(bt, mp);
        mpage_dirty(bt, mp);

        return mp;
}

static size_t
bt_leaf_size(struct btree *bt, struct mpage *mp, struct btval *key, struct btval *data)
{
        size_t           sz;

        sz = LEAFSIZE(key, data);
        if (bt_is_overflow(bt, mp, key->size, data->size)) {
                /* put on overflow page */
                sz -= data->size - sizeof(pgno_t);
        }

        return sz + sizeof(indx_t);
}

static int
bt_is_overflow(struct btree *bt, struct mpage *mp, size_t ksize, size_t dsize)
{
        assert(bt && mp);
#ifdef ENABLE_BIG_KEYS
        size_t node_size = dsize + ksize + NODESIZE;
        if ((node_size + sizeof(indx_t) > SIZELEFT(mp))
            || (NUMKEYS(mp) == 0 && (SIZELEFT(mp) - (node_size + sizeof(indx_t))) < MAXKEYSIZE))
                return 1;
#else
        (void)ksize;
        if (dsize >= bt->head.psize / BT_MINKEYS)
                return 1;

#endif
        return 0;
}

static size_t
bt_branch_size(struct btree *bt, struct btval *key)
{
        size_t           sz;

        sz = INDXSIZE(key);
        if (sz >= bt->head.psize / BT_MINKEYS) {
                /* put on overflow page */
                /* not implemented */
                /* sz -= key->size - sizeof(pgno_t); */
        }

        return sz + sizeof(indx_t);
}

static int
btree_write_overflow_data(struct btree *bt, struct page *p, struct btval *data)
{
        size_t           done = 0;
        size_t           sz;
        size_t           max;
        pgno_t          *linkp;                 /* linked page stored here */
        struct mpage    *next = NULL;

        max = bt->head.psize - PAGEHDRSZ;

        while (done < data->size) {
                if (next != NULL)
                        p = next->page;
                linkp = &p->p_next_pgno;
                if (data->size - done > max) {
                        /* need another overflow page */
                        if ((next = btree_new_page(bt, P_OVERFLOW)) == NULL)
                                return BT_FAIL;
                        *linkp = next->pgno;
                        DPRINTF("linking overflow page %u", next->pgno);
                } else
                        *linkp = 0;             /* indicates end of list */
                sz = data->size - done;
                if (sz > max)
                        sz = max;
                DPRINTF("copying %zu bytes to overflow page %u", sz, p->pgno);
                bcopy((char *)data->data + done, p->ptrs, sz);
                done += sz;
        }

        return BT_SUCCESS;
}

/* Key prefix should already be stripped.
 */
static int
btree_add_node(struct btree *bt, struct mpage *mp, indx_t indx,
    struct btval *key, struct btval *data, pgno_t pgno, uint8_t flags)
{
        unsigned int     i;
        size_t           node_size = NODESIZE;
        indx_t           ofs;
        struct node     *node;
        struct page     *p;
        struct mpage    *ofp = NULL;            /* overflow page */

        p = mp->page;
        assert(p->upper >= p->lower);

        DPRINTF("add node [%.*s] to %s page %u at index %i, key size %zu",
            key ? (int)key->size : 0, key ? (char *)key->data : NULL,
            IS_LEAF(mp) ? "leaf" : "branch",
            mp->pgno, indx, key ? key->size : 0);

        if (key != NULL)
                node_size += key->size;

        if (IS_LEAF(mp)) {
                assert(data);
                node_size += data->size;
                if (F_ISSET(flags, F_BIGDATA)) {
                        /* Data already on overflow page. */
                        node_size -= data->size - sizeof(pgno_t);
#ifdef ENABLE_BIG_KEYS
                } else if (bt_is_overflow(bt, mp, (key ? key->size : 0), data->size)) {
#else
                } else if (bt_is_overflow(bt, mp, (key ? key->size : 0), data->size)
                           || (node_size + sizeof(indx_t) > SIZELEFT(mp))) {
#endif
                        /* Put data on overflow page. */
                        DPRINTF("data size is %zu, put on overflow page",
                            data->size);
                        node_size -= data->size - sizeof(pgno_t);
                        if ((ofp = btree_new_page(bt, P_OVERFLOW)) == NULL)
                                return BT_FAIL;
                        DPRINTF("allocated overflow page %u", ofp->pgno);
                        flags |= F_BIGDATA;
                }
        }

        if (node_size + sizeof(indx_t) > SIZELEFT(mp)) {
                DPRINTF("not enough room in page %u, got %lu ptrs",
                    mp->pgno, NUMKEYS(mp));
                DPRINTF("upper - lower = %u - %u = %u", p->upper, p->lower,
                    p->upper - p->lower);
                DPRINTF("node size = %zu", node_size);
                return BT_FAIL;
        }

        /* Move higher pointers up one slot. */
        for (i = NUMKEYS(mp); i > indx; i--)
                p->ptrs[i] = p->ptrs[i - 1];

        /* Adjust free space offsets. */
        ofs = p->upper - node_size;
        assert(ofs >= p->lower + sizeof(indx_t));
        p->ptrs[indx] = ofs;
        p->upper = ofs;
        p->lower += sizeof(indx_t);

        /* Write the node data. */
        node = NODEPTR(mp, indx);
        node->ksize = (key == NULL) ? 0 : key->size;
        node->flags = flags;
        if (IS_LEAF(mp)) {
                node->n_dsize = data->size;
        } else {
                node->n_pgno = pgno;
        }

        if (key)
                bcopy(key->data, NODEKEY(node), key->size);

        if (IS_LEAF(mp)) {
                assert(key);
                if (ofp == NULL) {
                        if (F_ISSET(flags, F_BIGDATA))
                                bcopy(data->data, node->data + key->size,
                                    sizeof(pgno_t));
                        else
                                bcopy(data->data, node->data + key->size,
                                    data->size);
                } else {
                        bcopy(&ofp->pgno, node->data + key->size,
                            sizeof(pgno_t));
                        if (btree_write_overflow_data(bt, ofp->page,
                            data) == BT_FAIL)
                                return BT_FAIL;
                }
        }

        return BT_SUCCESS;
}

static void
btree_del_node(struct btree *, struct mpage *mp, indx_t indx)
{
        unsigned int     sz;
        indx_t           i, j, numkeys, ptr;
        struct node     *node;
        char            *base;

        DPRINTF("delete node %u on %s page %u", indx,
            IS_LEAF(mp) ? "leaf" : "branch", mp->pgno);
        assert(indx < NUMKEYS(mp));

        node = NODEPTR(mp, indx);
        sz = NODESIZE + node->ksize;
        if (IS_LEAF(mp)) {
                if (F_ISSET(node->flags, F_BIGDATA))
                        sz += sizeof(pgno_t);
                else
                        sz += NODEDSZ(node);
        }

        ptr = mp->page->ptrs[indx];
        numkeys = NUMKEYS(mp);
        for (i = j = 0; i < numkeys; i++) {
                if (i != indx) {
                        mp->page->ptrs[j] = mp->page->ptrs[i];
                        if (mp->page->ptrs[i] < ptr)
                                mp->page->ptrs[j] += sz;
                        j++;
                }
        }

        base = (char *)mp->page + mp->page->upper;
        bcopy(base, base + sz, ptr - mp->page->upper);

        mp->page->lower -= sizeof(indx_t);
        mp->page->upper += sz;
}

struct cursor *
btree_txn_cursor_open(struct btree *bt, struct btree_txn *txn)
{
        struct cursor   *cursor;

        if (bt != NULL && txn != NULL && bt != txn->bt) {
                errno = EINVAL;
                DPRINTF("bt=%p does not belong to txn=%p (txn->bt=%p)", bt, txn, txn->bt);
                return NULL;
        }

        if (bt == NULL) {
                if (txn == NULL) {
                        errno = EINVAL;
                        DPRINTF("bt and txn both null");
                        return NULL;
                }
                bt = txn->bt;
        }

        if ((cursor = (struct cursor *)calloc(1, sizeof(struct cursor))) != NULL) {
                SLIST_INIT(&cursor->stack);
                cursor->bt = bt;
                cursor->txn = txn;
                btree_ref(bt);
        }

        return cursor;
}

void
btree_cursor_close(struct cursor *cursor)
{
        if (cursor != NULL) {
                while (!CURSOR_EMPTY(cursor))
                        cursor_pop_page(cursor);

                btree_close(cursor->bt);
                free(cursor);
        }
}

static int
btree_update_key(struct btree *, struct mpage *mp, indx_t indx,
                 struct btval *key)
{
        indx_t                   ptr, i, numkeys;
        int                      delta;
        size_t                   len;
        struct node             *node;
        char                    *base;

        node = NODEPTR(mp, indx);
        ptr = mp->page->ptrs[indx];
        DPRINTF("update key %u (ofs %u) [%.*s] to [%.*s] on page %u",
            indx, ptr,
            (int)node->ksize, (char *)NODEKEY(node),
            (int)key->size, (char *)key->data,
            mp->pgno);

        if (key->size != node->ksize) {
                delta = key->size - node->ksize;
                if (delta > 0 && SIZELEFT(mp) < delta) {
                        DPRINTF("OUCH! Not enough room, delta = %d", delta);
                        return BT_FAIL;
                }

                numkeys = NUMKEYS(mp);
                for (i = 0; i < numkeys; i++) {
                        if (mp->page->ptrs[i] <= ptr)
                                mp->page->ptrs[i] -= delta;
                }

                base = (char *)mp->page + mp->page->upper;
                len = ptr - mp->page->upper + NODESIZE;
                bcopy(base, base - delta, len);
                mp->page->upper -= delta;

                node = NODEPTR(mp, indx);
                node->ksize = key->size;
        }

        bcopy(key->data, NODEKEY(node), key->size);

        return BT_SUCCESS;
}

static int
btree_adjust_prefix(struct btree *bt, struct mpage *src, int delta)
{
        indx_t           i;
        struct node     *node;
        struct btkey     tmpkey;
        struct btval     key;

        DPRINTF("adjusting prefix lengths on page %u with delta %d",
            src->pgno, delta);
        assert(delta != 0);

        for (i = 0; i < NUMKEYS(src); i++) {
                node = NODEPTR(src, i);
                tmpkey.len = node->ksize - delta;
                if (delta > 0) {
                        if (F_ISSET(bt->flags, BT_REVERSEKEY))
                                bcopy(NODEKEY(node), tmpkey.str, tmpkey.len);
                        else
                                bcopy((char *)NODEKEY(node) + delta, tmpkey.str,
                                    tmpkey.len);
                } else {
                        if (F_ISSET(bt->flags, BT_REVERSEKEY)) {
                                bcopy(NODEKEY(node), tmpkey.str, node->ksize);
                                bcopy(src->prefix.str, tmpkey.str + node->ksize,
                                    -delta);
                        } else {
                                bcopy(src->prefix.str + src->prefix.len + delta,
                                    tmpkey.str, -delta);
                                bcopy(NODEKEY(node), tmpkey.str - delta,
                                    node->ksize);
                        }
                }
                key.size = tmpkey.len;
                key.data = tmpkey.str;
                if (btree_update_key(bt, src, i, &key) != BT_SUCCESS)
                        return BT_FAIL;
        }

        return BT_SUCCESS;
}

/* Move a node from src to dst.
 */
static int
btree_move_node(struct btree *bt, struct mpage *src, indx_t srcindx,
    struct mpage *dst, indx_t dstindx)
{
        int                      rc;
        unsigned int             pfxlen, mp_pfxlen = 0;
        struct node             *srcnode;
        struct mpage            *mp = NULL;
        struct btkey             tmpkey, srckey;
        struct btval             key, data;

        assert(src->parent);
        assert(dst->parent);

        srcnode = NODEPTR(src, srcindx);
        DPRINTF("moving %s node %u [%.*s] on page %u to node %u on page %u",
            IS_LEAF(src) ? "leaf" : "branch",
            srcindx,
            (int)srcnode->ksize, (char *)NODEKEY(srcnode),
            src->pgno,
            dstindx, dst->pgno);

        find_common_prefix(bt, src);

        if (IS_BRANCH(src)) {
                /* Need to check if the page the moved node points to
                 * changes prefix.
                 */
                if ((mp = btree_get_mpage(bt, NODEPGNO(srcnode))) == NULL)
                        return BT_FAIL;
                mp->parent = src;
                mp->parent_index = srcindx;
                find_common_prefix(bt, mp);
                mp_pfxlen = mp->prefix.len;
        }

        /* Mark src and dst as dirty. */
        if ((src = mpage_touch(bt, src)) == NULL ||
            (dst = mpage_touch(bt, dst)) == NULL)
                return BT_FAIL;

        find_common_prefix(bt, dst);

        /* Check if src node has destination page prefix. Otherwise the
         * destination page must expand its prefix on all its nodes.
         */
        srckey.len = srcnode->ksize;
        bcopy(NODEKEY(srcnode), srckey.str, srckey.len);
        common_prefix(bt, &srckey, &dst->prefix, &tmpkey);
        if (tmpkey.len != dst->prefix.len) {
                if (btree_adjust_prefix(bt, dst,
                    tmpkey.len - dst->prefix.len) != BT_SUCCESS)
                        return BT_FAIL;
                bcopy(&tmpkey, &dst->prefix, sizeof(tmpkey));
        }

        if (srcindx == 0 && IS_BRANCH(src)) {
                struct mpage    *low;

                /* must find the lowest key below src
                 */
                assert(btree_search_page_root(bt, src, NULL, NULL, SearchFirst, 0,
                                              &low) == BT_SUCCESS);
                expand_prefix(bt, low, 0, &srckey);
                DPRINTF("found lowest key [%.*s] on leaf page %u",
                    (int)srckey.len, srckey.str, low->pgno);
        } else {
                srckey.len = srcnode->ksize;
                bcopy(NODEKEY(srcnode), srckey.str, srcnode->ksize);
        }
        find_common_prefix(bt, src);

        /* expand the prefix */
        tmpkey.len = sizeof(tmpkey.str);
        concat_prefix(bt, src->prefix.str, src->prefix.len,
            srckey.str, srckey.len, tmpkey.str, &tmpkey.len);

        /* Add the node to the destination page. Adjust prefix for
         * destination page.
         */
        key.size = tmpkey.len;
        key.data = tmpkey.str;
        remove_prefix(bt, &key, dst->prefix.len);
        data.size = NODEDSZ(srcnode);
        data.data = NODEDATA(srcnode);
        rc = btree_add_node(bt, dst, dstindx, &key, &data, NODEPGNO(srcnode),
            srcnode->flags);
        if (rc != BT_SUCCESS)
                return rc;

        /* Delete the node from the source page.
         */
        btree_del_node(bt, src, srcindx);

        /* Update the parent separators.
         */
        if (srcindx == 0 && src->parent_index != 0) {
                expand_prefix(bt, src, 0, &tmpkey);
                key.size = tmpkey.len;
                key.data = tmpkey.str;
                remove_prefix(bt, &key, src->parent->prefix.len);

                DPRINTF("update separator for source page %u to [%.*s]",
                    src->pgno, (int)key.size, (char *)key.data);
                if (btree_update_key(bt, src->parent, src->parent_index,
                    &key) != BT_SUCCESS)
                        return BT_FAIL;
        }

        if (srcindx == 0 && IS_BRANCH(src)) {
                struct btval     nullkey;
                nullkey.size = 0;
                assert(btree_update_key(bt, src, 0, &nullkey) == BT_SUCCESS);
        }

        if (dstindx == 0 && dst->parent_index != 0) {
                expand_prefix(bt, dst, 0, &tmpkey);
                key.size = tmpkey.len;
                key.data = tmpkey.str;
                remove_prefix(bt, &key, dst->parent->prefix.len);

                DPRINTF("update separator for destination page %u to [%.*s]",
                    dst->pgno, (int)key.size, (char *)key.data);
                if (btree_update_key(bt, dst->parent, dst->parent_index,
                    &key) != BT_SUCCESS)
                        return BT_FAIL;
        }

        if (dstindx == 0 && IS_BRANCH(dst)) {
                struct btval     nullkey;
                nullkey.size = 0;
                assert(btree_update_key(bt, dst, 0, &nullkey) == BT_SUCCESS);
        }

        /* We can get a new page prefix here!
         * Must update keys in all nodes of this page!
         */
        pfxlen = src->prefix.len;
        find_common_prefix(bt, src);
        if (src->prefix.len != pfxlen) {
                if (btree_adjust_prefix(bt, src,
                    src->prefix.len - pfxlen) != BT_SUCCESS)
                        return BT_FAIL;
        }

        pfxlen = dst->prefix.len;
        find_common_prefix(bt, dst);
        if (dst->prefix.len != pfxlen) {
                if (btree_adjust_prefix(bt, dst,
                    dst->prefix.len - pfxlen) != BT_SUCCESS)
                        return BT_FAIL;
        }

        if (IS_BRANCH(dst)) {
                assert(mp);
                mp->parent = dst;
                mp->parent_index = dstindx;
                find_common_prefix(bt, mp);
                if (mp->prefix.len != mp_pfxlen) {
                        DPRINTF("moved branch node has changed prefix");
                        if ((mp = mpage_touch(bt, mp)) == NULL)
                                return BT_FAIL;
                        if (btree_adjust_prefix(bt, mp,
                            mp->prefix.len - mp_pfxlen) != BT_SUCCESS)
                                return BT_FAIL;
                }
        }

        return BT_SUCCESS;
}

static int
btree_merge(struct btree *bt, struct mpage *src, struct mpage *dst)
{
        int                      rc;
        indx_t                   i;
        unsigned int             pfxlen;
        struct node             *srcnode;
        struct btkey             tmpkey, dstpfx;
        struct btval             key, data;

        DPRINTF("merging page %u and %u", src->pgno, dst->pgno);

        assert(src->parent);    /* can't merge root page */
        assert(dst->parent);
        assert(bt->txn != NULL);

        /* Mark src and dst as dirty. */
        if ((src = mpage_touch(bt, src)) == NULL ||
            (dst = mpage_touch(bt, dst)) == NULL)
                return BT_FAIL;

        find_common_prefix(bt, src);
        find_common_prefix(bt, dst);

        /* Check if source nodes has destination page prefix. Otherwise
         * the destination page must expand its prefix on all its nodes.
         */
        common_prefix(bt, &src->prefix, &dst->prefix, &dstpfx);
        if (dstpfx.len != dst->prefix.len) {
                if (btree_adjust_prefix(bt, dst,
                    dstpfx.len - dst->prefix.len) != BT_SUCCESS)
                        return BT_FAIL;
                bcopy(&dstpfx, &dst->prefix, sizeof(dstpfx));
        }

        /* Move all nodes from src to dst.
         */
        for (i = 0; i < NUMKEYS(src); i++) {
                srcnode = NODEPTR(src, i);

                /* If branch node 0 (implicit key), find the real key.
                 */
                if (i == 0 && IS_BRANCH(src)) {
                        struct mpage    *low;

                        /* must find the lowest key below src
                         */
                        assert(btree_search_page_root(bt, src, NULL, NULL, SearchFirst, 0,
                                                      &low) == BT_SUCCESS);
                        expand_prefix(bt, low, 0, &tmpkey);
                        DPRINTF("found lowest key [%.*s] on leaf page %u",
                            (int)tmpkey.len, tmpkey.str, low->pgno);
                } else {
                        expand_prefix(bt, src, i, &tmpkey);
                }

                key.size = tmpkey.len;
                key.data = tmpkey.str;

                remove_prefix(bt, &key, dst->prefix.len);
                data.size = NODEDSZ(srcnode);
                data.data = NODEDATA(srcnode);
                rc = btree_add_node(bt, dst, NUMKEYS(dst), &key,
                    &data, NODEPGNO(srcnode), srcnode->flags);
                if (rc != BT_SUCCESS)
                        return rc;
        }

        DPRINTF("dst page %u now has %lu keys (%.1f%% filled)",
            dst->pgno, NUMKEYS(dst), (float)PAGEFILL(bt, dst) / 10);

        /* Unlink the src page from parent.
         */
        btree_del_node(bt, src->parent, src->parent_index);
        if (src->parent_index == 0) {
                key.size = 0;
                if (btree_update_key(bt, src->parent, 0, &key) != BT_SUCCESS)
                        return BT_FAIL;

                pfxlen = src->prefix.len;
                find_common_prefix(bt, src);
                assert (src->prefix.len == pfxlen);
        }

        if (IS_LEAF(src))
                bt->meta.leaf_pages--;
        else
                bt->meta.branch_pages--;

        return btree_rebalance(bt, src->parent);
}

#define FILL_THRESHOLD   250

static int
btree_rebalance(struct btree *bt, struct mpage *mp)
{
        struct node     *node;
        struct mpage    *parent;
        struct mpage    *root;
        struct mpage    *neighbor = NULL;
        indx_t           si = 0, di = 0;

        assert(bt != NULL);
        assert(bt->txn != NULL);
        assert(mp != NULL);

        DPRINTF("rebalancing %s page %u (has %lu keys, %.1f%% full)",
            IS_LEAF(mp) ? "leaf" : "branch",
            mp->pgno, NUMKEYS(mp), (float)PAGEFILL(bt, mp) / 10);

        if (PAGEFILL(bt, mp) >= FILL_THRESHOLD) {
                DPRINTF("no need to rebalance page %u, above fill threshold",
                    mp->pgno);
                return BT_SUCCESS;
        }

        parent = mp->parent;

        if (parent == NULL) {
                if (NUMKEYS(mp) == 0) {
                        DPRINTF("tree is completely empty");
                        bt->txn->root = P_INVALID;
                        bt->meta.depth--;
                        bt->meta.leaf_pages--;
                } else if (IS_BRANCH(mp) && NUMKEYS(mp) == 1) {
                        DPRINTF("collapsing root page!");
                        bt->txn->root = NODEPGNO(NODEPTR(mp, 0));
                        if ((root = btree_get_mpage(bt, bt->txn->root)) == NULL)
                                return BT_FAIL;
                        root->parent = NULL;
                        bt->meta.depth--;
                        bt->meta.branch_pages--;
                } else
                        DPRINTF("root page doesn't need rebalancing");
                return BT_SUCCESS;
        }

        /* The parent (branch page) must have at least 2 pointers,
         * otherwise the tree is invalid.
         */
        assert(NUMKEYS(parent) > 1);

        /* Leaf page fill factor is below the threshold.
         * Try to move keys from left or right neighbor, or
         * merge with a neighbor page.
         */

        /* Find neighbors.
         */
        if (mp->parent_index == 0) {
                /* We're the leftmost leaf in our parent.
                 */
                DPRINTF("reading right neighbor");
                node = NODEPTR(parent, mp->parent_index + 1);
                if ((neighbor = btree_get_mpage(bt, NODEPGNO(node))) == NULL)
                        return BT_FAIL;
                neighbor->parent_index = mp->parent_index + 1;
                si = 0;
                di = NUMKEYS(mp);
        } else {
                /* There is at least one neighbor to the left.
                 */
                DPRINTF("reading left neighbor");
                node = NODEPTR(parent, mp->parent_index - 1);
                if ((neighbor = btree_get_mpage(bt, NODEPGNO(node))) == NULL)
                        return BT_FAIL;
                neighbor->parent_index = mp->parent_index - 1;
                si = NUMKEYS(neighbor) - 1;
                di = 0;
        }
        neighbor->parent = parent;

        DPRINTF("found neighbor page %u (%lu keys, %.1f%% full)",
            neighbor->pgno, NUMKEYS(neighbor), (float)PAGEFILL(bt, neighbor) / 10);

        // Calculate the size of the node to be moved
        find_common_prefix (bt, neighbor);
        struct btkey oldMpPrefix = mp->prefix;
        find_common_prefix (bt, mp);

        node = NODEPTR(neighbor, si);
        size_t siSize = NODESIZE + node->ksize + neighbor->prefix.len;
        siSize += IS_BRANCH(neighbor) ? 0 : NODEDSZ(node);

        // Calculate the delta for the destination page prefix
        struct btkey nKey, newPrefix;
        nKey.len = node->ksize;
        bcopy(NODEKEY(node), nKey.str, nKey.len);
        common_prefix(bt, &nKey, &oldMpPrefix, &newPrefix);
        size_t prfxDelta = mp->prefix.len - newPrefix.len;

        /* If the neighbor page is above threshold and has at least two
         * keys, move one key from it.
         *
         * Otherwise we should try to merge them, but that might not be
         * possible, even if both are below threshold, as prefix expansion
         * might make keys larger. FIXME: detect this
         */
        if (PAGEFILL(bt, neighbor) >= FILL_THRESHOLD && NUMKEYS(neighbor) >= 2 &&
                NUMKEYS(mp)*prfxDelta + siSize < SIZELEFT(mp)) {
            // Key in parent of the source can change, if
            // we move the node from idx 0
            // We need to make sure that the new key fits into
            // parent page
            bool canUpdate = true;
            if (si == 0 && neighbor->parent_index != 0) {
                expand_prefix(bt, neighbor, 1, &nKey);
                node = NODEPTR(neighbor->parent, neighbor->parent_index);
                int oldLength = node->ksize;
                int newLength = nKey.len - neighbor->parent->prefix.len;
                if (newLength - oldLength > SIZELEFT(neighbor->parent)) canUpdate = false;
            }
            // Key in parent can change if we move into index 0
            // in destination page
            // We need to ensure that the new key fits into
            // parent page
            if (canUpdate && di == 0 && mp->parent_index != 0) {
                expand_prefix(bt, neighbor, si, &nKey);
                node = NODEPTR(mp->parent, mp->parent_index);
                int oldLength = node->ksize;
                int newLength = nKey.len - mp->parent->prefix.len;
                if (newLength - oldLength > SIZELEFT(mp->parent)) canUpdate = false;
            }
            if (canUpdate) {
                return btree_move_node(bt, neighbor, si, mp, di);
            }
        }
        else { /* FIXME: if (has_enough_room()) */
            // Calculate the worst case space requirement
            // This could be improved by calculating the 'prefix difference'
            // Note that worst case free can be negative
            // We are not changing the node at idx 0 so the parent
            // page free space shouldn't be an issue
            int nFree = SIZELEFT(neighbor) - NUMKEYS(neighbor)*neighbor->prefix.len;
            int mpFree = SIZELEFT(mp) - NUMKEYS(mp)*mp->prefix.len;
            int nRequired = 0;
            for (unsigned int i=0; i<NUMKEYS(neighbor); i++) {
                node = NODEPTR(neighbor, i);
                nRequired += NODESIZE + node->ksize;
                nRequired += IS_BRANCH(neighbor) ? 0 : NODEDSZ(node);
            }
            int mpRequired = 0;
            for (unsigned int i=0; i<NUMKEYS(mp); i++) {
                node = NODEPTR(mp, i);
                mpRequired += NODESIZE + node->ksize;
                mpRequired += IS_BRANCH(mp) ? 0 : NODEDSZ(node);
            }

                if (mp->parent_index == 0 && mpFree > nRequired)
                        return btree_merge(bt, neighbor, mp);
                else if (nFree > mpRequired)
                        return btree_merge(bt, mp, neighbor);
        }
        return BT_SUCCESS;
}

int
btree_txn_del(struct btree *bt, struct btree_txn *txn,
    struct btval *key, struct btval *data)
{
        int              rc, exact, close_txn = 0;
        unsigned int     ki;
        struct node     *leaf;
        struct mpage    *mp;

        DPRINTF("========> delete key %.*s", (int)key->size, (char *)key->data);

        assert(key != NULL);

        if (bt != NULL && txn != NULL && bt != txn->bt) {
                errno = EINVAL;
                return BT_FAIL;
        }

        if (txn != NULL && F_ISSET(txn->flags, BT_TXN_RDONLY)) {
                errno = EINVAL;
                return BT_FAIL;
        }

        if (bt == NULL) {
                if (txn == NULL) {
                        errno = EINVAL;
                        return BT_FAIL;
                }
                bt = txn->bt;
        }

        if (key->size == 0 || key->size > MAXKEYSIZE) {
                errno = EINVAL;
                return BT_FAIL;
        }

        if (txn == NULL) {
                close_txn = 1;
                if ((txn = btree_txn_begin(bt, 0)) == NULL)
                        return BT_FAIL;
        }

        if ((rc = btree_search_page(bt, txn, key, NULL, SearchKey, 1, &mp)) != BT_SUCCESS)
                goto done;

        leaf = btree_search_node(bt, mp, key, &exact, &ki);
        if (leaf == NULL || !exact) {
                errno = ENOENT;
                rc = BT_FAIL;
                goto done;
        }

        if (data && (rc = btree_read_data(bt, NULL, leaf, data)) != BT_SUCCESS)
                goto done;
        btree_del_node(bt, mp, ki);
        bt->meta.entries--;
        rc = btree_rebalance(bt, mp);
        if (rc != BT_SUCCESS)
                txn->flags |= BT_TXN_ERROR;

done:
        if (close_txn) {
                if (rc == BT_SUCCESS)
                        rc = btree_txn_commit(txn, 0, 0);
                else
                        btree_txn_abort(txn);
        }
        mpage_prune(bt);
        return rc;
}

/* Reduce the length of the prefix separator <sep> to the minimum length that
 * still makes it uniquely distinguishable from <min>.
 *
 * <min> is guaranteed to be sorted less than <sep>
 *
 * On return, <sep> is modified to the minimum length.
 */
static void
bt_reduce_separator(struct btree *bt, struct node *min, struct btval *sep)
{
        assert(bt);
        size_t           n = 0;
        char            *p1;
        char            *p2;

        if (F_ISSET(bt->flags, BT_REVERSEKEY)) {

                assert(sep->size > 0);

                p1 = (char *)NODEKEY(min) + min->ksize - 1;
                p2 = (char *)sep->data + sep->size - 1;

                while (p1 >= (char *)NODEKEY(min) && *p1 == *p2) {
                        assert(p2 > (char *)sep->data);
                        p1--;
                        p2--;
                        n++;
                }

                sep->data = p2;
                sep->size = n + 1;
        } else {

                assert(min->ksize > 0);
                assert(sep->size > 0);

                p1 = (char *)NODEKEY(min);
                p2 = (char *)sep->data;

                while (*p1 == *p2) {
                        p1++;
                        p2++;
                        n++;
                        if (n == min->ksize || n == sep->size)
                                break;
                }

                sep->size = n + 1;
        }

        DPRINTF("reduced separator to [%.*s] > [%.*s]",
            (int)sep->size, (char *)sep->data,
            (int)min->ksize, (char *)NODEKEY(min));
}

/* Split page <*mpp>, and insert <key,(data|newpgno)> in either left or
 * right sibling, at index <*newindxp> (as if unsplit). Updates *mpp and
 * *newindxp with the actual values after split, ie if *mpp and *newindxp
 * refer to a node in the new right sibling page.
 */
static int
btree_split(struct btree *bt, struct mpage **mpp, unsigned int *newindxp,
    struct btval *newkey, struct btval *newdata, pgno_t newpgno)
{
        uint8_t          flags;
        int              rc = BT_SUCCESS, ins_new = 0;
        indx_t           newindx;
        pgno_t           pgno = 0;
        size_t           orig_pfx_len, left_pfx_diff, right_pfx_diff, pfx_diff;
        unsigned int     i, j, split_indx;
        struct node     *node;
        struct mpage    *pright, *p, *mp;
        struct btval     sepkey, tmpkey, rkey, rdata;
        struct page     *copy;
        void *allocated_block = 0;

        assert(bt != NULL);
        assert(bt->txn != NULL);

        mp = *mpp;
        newindx = *newindxp;

        DPRINTF("-----> splitting %s page %u and adding [%.*s] at index %i",
            IS_LEAF(mp) ? "leaf" : "branch", mp->pgno,
            (int)newkey->size, (char *)newkey->data, *newindxp);
        DPRINTF("page %u has prefix [%.*s]", mp->pgno,
            (int)mp->prefix.len, (char *)mp->prefix.str);
        orig_pfx_len = mp->prefix.len;

        if (mp->parent == NULL) {
                if ((mp->parent = btree_new_page(bt, P_BRANCH)) == NULL)
                        return BT_FAIL;
                mp->parent_index = 0;
                bt->txn->root = mp->parent->pgno;
                DPRINTF("root split! new root = %u", mp->parent->pgno);
                bt->meta.depth++;

                /* Add left (implicit) pointer. */
                if (btree_add_node(bt, mp->parent, 0, NULL, NULL,
                    mp->pgno, 0) != BT_SUCCESS)
                        return BT_FAIL;
        } else {
                DPRINTF("parent branch page is %u", mp->parent->pgno);
        }

        /* Create a right sibling. */
        if ((pright = btree_new_page(bt, mp->page->flags)) == NULL)
                return BT_FAIL;

        pright->parent = mp->parent;
        pright->parent_index = mp->parent_index + 1;
        DPRINTF("new right sibling: page %u", pright->pgno);

        /* Move half of the keys to the right sibling. */
        if ((copy = (page *)malloc(bt->head.psize)) == NULL)
                return BT_FAIL;
        bcopy(mp->page, copy, bt->head.psize);
        assert(mp->ref == 0);                           /* XXX */
        bzero(&mp->page->ptrs, bt->head.psize - PAGEHDRSZ);
        mp->page->lower = PAGEHDRSZ;
        mp->page->upper = bt->head.psize;

        split_indx = NUMKEYSP(copy) / 2 + 1;
        DPRINTF("splitting copy of page %d [split index:%d, newindex:%d]", copy->pgno, split_indx, newindx);

        /* First find the separating key between the split pages.
         */
        bzero(&sepkey, sizeof(sepkey));

#ifdef ENABLE_BIG_KEYS
        /* This could happen if there are less than 3 keys in the tree
         */
        if (split_indx >= NUMKEYSP(copy))
                split_indx = NUMKEYSP(copy) - 1;
#endif

        if (newindx == split_indx) {
                sepkey.size = newkey->size;
                sepkey.data = newkey->data;
                remove_prefix(bt, &sepkey, mp->prefix.len);
        } else {
                node = NODEPTRP(copy, split_indx);
                sepkey.size = node->ksize;
                sepkey.data = NODEKEY(node);
        }

        if (IS_LEAF(mp) && bt->cmp == NULL) {
                /* Find the smallest separator. */
                /* Ref: Prefix B-trees, R. Bayer, K. Unterauer, 1977 */
                node = NODEPTRP(copy, split_indx - 1);
                bt_reduce_separator(bt, node, &sepkey);
        }

        /* Fix separator wrt parent prefix. */
        if (bt->cmp == NULL) {
                DPRINTF("concat prefix [%.*s] to separator [%.*s]",
                        mp->prefix.len, mp->prefix.str,
                        sepkey.size, (char *)sepkey.data);
                tmpkey.size = mp->prefix.len + sepkey.size;
                tmpkey.data = allocated_block = malloc(tmpkey.size);
                tmpkey.free_data = 0;
                tmpkey.mp = 0;
                concat_prefix(bt, mp->prefix.str, mp->prefix.len,
                              (char *)sepkey.data, sepkey.size, (char*)tmpkey.data, &tmpkey.size);
                sepkey = tmpkey;
        }

        DPRINTF("separator is [%.*s]", (int)sepkey.size, (char *)sepkey.data);
        DPRINTF("%d bytes left in parent page %d with branch size %d",
                SIZELEFT(pright->parent), pright->parent->pgno,
                bt_branch_size(bt, &sepkey));

        /* Copy separator key to the parent.
         */
        if (SIZELEFT(pright->parent) < bt_branch_size(bt, &sepkey)) {
                rc = btree_split(bt, &pright->parent, &pright->parent_index,
                    &sepkey, NULL, pright->pgno);

                /* Right page might now have changed parent.
                 * Check if left page also changed parent.
                 */
                if (pright->parent != mp->parent &&
                    mp->parent_index >= NUMKEYS(mp->parent)) {
                        mp->parent = pright->parent;
                        mp->parent_index = pright->parent_index - 1;
                }
        } else {
                DPRINTF("removing %d bytes from seperator [%.*s]",
                    pright->parent->prefix.len,
                    sepkey.size, sepkey.data);
                remove_prefix(bt, &sepkey, pright->parent->prefix.len);
                DPRINTF("adding sepkey [%.*s] to page %d with reference to page %d",
                        sepkey.size, sepkey.data,
                        pright->parent->pgno, pright->pgno);
                rc = btree_add_node(bt, pright->parent, pright->parent_index,
                    &sepkey, NULL, pright->pgno, 0);
        }

        if (rc != BT_SUCCESS) {
                free(copy);
                if (allocated_block) {
                    sepkey.data = allocated_block;
                    sepkey.free_data = 1;
                    btval_reset(&sepkey);
                }
                return BT_FAIL;
        }

        /* Update prefix for right and left page, if the parent was split.
         */
        find_common_prefix(bt, pright);
        assert(orig_pfx_len <= pright->prefix.len);
        right_pfx_diff = pright->prefix.len - orig_pfx_len;
        DPRINTF("right page %d prefix length = %d, diff = %d", pright->pgno, pright->prefix.len, right_pfx_diff);

        find_common_prefix(bt, mp);
        assert(orig_pfx_len <= mp->prefix.len);
        left_pfx_diff = mp->prefix.len - orig_pfx_len;
        DPRINTF("left page %d prefix length = %d, diff = %d", mp->pgno, mp->prefix.len, left_pfx_diff);

        for (i = j = 0; i <= NUMKEYSP(copy); j++) {
                if (i < split_indx) {
                        /* Re-insert in left sibling. */
                        p = mp;
                        pfx_diff = left_pfx_diff;
                } else {
                        /* Insert in right sibling. */
                        if (i == split_indx) {
                                /* Reset insert index for right sibling. */
                                j = (i == newindx && ins_new);
                        }
                        p = pright;
                        pfx_diff = right_pfx_diff;
                }

                if (i == newindx && !ins_new) {
                        /* Insert the original entry that caused the split. */
                        rkey.data = newkey->data;
                        rkey.size = newkey->size;
                        if (IS_LEAF(mp)) {
                                rdata.data = newdata->data;
                                rdata.size = newdata->size;
                        } else
                                pgno = newpgno;
                        flags = 0;
                        pfx_diff = p->prefix.len;

                        ins_new = 1;

                        /* Update page and index for the new key. */
                        *newindxp = j;
                        *mpp = p;
                } else if (i == NUMKEYSP(copy)) {
                        break;
                } else {
                        node = NODEPTRP(copy, i);
                        rkey.data = NODEKEY(node);
                        rkey.size = node->ksize;
                        if (IS_LEAF(mp)) {
                                rdata.data = NODEDATA(node);
                                rdata.size = node->n_dsize;
                        } else
                                pgno = node->n_pgno;
                        flags = node->flags;

                        i++;
                }

                if (!IS_LEAF(mp) && j == 0) {
                        /* First branch index doesn't need key data. */
                        rkey.size = 0;
                } else {
                        remove_prefix(bt, &rkey, pfx_diff);
                }

                rc = btree_add_node(bt, p, j, &rkey, &rdata, pgno,flags);
        }

        free(copy);
        if (allocated_block) {
                sepkey.data = allocated_block;
                sepkey.free_data = 1;
                btval_reset(&sepkey);
        }
        return rc;
}

int
btree_txn_put(struct btree *bt, struct btree_txn *txn,
    struct btval *key, struct btval *data, unsigned int flags)
{
        int              rc = BT_SUCCESS, exact, close_txn = 0;
        unsigned int     ki;
        struct node     *leaf;
        struct mpage    *mp;
        struct btval     xkey;

        assert(key != NULL);
        assert(data != NULL);

        if (bt != NULL && txn != NULL && bt != txn->bt) {
                fprintf(stderr, "%s:%d: transaction does not belong to btree\n",
                        __FUNCTION__, __LINE__);
                errno = EINVAL;
                return BT_FAIL;
        }

        if (txn != NULL && F_ISSET(txn->flags, BT_TXN_RDONLY)) {
                fprintf(stderr, "%s:%d: read-only transaction\n",
                        __FUNCTION__, __LINE__);
                errno = EINVAL;
                return BT_FAIL;
        }

        if (bt == NULL) {
                if (txn == NULL) {
                        fprintf(stderr, "%s:%d: neither transaction nor btree\n",
                                __FUNCTION__, __LINE__);
                        errno = EINVAL;
                        return BT_FAIL;
                }
                bt = txn->bt;
        }

        if (key->size == 0 || key->size > MAXKEYSIZE) {
                fprintf(stderr, "%s:%d: bad key size %Zu (MAXKEYSIZE %d)\n",
                        __FUNCTION__, __LINE__, key->size, MAXKEYSIZE);
                errno = EINVAL;
                return BT_FAIL;
        }

        DPRINTF("==> put key %.*s, size %zu, data size %zu",
                (int)key->size, (char *)key->data, key->size, data->size);

        if (txn == NULL) {
                close_txn = 1;
                if ((txn = btree_txn_begin(bt, 0)) == NULL)
                        return BT_FAIL;
        }

        rc = btree_search_page(bt, txn, key, NULL, SearchKey, 1, &mp);
        if (rc == BT_SUCCESS) {
                leaf = btree_search_node(bt, mp, key, &exact, &ki);
                if (leaf && exact) {
                        if (F_ISSET(flags, BT_NOOVERWRITE)) {
                                DPRINTF("duplicate key %.*s",
                                    (int)key->size, (char *)key->data);
                                fprintf(stderr, "db=%s flags=%x duplicate key %.*s",
                                        bt->path, flags,
                                    (int)key->size, (char *)key->data);
                                errno = EEXIST;
                                rc = BT_FAIL;
                                goto done;
                        }
                        if (!F_ISSET(flags, BT_ALLOWDUPS))
                                btree_del_node(bt, mp, ki);
                }
                if (leaf == NULL) {             /* append if not found */
                        ki = NUMKEYS(mp);
                        DPRINTF("appending key at index %i", ki);
                }
        } else if (errno == ENOENT) {
                /* new file, just write a root leaf page */
                DPRINTF("allocating new root leaf page");
                if ((mp = btree_new_page(bt, P_LEAF)) == NULL) {
                        rc = BT_FAIL;
                        goto done;
                }
                txn->root = mp->pgno;
                bt->meta.depth++;
                ki = 0;
        }
        else
                goto done;

        assert(IS_LEAF(mp));
        DPRINTF("there are %lu keys, should insert new key at index %i",
                NUMKEYS(mp), ki);

        /* Copy the key pointer as it is modified by the prefix code. The
         * caller might have malloc'ed the data.
         */
        xkey.data = key->data;
        xkey.size = key->size;

        if (SIZELEFT(mp) < bt_leaf_size(bt, mp, key, data)) {
                rc = btree_split(bt, &mp, &ki, &xkey, data, P_INVALID);
        } else {
                /* There is room already in this leaf page. */
                remove_prefix(bt, &xkey, mp->prefix.len);
                rc = btree_add_node(bt, mp, ki, &xkey, data, 0, 0);
        }

        if (rc != BT_SUCCESS)
                txn->flags |= BT_TXN_ERROR;
        else
                bt->meta.entries++;

done:
        if (close_txn) {
                if (rc == BT_SUCCESS)
                        rc = btree_txn_commit(txn, 0, 0);
                else
                        btree_txn_abort(txn);
        }
        mpage_prune(bt);
        return rc;
}

static pgno_t
btree_compact_tree(struct btree *bt, pgno_t pgno, struct btree *btc)
{
        ssize_t          rc;
        indx_t           i;
        pgno_t          *pnext, next;
        struct node     *node;
        struct page     *p;
        struct mpage    *mp;
        /* Get the page and make a copy of it.
         */
        if ((mp = btree_get_mpage(bt, pgno)) == NULL)
                return P_INVALID;
        if ((p = (page *)malloc(bt->head.psize)) == NULL)
                return P_INVALID;
        bcopy(mp->page, p, bt->head.psize);

        /* Go through all nodes in the (copied) page and update the
         * page pointers.
         */
        if (F_ISSET(p->flags, P_BRANCH)) {
                for (i = 0; i < NUMKEYSP(p); i++) {
                        node = NODEPTRP(p, i);
                        node->n_pgno = btree_compact_tree(bt, node->n_pgno, btc);
                        if (node->n_pgno == P_INVALID) {
                                free(p);
                                return P_INVALID;
                        }
                }
        } else if (F_ISSET(p->flags, P_LEAF)) {
                for (i = 0; i < NUMKEYSP(p); i++) {
                        node = NODEPTRP(p, i);
                        if (F_ISSET(node->flags, F_BIGDATA)) {
                                bcopy(NODEDATA(node), &next, sizeof(next));
                                next = btree_compact_tree(bt, next, btc);
                                if (next == P_INVALID) {
                                        free(p);
                                        return P_INVALID;
                                }
                                bcopy(&next, NODEDATA(node), sizeof(next));
                        }
                }
        } else if (F_ISSET(p->flags, P_OVERFLOW)) {
                pnext = &p->p_next_pgno;
                if (*pnext > 0) {
                        *pnext = btree_compact_tree(bt, *pnext, btc);
                        if (*pnext == P_INVALID) {
                                free(p);
                                return P_INVALID;
                        }
                }
        } else
                assert(0);

        pgno = p->pgno = btc->txn->next_pgno++;
        p->checksum = calculate_checksum(bt, p);
        DPRINTF("writing page %u with checksum %x", p->pgno, p->checksum);
        rc = write(btc->fd, p, bt->head.psize);
        free(p);
        if (rc != (ssize_t)bt->head.psize)
                return P_INVALID;
        mpage_prune(bt);
        return pgno;
}

int
btree_compact(struct btree *bt)
{
        char                    *compact_path = NULL;
        const char               compact_ext[] = ".compact.XXXXXX";
        struct btree            *btc;
        struct btree_txn        *txn, *txnc = NULL;
        int                      fd;
        pgno_t                   root;

        assert(bt != NULL);

        DPRINTF("compacting btree %p with path %s", bt, bt->path);

        if (bt->path == NULL) {
                errno = EINVAL;
                return BT_FAIL;
        }

        if ((txn = btree_txn_begin(bt, 0)) == NULL)
                return BT_FAIL;

        compact_path = (char*)malloc(strlen(bt->path) + strlen(compact_ext) + 1);
        strcpy(compact_path, bt->path);
        strcat(compact_path, compact_ext);

        fd = mkstemp(compact_path);
        if (fd == -1) {
                EPRINTF("failed to get fd for compact file");
                free(compact_path);
                btree_txn_abort(txn);
                return BT_FAIL;
        }

        if ((btc = btree_open_fd(compact_path, fd, bt->flags)) == NULL)
                goto failed;
        DPRINTF("opened btree %p for compacting", btc);
        bcopy(&bt->meta, &btc->meta, sizeof(bt->meta));
        btc->meta.revisions = 0;

        if ((txnc = btree_txn_begin(btc, 0)) == NULL)
                goto failed;

        if (bt->meta.root != P_INVALID) {
                root = btree_compact_tree(bt, bt->meta.root, btc);
                if (root == P_INVALID)
                        goto failed;
                fsync(fd);
                if (btree_write_meta(btc, root, BT_MARKER, bt->meta.tag) != BT_SUCCESS)
                        goto failed;
        } else {
            if (btree_write_meta(btc, btc->meta.root, BT_MARKER, bt->meta.tag) != BT_SUCCESS)
                    goto failed;
        }

        fsync(fd);

        DPRINTF("renaming %s to %s", compact_path, bt->path);
        if (rename(compact_path, bt->path) != 0)
                goto failed;

        /* Write a "tombstone" meta page so other processes can pick up
         * the change and re-open the file.
         */
        if (btree_write_meta(bt, P_INVALID, BT_TOMBSTONE, 0) != BT_SUCCESS)
                goto failed;

        btree_txn_abort(txn);
        btree_txn_abort(txnc);
        free(compact_path);
        btree_close(btc);
        mpage_prune(bt);
        return 0;

failed:
        btree_txn_abort(txn);
        btree_txn_abort(txnc);
        unlink(compact_path);
        free(compact_path);
        btree_close(btc);
        mpage_prune(bt);
        return BT_FAIL;
}

/* Reverts the last change. Truncates the file at the last root page.
 */
int
btree_revert(struct btree *bt)
{
        if (btree_read_meta(bt, NULL) != 0)
                return -1;

        DPRINTF("truncating file at page %u", bt->meta.root);
        fprintf(stderr, "truncating file at page %u\n", bt->meta.root);
        return ftruncate(bt->fd, bt->head.psize * bt->meta.root);
}

/* Rollback to the previous meta page. Truncates the file at that meta page.
 */
int
btree_rollback(struct btree *bt)
{
        struct bt_meta  *meta;
        struct mpage    *mp;
        pgno_t prev_meta_pgno;
        int ret;

        if (btree_read_meta(bt, NULL) != 0)
                return -1;

        prev_meta_pgno = bt->meta.prev_meta;
        DPRINTF("prev_meta_pgno=%d\n", prev_meta_pgno);
        if ((mp = btree_get_mpage(bt, prev_meta_pgno)) == NULL) {
                return -1;
        }
        if (btree_is_meta_page(bt, mp->page)) {
                meta = METADATA(mp->page);
        } else {
                fprintf(stderr, "mp->page flags %x\n", mp->page->flags);
                return -2;
        }

        if (prev_meta_pgno != meta->pgno) {
                fprintf(stderr, "read wrong meta page! %d != %d\n", prev_meta_pgno, meta->pgno);
                return -1;
        }

        DPRINTF("truncating file at page %u", prev_meta_pgno);
        ret = ftruncate(bt->fd, bt->head.psize * prev_meta_pgno);
        if (ret != 0) {
                fprintf(stderr, "ftruncate failed on size %d\n", bt->head.psize * prev_meta_pgno);
                return ret;
        }
        bt->size = bt->head.psize * prev_meta_pgno;
        memcpy(&bt->meta, meta, sizeof(struct bt_meta));
        return BT_SUCCESS;
}

void
btree_set_cache_size(struct btree *bt, unsigned int cache_size)
{
        assert(bt);
        if (cache_size)
                bt->stat.max_cache = cache_size;
}

unsigned int
btree_get_flags(struct btree *bt)
{
        return (bt->flags & ~BT_FIXPADDING);
}

const char *
btree_get_path(struct btree *bt)
{
        return bt->path;
}

const struct btree_stat *
btree_stat(struct btree *bt)
{
        if (bt == NULL)
                return NULL;

        bt->stat.branch_pages = bt->meta.branch_pages;
        bt->stat.leaf_pages = bt->meta.leaf_pages;
        bt->stat.overflow_pages = bt->meta.overflow_pages;
        bt->stat.revisions = bt->meta.revisions;
        bt->stat.depth = bt->meta.depth;
        bt->stat.entries = bt->meta.entries;
        bt->stat.psize = bt->head.psize;
        bt->stat.created_at = bt->meta.created_at;
        bt->stat.tag = bt->meta.tag;
        bt->stat.ksize = bt->head.ksize;

        return &bt->stat;
}

const char *
tohexstr(const char *src, size_t srclen, char* dst, size_t dstlen, int tobytes = 0)
{
    assert(dstlen > 1);
    int bytesleft = dstlen - 1; // reserve space for the null charact
    *dst = 0;
    char *ptr = dst;
    for (size_t i = 0; i < srclen; ++i) {
            if (!tobytes) {
                    if (bytesleft < 2)
                            break;
                    sprintf(ptr, "%02X",(unsigned char)src[i]);
                    bytesleft -= 2;
                    ptr += 2;
            } else {
                    if (bytesleft < 1)
                            break;
                    sprintf(ptr, "%c", src[i]);
                    bytesleft -= 1;
                    ptr += 1;
            }
    }
    return dst;
}

const char *
get_node_data(struct btree *bt, struct node *node, char *dst, size_t dstlen)
{
    assert(dstlen > 1);
    if (F_ISSET(node->flags, F_BIGDATA)) {
        btval data;
        if (btree_read_data(bt, 0, node, &data) == BT_FAIL) {
            strncpy(dst, "ERROR: could not read overflow page", dstlen);
            dst[dstlen - 1] = 0;
            return dst;
        }
        tohexstr((const char *)data.data, data.size, dst, dstlen);
        btval_reset(&data);
        return dst;
    } else {
        return tohexstr((const char*)NODEDATA(node), NODEDSZ(node), dst, dstlen);
    }
}

void
btree_dump_tree(struct btree *bt, pgno_t pgno, int depth)
{
        indx_t           i;
        pgno_t          *pnext, next;
        struct node     *node;
        struct page     *p;
        struct mpage    *mp;
        char indent[32] = {0};
        const int hexlen = MAXKEYSIZE;
        char khexstr[hexlen];
        char dhexstr[hexlen];

        for (i = 0; i < depth + 1; ++i)
                strcat(&indent[i], "\t");

        /* Get the page.
         */
        if ((mp = btree_get_mpage(bt, pgno)) == NULL)
                return;
        p = mp->page;
        if (F_ISSET(p->flags, P_BRANCH)) {
                fprintf(stderr, "%s", indent);
                fprintf(stderr, "Branch page %d [bytes-free:%d, num-keys:%zu]\n",
                        pgno,
                        SIZELEFT(mp),
                        NUMKEYSP(p));
                for (i = 0; i < NUMKEYSP(p); i++) {
                        node = NODEPTRP(p, i);
                        fprintf(stderr, "%s", indent);
                        fprintf(stderr, "-> Node %d points to page %d with seperator [%s]\n",
                                i,
                                NODEPGNO(node),
                                tohexstr((const char*)node->data, node->ksize, khexstr, hexlen));
                        btree_dump_tree(bt, node->n_pgno, depth + 1);
                }
        } else if (F_ISSET(p->flags, P_LEAF)) {
                fprintf(stderr, "%s", indent);
                fprintf(stderr, "Leaf page %d [bytes-free:%d, num-keys:%zu] with prefix [%.*s]\n",
                        pgno,
                        SIZELEFT(mp),
                        NUMKEYSP(p),
                        (int)mp->prefix.len, mp->prefix.str);
                for (i = 0; i < NUMKEYSP(p); i++) {
                        node = NODEPTRP(p, i);
                        fprintf(stderr, "%s", indent);
                        fprintf(stderr, "-> Node %d [key:%s, data:%s]\n",
                                i,
                                tohexstr((const char*)node->data, node->ksize, khexstr, hexlen),
                                get_node_data(bt, node, dhexstr, hexlen));
                        if (F_ISSET(node->flags, F_BIGDATA)) {
                                bcopy(NODEDATA(node), &next, sizeof(next));
                                fprintf(stderr, "%s", indent);
                                fprintf (stderr, "[!] Data size %d is on overflow page %d\n", node->n_dsize, next);
                                btree_dump_tree(bt, next, depth + 1);
                        }
                }
        } else if (F_ISSET(p->flags, P_OVERFLOW)) {
                fprintf(stderr, "%s", indent);
                pnext = &p->p_next_pgno;
                if (*pnext > 0)
                        fprintf (stderr, "Overflow page %d -> %d\n", pgno, *pnext);
                else
                        fprintf (stderr, "Overflow page %d -> NULL\n", pgno);
                if (*pnext > 0)
                        btree_dump_tree(bt, *pnext, depth);
        } else
                assert(0);
}

void
btree_dump(struct btree *bt)
{
        assert(bt != NULL);
        fprintf(stderr, "btree_dump %s\n", bt->path);
        if (bt->meta.root != P_INVALID) {
                fprintf(stderr, "Root page %d [depth:%d, entries:%" PRIu64 ", leaves:%d, branches:%d, bt-size:%ld, psize:%d]\n",
                        bt->meta.root,
                        bt->meta.depth,
                        bt->meta.entries,
                        bt->meta.leaf_pages,
                        bt->meta.branch_pages,
                        (long)bt->size,
                        bt->head.psize);
                btree_dump_tree(bt, bt->meta.root, 0);
        } else {
                fprintf(stderr, "Root page invalid.\n");
        }
        fflush(stderr);
}

void
btree_dump_page_from_memory(struct page *p)
{
        indx_t           i;
        pgno_t           pgno;
        struct node     *node;
        struct bt_head  *head;
        const char      *pgstr = F_ISSET(p->flags, P_BRANCH) ? "Branch" : "Leaf";
        const int hexlen = 512;
        char dhexstr[hexlen];
        char khexstr[hexlen];

        head = (bt_head*)METADATA(p);
        pgno = p->pgno;

        if (head->magic != BT_MAGIC) {
                EPRINTF("header has invalid magic");
                errno = EINVAL;
                return;
        }

        if (head->version != BT_VERSION) {
                EPRINTF("database is version %u, expected version %u",
                    head->version, BT_VERSION);
                errno = EINVAL;
                return;
        }

        if (head->ksize != MAXKEYSIZE) {
                EPRINTF("database uses max key size %u, expected max key size %u",
                    head->ksize, MAXKEYSIZE);
                errno = EINVAL;
                return;
        }

        fprintf(stderr, "* %s page %d with flags %0X offsets [%d -> %d]\n", pgstr, pgno,
                p->flags, p->lower, p->upper);

        for (i = 0; i < NUMKEYSP(p); i++) {
                node = NODEPTRP(p, i);
                fprintf(stderr, "-> Node %d [key:%s, data:%s]\n",
                        i,
                        tohexstr((const char*)node->data, node->ksize, khexstr, hexlen),
                        tohexstr((const char*)NODEDATA(node), NODEDSZ(node), dhexstr, hexlen));
        }
}

int
btree_dump_page_from_file(const char *filename, unsigned int pagen)
{
    int fd;
    ssize_t rc;
    char header[PAGESIZE];
    struct page *p;
    struct bt_head *h;

    fd = open(filename, O_RDONLY);
    if (fd == 0)
        return 0;

    // read header
    if ((rc = pread(fd, header, PAGESIZE, 0)) == 0) {
        errno = ENOENT;
        return 0;
    } else if (rc != PAGESIZE) {
        if (rc > 0)
            errno = EINVAL;
        EPRINTF("read: %s", strerror(errno));
        return 0;
    }

    p = (struct page *)header;

    if (!F_ISSET(p->flags, P_HEAD)) {
            EPRINTF("page %d not a header page", p->pgno);
            errno = EINVAL;
            return 0;
    }

    h = (bt_head *)METADATA(p);
    if (h->magic != BT_MAGIC) {
            EPRINTF("header has invalid magic");
            errno = EINVAL;
            return 0;
    }

    if (h->version != BT_VERSION) {
            EPRINTF("database is version %u, expected version %u",
                h->version, BT_VERSION);
            errno = EINVAL;
            return 0;
    }

    if (h->ksize != MAXKEYSIZE) {
            EPRINTF("database uses max key size %u, expected max key size %u",
                h->ksize, MAXKEYSIZE);
            errno = EINVAL;
            return 0;
    }

    if ((p = (page *)calloc(1, h->psize)) == NULL)
            return 0;
    rc = pread(fd, p, h->psize, pagen * h->psize);
    if (rc < 0) {
        fprintf(stderr, "error reading page %d\n", pagen);
        return 0;
    } else if (rc < (ssize_t)h->psize) {
        fprintf(stderr, "read incomplete page %d (%d != %d)\n", pagen, h->psize, (int32_t)rc);
        return 0;
    }

    fprintf(stderr, "page %d:\n", pagen);
    fprintf(stderr, "\tpgno:%d\n", p->pgno);
    fprintf(stderr, "\tflags:%d: ", p->flags);
    if (p->flags & P_BRANCH)
        fprintf(stderr, "BRANCH ");
    if (p->flags & P_LEAF)
        fprintf(stderr, "LEAF ");
    if (p->flags & P_OVERFLOW)
        fprintf(stderr, "OVERFLOW ");
    if (p->flags & P_META)
        fprintf(stderr, "META ");
    if (p->flags & P_HEAD)
        fprintf(stderr, "HEAD");
    fprintf(stderr, "\n");
    fprintf(stderr, "\tb.fb_lower:%d\n", p->b.fb.fb_lower);
    fprintf(stderr, "\tb.fb_upper:%d\n", p->b.fb.fb_upper);
    fprintf(stderr, "\tb.pb_next_pgno:%d\n", p->b.pb_next_pgno);

    if (p->flags & P_META) {
        bt_meta *m = METADATA(p);
        fprintf(stderr, "\n");
        fprintf(stderr, "\tmeta->pgno: %d\n", m->pgno);
        fprintf(stderr, "\tmeta->flags: %d\n", m->flags);
        fprintf(stderr, "\tmeta->root: %d\n", m->root);
        fprintf(stderr, "\tmeta->prev_meta: %d\n", m->prev_meta);
        char *st = ctime((const time_t *)&m->created_at);
        fprintf(stderr, "\tmeta->created_at: %s", (st ? st : "(null)\n"));
        fprintf(stderr, "\tmeta->tag: %d\n", m->tag);
    }
    free(p);
    return 1;
}
