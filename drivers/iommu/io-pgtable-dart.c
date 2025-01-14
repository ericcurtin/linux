// SPDX-License-Identifier: GPL-2.0-only
/*
 * Apple DART page table allocator.
 *
 * Copyright (C) 2022 The Asahi Linux Contributors
 *
 * Based on io-pgtable-arm.
 *
 * Copyright (C) 2014 ARM Limited
 *
 * Author: Will Deacon <will.deacon@arm.com>
 */

#define pr_fmt(fmt)	"dart io-pgtable: " fmt

#include <linux/atomic.h>
#include <linux/bitfield.h>
#include <linux/bitops.h>
#include <linux/io-pgtable.h>
#include <linux/kernel.h>
#include <linux/sizes.h>
#include <linux/slab.h>
#include <linux/types.h>

#include <asm/barrier.h>

#define DART_MAX_ADDR_BITS		52
#define DART_MAX_LEVELS		3

/* Struct accessors */
#define io_pgtable_to_data(x)						\
	container_of((x), struct dart_io_pgtable, iop)

#define io_pgtable_ops_to_data(x)					\
	io_pgtable_to_data(io_pgtable_ops_to_pgtable(x))

/*
 * Calculate the right shift amount to get to the portion describing level l
 * in a virtual address mapped by the pagetable in d.
 */
#define DART_LVL_SHIFT(l, d)						\
	(((DART_MAX_LEVELS - (l)) * (d)->bits_per_level) +		\
	ilog2(sizeof(dart_iopte)))

#define DART_GRANULE(d)						\
	(sizeof(dart_iopte) << (d)->bits_per_level)
#define DART_PGD_SIZE(d)					\
	(sizeof(dart_iopte) << (d)->pgd_bits)

#define DART_PTES_PER_TABLE(d)					\
	(DART_GRANULE(d) >> ilog2(sizeof(dart_iopte)))

/*
 * Calculate the index at level l used to map virtual address a using the
 * pagetable in d.
 */
#define DART_PGD_IDX(l, d)						\
	((l) == (d)->start_level ? (d)->pgd_bits - (d)->bits_per_level : 0)

#define DART_LVL_IDX(a, l, d)						\
	(((u64)(a) >> DART_LVL_SHIFT(l, d)) &				\
	 ((1 << ((d)->bits_per_level + DART_PGD_IDX(l, d))) - 1))

/* Calculate the block/page mapping size at level l for pagetable in d. */
#define DART_BLOCK_SIZE(l, d)	(1ULL << DART_LVL_SHIFT(l, d))

#define APPLE_DART_PTE_SUBPAGE_START   GENMASK_ULL(63, 52)
#define APPLE_DART_PTE_SUBPAGE_END     GENMASK_ULL(51, 40)

#define APPLE_DART1_PADDR_MASK	GENMASK_ULL(35, 12)
#define APPLE_DART2_PADDR_MASK	GENMASK_ULL(37, 10)
#define APPLE_DART2_PADDR_SHIFT	(4)

/* Apple DART1 protection bits */
#define APPLE_DART1_PTE_PROT_NO_READ	BIT(8)
#define APPLE_DART1_PTE_PROT_NO_WRITE	BIT(7)
#define APPLE_DART1_PTE_PROT_SP_DIS	BIT(1)

/* Apple DART2 protection bits */
#define APPLE_DART2_PTE_PROT_NO_READ	BIT(3)
#define APPLE_DART2_PTE_PROT_NO_WRITE	BIT(2)
#define APPLE_DART2_PTE_PROT_NO_CACHE	BIT(1)

/* marks PTE as valid */
#define APPLE_DART_PTE_VALID		BIT(0)

/* IOPTE accessors */
#define iopte_deref(pte, d) __va(iopte_to_paddr(pte, d))

struct dart_io_pgtable {
	struct io_pgtable	iop;

	int			pgd_bits;
	int			start_level;
	int			bits_per_level;

	void			*pgd;
};

typedef u64 dart_iopte;

static inline bool iopte_leaf(dart_iopte pte, int lvl,
			      enum io_pgtable_fmt fmt)
{
	return (lvl == (DART_MAX_LEVELS - 1)) && (pte & APPLE_DART_PTE_VALID);
}

static dart_iopte paddr_to_iopte(phys_addr_t paddr,
				     struct dart_io_pgtable *data)
{
	dart_iopte pte;

