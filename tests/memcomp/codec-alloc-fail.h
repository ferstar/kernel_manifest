/* SPDX-License-Identifier: GPL-2.0-only */
/* Force-include only for mm/memcomp.o in the allocation rollback test kernel. */
#ifndef MEMCOMP_TEST_CODEC_ALLOC_FAIL_H
#define MEMCOMP_TEST_CODEC_ALLOC_FAIL_H

#include <linux/crypto.h>
#include <linux/err.h>
#include <linux/init.h>
#include <linux/printk.h>
#include <linux/string.h>

/* compressor_store() and its rollback both hold memcomp_select_lock. */
static bool memcomp_test_fail_zstd_alloc;
static struct crypto_comp *memcomp_test_first_encoder;

static int __init memcomp_test_zstd_setup(char *value)
{
	memcomp_test_fail_zstd_alloc = !strcmp(value, "1");
	return 1;
}
__setup("memcomp_test.fail_zstd_alloc=", memcomp_test_zstd_setup);

static inline struct crypto_comp *
memcomp_test_alloc_comp(const char *name, u32 type, u32 mask, const char *caller)
{
	struct crypto_comp *tfm;

	if (strcmp(caller, "compressor_store") || !memcomp_test_fail_zstd_alloc ||
	    strcmp(name, "zstd"))
		return crypto_alloc_comp(name, type, mask);

	if (memcomp_test_first_encoder) {
		memcomp_test_fail_zstd_alloc = false;
		pr_info("memcomp-test: injecting -ENOMEM at zstd encoder 2\n");
		return ERR_PTR(-ENOMEM);
	}

	tfm = crypto_alloc_comp(name, type, mask);
	if (IS_ERR(tfm))
		memcomp_test_fail_zstd_alloc = false;
	else
		memcomp_test_first_encoder = tfm;
	return tfm;
}

static inline void memcomp_test_free_comp(struct crypto_comp *tfm,
					  const char *caller)
{
	bool watched = !strcmp(caller, "memcomp_discard_codec") &&
		       tfm && tfm == memcomp_test_first_encoder;

	if (watched)
		memcomp_test_first_encoder = NULL;
	crypto_free_comp(tfm);
	if (watched)
		pr_info("memcomp-test: rollback freed zstd encoder 1\n");
}

#define crypto_alloc_comp(name, type, mask) \
	memcomp_test_alloc_comp(name, type, mask, __func__)
#define crypto_free_comp(tfm) memcomp_test_free_comp(tfm, __func__)

/* memcomp.c supplies its own printk prefix after this forced include. */
#undef pr_fmt

#endif
