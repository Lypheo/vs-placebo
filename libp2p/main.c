#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include "p2p_api.h"

static void test_api()
{
	int i;

	puts(__FUNCTION__);

	for (i = 0; i < p2p_packing_max; ++i) {
		p2p_pack_func pack_ptr = p2p_select_pack_func(i);
		p2p_unpack_func unpack_ptr = p2p_select_unpack_func(i);

		if (!pack_ptr || !unpack_ptr)
			printf("%d pack: %p unpack: %p", i, (void *)pack_ptr, (void *)unpack_ptr);
	}
}

static void test_rgb24_be()
{
    uint16_t src[3][1] = { { 1000 }, { 2000 }, { 3000 } };
    uint16_t dst[4];
    void *src_p[4] = { &src[0], &src[1], &src[2], NULL };

    puts(__FUNCTION__);

    struct p2p_buffer_param p = {};
    p.width = 1;
    p.height = 1;
    for (int i = 0; i < 4; ++i) {
        p.src[i] = src_p[i];
        p.src_stride[i] = 2;
    }
    p.dst[0] = dst;
    p.dst_stride[0] = 8;
    p.packing = p2p_abgr64_le;
    p2p_pack_frame(&p, P2P_ALPHA_SET_ONE);
    printf("original: %d %d %d\n", src[0][0], src[1][0], src[2][0]);
    printf("packed: %d %d %d %d\n", dst[0], dst[1], dst[2], dst[3]);

    for (int j = 0; j < 4; ++j) {
        p.dst[j] = p.src[j];
        p.dst_stride[j] = p.src_stride[j];
    }
    p.src[0] = dst;
    p.src_stride[0] = 8;
    p2p_unpack_frame(&p, 0);
    printf("planar: %d %d %d\n", src[0][0], src[1][0], src[2][0]);
}

//static void test_rgb24_be()
//{
//	uint8_t src[3][1] = { { 0xA0 }, { 0xB0 }, { 0xC0 } };
//	uint8_t dst[3];
//	void *src_p[4] = { &src[0], &src[1], &src[2], NULL };
//
//	puts(__FUNCTION__);
//
//	p2p_select_pack_func(p2p_rgb24_be)((const void * const *)src_p, &dst, 0, 1);
//	printf("packed: %x %x %x\n", dst[0], dst[1], dst[2]);
//
//	p2p_select_unpack_func(p2p_rgb24_be)(&dst, src_p, 0, 1);
//	printf("planar: %x %x %x\n", src[0][0], src[1][0], src[2][0]);
//}

static void test_rgb24_le()
{
	uint8_t src[3][1] = { { 0xA0 }, { 0xB0 }, { 0xC0 } };
	uint8_t dst[3];
	void *src_p[4] = { &src[0], &src[1], &src[2], NULL };

	puts(__FUNCTION__);

	p2p_select_pack_func(p2p_rgb24_le)((const void * const *)src_p, &dst, 0, 1);
	printf("packed: %x %x %x\n", dst[0], dst[1], dst[2]);

	p2p_select_unpack_func(p2p_rgb24_le)(&dst, src_p, 0, 1);
	printf("planar: %x %x %x\n", src[0][0], src[1][0], src[2][0]);
}

static void test_rgbx_be()
{
	uint8_t src[3][2] = { { 0xA0, 0xA1 }, { 0xB0, 0xB1 }, { 0xC0, 0xC1 } };
	union { uint8_t b[8]; uint32_t dw[2]; } dst;
	void *src_p[4] = { &src[0], &src[1], &src[2], NULL };

	puts(__FUNCTION__);

	p2p_select_pack_func_ex(p2p_argb32_be, 1)((const void * const *)src_p, &dst.dw, 0, 2);
	printf("packed: [%x] %x %x %x | [%x] %x %x %x\n", dst.b[0], dst.b[1], dst.b[2], dst.b[3], dst.b[4], dst.b[5], dst.b[6], dst.b[7]);

	p2p_select_unpack_func(p2p_argb32_be)(&dst.dw, src_p, 0, 2);
	printf("planar: %x %x %x | %x %x %x\n", src[0][0], src[1][0], src[2][0], src[0][1], src[1][1], src[2][1]);
}