	if (data->iop.fmt == APPLE_DART)
		return paddr & APPLE_DART1_PADDR_MASK;

	/* format is APPLE_DART2 */
	pte = paddr >> APPLE_DART2_PADDR_SHIFT;
	pte &= APPLE_DART2_PADDR_MASK;

	return pte;
}

static phys_addr_t iopte_to_paddr(dart_iopte pte,
				  struct dart_io_pgtable *data)
{
	u64 paddr;

	if (data->iop.fmt == APPLE_DART)
		return pte & APPLE_DART1_PADDR_MASK;

	/* format is APPLE_DART2 */
	paddr = pte & APPLE_DART2_PADDR_MASK;
	paddr <<= APPLE_DART2_PADDR_SHIFT;

	return paddr;
}

static void *__dart_alloc_pages(size_t size, gfp_t gfp,
				    struct io_pgtable_cfg *cfg)
{
	struct device *dev = cfg->iommu_dev;
	int order = get_order(size);
	struct page *p;

	VM_BUG_ON((gfp & __GFP_HIGHMEM));
	p = alloc_pages_node(dev ? dev_to_node(dev) : NUMA_NO_NODE,
			     gfp | __GFP_ZERO, order);
	if (!p)
		return NULL;

	return page_address(p);
}

static void __dart_free_pages(void *pages, size_t size)
{
	free_pages((unsigned long)pages, get_order(size));
}

static void __dart_init_pte(struct dart_io_pgtable *data,
				phys_addr_t paddr, dart_iopte prot,
				int lvl, int num_entries, dart_iopte *ptep)
{
	dart_iopte pte = prot;
	size_t sz = DART_BLOCK_SIZE(lvl, data);
	int i;

	if (lvl == DART_MAX_LEVELS - 1 && data->iop.fmt == APPLE_DART)
		pte |= APPLE_DART1_PTE_PROT_SP_DIS;

	pte |= APPLE_DART_PTE_VALID;

	/* subpage protection: always allow access to the entire page */
	pte |= FIELD_PREP(APPLE_DART_PTE_SUBPAGE_START, 0);
	pte |= FIELD_PREP(APPLE_DART_PTE_SUBPAGE_END, 0xfff);

	for (i = 0; i < num_entries; i++)
		ptep[i] = pte | paddr_to_iopte(paddr + i * sz, data);
}

static int dart_init_pte(struct dart_io_pgtable *data,
			     unsigned long iova, phys_addr_t paddr,
			     dart_iopte prot, int lvl, int num_entries,
			     dart_iopte *ptep)
{
	int i;

	for (i = 0; i < num_entries; i++)
		if (iopte_leaf(ptep[i], lvl, data->iop.fmt)) {
			/* We require an unmap first */
			WARN_ON(iopte_leaf(ptep[i], lvl, data->iop.fmt));
			return -EEXIST;
		}

	__dart_init_pte(data, paddr, prot, lvl, num_entries, ptep);
	return 0;
}

static dart_iopte dart_install_table(dart_iopte *table,
					     dart_iopte *ptep,
					     dart_iopte curr,
					     struct dart_io_pgtable *data)
{
	dart_iopte old, new;

	new = paddr_to_iopte(__pa(table), data) | APPLE_DART_PTE_VALID;

	/*
	 * Ensure the table itself is visible before its PTE can be.
	 * Whilst we could get away with cmpxchg64_release below, this
	 * doesn't have any ordering semantics when !CONFIG_SMP.
	 */
	dma_wmb();

	old = cmpxchg64_relaxed(ptep, curr, new);

	return old;
}

