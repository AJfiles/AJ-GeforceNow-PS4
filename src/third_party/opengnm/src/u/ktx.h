#ifndef _U_KTX_H_
#define _U_KTX_H_

#include <stddef.h>
#include <stdint.h>

typedef enum {
	UKTX_ERR_OK = 0,
	UKTX_ERR_INVAL_ARG,
	UKTX_ERR_INVAL_ARRAYIDX,
	UKTX_ERR_INVAL_ID,
	UKTX_ERR_INVAL_ENDIAN,
	UKTX_ERR_INVAL_FACES,
	UKTX_ERR_INVAL_FMT,
	UKTX_ERR_INVAL_LEVEL,
	UKTX_ERR_INVAL_TYPE,
	UKTX_ERR_INVAL_TYPESZ,
	UKTX_ERR_TOO_MANY_KEYVALS,
	UKTX_ERR_TOO_MANY_LEVELS,
	UKTX_ERR_TOO_MANY_SLICES,
	UKTX_ERR_TOO_SMALL,
	UKTX_ERR_INTERNAL,
	UKTX_ERR_OVERFLOW,
	UKTX_ERR_UNSUPPORTED,
} UKtxError;

static inline const char* struktxerr(UKtxError err) {
	switch (err) {
	case UKTX_ERR_OK:
		return "No error";
	case UKTX_ERR_INVAL_ARG:
		return "An invalid argument was used";
	case UKTX_ERR_INVAL_ARRAYIDX:
		return "An invalid array index was used";
	case UKTX_ERR_INVAL_ID:
		return "An invalid identifier was used";
	case UKTX_ERR_INVAL_ENDIAN:
		return "Texture endianess is invalid";
	case UKTX_ERR_INVAL_FACES:
		return "Texture faces count is invalid";
	case UKTX_ERR_INVAL_FMT:
		return "Texture format is invalid";
	case UKTX_ERR_INVAL_LEVEL:
		return "A level is invalid";
	case UKTX_ERR_INVAL_TYPE:
		return "Texture type is invalid";
	case UKTX_ERR_INVAL_TYPESZ:
		return "Texture type size is invalid";
	case UKTX_ERR_TOO_MANY_KEYVALS:
		return "Too many key values in the texture";
	case UKTX_ERR_TOO_MANY_LEVELS:
		return "Too many mip levels in the texture";
	case UKTX_ERR_TOO_MANY_SLICES:
		return "Too many array slices in the texture";
	case UKTX_ERR_TOO_SMALL:
		return "Buffer is too small";
	case UKTX_ERR_INTERNAL:
		return "An internal error has occured";
	case UKTX_ERR_OVERFLOW:
		return "A buffer overflow has occured";
	case UKTX_ERR_UNSUPPORTED:
		return "An unsupported feature is required";
	default:
		return "Unknown error";
	}
}

// change this constant if you want to load/save textures with more mip levels.
// 16 should be a fine default as it assumes the max dimension is 2^16, or 65536
#define UKTX_MAX_LEVELS 16
#define UKTX_MAX_SLICES 16
#define UKTX_MAX_KEYVALS 16
#define UKTX_MAX_KEY_LEN 64
#define UKTX_MAX_VAL_LEN 64

typedef enum {
	UKTX_TYPE_NONE = 0,
	UKTX_TYPE_BYTE = 0x1400,
	UKTX_TYPE_UNSIGNED_BYTE = 0x1401,
	UKTX_TYPE_SHORT = 0x1402,
	UKTX_TYPE_UNSIGNED_SHORT = 0x1403,
	UKTX_TYPE_INT = 0x1404,
	UKTX_TYPE_UNSIGNED_INT = 0x1405,
	UKTX_TYPE_FLOAT = 0x1406,
	UKTX_TYPE_HALF_FLOAT = 0x140b,
	UKTX_TYPE_UNSIGNED_BYTE_3_3_2 = 0x8032,
	UKTX_TYPE_UNSIGNED_SHORT_4_4_4_4 = 0x8033,
	UKTX_TYPE_UNSIGNED_SHORT_5_5_5_1 = 0x8034,
	UKTX_TYPE_UNSIGNED_INT_8_8_8_8 = 0x8035,
	UKTX_TYPE_UNSIGNED_INT_10_10_10_2 = 0x8036,
	UKTX_TYPE_UNSIGNED_BYTE_2_3_3_REV = 0x8362,
	UKTX_TYPE_UNSIGNED_SHORT_5_6_5 = 0x8363,
	UKTX_TYPE_UNSIGNED_SHORT_5_6_5_REV = 0x8364,
	UKTX_TYPE_UNSIGNED_SHORT_4_4_4_4_REV = 0x8365,
	UKTX_TYPE_UNSIGNED_SHORT_1_5_5_5_REV = 0x8366,
	UKTX_TYPE_UNSIGNED_INT_8_8_8_8_REV = 0x8367,
	UKTX_TYPE_UNSIGNED_INT_2_10_10_10_REV = 0x8368,
	UKTX_TYPE_UNSIGNED_INT_24_8 = 0x84fa,
	UKTX_TYPE_UNSIGNED_INT_10F_11F_11F_REV = 0x8c3b,
	UKTX_TYPE_UNSIGNED_INT_5_9_9_9_REV = 0x8c3e,
} UKtxType;

