#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define MAX_STR_LEN_BITS (54)
#define MAX_STR_LEN ((1UL << MAX_STR_LEN_BITS) - 1)

#define STACK_SIZE 15
#define LARGE_STRING_LEN 256

typedef union {
    /* allow strings up to 15 bytes to stay on the stack
     * use the last byte as a null terminator and to store flags
     * much like fbstring:
     * https://github.com/facebook/folly/blob/master/folly/docs/FBString.md
     */
    char data[STACK_SIZE + 1];

    struct {
        uint8_t filler[STACK_SIZE],
            /* how many free bytes in this stack allocated string
             * same idea as fbstring
             */
            space_left : 4,
            /* if it is on heap, set to 1 */
            is_ptr : 1, is_large_string : 1, flag2 : 1, flag3 : 1;
    };

    /* heap allocated */
    struct {
        char *ptr;
        /* supports strings up to 2^MAX_STR_LEN_BITS - 1 bytes */
        size_t size : MAX_STR_LEN_BITS,
                      /* capacity is always a power of 2 (unsigned)-1 */
                      capacity : 6;
        /* the last 4 bits are important flags */
    };
} xs;

static inline bool xs_is_ptr(const xs *x) { return x->is_ptr; }

static inline bool xs_is_large_string(const xs *x)
{
    return x->is_large_string;
}

static inline size_t xs_size(const xs *x)
{
    return xs_is_ptr(x) ? x->size : STACK_SIZE - x->space_left;
}

static inline void xs_set_size(xs *x, size_t s)
{
    if (!xs_is_ptr(x))
        x->space_left = STACK_SIZE - s;
    else
        x->size = s;
}

static inline char *xs_data(const xs *x)
{
    if (!xs_is_ptr(x))
        return (char *) x->data;
    else if (xs_is_large_string(x))
        return (char *) (x->ptr + 4);
    else 
        return (char *) x->ptr;
}

static inline size_t xs_capacity(const xs *x)
{
    return xs_is_ptr(x) ? ((size_t) 1UL << x->capacity) - 1 : 15;
}

static inline void xs_set_refcnt(const xs *x, int val)
{
    *((int *) x->ptr) = val;
}

static inline void xs_inc_refcnt(const xs *x)
{
    if (xs_is_large_string(x))
        ++(*(int *) x->ptr);
}

static inline int xs_dec_refcnt(const xs *x)
{
    if (!xs_is_large_string(x))
        return 0;
    return --(*(int *) x->ptr);
}

static inline int xs_get_refcnt(const xs *x)
{
    if (!xs_is_large_string(x))
        return 0;
    return *(int *) x->ptr;
}

#define xs_literal_empty() \
    (xs) { .data[0] = '\0', .space_left = 15, .is_ptr = 0, .is_large_string = 0 }

/* lowerbound (floor log2) */
static inline int ilog2(uint32_t n) { return 32 - __builtin_clz(n) - 1; }

static inline xs *xs_newempty(xs *x)
{
    *x = xs_literal_empty();
    return x;
}

static inline xs *xs_free(xs *x)
{
    if (xs_is_ptr(x) && xs_dec_refcnt(x) <= 0)
        free(x->ptr);
    return xs_newempty(x);
}

/* Allocate enough memory to store string of size len.
 * It will free the previously allocated memory if there is.
 */
static void xs_allocate(xs *x, size_t len)
{
    x->capacity = ilog2(len) + 1;
    xs_free(x);

    if (len >= LARGE_STRING_LEN) {
        /* Large string */
        x->is_large_string = 1;
        /* The extra 4 bytes are used to store the reference count */
        x->ptr = malloc((size_t)(1UL << x->capacity) + 4);
        x->is_ptr = 1;
        xs_set_refcnt(x, 1);
    } else if (len > STACK_SIZE) {
        /* Medium string */
        x->ptr = malloc((size_t) 1UL << x->capacity);
        x->is_ptr = 1;
    }
}

xs *xs_new(xs *x, const void *p)
{
    size_t len = strlen(p);
    xs_allocate(x, len);
    memcpy(xs_data(x), p, len + 1);
    xs_set_size(x, len);
    return x;
}

/* Memory leaks happen if the string is too long but it is still useful for
 * short strings.
 */
#define xs_tmp(x)                                                   \
    ((void) ((struct {                                              \
         _Static_assert(sizeof(x) <= MAX_STR_LEN, "it is too big"); \
         int dummy;                                                 \
     }){1}),                                                        \
     xs_new(&xs_literal_empty(), x))