static int __dart_map(struct dart_io_pgtable *data, unsigned long iova,
			  phys_addr_t paddr, size_t size, size_t pgcount,
			  dart_iopte prot, int lvl, dart_iopte *ptep,
			  gfp_t gfp, size_t *mapped)
{
	dart_iopte *cptep, pte;
	size_t block_size = DART_BLOCK_SIZE(lvl, data);
	size_t tblsz = DART_GRANULE(data);
	struct io_pgtable_cfg *cfg = &data->iop.cfg;
	int ret = 0, num_entries, max_entries, map_idx_start;

	/* Find our entry at the current level */
	map_idx_start = DART_LVL_IDX(iova, lvl, data);
	ptep += map_idx_start;

	/* If we can install a leaf entry at this level, then do so */
	if (size == block_size) {
		max_entries = DART_PTES_PER_TABLE(data) - map_idx_start;
		num_entries = min_t(int, pgcount, max_entries);
		ret = dart_init_pte(data, iova, paddr, prot, lvl, num_entries, ptep);
		if (!ret && mapped)
			*mapped += num_entries * size;

		return ret;
	}

	/* We can't allocate tables at the final level */
	if (WARN_ON(lvl >= DART_MAX_LEVELS - 1))
		return -EINVAL;

	/* Grab a pointer to the next level */
	pte = READ_ONCE(*ptep);
	if (!pte) {
		cptep = __dart_alloc_pages(tblsz, gfp, cfg);
		if (!cptep)
			return -ENOMEM;

		pte = dart_install_table(cptep, ptep, 0, data);
		if (pte)
			__dart_free_pages(cptep, tblsz);
	}

	if (pte && !iopte_leaf(pte, lvl, data->iop.fmt)) {
		cptep = iopte_deref(pte, data);
	} else if (pte) {
		/* We require an unmap first */
		WARN_ON(pte);
		return -EEXIST;
	}

	/* Rinse, repeat */
	return __dart_map(data, iova, paddr, size, pgcount, prot, lvl + 1,
			      cptep, gfp, mapped);
}

static dart_iopte dart_prot_to_pte(struct dart_io_pgtable *data,
					   int prot)
{
	dart_iopte pte = 0;

	if (data->iop.fmt == APPLE_DART) {
		if (!(prot & IOMMU_WRITE))
			pte |= APPLE_DART1_PTE_PROT_NO_WRITE;
		if (!(prot & IOMMU_READ))
			pte |= APPLE_DART1_PTE_PROT_NO_READ;
	}
	if (data->iop.fmt == APPLE_DART2) {
		if (!(prot & IOMMU_WRITE))
			pte |= APPLE_DART2_PTE_PROT_NO_WRITE;
		if (!(prot & IOMMU_READ))
			pte |= APPLE_DART2_PTE_PROT_NO_READ;
		if (!(prot & IOMMU_CACHE))
			pte |= APPLE_DART2_PTE_PROT_NO_CACHE;
	}

	return pte;
}

static int dart_map_pages(struct io_pgtable_ops *ops, unsigned long iova,
			      phys_addr_t paddr, size_t pgsize, size_t pgcount,
			      int iommu_prot, gfp_t gfp, size_t *mapped)
{
	struct dart_io_pgtable *data = io_pgtable_ops_to_data(ops);
	struct io_pgtable_cfg *cfg = &data->iop.cfg;
	dart_iopte *ptep = data->pgd;
	int ret, lvl = data->start_level;
	dart_iopte prot;
	long iaext = (s64)iova >> cfg->ias;

	if (WARN_ON(!pgsize || (pgsize & cfg->pgsize_bitmap) != pgsize))
		return -EINVAL;

	if (WARN_ON(iaext || paddr >> cfg->oas))
		return -ERANGE;

	/* If no access, then nothing to do */
	if (!(iommu_prot & (IOMMU_READ | IOMMU_WRITE)))
		return 0;

	prot = dart_prot_to_pte(data, iommu_prot);
	ret = __dart_map(data, iova, paddr, pgsize, pgcount, prot, lvl,
			     ptep, gfp, mapped);
	/*
	 * Synchronise all PTE updates for the new mapping before there's
	 * a chance for anything to kick off a table walk for the new iova.
	 */
	wmb();

	return ret;
}

static int dart_map(struct io_pgtable_ops *ops, unsigned long iova,
			phys_addr_t paddr, size_t size, int iommu_prot, gfp_t gfp)
{
	return dart_map_pages(ops, iova, paddr, size, 1, iommu_prot, gfp,
				  NULL);
}

static void __dart_free_pgtable(struct dart_io_pgtable *data, int lvl,
				    dart_iopte *ptep)
{
	dart_iopte *start, *end;
	unsigned long table_size;

	if (lvl == data->start_level)
		table_size = DART_PGD_SIZE(data);
	else
		table_size = DART_GRANULE(data);

	start = ptep;

	/* Only leaf entries at the last level */
	if (lvl == DART_MAX_LEVELS - 1)
		end = ptep;
	else
		end = (void *)ptep + table_size;

	while (ptep != end) {
		dart_iopte pte = *ptep++;

		if (!pte || iopte_leaf(pte, lvl, data->iop.fmt))
			continue;

		__dart_free_pgtable(data, lvl + 1, iopte_deref(pte, data));
	}

	__dart_free_pages(start, table_size);
}