typedef enum {
	UKTX_DFMT_NONE = 0,
	UKTX_DFMT_STENCIL_INDEX = 0x1901,
	UKTX_DFMT_DEPTH_COMPONENT = 0x1902,
	UKTX_DFMT_RED = 0x1903,
	UKTX_DFMT_GREEN = 0x1904,
	UKTX_DFMT_BLUE = 0x1905,
	UKTX_DFMT_ALPHA = 0x1906,
	UKTX_DFMT_RGB = 0x1907,
	UKTX_DFMT_RGBA = 0x1908,
	UKTX_DFMT_BGR = 0x80e0,
	UKTX_DFMT_BGRA = 0x80e1,
	UKTX_DFMT_RED_INTEGER = 0x8d94,
	UKTX_DFMT_GREEN_INTEGER = 0x8d95,
	UKTX_DFMT_BLUE_INTEGER = 0x8d96,
	UKTX_DFMT_RGB_INTEGER = 0x8d98,
	UKTX_DFMT_RGBA_INTEGER = 0x8d99,
	UKTX_DFMT_BGR_INTEGER = 0x8d9a,
	UKTX_DFMT_BGRA_INTEGER = 0x8d9b,
} UKtxDataFormat;

typedef enum {
	UKTX_IFMT_NONE = 0,
	UKTX_IFMT_R3_G3_B2 = 0x2a10,
	UKTX_IFMT_RGB4 = 0x804f,
	UKTX_IFMT_RGB5 = 0x8050,
	UKTX_IFMT_RGB8 = 0x8051,
	UKTX_IFMT_RGB10 = 0x8052,
	UKTX_IFMT_RGB12 = 0x8053,
	UKTX_IFMT_RGB16 = 0x8054,
	UKTX_IFMT_RGBA2 = 0x8055,
	UKTX_IFMT_RGBA4 = 0x8056,
	UKTX_IFMT_RGB5_A1 = 0x8057,
	UKTX_IFMT_RGBA8 = 0x8058,
	UKTX_IFMT_RGB10_A2 = 0x8059,
	UKTX_IFMT_RGBA12 = 0x805a,
	UKTX_IFMT_RGBA16 = 0x805b,
	UKTX_IFMT_DEPTH_COMPONENT16 = 0x81a5,
	UKTX_IFMT_DEPTH_COMPONENT24 = 0x81a6,
	UKTX_IFMT_DEPTH_COMPONENT32 = 0x81a7,
	UKTX_IFMT_COMPRESSED_RED = 0x8225,
	UKTX_IFMT_COMPRESSED_RG = 0x8226,
	UKTX_IFMT_R8 = 0x8229,
	UKTX_IFMT_R16 = 0x822a,
	UKTX_IFMT_RG8 = 0x822b,
	UKTX_IFMT_RG16 = 0x822c,
	UKTX_IFMT_R16F = 0x822d,
	UKTX_IFMT_R32F = 0x822e,
	UKTX_IFMT_RG16F = 0x822f,
	UKTX_IFMT_RG32F = 0x8230,
	UKTX_IFMT_R8I = 0x8231,
	UKTX_IFMT_R8UI = 0x8232,
	UKTX_IFMT_R16I = 0x8233,
	UKTX_IFMT_R16UI = 0x8234,
	UKTX_IFMT_R32I = 0x8235,
	UKTX_IFMT_R32UI = 0x8236,
	UKTX_IFMT_RG8I = 0x8237,
	UKTX_IFMT_RG8UI = 0x8238,
	UKTX_IFMT_RG16I = 0x8239,
	UKTX_IFMT_RG16UI = 0x823a,
	UKTX_IFMT_RG32I = 0x823b,
	UKTX_IFMT_RG32UI = 0x823c,
	UKTX_IFMT_COMPRESSED_RGB_S3TC_DXT1 = 0x83f0,
	UKTX_IFMT_COMPRESSED_RGBA_S3TC_DXT1 = 0x83f1,
	UKTX_IFMT_COMPRESSED_RGBA_S3TC_DXT3 = 0x83f2,
	UKTX_IFMT_COMPRESSED_RGBA_S3TC_DXT5 = 0x83f3,
	UKTX_IFMT_COMPRESSED_RGB = 0x84ed,
	UKTX_IFMT_COMPRESSED_RGBA = 0x84ee,
	UKTX_IFMT_RGBA32F = 0x8814,
	UKTX_IFMT_RGB32F = 0x8815,
	UKTX_IFMT_RGBA16F = 0x881a,
	UKTX_IFMT_RGB16F = 0x881b,
	UKTX_IFMT_DEPTH24_STENCIL8 = 0x88f0,
	UKTX_IFMT_R11F_G11F_B10F = 0x8c3a,
	UKTX_IFMT_RGB9_E5 = 0x8c3d,
	UKTX_IFMT_SRGB8 = 0x8c41,
	UKTX_IFMT_SRGB8_ALPHA8 = 0x8c43,
	UKTX_IFMT_COMPRESSED_SRGB = 0x8c48,
	UKTX_IFMT_COMPRESSED_SRGB_ALPHA = 0x8c49,
	UKTX_IFMT_COMPRESSED_SRGB_S3TC_DXT1 = 0x8c4c,
	UKTX_IFMT_COMPRESSED_SRGB_ALPHA_S3TC_DXT1 = 0x8c4d,
	UKTX_IFMT_COMPRESSED_SRGB_ALPHA_S3TC_DXT3 = 0x8c4e,
	UKTX_IFMT_COMPRESSED_SRGB_ALPHA_S3TC_DXT5 = 0x8c4f,
	UKTX_IFMT_DEPTH_COMPONENT32F = 0x8cac,
	UKTX_IFMT_DEPTH32F_STENCIL8 = 0x8cad,
	UKTX_IFMT_STENCIL_INDEX1 = 0x8d46,
	UKTX_IFMT_STENCIL_INDEX4 = 0x8d47,
	UKTX_IFMT_STENCIL_INDEX8 = 0x8d48,
	UKTX_IFMT_STENCIL_INDEX16 = 0x8d49,
	UKTX_IFMT_RGBA32UI = 0x8d70,
	UKTX_IFMT_RGB32UI = 0x8d71,
	UKTX_IFMT_RGBA16UI = 0x8d76,
	UKTX_IFMT_RGB16UI = 0x8d77,
	UKTX_IFMT_RGBA8UI = 0x8d7c,
	UKTX_IFMT_RGB8UI = 0x8d7d,
	UKTX_IFMT_RGBA32I = 0x8d82,
	UKTX_IFMT_RGB32I = 0x8d83,
	UKTX_IFMT_RGBA16I = 0x8d88,
	UKTX_IFMT_RGB16I = 0x8d89,
	UKTX_IFMT_RGBA8I = 0x8d8e,
	UKTX_IFMT_RGB8I = 0x8d8f,
	UKTX_IFMT_COMPRESSED_RED_RGTC1 = 0x8dbb,
	UKTX_IFMT_COMPRESSED_SIGNED_RED_RGTC1 = 0x8dbc,
	UKTX_IFMT_COMPRESSED_RG_RGTC2 = 0x8dbd,
	UKTX_IFMT_COMPRESSED_SIGNED_RG_RGTC2 = 0x8dbe,
	UKTX_IFMT_COMPRESSED_RGBA_BPTC_UNORM = 0x8e8c,
	UKTX_IFMT_COMPRESSED_SRGB_ALPHA_BPTC_UNORM = 0x8e8d,
	UKTX_IFMT_COMPRESSED_RGB_BPTC_SIGNED_FLOAT = 0x8e8e,
	UKTX_IFMT_COMPRESSED_RGB_BPTC_UNSIGNED_FLOAT = 0x8e8f,
	UKTX_IFMT_R8_SNORM = 0x8f94,
	UKTX_IFMT_RG8_SNORM = 0x8f95,
	UKTX_IFMT_RGB8_SNORM = 0x8f96,
	UKTX_IFMT_RGBA8_SNORM = 0x8f97,
	UKTX_IFMT_R16_SNORM = 0x8f98,
	UKTX_IFMT_RG16_SNORM = 0x8f99,
	UKTX_IFMT_RGB16_SNORM = 0x8f9a,
	UKTX_IFMT_RGBA16_SNORM = 0x8f9b,
	UKTX_IFMT_COMPRESSED_R11_EAC = 0x9270,
	UKTX_IFMT_COMPRESSED_SIGNED_R11_EAC = 0x9271,
	UKTX_IFMT_COMPRESSED_RG11_EAC = 0x9272,
	UKTX_IFMT_COMPRESSED_SIGNED_RG11_EAC = 0x9273,
	UKTX_IFMT_COMPRESSED_RGB8_ETC2 = 0x9274,
	UKTX_IFMT_COMPRESSED_SRGB8_ETC2 = 0x9275,
	UKTX_IFMT_COMPRESSED_RGB8_PUNCHTHROUGH_ALPHA1_ETC2 = 0x9276,
	UKTX_IFMT_COMPRESSED_SRGB8_PUNCHTHROUGH_ALPHA1_ETC2 = 0x9277,
	UKTX_IFMT_COMPRESSED_RGBA8_ETC2_EAC = 0x9278,
	UKTX_IFMT_COMPRESSED_SRGB8_ALPHA8_ETC2_EAC = 0x9279,
} UKtxInternalFormat;

