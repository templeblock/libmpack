#include <assert.h>

#include "mpack.h"

#define UNUSED(p) (void)p;
#define ADVANCE(buf, buflen) ((*buflen)--, (unsigned char)*((*buf)++))
#define TLEN(val, range_start) ((size_t)(1 << (val - range_start)))
#ifndef MIN
# define MIN(X, Y) ((X) < (Y) ? (X) : (Y))
#endif
#define MPACK_BIG_ENDIAN (*(mpack_uint32_t *)"\xff\0\0\0" == 0xff000000)
#define MPACK_SWAP_VALUE(val)                                  \
  do {                                                         \
    mpack_uint32_t lo = val.components.lo;                     \
    val.components.lo = val.components.hi;                     \
    val.components.hi = lo;                                    \
  } while (0)

enum {
  UNPACK_TYPE,
  UNPACK_VALUE,
  UNPACK_BYTE_ARRAY,
  UNPACK_EXT_TYPE,
  UNPACK_COLLECTION
};

/* state handlers */
static void unpack_type(mpack_unpacker_t *u, const char **b, size_t *bl);
static void unpack_value(mpack_unpacker_t *u, const char **b, size_t *bl);
static void unpack_ext_type(mpack_unpacker_t *u, const char **b, size_t *bl);
static void unpack_byte_array(mpack_unpacker_t *u, const char **b, size_t *bl);
static void unpack_collection(mpack_unpacker_t *u, const char **b, size_t *bl);
/* shift helpers */
static void shift_value(mpack_unpacker_t *u, mpack_token_type_t t,
    mpack_value_t w, size_t l);
static void shift_chunk(mpack_unpacker_t *u, const char *p, size_t l);
static void shift_byte_array(mpack_unpacker_t *u, mpack_token_type_t t,
    size_t l, int ext_type);
static void shift_collection(mpack_unpacker_t *u, mpack_token_type_t t,
    size_t l);
/* stack helpers */
static void push_state(mpack_unpacker_t *u, mpack_token_type_t t,
    int c, size_t r);
static void pop_state(mpack_unpacker_t *u);
static mpack_unpack_state_t *top(mpack_unpacker_t *unpacker);
/* misc */
static mpack_value_t byte(mpack_unpacker_t *u, unsigned char b);
static void process_token(mpack_unpacker_t *unpacker, mpack_token_t *t);
static void process_float_token(mpack_token_t *t);
static void process_integer_token(mpack_token_t *t);
/* pack helpers */
static void pack1(char **b, size_t *bl, mpack_uint32_t v);
static void pack2(char **b, size_t *bl, mpack_uint32_t v);
static void pack4(char **b, size_t *bl, mpack_uint32_t v);
static mpack_value_t pack_double(double v);
static mpack_uint32_t pack_float(float v);

void mpack_unpacker_init(mpack_unpacker_t *unpacker)
{
  assert(unpacker);
  /* stack/error initialization */
  unpacker->stackpos = 0;
  unpacker->error_code = 0;
  push_state(unpacker, 0, UNPACK_TYPE, 0);
}

mpack_token_t *mpack_unpack(mpack_unpacker_t *unpacker, const char **buf,
    size_t *buflen)
{
  assert(buflen && *buflen);
  assert(buf && *buf);

  if (unpacker->error_code) {
    return NULL;
  }

  unpacker->result = NULL;

  do {
    switch (top(unpacker)->code) {
      case UNPACK_TYPE: unpack_type(unpacker, buf, buflen); break;
      case UNPACK_VALUE: unpack_value(unpacker, buf, buflen); break;
      case UNPACK_BYTE_ARRAY: unpack_byte_array(unpacker, buf, buflen); break;
      case UNPACK_EXT_TYPE: unpack_ext_type(unpacker, buf, buflen); break;
      case UNPACK_COLLECTION: unpack_collection(unpacker, buf, buflen); break;
      default: unpacker->error_code = INT_MAX; return NULL;
    }
  } while (!unpacker->result && !unpacker->error_code && *buflen); 

  return unpacker->result;
}

void mpack_pack_nil(char **buf, size_t *buflen)
{
  pack1(buf, buflen, 0xc0);
}

void mpack_pack_boolean(char **buf, size_t *buflen, mpack_uint32_t val)
{
  pack1(buf, buflen, val ? 0xc3 : 0xc2);
}