static size_t __dart_unmap(struct dart_io_pgtable *data,
			       struct iommu_iotlb_gather *gather,
			       unsigned long iova, size_t size, size_t pgcount,
			       int lvl, dart_iopte *ptep)
{
	dart_iopte pte;
	struct io_pgtable *iop = &data->iop;
	int i = 0, num_entries, max_entries, unmap_idx_start;

	/* Something went horribly wrong and we ran out of page table */
	if (WARN_ON(lvl == DART_MAX_LEVELS))
		return 0;

	unmap_idx_start = DART_LVL_IDX(iova, lvl, data);
	ptep += unmap_idx_start;
	pte = READ_ONCE(*ptep);
	if (WARN_ON(!pte))
		return 0;

	/* If the size matches this level, we're in the right place */
	if (size == DART_BLOCK_SIZE(lvl, data)) {
		max_entries = DART_PTES_PER_TABLE(data) - unmap_idx_start;
		num_entries = min_t(int, pgcount, max_entries);

		while (i < num_entries) {
			pte = READ_ONCE(*ptep);
			if (WARN_ON(!pte))
				break;

			/* clear pte */
			*ptep = 0;

			if (!iopte_leaf(pte, lvl, iop->fmt)) {
				/* Also flush any partial walks */
				io_pgtable_tlb_flush_walk(iop, iova + i * size, size,
							  DART_GRANULE(data));
				__dart_free_pgtable(data, lvl + 1, iopte_deref(pte, data));
			} else if (!iommu_iotlb_gather_queued(gather)) {
				io_pgtable_tlb_add_page(iop, gather, iova + i * size, size);
			}

			ptep++;
			i++;
		}

		return i * size;
	}

	/* Keep on walkin' */
	ptep = iopte_deref(pte, data);
	return __dart_unmap(data, gather, iova, size, pgcount, lvl + 1, ptep);
}

static size_t dart_unmap_pages(struct io_pgtable_ops *ops, unsigned long iova,
				   size_t pgsize, size_t pgcount,
				   struct iommu_iotlb_gather *gather)
{
	struct dart_io_pgtable *data = io_pgtable_ops_to_data(ops);
	struct io_pgtable_cfg *cfg = &data->iop.cfg;
	dart_iopte *ptep = data->pgd;
	long iaext = (s64)iova >> cfg->ias;

	if (WARN_ON(!pgsize || (pgsize & cfg->pgsize_bitmap) != pgsize || !pgcount))
		return 0;

	if (WARN_ON(iaext))
		return 0;

	return __dart_unmap(data, gather, iova, pgsize, pgcount,
				data->start_level, ptep);
}

static size_t dart_unmap(struct io_pgtable_ops *ops, unsigned long iova,
			     size_t size, struct iommu_iotlb_gather *gather)
{
	return dart_unmap_pages(ops, iova, size, 1, gather);
}

static phys_addr_t dart_iova_to_phys(struct io_pgtable_ops *ops,
					 unsigned long iova)
{
	struct dart_io_pgtable *data = io_pgtable_ops_to_data(ops);
	dart_iopte pte, *ptep = data->pgd;
	int lvl = data->start_level;

	do {
		/* Valid IOPTE pointer? */
		if (!ptep)
			return 0;

		/* Grab the IOPTE we're interested in */
		ptep += DART_LVL_IDX(iova, lvl, data);
		pte = READ_ONCE(*ptep);

		/* Valid entry? */
		if (!pte)
			return 0;

		/* Leaf entry? */
		if (iopte_leaf(pte, lvl, data->iop.fmt))
			goto found_translation;

		/* Take it to the next level */
		ptep = iopte_deref(pte, data);
	} while (++lvl < DART_MAX_LEVELS);

	/* Ran out of page tables to walk */
	return 0;

found_translation:
	iova &= (DART_BLOCK_SIZE(lvl, data) - 1);
	return iopte_to_paddr(pte, data) | iova;
}