typedef enum {
	UKTX_BIFMT_NONE = 0,
	UKTX_BIFMT_STENCIL_INDEX = 0x1901,
	UKTX_BIFMT_DEPTH_COMPONENT = 0x1902,
	UKTX_BIFMT_RED = 0x1903,
	UKTX_BIFMT_RGB = 0x1907,
	UKTX_BIFMT_RGBA = 0x1908,
	UKTX_BIFMT_RG = 0x8227,
	UKTX_BIFMT_DEPTH_STENCIL = 0x84f9,
} UKtxBaseInternalFormat;

typedef struct {
	size_t (*read)(void* buf, size_t len, void* userdata);
	int (*seek)(size_t off, void* userdata);
	size_t (*tell)(void* userdata);
} UKtxReader;

typedef void (*UKtxWriter)(const void* buf, size_t len, void* userdata);

typedef struct {
	const uint8_t* data;
	size_t len;
	size_t off;
} UKtxMemBuffer;

typedef struct {
	UKtxReader r;
	void* ud;
} UKtxContext;

typedef struct {
	size_t keylen;
	char key[UKTX_MAX_KEY_LEN];
	size_t val_len;
	char val[UKTX_MAX_VAL_LEN];
} UKtxKeyValue;

typedef struct {
	uint32_t byteoff;
	uint32_t bytelen;
} UKtxFace;