void mpack_pack_uint32(char **buf, size_t *buflen, mpack_uint32_t val)
{
  if (val < 0x80) {
    pack1(buf, buflen, val);
  } else if (val < 0x100) {
    pack1(buf, buflen, 0xcc);
    pack1(buf, buflen, val);
  } else if (val < 0x10000) {
    pack1(buf, buflen, 0xcd);
    pack2(buf, buflen, val);
  } else {
    pack1(buf, buflen, 0xce);
    pack4(buf, buflen, val);
  }
}

void mpack_pack_int32(char **buf, size_t *buflen, mpack_int32_t v)
{
  mpack_uint32_t val = (mpack_uint32_t)v;
  if (v > -1) {
    mpack_pack_uint32(buf, buflen, val);
  } else if (v > -0x21) {
    pack1(buf, buflen, (mpack_uint32_t)(0x100 + v));
  } else if (v > -0x81) {
    pack1(buf, buflen, 0xd0);
    pack1(buf, buflen, val);
  } else if (v > -0x8001) {
    pack1(buf, buflen, 0xd1);
    pack2(buf, buflen, val);
  } else {
    pack1(buf, buflen, 0xd2);
    pack4(buf, buflen, val);
  }
}

void mpack_pack_uint64(char **buf, size_t *buflen, mpack_value_t val)
{
  if (MPACK_BIG_ENDIAN) {
    /* swap for checking hi/lo */
    MPACK_SWAP_VALUE(val);
  }

  if (!val.components.hi) {
    /* fits into smaller uint */
    mpack_pack_uint32(buf, buflen, val.components.lo);
  } else {
    pack1(buf, buflen, 0xcf);
    pack4(buf, buflen, val.components.hi);
    pack4(buf, buflen, val.components.lo);
  }
}

void mpack_pack_int64(char **buf, size_t *buflen, mpack_value_t val)
{
  if (MPACK_BIG_ENDIAN) {
    /* swap for checking hi/lo */
    MPACK_SWAP_VALUE(val);
  }

  if (val.components.hi > 0x7fffffff) {
    /* negative */
    if (val.components.lo > 0x7fffffff) {
      /* fits into smaller int */
      mpack_pack_int32(buf, buflen, (mpack_int32_t)val.components.lo);
    } else {
      pack1(buf, buflen, 0xd3);
      pack4(buf, buflen, val.components.hi);
      pack4(buf, buflen, val.components.lo);
    }
  } else {
    if (MPACK_BIG_ENDIAN) {
      /* mpack_pack_uint64 already swaps, so we need to swap back */
      MPACK_SWAP_VALUE(val);
    }
    mpack_pack_uint64(buf, buflen, val);
  }
}

void mpack_pack_float(char **buf, size_t *buflen, double val)
{
  if (((double)(float)val) == (double)val) {
    pack1(buf, buflen, 0xca);
    pack4(buf, buflen, pack_float((float)val));
  } else {
    mpack_value_t v = pack_double(val);
    pack1(buf, buflen, 0xcb);
    pack4(buf, buflen, v.components.hi);
    pack4(buf, buflen, v.components.lo);
  }
}

void mpack_pack_str(char **buf, size_t *buflen, mpack_uint32_t len)
{
  if (len < 0x20) {
    pack1(buf, buflen, 0xa0 | len);
  } else if (len < 0x100) {
    pack1(buf, buflen, 0xd9);
    pack1(buf, buflen, len);
  } else if (len < 0x10000) {
    pack1(buf, buflen, 0xda);
    pack2(buf, buflen, len);
  } else {
    pack1(buf, buflen, 0xdb);
    pack4(buf, buflen, len);
  }
}

void mpack_pack_bin(char **buf, size_t *buflen, mpack_uint32_t len)
{
  if (len < 0x100) {
    pack1(buf, buflen, 0xc4);
    pack1(buf, buflen, len);
  } else if (len < 0x10000) {
    pack1(buf, buflen, 0xc5);
    pack2(buf, buflen, len);
  } else {
    pack1(buf, buflen, 0xc6);
    pack4(buf, buflen, len);
  }
}

void mpack_pack_ext(char **buf, size_t *buflen, int type, mpack_uint32_t len)
{
  mpack_uint32_t t;
  assert(type >= 0 && type < 0x80);
  t = (mpack_uint32_t)type;
  switch (len) {
    case 1: pack1(buf, buflen, 0xd4); pack1(buf, buflen, t); break;
    case 2: pack1(buf, buflen, 0xd5); pack1(buf, buflen, t); break;
    case 4: pack1(buf, buflen, 0xd6); pack1(buf, buflen, t); break;
    case 8: pack1(buf, buflen, 0xd7); pack1(buf, buflen, t); break;
    case 16: pack1(buf, buflen, 0xd8); pack1(buf, buflen, t); break;
    default:
      if (len < 0x100) {
        pack1(buf, buflen, 0xc7);
        pack1(buf, buflen, len);
        pack1(buf, buflen, t);
      } else if (len < 0x10000) {
        pack1(buf, buflen, 0xc8);
        pack2(buf, buflen, len);
        pack1(buf, buflen, t);
      } else {
        pack1(buf, buflen, 0xc9);
        pack4(buf, buflen, len);
        pack1(buf, buflen, t);
      }
      break;
  }
}

