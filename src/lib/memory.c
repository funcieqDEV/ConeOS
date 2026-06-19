#include <stddef.h>

void *memset(void *dest, int value, size_t count) {
  unsigned char *bytes = dest;

  while (count--)
    *bytes++ = (unsigned char)value;

  return dest;
}

void *memcpy(void *dest, const void *src, size_t count) {
  unsigned char *d = dest;
  const unsigned char *s = src;

  while (count--)
    *d++ = *s++;

  return dest;
}

void *memmove(void *dest, const void *src, size_t count) {
  unsigned char *d = dest;
  const unsigned char *s = src;

  if (d == s)
    return dest;

  if (d < s) {
    while (count--)
      *d++ = *s++;
  } else {
    d += count;
    s += count;
    while (count--)
      *--d = *--s;
  }

  return dest;
}

int memcmp(const void *lhs, const void *rhs, size_t count) {
  const unsigned char *l = lhs;
  const unsigned char *r = rhs;

  while (count--) {
    if (*l != *r)
      return *l - *r;
    l++;
    r++;
  }

  return 0;
}