typedef struct {
	UKtxFace faces[6];
} UKtxSlice;

typedef struct {
	UKtxSlice slices[UKTX_MAX_SLICES];
} UKtxLevel;

typedef struct {
	UKtxType type;
	uint32_t typesize;
	UKtxDataFormat datafmt;
	UKtxInternalFormat intfmt;
	UKtxBaseInternalFormat baseintfmt;
	uint32_t pixelwidth;
	uint32_t pixeldepth;
	uint32_t pixelheight;
	uint32_t numarrayelems;
	uint32_t numfaces;
	uint32_t numlevels;

	uint32_t numkeyvals;
	UKtxKeyValue keyvals[UKTX_MAX_KEYVALS];
	UKtxLevel levels[UKTX_MAX_LEVELS];
} UKtx;

typedef struct {
	const char* key;
	size_t keylen;
	const char* val;
	size_t val_len;
} UKtxWriteKeyValue;

typedef struct {
	const void* data;
	uint32_t len;
} UKtxWriteFace;

typedef struct {
	UKtxWriteFace faces[6];
} UKtxWriteSlice;

typedef struct {
	UKtxWriteSlice* slices;
} UKtxWriteLevel;

typedef struct {
	UKtxType type;
	uint32_t typesize;
	UKtxDataFormat datafmt;
	UKtxInternalFormat intfmt;
	UKtxBaseInternalFormat baseintfmt;
	uint32_t pixelwidth;
	uint32_t pixeldepth;
	uint32_t pixelheight;
	uint32_t numarrayelems;
	uint32_t numfaces;
	uint32_t numlevels;

	uint32_t numkeyvals;
	const UKtxWriteKeyValue* keyvals;
	const UKtxWriteLevel* levels;
} UKtxWriteInfo;

UKtxContext uktx_open(UKtxReader* reader, void* userdata);
// NOTE: mem must be kept alive along with UKtxContext or else memory errors
// will occur
UKtxContext uktx_openmem(UKtxMemBuffer* mem);

UKtxError uktx_read(UKtxContext* ctx, UKtx* outinfo);
UKtxError uktx_calcimagelen(const UKtx* info, size_t* outlen, uint32_t mipidx);
UKtxError uktx_loadimage(
    UKtxContext* ctx, const UKtx* info, void* outbuf, size_t outbuflen,
    uint32_t mipidx, uint32_t arrayslice, uint32_t faceidx
);

UKtxError uktx_write(
    UKtxWriter writer, void* userdata, const UKtxWriteInfo* info
);

#ifdef UKTX_IMPLEMENTATION

static inline uint32_t swapu32(uint32_t x) {
	return x >> 24 | (x >> 8 & 0xff00) | (x << 8 & 0xff0000) | x << 24;
}

// macros can suck but using these make parsing much easier
#define readu32(reader, ud, val, s)                              \
	if (reader.read(&val, sizeof(val), ud) != sizeof(val)) { \
		return UKTX_ERR_OVERFLOW;                        \
	}                                                        \
	val = s ? swapu32(val) : val;

static const uint8_t KTX_ID[12] = {
    0xab, 0x4b, 0x54, 0x58, 0x20, 0x31, 0x31, 0xbb, 0x0d, 0x0a, 0x1a, 0x0a,
};

static inline uint32_t alignoff32(uint32_t x) {
	return (x + 3) & ~3;
}

static inline void copymem(void* dst, const void* src, size_t len) {
	for (size_t i = 0; i < len; i += 1) {
		((uint8_t*)dst)[i] = ((const uint8_t*)src)[i];
	}
}

static inline uint32_t findsep(const char* str, size_t len) {
	for (size_t i = 0; i < len; i += 1) {
		if (str[i] == 0) {
			return i;
		}
	}
	return 0xffffffff;
}

static inline int istypeval(uint32_t type) {
	return (type >= UKTX_TYPE_BYTE && type <= UKTX_TYPE_FLOAT) ||
	       type >= UKTX_TYPE_HALF_FLOAT ||
	       (type >= UKTX_TYPE_UNSIGNED_BYTE_3_3_2 &&
		type <= UKTX_TYPE_UNSIGNED_INT_10_10_10_2) ||
	       (type >= UKTX_TYPE_UNSIGNED_BYTE_2_3_3_REV &&
		type <= UKTX_TYPE_UNSIGNED_INT_2_10_10_10_REV) ||
	       type == UKTX_TYPE_UNSIGNED_INT_24_8 ||
	       type == UKTX_TYPE_UNSIGNED_INT_10F_11F_11F_REV ||
	       type == UKTX_TYPE_UNSIGNED_INT_5_9_9_9_REV;
}