static void test_rgb48_be()
{
	uint16_t src[3][1] = { { 0xA0A1 }, { 0xB0B1 }, { 0xC0C1 } };
	union { uint8_t b[6]; uint16_t w[3]; } dst;
	void *src_p[4] = { &src[0], &src[1], &src[2], NULL };

	puts(__FUNCTION__);

	p2p_select_pack_func(p2p_rgb48_be)((const void * const *)src_p, &dst.b, 0, 1);
	printf("packed: %x%02x %x%02x %x%02x\n", dst.b[0], dst.b[1], dst.b[2], dst.b[3], dst.b[4], dst.b[5]);

	p2p_select_unpack_func(p2p_rgb48_be)(&dst.b, src_p, 0, 1);
	printf("planar: %x %x %x\n", src[0][0], src[1][0], src[2][0]);
}

static void test_y410_le()
{
	uint16_t src[4][1] = { { 0x1A0 }, { 0x1B0 }, { 0x1C0 }, { 0x02 } };
	union { uint8_t b[4]; uint32_t dw; } dst;
	void *src_p[4] = { &src[0], &src[1], &src[2], &src[3] };

	puts(__FUNCTION__);

	p2p_select_pack_func(p2p_y410_be)((const void * const *)src_p, &dst.dw, 0, 1);
	printf("packed: %x%02x%02x%02x\n", dst.b[0], dst.b[1], dst.b[2], dst.b[3]);

	p2p_select_unpack_func(p2p_y410_be)(&dst.dw, src_p, 0, 1);
	printf("planar: %x %x %x %x\n", src[0][0], src[1][0], src[2][0], src[3][0]);
}

static void test_y416_le()
{
	uint16_t src[4][1] = { { 0xA0A1 }, { 0xB0B1 }, { 0xC0C1 }, {0xD0D1 } };
	union { uint8_t b[8]; uint16_t w[4]; uint64_t qw; } dst;
	void *src_p[4] = { &src[0], &src[1], &src[2], &src[3] };

	puts(__FUNCTION__);

	p2p_select_pack_func(p2p_y416_le)((const void * const *)src_p, &dst.qw, 0, 1);
	printf("packed: %x%02x %x%02x %x%02x %x%02x\n", dst.b[0], dst.b[1], dst.b[2], dst.b[3], dst.b[4], dst.b[5], dst.b[6], dst.b[7]);

	p2p_select_unpack_func(p2p_y416_le)(&dst.qw, src_p, 0, 1);
	printf("planar: %x %x %x %x\n", src[0][0], src[1][0], src[2][0], src[3][0]);
}

static void test_uyvy()
{
	uint8_t src[3][2] = {
		{ 0xA0, 0xB0 },
		{ 0x40 },
		{ 0x50 },
	};
	union { uint8_t b[4]; uint16_t w[2]; uint32_t dw; } dst;
	void *src_p[4] = { &src[0], &src[1], &src[2], NULL };

	puts(__FUNCTION__);

	p2p_select_pack_func(p2p_uyvy)((const void * const *)src_p, &dst.dw, 0, 2);
	printf("packed: %x %x %x %x\n", dst.b[0], dst.b[1], dst.b[2], dst.b[3]);

	p2p_select_unpack_func(p2p_uyvy)(&dst.dw, src_p, 0, 2);
	printf("planar: %x %x %x %x\n", src[0][0], src[0][1], src[1][0], src[2][0]);
}