/* grow up to specified size */
xs *xs_grow(xs *x, size_t len)
{
    char buf[16];
    char *backup, *f = NULL;

    if (len <= xs_capacity(x))
        return x;

    /* Backup first */
    if (xs_is_ptr(x)) {
        backup = xs_data(x);
        f = x->ptr;
        x->is_ptr = 0;
    } else {
        memcpy(buf, x->data, 16);
        backup = (char *) &buf;
    }

    xs_allocate(x, len);
    memcpy(xs_data(x), backup, xs_size(x));

    if (f)
        free(f);

    return x;
}

static inline xs *xs_cpy(xs *dest, xs *src)
{
    xs_free(dest);
    *dest = *src;
    size_t len = xs_size(src);
    if (len >= LARGE_STRING_LEN)
        xs_inc_refcnt(src);
    else if (len > STACK_SIZE) {
        dest->is_ptr = 0;
        xs_allocate(dest, len);
        memcpy(xs_data(dest), xs_data(src), len + 1);
    }
    return dest;
}

static bool xs_cow_lazy_copy(xs *x)
{
    if (xs_get_refcnt(x) <= 1)
        return false;

    /* Lazy copy */
    char *data = xs_data(x);
    xs_dec_refcnt(x);
    x->is_ptr = 0;
    xs_allocate(x, xs_size(x));

    memcpy(xs_data(x), data, x->size + 1);
    return true;
}

xs *xs_concat(xs *string, const xs *prefix, const xs *suffix)
{
    size_t pres = xs_size(prefix), sufs = xs_size(suffix),
           size = xs_size(string), capacity = xs_capacity(string);

    xs_cow_lazy_copy(string);
    char *pre = xs_data(prefix), *suf = xs_data(suffix),
         *data = xs_data(string);

    if (size + pres + sufs <= capacity) {
        memmove(data + pres, data, size);
        memcpy(data, pre, pres);
        memcpy(data + pres + size, suf, sufs + 1);

        if (xs_is_ptr(string))
            string->size = size + pres + sufs;
        else
            string->space_left = 15 - (size + pres + sufs);
    } else {
        xs tmps = xs_literal_empty();
        xs_grow(&tmps, size + pres + sufs);
        char *tmpdata = xs_data(&tmps);
        memcpy(tmpdata + pres, data, size);
        memcpy(tmpdata, pre, pres);
        memcpy(tmpdata + pres + size, suf, sufs + 1);
        xs_free(string);
        *string = tmps;
        string->size = size + pres + sufs;
    }
    return string;
}

xs *xs_trim(xs *x, const char *trimset)
{
    if (!trimset[0])
        return x;

    xs_cow_lazy_copy(x);
    char *dataptr = xs_data(x), *orig = dataptr;

    /* similar to strspn/strpbrk but it operates on binary data */
    uint8_t mask[32] = {0};

#define check_bit(i) (mask[(uint8_t) i >> 3] & 1 << ((uint8_t) i & 7))
#define set_bit(i) (mask[(uint8_t) i >> 3] |= 1 << ((uint8_t) i & 7))
    size_t i, slen = xs_size(x), trimlen = strlen(trimset);

    for (i = 0; i < trimlen; i++)
        set_bit(trimset[i]);
    for (i = 0; i < slen; i++)
        if (!check_bit(dataptr[i]))
            break;
    for (; slen > 0; slen--)
        if (!check_bit(dataptr[slen - 1]))
            break;
    dataptr += i;
    slen -= i;

    /* reserved space as a buffer on the heap.
     * Do not reallocate immediately. Instead, reuse it as possible.
     * Do not shrink to in place if < 16 bytes.
     */
    memmove(orig, dataptr, slen);
    /* do not dirty memory unless it is needed */
    if (orig[slen])
        orig[slen] = 0;

    xs_set_size(x, slen);
    return x;
#undef check_bit
#undef set_bit
}

int main(int argc, char *argv[])
{
    xs string = *xs_tmp("\n foobarbar \n\n\n");
    xs_trim(&string, "\n ");
    printf("[%s] : %2zu\n", xs_data(&string), xs_size(&string));

    xs prefix = *xs_tmp("((("), suffix = *xs_tmp(")))");
    xs_concat(&string, &prefix, &suffix);
    printf("[%s] : %2zu\n", xs_data(&string), xs_size(&string));
    return 0;
}
