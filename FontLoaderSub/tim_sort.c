#include "tim_sort.h"

typedef struct {
  uint8_t *data;
  uint8_t *temp;
  size_t size;
  Sort_Compare comp;
  void *arg;
} Sort_Ctx;

static void select_sort(size_t low, size_t high, Sort_Ctx *ctx) {
  uint8_t *data = ctx->data;
  const size_t size = ctx->size;
  for (size_t i = low; i != high; i++) {
    size_t m = i;
    for (size_t j = i + 1; j != high; j++) {
      if (ctx->comp(&data[m * size], &data[j * size], ctx->arg) > 0) {
        m = j;
      }
    }
    if (m != i) {
      for (size_t j = 0; j != size; j++) {
        const uint8_t t = data[i * size + j];
        data[i * size + j] = data[m * size + j];
        data[m * size + j] = t;
      }
    }
  }
}

static void plz_rep_mov(uint8_t *dst, const uint8_t *src, size_t size) {
  for (size_t i = 0; i != size; i++) {
    dst[i] = src[i];
  }
}

static void tim_sort_i(size_t low, size_t high, Sort_Ctx *ctx) {
  if (low + 4 > high) {
    select_sort(low, high, ctx);
    return;
  }
  uint8_t *data = ctx->data;
  uint8_t *temp = ctx->temp;
  const size_t size = ctx->size;
  const size_t mid = low + (high - low) / 2;
  tim_sort_i(low, mid, ctx);
  tim_sort_i(mid, high, ctx);

  uint8_t *pa = &temp[low * size];
  uint8_t *pb = &temp[mid * size];
  uint8_t *p_mid = pb;
  uint8_t *p_high = &temp[high * size];
  uint8_t *pt = &data[low * size];
  plz_rep_mov(pa, pt, size * (high - low));

  while (pa != p_mid && pb != p_high) {
    if (ctx->comp(pa, pb, ctx->arg) <= 0) {
      plz_rep_mov(pt, pa, size);
      pa += size;
    } else {
      plz_rep_mov(pt, pb, size);
      pb += size;
    }
    pt += size;
  }
  while (pa != p_mid) {
    plz_rep_mov(pt, pa, size);
    pt += size, pa += size;
  }
  while (pb != p_high) {
    plz_rep_mov(pt, pb, size);
    pt += size, pb += size;
  }
}

void tim_sort(
    void *ptr,
    size_t count,
    size_t size,
    allocator_t *alloc,
    Sort_Compare comp,
    void *arg) {
  if (count < 2) {
    return;
  }
  Sort_Ctx ctx = {.data = ptr,
                  .temp = alloc->alloc(NULL, count * size, alloc->arg),
                  .size = size,
                  .comp = comp,
                  .arg = arg};
  if (ctx.temp == NULL) {
    // fallback
    select_sort(0, count, &ctx);
  } else {
    tim_sort_i(0, count, &ctx);
    alloc->alloc(ctx.temp, 0, alloc->arg);
  }
}