static void test_v210_le()
{
	uint16_t src[3][6] = {
		{ 0x01A0, 0x01B0, 0x01C0, 0x01D0, 0x01E0, 0x01F0 },
		{ 0x0140, 0x0150, 0x0160 },
		{ 0x0210, 0x0220, 0x0230 },
	};
	union { uint8_t b[16]; uint32_t dw[4]; } dst;
	void *src_p[4] = { &src[0], &src[1], &src[2], NULL };

	puts(__FUNCTION__);

	p2p_select_pack_func(p2p_v210_le)((const void * const *)src_p, &dst.dw, 0, 6);
	printf("packed: %x%02x%02x%02x %x%02x%02x%02x %x%02x%02x%02x %x%02x%02x%02x\n",
	       dst.b[0], dst.b[1], dst.b[2], dst.b[3], dst.b[4], dst.b[5], dst.b[6], dst.b[7],
	       dst.b[8], dst.b[9], dst.b[10], dst.b[11], dst.b[12], dst.b[13], dst.b[14], dst.b[15]);

	p2p_select_unpack_func(p2p_v210_le)(&dst.dw, src_p, 0, 6);
	printf("planar: %x %x %x %x %x %x | %x %x %x | %x %x %x\n",
	       src[0][0], src[0][1], src[0][2], src[0][3], src[0][4], src[0][5],
	       src[1][0], src[1][1], src[1][2], src[2][0], src[2][1], src[2][2]);
}

static void test_v216_be()
{
	uint16_t src[3][2] = {
		{ 0xA0A1, 0xB0B1 },
		{ 0x4041 },
		{ 0x5051 },
	};
	union { uint8_t b[8]; uint16_t w[4]; uint32_t dw[2]; uint64_t qw; } dst;
	void *src_p[4] = { &src[0], &src[1], &src[2], NULL };

	puts(__FUNCTION__);

	p2p_select_pack_func(p2p_v216_be)((const void * const *)src_p, &dst.qw, 0, 2);
	printf("packed: %x%02x %x%02x %x%02x %x%02x\n", dst.b[0], dst.b[1], dst.b[2], dst.b[3], dst.b[4], dst.b[5], dst.b[6], dst.b[7]);

	p2p_select_unpack_func(p2p_v216_be)(&dst.qw, src_p, 0, 2);
	printf("planar: %x %x %x %x\n", src[0][0], src[0][1], src[1][0], src[2][0]);
}

static void test_nv12_be()
{
	uint8_t src[3][1] = { { 0 }, { 0x40 }, { 0x50 } };
	uint8_t dst[2];
	void *src_p[4] = { NULL, &src[1], &src[2], NULL };

	puts(__FUNCTION__);

	p2p_select_pack_func(p2p_nv12_be)((const void * const *)src_p, &dst, 0, 2);
	printf("packed: %x %x\n", dst[0], dst[1]);

	p2p_select_unpack_func(p2p_nv12_be)(&dst, src_p, 0, 2);
	printf("planar: %x %x\n", src[1][0], src[2][0]);
}

static void test_nv12_le()
{
	uint8_t src[3][1] = { { 0 }, { 0x40 }, { 0x50 } };
	uint8_t dst[2];
	void *src_p[4] = { NULL, &src[1], &src[2], NULL };

	puts(__FUNCTION__);

	p2p_select_pack_func(p2p_nv12_le)((const void * const *)src_p, &dst, 0, 2);
	printf("packed: %x %x\n", dst[0], dst[1]);

	p2p_select_unpack_func(p2p_nv12_le)(&dst, src_p, 0, 2);
	printf("planar: %x %x\n", src[1][0], src[2][0]);
}

static void test_p016_le()
{
	uint16_t src[3][1] = { { 0 }, { 0x0140 }, { 0x0250 } };
	union { uint8_t b[4]; uint16_t w[2]; uint32_t dw; } dst;
	void *src_p[4] = { NULL, &src[1], &src[2], NULL };

	puts(__FUNCTION__);

	p2p_select_pack_func(p2p_p016_le)((const void * const *)src_p, &dst, 0, 2);
	printf("packed: %x%02x %x%02x\n", dst.b[0], dst.b[1], dst.b[2], dst.b[3]);

	p2p_select_unpack_func(p2p_p016_le)(&dst, src_p, 0, 2);
	printf("planar: %x %x\n", src[1][0], src[2][0]);
}

int main(int argc, char **argv)
{
	(void)argc;
	(void)argv;

	test_api();

	test_rgb24_be();
//	test_rgb24_le();
//	test_rgbx_be();
//	test_rgb48_be();
//	test_y410_le();
//	test_y416_le();
//	test_uyvy();
//	test_v210_le();
//	test_v216_be();
//	test_nv12_be();
//	test_nv12_le();
//	test_p016_le();

//	puts("press any key to continue");
//	getc(stdin);

	return 0;
}