static inline int isdfmtval(uint32_t x) {
	return (x >= UKTX_DFMT_STENCIL_INDEX && x <= UKTX_DFMT_RGBA) ||
	       (x >= UKTX_DFMT_BGR && x <= UKTX_DFMT_BGRA) ||
	       (x >= UKTX_DFMT_RED_INTEGER && x <= UKTX_DFMT_BGRA_INTEGER);
}

static inline int isifmtval(uint32_t x) {
	return x == UKTX_IFMT_R3_G3_B2 ||
	       (x >= UKTX_IFMT_RGB4 && x <= UKTX_IFMT_RGBA16) ||
	       (x >= UKTX_IFMT_DEPTH_COMPONENT16 &&
		x <= UKTX_IFMT_DEPTH_COMPONENT32) ||
	       (x >= UKTX_IFMT_COMPRESSED_RED && x <= UKTX_IFMT_COMPRESSED_RG
	       ) ||
	       (x >= UKTX_IFMT_R8 && x <= UKTX_IFMT_RG32UI) ||
	       (x >= UKTX_IFMT_COMPRESSED_RGB_S3TC_DXT1 &&
		x <= UKTX_IFMT_COMPRESSED_RGBA_S3TC_DXT5) ||
	       (x >= UKTX_IFMT_COMPRESSED_RGB && x <= UKTX_IFMT_COMPRESSED_RGBA
	       ) ||
	       (x >= UKTX_IFMT_RGBA32F && x <= UKTX_IFMT_RGB32F) ||
	       (x >= UKTX_IFMT_RGBA16F && x <= UKTX_IFMT_RGB16F) ||
	       x == UKTX_IFMT_DEPTH24_STENCIL8 || x == UKTX_IFMT_RGB9_E5 ||
	       x == UKTX_IFMT_SRGB8 || x == UKTX_IFMT_SRGB8_ALPHA8 ||
	       (x >= UKTX_IFMT_COMPRESSED_SRGB &&
		x <= UKTX_IFMT_COMPRESSED_SRGB_ALPHA) ||
	       (x >= UKTX_IFMT_COMPRESSED_SRGB_ALPHA_S3TC_DXT1 &&
		x <= UKTX_IFMT_COMPRESSED_SRGB_ALPHA_S3TC_DXT5) ||
	       (x >= UKTX_IFMT_DEPTH_COMPONENT32F &&
		x <= UKTX_IFMT_DEPTH32F_STENCIL8) ||
	       (x >= UKTX_IFMT_STENCIL_INDEX1 && x <= UKTX_IFMT_STENCIL_INDEX16
	       ) ||
	       (x >= UKTX_IFMT_RGBA32UI && x <= UKTX_IFMT_RGB32UI) ||
	       (x >= UKTX_IFMT_RGBA16UI && x <= UKTX_IFMT_RGB8UI) ||
	       (x >= UKTX_IFMT_RGBA32I && x <= UKTX_IFMT_RGB32I) ||
	       (x >= UKTX_IFMT_RGBA16I && x <= UKTX_IFMT_RGB16I) ||
	       (x >= UKTX_IFMT_RGBA8I && x <= UKTX_IFMT_RGB8I) ||
	       (x >= UKTX_IFMT_COMPRESSED_RED_RGTC1 &&
		x <= UKTX_IFMT_COMPRESSED_SIGNED_RG_RGTC2) ||
	       (x >= UKTX_IFMT_COMPRESSED_RGBA_BPTC_UNORM &&
		x <= UKTX_IFMT_COMPRESSED_RGB_BPTC_UNSIGNED_FLOAT) ||
	       (x >= UKTX_IFMT_R8_SNORM && x <= UKTX_IFMT_RGBA16_SNORM) ||
	       (x >= UKTX_IFMT_COMPRESSED_R11_EAC &&
		x <= UKTX_IFMT_COMPRESSED_SRGB8_ALPHA8_ETC2_EAC);
}

static inline int isbifmtval(uint32_t x) {
	return (x >= UKTX_BIFMT_STENCIL_INDEX && x <= UKTX_BIFMT_RED) ||
	       (x >= UKTX_BIFMT_RGB && x <= UKTX_BIFMT_RGBA) ||
	       x == UKTX_BIFMT_RG || x == UKTX_BIFMT_DEPTH_STENCIL;
}

static size_t uktx_memread(void* buf, size_t len, void* userdata) {
	UKtxMemBuffer* mem = userdata;
	const size_t remainlen = mem->len - mem->off;
	const size_t readlen = len > remainlen ? remainlen : len;
	for (size_t i = 0; i < readlen; i += 1) {
		((uint8_t*)buf)[i] = mem->data[mem->off + i];
	}
	mem->off += readlen;
	return readlen;
}

static int uktx_memseek(size_t off, void* userdata) {
	UKtxMemBuffer* mem = userdata;
	if (off >= mem->len) {
		return 0;
	}
	mem->off = off;
	return 1;
}

static size_t uktx_memtell(void* userdata) {
	UKtxMemBuffer* mem = userdata;
	return mem->off;
}