void mpack_pack_array(char **buf, size_t *buflen, mpack_uint32_t len)
{
  if (len < 0x10) {
    pack1(buf, buflen, 0x90 | len);
  } else if (len < 0x10000) {
    pack1(buf, buflen, 0xdc);
    pack2(buf, buflen, len);
  } else {
    pack1(buf, buflen, 0xdd);
    pack4(buf, buflen, len);
  }
}

void mpack_pack_map(char **buf, size_t *buflen, mpack_uint32_t len)
{
  if (len < 0x10) {
    pack1(buf, buflen, 0x80 | len);
  } else if (len < 0x10000) {
    pack1(buf, buflen, 0xde);
    pack2(buf, buflen, len);
  } else {
    pack1(buf, buflen, 0xdf);
    pack4(buf, buflen, len);
  }
}

static void unpack_type(mpack_unpacker_t *unpacker, const char **buf,
    size_t *buflen)
{
  unsigned char t = ADVANCE(buf, buflen);
  if (unpacker->stackpos > 1) {
    /* pop after unpacking type for a container element so the parent
     * state will be called in the next iteration. */
    pop_state(unpacker);
  }
  
  if (t < 0x80) {
    /* positive fixint */
    shift_value(unpacker, MPACK_TOKEN_UINT, byte(unpacker, t), 1);
  } else if (t < 0x90) {
    /* fixmap */
    shift_collection(unpacker, MPACK_TOKEN_MAP, (t & 0xf) * (size_t)2);
  } else if (t < 0xa0) {
    /* fixarray */
    shift_collection(unpacker, MPACK_TOKEN_ARRAY, t & 0xf);
  } else if (t < 0xc0) {
    /* fixstr */
    shift_byte_array(unpacker, MPACK_TOKEN_STR, t & 0x1f, 0);
  } else if (t < 0xe0) {
    switch (t) {
      case 0xc0:  /* nil */
        shift_value(unpacker, MPACK_TOKEN_NIL, byte(unpacker, 0), 1);
        break;
      case 0xc2:  /* false */
        shift_value(unpacker, MPACK_TOKEN_BOOLEAN, byte(unpacker, 0), 1);
        break;
      case 0xc3:  /* true */
        shift_value(unpacker, MPACK_TOKEN_BOOLEAN, byte(unpacker, 1), 1);
        break;
      case 0xc4:  /* bin 8 */
      case 0xc5:  /* bin 16 */
      case 0xc6:  /* bin 32 */
        push_state(unpacker, MPACK_TOKEN_BIN, UNPACK_VALUE, TLEN(t, 0xc4));
        break;
      case 0xc7:  /* ext 8 */
      case 0xc8:  /* ext 16 */
      case 0xc9:  /* ext 32 */
        push_state(unpacker, MPACK_TOKEN_EXT, UNPACK_VALUE, TLEN(t, 0xc7));
        break;
      case 0xca:  /* float 32 */
      case 0xcb:  /* float 64 */
        push_state(unpacker, MPACK_TOKEN_FLOAT, UNPACK_VALUE, TLEN(t, 0xc8));
        break;
      case 0xcc:  /* uint 8 */
      case 0xcd:  /* uint 16 */
      case 0xce:  /* uint 32 */
      case 0xcf:  /* uint 64 */
        push_state(unpacker, MPACK_TOKEN_UINT, UNPACK_VALUE, TLEN(t, 0xcc));
        break;
      case 0xd0:  /* int 8 */
      case 0xd1:  /* int 16 */
      case 0xd2:  /* int 32 */
      case 0xd3:  /* int 64 */
        push_state(unpacker, MPACK_TOKEN_SINT, UNPACK_VALUE, TLEN(t, 0xd0));
        break;
      case 0xd4:  /* fixext 1 */
      case 0xd5:  /* fixext 2 */
      case 0xd6:  /* fixext 4 */
      case 0xd7:  /* fixext 8 */
      case 0xd8:  /* fixext 16 */
        push_state(unpacker, MPACK_TOKEN_EXT, UNPACK_EXT_TYPE, TLEN(t, 0xd4));
        break;
      case 0xd9:  /* str 8 */
      case 0xda:  /* str 16 */
      case 0xdb:  /* str 32 */
        push_state(unpacker, MPACK_TOKEN_STR, UNPACK_VALUE, TLEN(t, 0xd9));
        break;
      case 0xdc:  /* array 16 */
      case 0xdd:  /* array 32 */
        push_state(unpacker, MPACK_TOKEN_ARRAY, UNPACK_VALUE, TLEN(t, 0xdb));
        break;
      case 0xde:  /* map 16 */
      case 0xdf:  /* map 32 */
        push_state(unpacker, MPACK_TOKEN_MAP, UNPACK_VALUE, TLEN(t, 0xdd));
        break;
      default:
        unpacker->error_code = 1;
        break;
    }
  } else {
    /* negative fixint */
    shift_value(unpacker, MPACK_TOKEN_SINT, byte(unpacker, t), 1);
  }
}

