/* Test-only forced busy result for the pinned-page rollback contract. */
#include <linux/mm.h>
#include <linux/memcomp.h>
#include <linux/rmap.h>
#include <linux/sched.h>

static inline int test_share_anon_rmap(struct page *page)
{
	if (!strcmp(current->comm, "v9-pin-check") &&
	    folio_memcomp_reserved(page_folio(page))) {
		pr_info_once("memcomp-test: simulated busy pin at rmap share\n");
		return -EBUSY;
	}
	return page_try_share_anon_rmap(page);
}
#define page_try_share_anon_rmap(page) test_share_anon_rmap(page)

// The production configuration is ARM64 with 4 KiB pages and THP.
#include "mm/memcomp/pgtable.h"
#include <linux/init.h>
static int __init test_memcomp_pmd_types(void)
{
	pmd_t pmds[] = { __pmd(0), pmd_mkhuge(mk_pmd(ZERO_PAGE(0), PAGE_KERNEL)) };
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(pmds); i++) {
		spinlock_t *ptl = NULL;
		pte_t *pte = memcomp_pte_offset_map_lock(&init_mm, &pmds[i], 0, &ptl);

		if (pte) {
			pte_unmap_unlock(pte, ptl);
			return -EINVAL;
		}
		if (ptl)
			return -EINVAL;
	}
	pr_info("memcomp-test: empty and huge PMDs skipped\n");
	return 0;
}
late_initcall(test_memcomp_pmd_types);