static void dart_restrict_pgsizes(struct io_pgtable_cfg *cfg)
{
	unsigned long granule, page_sizes;
	unsigned int max_addr_bits = 48;

	/*
	 * We need to restrict the supported page sizes to match the
	 * translation regime for a particular granule. Aim to match
	 * the CPU page size if possible, otherwise prefer smaller sizes.
	 * While we're at it, restrict the block sizes to match the
	 * chosen granule.
	 */
	if (cfg->pgsize_bitmap & PAGE_SIZE)
		granule = PAGE_SIZE;
	else if (cfg->pgsize_bitmap & ~PAGE_MASK)
		granule = 1UL << __fls(cfg->pgsize_bitmap & ~PAGE_MASK);
	else if (cfg->pgsize_bitmap & PAGE_MASK)
		granule = 1UL << __ffs(cfg->pgsize_bitmap & PAGE_MASK);
	else
		granule = 0;

	switch (granule) {
	case SZ_4K:
		page_sizes = (SZ_4K | SZ_2M | SZ_1G);
		break;
	case SZ_16K:
		page_sizes = (SZ_16K | SZ_32M);
		break;
	default:
		page_sizes = 0;
	}

	cfg->pgsize_bitmap &= page_sizes;
	cfg->ias = min(cfg->ias, max_addr_bits);
	cfg->oas = min(cfg->oas, max_addr_bits);
}

static struct dart_io_pgtable *
dart_alloc_pgtable(struct io_pgtable_cfg *cfg)
{
	struct dart_io_pgtable *data;
	int bits_per_level, levels, va_bits, pg_shift;

	dart_restrict_pgsizes(cfg);

	if (!(cfg->pgsize_bitmap & (SZ_4K | SZ_16K)))
		return NULL;

	if (cfg->ias > DART_MAX_ADDR_BITS)
		return NULL;

	if (cfg->oas > DART_MAX_ADDR_BITS)
		return NULL;

	pg_shift = __ffs(cfg->pgsize_bitmap);
	bits_per_level = pg_shift - ilog2(sizeof(dart_iopte));

	va_bits = cfg->ias - pg_shift;
	levels = DIV_ROUND_UP(va_bits, bits_per_level);
	if (levels > DART_MAX_LEVELS)
		return NULL;

	data = kmalloc(sizeof(*data), GFP_KERNEL);
	if (!data)
		return NULL;

	data->bits_per_level = bits_per_level;
	data->start_level = DART_MAX_LEVELS - levels;

	/* Calculate the actual size of our pgd (without concatenation) */
	data->pgd_bits = va_bits - (data->bits_per_level * (levels - 1));

	data->iop.ops = (struct io_pgtable_ops) {
		.map		= dart_map,
		.map_pages	= dart_map_pages,
		.unmap		= dart_unmap,
		.unmap_pages	= dart_unmap_pages,
		.iova_to_phys	= dart_iova_to_phys,
	};

	return data;
}

static struct io_pgtable *
apple_dart_alloc_pgtable(struct io_pgtable_cfg *cfg, void *cookie)
{
	struct dart_io_pgtable *data;
	int i;

	if (!cfg->coherent_walk)
		return NULL;

	if (cfg->oas != 36 && cfg->oas != 42)
		return NULL;

	data = dart_alloc_pgtable(cfg);
	if (!data)
		return NULL;

	/*
	 * The table format itself always uses two levels, but the total VA
	 * space is mapped by four separate tables, making the MMIO registers
	 * an effective "level 1". For simplicity, though, we treat this
	 * equivalently to LPAE stage 2 concatenation at level 2, with the
	 * additional TTBRs each just pointing at consecutive pages.
	 */
	if (data->start_level == 0 && data->pgd_bits > 2)
		goto out_free_data;
	if (data->start_level > 0)
		data->pgd_bits = 0;
	data->start_level = 1;
	cfg->apple_dart_cfg.n_ttbrs = 1 << data->pgd_bits;
	data->pgd_bits += data->bits_per_level;

	data->pgd = __dart_alloc_pages(DART_PGD_SIZE(data), GFP_KERNEL,
					   cfg);
	if (!data->pgd)
		goto out_free_data;

	for (i = 0; i < cfg->apple_dart_cfg.n_ttbrs; ++i)
		cfg->apple_dart_cfg.ttbr[i] =
			virt_to_phys(data->pgd + i * DART_GRANULE(data));

	return &data->iop;

out_free_data:
	kfree(data);
	return NULL;
}

static void apple_dart_free_pgtable(struct io_pgtable *iop)
{
	struct dart_io_pgtable *data = io_pgtable_to_data(iop);

	__dart_free_pgtable(data, data->start_level, data->pgd);
	kfree(data);
}

struct io_pgtable_init_fns io_pgtable_apple_dart_init_fns = {
	.alloc	= apple_dart_alloc_pgtable,
	.free	= apple_dart_free_pgtable,
};