UKtxContext uktx_open(UKtxReader* reader, void* userdata) {
	return (UKtxContext){
	    .r = *reader,
	    .ud = userdata,
	};
}

UKtxContext uktx_openmem(UKtxMemBuffer* mem) {
	return (UKtxContext){
	    .r =
		{
		    .read = &uktx_memread,
		    .seek = &uktx_memseek,
		    .tell = &uktx_memtell,
		},
	    .ud = mem,
	};
}

UKtxError uktx_read(UKtxContext* ctx, UKtx* outinfo) {
	if (!ctx || !ctx->r.read || !ctx->r.seek || !ctx->r.tell || !outinfo) {
		return UKTX_ERR_INVAL_ARG;
	}

	// read header
	uint8_t id[12] = {0};
	_Static_assert(sizeof(id) == sizeof(KTX_ID), "");
	if (ctx->r.read(&id, sizeof(id), ctx->ud) != sizeof(id)) {
		return UKTX_ERR_OVERFLOW;
	}
	for (size_t i = 0; i < sizeof(id); i += 1) {
		if (id[i] != KTX_ID[i]) {
			return UKTX_ERR_INVAL_ID;
		}
	}

	uint32_t endianess = 0;
	if (ctx->r.read(&endianess, sizeof(endianess), ctx->ud) !=
	    sizeof(endianess)) {
		return UKTX_ERR_OVERFLOW;
	}
	if (endianess != 0x1020304 && endianess != 0x4030201) {
		return UKTX_ERR_INVAL_ENDIAN;
	}

	const int doswap = endianess == 0x1020304;

	uint32_t type = 0;
	readu32(ctx->r, ctx->ud, type, doswap);
	uint32_t typesize = 0;
	readu32(ctx->r, ctx->ud, typesize, doswap);
	uint32_t dfmt = 0;
	readu32(ctx->r, ctx->ud, dfmt, doswap);
	uint32_t ifmt = 0;
	readu32(ctx->r, ctx->ud, ifmt, doswap);
	uint32_t bifmt = 0;
	readu32(ctx->r, ctx->ud, bifmt, doswap);
	uint32_t pixelwidth = 0;
	readu32(ctx->r, ctx->ud, pixelwidth, doswap);
	uint32_t pixelheight = 0;
	readu32(ctx->r, ctx->ud, pixelheight, doswap);
	uint32_t pixeldepth = 0;
	readu32(ctx->r, ctx->ud, pixeldepth, doswap);
	uint32_t numarrayelems = 0;
	readu32(ctx->r, ctx->ud, numarrayelems, doswap);
	uint32_t numfaces = 0;
	readu32(ctx->r, ctx->ud, numfaces, doswap);
	uint32_t numlevels = 0;
	readu32(ctx->r, ctx->ud, numlevels, doswap);
	uint32_t kvbytelen = 0;
	readu32(ctx->r, ctx->ud, kvbytelen, doswap);

	if (type != 0 && !istypeval(type)) {
		return UKTX_ERR_INVAL_TYPE;
	}
	if (dfmt != 0 && !isdfmtval(dfmt)) {
		return UKTX_ERR_INVAL_FMT;
	}
	if (!isifmtval(ifmt)) {
		return UKTX_ERR_INVAL_FMT;
	}
	if (!isbifmtval(bifmt)) {
		return UKTX_ERR_INVAL_FMT;
	}

	// not sure if spec prohibits zero type size
	if (typesize == 0) {
		return UKTX_ERR_INVAL_TYPESZ;
	}

	// numberOfFaces specifies the number of cubemap faces.
	// For cubemaps and cubemap arrays this should be 6.
	// For non cubemaps this should be 1.
	if (numfaces != 6 && numfaces != 1) {
		return UKTX_ERR_INVAL_FACES;
	}

	// Mip map generation not implemented — if numberOfMipmapLevels is 0,
	// it indicates a full mipmap pyramid should be generated from level 0.
	// Currently returns unsupported instead of generating mipmaps.
	if (numlevels == 0) {
		return UKTX_ERR_UNSUPPORTED;
	}

	numlevels = numlevels > 0 ? numlevels : 1;

	// special cases for uktx
	// NOTE: if you need more levels change the constant
	if (numlevels > UKTX_MAX_LEVELS) {
		return UKTX_ERR_TOO_MANY_LEVELS;
	}
	if (numarrayelems > UKTX_MAX_SLICES) {
		return UKTX_ERR_TOO_MANY_SLICES;
	}

	*outinfo = (UKtx){
	    // values omitted here are zero initialized
	    .type = type,
	    .typesize = typesize,
	    .datafmt = dfmt,
	    .intfmt = ifmt,
	    .baseintfmt = bifmt,
	    .pixelwidth = pixelwidth,
	    .pixelheight = pixelheight,
	    .pixeldepth = pixeldepth,
	    .numarrayelems = numarrayelems,
	    .numfaces = numfaces,
	    .numlevels = numlevels,
	};

	// read key values
	char kvbuf[UKTX_MAX_KEY_LEN + 1 + UKTX_MAX_VAL_LEN] = {0};
	uint32_t kvidx = 0;
	size_t kvnextoff = ctx->r.tell(ctx->ud);
	const size_t kvendoff = kvnextoff + kvbytelen;
	while (kvnextoff < kvendoff) {
		if (kvidx >= UKTX_MAX_KEYVALS) {
			return UKTX_ERR_TOO_MANY_KEYVALS;
		}

		// read keyAndValueByteSize
		uint32_t bytelen = 0;
		readu32(ctx->r, ctx->ud, bytelen, doswap);

		// NOTE: this probably means the buffer needs to be expanded
		if (bytelen > sizeof(kvbuf)) {
			return UKTX_ERR_INTERNAL;
		}

		// read keyAndValue
		if (ctx->r.read(&kvbuf, bytelen, ctx->ud) != bytelen) {
			return UKTX_ERR_OVERFLOW;
		}

		const uint32_t keylen = findsep(kvbuf, bytelen);
		const uint32_t val_len = bytelen - keylen - 1;

		UKtxKeyValue* kv = &outinfo->keyvals[kvidx];
		kv->keylen = keylen + 1;
		kv->val_len = val_len;
		// key and val already be zero initialized from before
		copymem(kv->key, kvbuf, keylen);
		copymem(kv->val, kvbuf + keylen + 1, val_len);

		kvidx += 1;
		kvnextoff = alignoff32(kvnextoff + sizeof(uint32_t) + bytelen);

		if (ctx->r.seek(kvnextoff, ctx->ud) == 0) {
			return UKTX_ERR_OVERFLOW;
		}
	}
	outinfo->numkeyvals = kvidx;

	// read mipmap levels info
	size_t curoff = ctx->r.tell(ctx->ud);
	for (uint32_t i = 0; i < numlevels; i += 1) {
		if (ctx->r.seek(curoff, ctx->ud) == 0) {
			return UKTX_ERR_OVERFLOW;
		}

		uint32_t imgsize = 0;
		readu32(ctx->r, ctx->ud, imgsize, doswap);

		curoff += sizeof(imgsize);

		const uint32_t numslices =
		    numarrayelems > 0 ? numarrayelems : 1;
		const uint32_t facelen = (numfaces == 6 && numarrayelems == 0)
					     ? imgsize
					     : imgsize / numslices;

		for (uint32_t s = 0; s < numslices; s += 1) {
			for (uint32_t f = 0; f < numfaces; f += 1) {
				outinfo->levels[i].slices[s].faces[f] =
				    (UKtxFace){
					.byteoff = curoff,
					.bytelen = facelen,
				    };
				curoff += alignoff32(facelen);
			}
		}
	}

	return UKTX_ERR_OK;
}