static void unpack_value(mpack_unpacker_t *unpacker, const char **buf,
    size_t *buflen)
{
  mpack_unpack_state_t *t = top(unpacker);
  do {
    mpack_uint32_t byte = ADVANCE(buf, buflen), byte_idx, byte_shift;
    byte_idx = (mpack_uint32_t)--t->remaining;
    byte_shift = (byte_idx % 4) * 8;
    t->token.data.value.components.lo |= byte << byte_shift;
    if (t->remaining == 4) {
      /* unpacked the first half of a 8-byte value, shift what was parsed to the
       * "hi" field and reset "lo" for the trailing 4 bytes. */
      t->token.data.value.components.hi = t->token.data.value.components.lo;
      t->token.data.value.components.lo = 0;
    }
  } while (*buflen && t->remaining);

  if (t->remaining) {
    /* not finished yet. */
    return;
  }

  pop_state(unpacker);
  if (t->token.type > MPACK_TOKEN_CHUNK) {
    /* internal value unpacking to get the length of a container or byte array.
     * note that msgpack only allows 32-bit sizes for arrays/maps/strings, so
     * the entire value will be contained in the "lo" field. */
    t->remaining = t->token.data.value.components.lo;
    assert(!t->token.data.value.components.hi);
    if (t->token.type > MPACK_TOKEN_EXT) {
      if (t->token.type == MPACK_TOKEN_MAP) {
        t->remaining *= 2;
      }
      shift_collection(unpacker, t->token.type, t->remaining);
    } else if (t->token.type == MPACK_TOKEN_EXT) {
      push_state(unpacker, MPACK_TOKEN_EXT, UNPACK_EXT_TYPE, t->remaining);
    } else {
      shift_byte_array(unpacker, t->token.type, t->remaining, 0);
    }
  } else {
    /* shift value to the user. */
    shift_value(unpacker, t->token.type, t->token.data.value, t->token.length);
  }
}

static void unpack_ext_type(mpack_unpacker_t *unpacker, const char **buf,
    size_t *buflen)
{
  int ext_type = ADVANCE(buf, buflen);
  mpack_unpack_state_t *t = top(unpacker);
  t->token.data.ext_type = ext_type;
  pop_state(unpacker);
  shift_byte_array(unpacker, MPACK_TOKEN_EXT, t->remaining, ext_type);
}

static void unpack_byte_array(mpack_unpacker_t *unpacker, const char **buf,
    size_t *buflen)
{
  mpack_unpack_state_t *t = top(unpacker);
  size_t len = MIN(t->remaining, *buflen);
  shift_chunk(unpacker, *buf, len);
  *buf += len;
  *buflen -= len;
  t->remaining -= len;

  if (!t->remaining) {
    pop_state(unpacker);
  }
}

static void unpack_collection(mpack_unpacker_t *unpacker, const char **buf,
    size_t *buflen)
{
  mpack_unpack_state_t *t = top(unpacker);
  UNUSED(buf);
  UNUSED(buflen);

  if (t->remaining) {
    t->remaining--;
    push_state(unpacker, 0, UNPACK_TYPE, 0);
  } else {
    pop_state(unpacker);
  }
}

static void shift_value(mpack_unpacker_t *unpacker, mpack_token_type_t type,
    mpack_value_t value, size_t length)
{
  mpack_unpack_state_t *t = top(unpacker);
  t->token.type = type;
  t->token.data.value = value;
  t->token.length = length;
  unpacker->result = &t->token;
  process_token(unpacker, unpacker->result);
}