static inline uint32_t countelemsdfmt(UKtxDataFormat fmt) {
	switch (fmt) {
	case UKTX_DFMT_STENCIL_INDEX:
	case UKTX_DFMT_DEPTH_COMPONENT:
	case UKTX_DFMT_RED:
	case UKTX_DFMT_GREEN:
	case UKTX_DFMT_BLUE:
	case UKTX_DFMT_ALPHA:
	case UKTX_DFMT_RED_INTEGER:
	case UKTX_DFMT_GREEN_INTEGER:
	case UKTX_DFMT_BLUE_INTEGER:
		return 1;
	case UKTX_DFMT_RGB:
	case UKTX_DFMT_BGR:
	case UKTX_DFMT_RGB_INTEGER:
	case UKTX_DFMT_BGR_INTEGER:
		return 3;
	case UKTX_DFMT_RGBA:
	case UKTX_DFMT_BGRA:
	case UKTX_DFMT_RGBA_INTEGER:
	case UKTX_DFMT_BGRA_INTEGER:
		return 4;
	default:
		return 0;
	}
}

UKtxError uktx_calcimagelen(const UKtx* info, size_t* outlen, uint32_t mipidx) {
	if (!info || !outlen) {
		return UKTX_ERR_INVAL_ARG;
	}

	if (mipidx > info->numlevels || mipidx > UKTX_MAX_LEVELS) {
		return UKTX_ERR_INVAL_LEVEL;
	}

	uint32_t baselen = (info->pixelwidth >> mipidx);
	if (info->pixelheight) {
		baselen *= (info->pixelheight >> mipidx);
	}
	if (info->pixeldepth) {
		baselen *= (info->pixeldepth >> mipidx);
	}
	const uint32_t elemlen = countelemsdfmt(info->datafmt);
	if (elemlen) {
		baselen *= info->typesize * countelemsdfmt(info->datafmt);
	}

	*outlen = baselen;
	return UKTX_ERR_OK;
}