static void shift_chunk(mpack_unpacker_t *unpacker, const char *ptr,
    size_t length)
{
  mpack_unpack_state_t *t = top(unpacker);
  t->token.type = MPACK_TOKEN_CHUNK;
  t->token.data.chunk_ptr = ptr;
  t->token.length = length;
  unpacker->result = &t->token;
}

static void shift_byte_array(mpack_unpacker_t *unpacker,
    mpack_token_type_t type, size_t length, int ext_type)
{
  mpack_unpack_state_t *t = top(unpacker);
  t->token.type = type;
  t->token.length = length;
  t->token.data.ext_type = ext_type;
  unpacker->result = &t->token;
  push_state(unpacker, type, UNPACK_BYTE_ARRAY, length);
}

static void shift_collection(mpack_unpacker_t *unpacker, mpack_token_type_t type,
    size_t length)
{
  mpack_unpack_state_t *t = top(unpacker);
  t->token.type = type;
  t->token.length = length;
  unpacker->result = &t->token;
  push_state(unpacker, type, UNPACK_COLLECTION, length);
}

static void push_state(mpack_unpacker_t *unpacker,
    mpack_token_type_t type, int code, size_t length)
{
  mpack_unpack_state_t *state = unpacker->stack + unpacker->stackpos;
  state->code = code;
  state->token.type = type;
  state->token.data.value = byte(unpacker, 0);
  state->remaining = state->token.length = length;
  unpacker->stackpos++;
}

static void pop_state(mpack_unpacker_t *unpacker)
{
  assert(unpacker->stackpos);
  unpacker->stackpos--;
}

static mpack_unpack_state_t *top(mpack_unpacker_t *unpacker)
{
  assert(unpacker->stackpos);
  return unpacker->stack + unpacker->stackpos - 1;
}

static mpack_value_t byte(mpack_unpacker_t *unpacker, unsigned char byte)
{
  mpack_value_t rv;
  UNUSED(unpacker);
  rv.components.lo = byte;
  rv.components.hi = 0;
  return rv;
}

static void pack1(char **b, size_t *bl, mpack_uint32_t v)
{
  assert(*bl); (*bl)--;
  *(*b)++ = (char)(v & 0xff);
}

static void pack2(char **b, size_t *bl, mpack_uint32_t v)
{
  assert(*bl > 1); *bl -= 2;
  *(*b)++ = (char)((v >> 8) & 0xff);
  *(*b)++ = (char)(v & 0xff);
}

static void pack4(char **b, size_t *bl, mpack_uint32_t v)
{
  assert(*bl > 3); *bl -= 4;
  *(*b)++ = (char)((v >> 24) & 0xff);
  *(*b)++ = (char)((v >> 16) & 0xff);
  *(*b)++ = (char)((v >> 8) & 0xff);
  *(*b)++ = (char)(v & 0xff);
}

static void process_token(mpack_unpacker_t *unpacker, mpack_token_t *t)
{
  UNUSED(unpacker);
  if (t->type == MPACK_TOKEN_FLOAT) {
    process_float_token(t);
  } else if (t->type == MPACK_TOKEN_SINT || t->type == MPACK_TOKEN_UINT) {
    process_integer_token(t);
  }
}

static void process_integer_token(mpack_token_t *t)
{
  if (t->type == MPACK_TOKEN_SINT && t->length < 8) {
    mpack_uint32_t w = t->data.value.components.lo;
    size_t l = t->length;
    if ((l == 1 && w > 0x7f)
        || (l == 2 && w > 0x7fff)
        || (l == 4 && w > 0x7fffffff)) {
      mpack_uint32_t fill = 0xffffff00 << ((l - 1) * 8);
      t->data.value.components.lo = fill + w;
      t->data.value.components.hi = 0xffffffff;
    }
  }

  if (MPACK_BIG_ENDIAN) {
    MPACK_SWAP_VALUE(t->data.value);
  }
}

#ifndef NO_NATIVE_IEEE754
static void process_float_token(mpack_token_t *t)
{
  if (t->length < 8) {
    union { float f; mpack_uint32_t u; } flt;
    flt.u = t->data.value.components.lo;
    t->data.value.f64 = flt.f;
  } else if (MPACK_BIG_ENDIAN) {
    MPACK_SWAP_VALUE(t->data.value);
  }
}

static mpack_value_t pack_double(double v)
{
  mpack_value_t rv;
  rv.f64 = v;
  if (MPACK_BIG_ENDIAN) {
    MPACK_SWAP_VALUE(rv);
  }
  return rv;
}

static mpack_uint32_t pack_float(float v)
{
  union {
    float f;
    mpack_uint32_t u;
  } flt;
  flt.f = v;
  return flt.u;
}
#else
#include "compat.c"
#endif