UKtxError uktx_loadimage(
    UKtxContext* ctx, const UKtx* info, void* outbuf, size_t outbuflen,
    uint32_t mipidx, uint32_t arrayslice, uint32_t faceidx
) {
	if (!ctx || !ctx->r.read || !ctx->r.seek || !ctx->r.tell || !info ||
	    !outbuf || !outbuflen) {
		return UKTX_ERR_INVAL_ARG;
	}

	if (mipidx > info->numlevels || mipidx > UKTX_MAX_LEVELS) {
		return UKTX_ERR_INVAL_LEVEL;
	}
	if (arrayslice > info->numarrayelems || arrayslice > UKTX_MAX_SLICES) {
		return UKTX_ERR_INVAL_ARRAYIDX;
	}
	if (faceidx > info->numfaces) {
		return UKTX_ERR_INVAL_FACES;
	}

	const UKtxLevel* mip = &info->levels[mipidx];

	uint32_t baselen = (info->pixelwidth >> mipidx);
	if (info->pixelheight) {
		baselen *= (info->pixelheight >> mipidx);
	}
	if (info->pixeldepth) {
		baselen *= (info->pixeldepth >> mipidx);
	}
	const uint32_t elemlen = countelemsdfmt(info->datafmt);
	if (elemlen) {
		baselen *= info->typesize * countelemsdfmt(info->datafmt);
	}

	const uint32_t off = mip->slices[arrayslice].faces[faceidx].byteoff;

	if (baselen > outbuflen) {
		return UKTX_ERR_TOO_SMALL;
	}
	if (ctx->r.seek(off, ctx->ud) == 0) {
		return UKTX_ERR_OVERFLOW;
	}
	if (ctx->r.read(outbuf, baselen, ctx->ud) == 0) {
		return UKTX_ERR_OVERFLOW;
	}

	return UKTX_ERR_OK;
}

UKtxError uktx_write(
    UKtxWriter writer, void* userdata, const UKtxWriteInfo* info
) {
	if (!writer || !info) {
		return UKTX_ERR_INVAL_ARG;
	}

	// count keyvalue data length
	uint32_t kvbytelen = 0;
	for (uint32_t i = 0; i < info->numkeyvals; i += 1) {
		const UKtxWriteKeyValue* kv = &info->keyvals[i];
		const uint32_t len = kv->keylen + kv->val_len;
		const uint32_t alignlen = alignoff32(len);
		const uint32_t padlen = alignlen - len;
		kvbytelen += sizeof(uint32_t) + len + padlen;
	}

	// header
	writer(&KTX_ID, sizeof(KTX_ID), userdata);
	const uint32_t endianess = 0x4030201;
	writer(&endianess, sizeof(endianess), userdata);
	writer(&info->type, sizeof(info->type), userdata);
	writer(&info->typesize, sizeof(info->typesize), userdata);
	writer(&info->datafmt, sizeof(info->datafmt), userdata);
	writer(&info->intfmt, sizeof(info->intfmt), userdata);
	writer(&info->baseintfmt, sizeof(info->baseintfmt), userdata);
	writer(&info->pixelwidth, sizeof(info->pixelwidth), userdata);
	writer(&info->pixelheight, sizeof(info->pixelheight), userdata);
	writer(&info->pixeldepth, sizeof(info->pixeldepth), userdata);
	writer(&info->numarrayelems, sizeof(info->numarrayelems), userdata);
	writer(&info->numfaces, sizeof(info->numfaces), userdata);
	writer(&info->numlevels, sizeof(info->numlevels), userdata);
	writer(&kvbytelen, sizeof(kvbytelen), userdata);

	// write key values
	char pad[4] = {0};
	for (uint32_t i = 0; i < info->numkeyvals; i += 1) {
		const UKtxWriteKeyValue* kv = &info->keyvals[i];
		const uint32_t curlen = kv->keylen + kv->val_len;
		writer(&curlen, sizeof(curlen), userdata);
		writer(kv->key, kv->keylen, userdata);
		writer(kv->val, kv->val_len, userdata);
		const uint32_t alignlen = alignoff32(curlen);
		const uint32_t padlen = alignlen - curlen;
		if (padlen > 0) {
			if (padlen > sizeof(pad)) {
				// should be an impossible error
				return UKTX_ERR_INTERNAL;
			}
			writer(&pad, padlen, userdata);
		}
	}

	const uint32_t numslices =
	    info->numarrayelems > 0 ? info->numarrayelems : 1;

	// write mip levels
	for (uint32_t m = 0; m < info->numlevels; m += 1) {
		uint32_t baselen = (info->pixelwidth >> m);
		if (info->pixelheight) {
			baselen *= (info->pixelheight >> m);
		}
		if (info->pixeldepth) {
			baselen *= (info->pixeldepth >> m);
		}
		const uint32_t elemlen = countelemsdfmt(info->datafmt);
		if (elemlen) {
			baselen *=
			    info->typesize * countelemsdfmt(info->datafmt);
		}

		const uint32_t facelen = baselen * info->numfaces;
		const uint32_t arraylen = facelen * numslices;

		// imageSize
		writer(&arraylen, sizeof(arraylen), userdata);

		const UKtxWriteLevel* mip = &info->levels[m];
		for (uint32_t ae = 0; ae < numslices; ae += 1) {
			const UKtxWriteSlice* sl = &mip->slices[ae];
			for (uint32_t f = 0; f < info->numfaces; f += 1) {
				const UKtxWriteFace* fa = &sl->faces[f];
				writer(fa->data, fa->len, userdata);
			}
		}
	}

	return UKTX_ERR_OK;
}

#endif	// UKTX_IMPLEMENTATION

#endif	// _U_KTX_H_
