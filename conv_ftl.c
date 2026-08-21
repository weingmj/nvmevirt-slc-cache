// SPDX-License-Identifier: GPL-2.0-only

#include <linux/vmalloc.h>
#include <linux/ktime.h>
#include <linux/sched/clock.h>

#ifdef RD
#include <linux/random.h>
#endif

#include "nvmev.h"
#include "conv_ftl.h"


static inline bool last_pg_in_wordline(struct conv_ftl *conv_ftl, struct ppa *ppa)
{
	struct ssdparams *spp = &conv_ftl->ssd->sp;
	return (ppa->g.pg % spp->pgs_per_oneshotpg) == (spp->pgs_per_oneshotpg - 1);
}

static inline bool last_pg_in_wordline_slc(struct conv_ftl *conv_ftl, struct ppa *ppa)
{
	struct ssdparams *spp = &conv_ftl->ssd->sp;
	return (ppa->g.pg % spp->pgs_per_oneshotpg_slc) == (spp->pgs_per_oneshotpg_slc - 1);
}

static bool should_gc(struct conv_ftl *conv_ftl)
{
	return (conv_ftl->lm.free_line_cnt <= conv_ftl->cp.gc_thres_lines);
}

static inline bool should_gc_high(struct conv_ftl *conv_ftl)
{
	return conv_ftl->lm.free_line_cnt <= conv_ftl->cp.gc_thres_lines_high;
}

static bool should_migration(struct conv_ftl *conv_ftl)
{
	return (conv_ftl->slm.free_line_cnt <= conv_ftl->cp.mig_thres_lines);
}

static bool should_migration_high(struct conv_ftl *conv_ftl)
{
	return (conv_ftl->slm.free_line_cnt <= conv_ftl->cp.mig_thres_lines_high);
}

static inline struct ppa get_maptbl_ent(struct conv_ftl *conv_ftl, uint64_t lpn)
{
	return conv_ftl->maptbl[lpn];
}

static inline void set_maptbl_ent(struct conv_ftl *conv_ftl, uint64_t lpn, struct ppa *ppa)
{
	NVMEV_ASSERT(lpn < conv_ftl->ssd->sp.tt_pgs);
	conv_ftl->maptbl[lpn] = *ppa;
}

static uint64_t ppa2pgidx(struct conv_ftl *conv_ftl, struct ppa *ppa)
{
	struct ssdparams *spp = &conv_ftl->ssd->sp;
	uint64_t pgidx;

	//NVMEV_INFO("%s: ch:%d, lun:%d, pl:%d, blk:%d, pg:%d\n", __func__,
	//		ppa->g.ch, ppa->g.lun, ppa->g.pl, ppa->g.blk, ppa->g.pg);

	if (conv_ftl->slc_mode == SLC_MODE) {
		if (ppa->g.blk < spp->tt_lines_slc) { // in slc index
			NVMEV_ASSERT(ppa->g.pg < spp->pgs_per_blk_slc);
			pgidx = ppa->g.ch * spp->pgs_per_ch + ppa->g.lun * spp->pgs_per_lun +
				ppa->g.pl * spp->pgs_per_pl + ppa->g.blk * spp->pgs_per_blk_slc + ppa->g.pg;
		} else {
			pgidx =	ppa->g.ch * spp->pgs_per_ch + ppa->g.lun * spp->pgs_per_lun +
				ppa->g.pl * spp->pgs_per_pl + (spp->tt_lines_slc * spp->pgs_per_blk_slc + 
				(ppa->g.blk - spp->tt_lines_slc) * spp->pgs_per_blk) + ppa->g.pg;
		}
	} else {
		pgidx = ppa->g.ch * spp->pgs_per_ch + ppa->g.lun * spp->pgs_per_lun +
			ppa->g.pl * spp->pgs_per_pl + ppa->g.blk * spp->pgs_per_blk + ppa->g.pg;
	}
	
	NVMEV_ASSERT(pgidx < spp->tt_pgs);
	return pgidx;
}

static inline uint64_t get_rmap_ent(struct conv_ftl *conv_ftl, struct ppa *ppa)
{
	uint64_t pgidx = ppa2pgidx(conv_ftl, ppa);

	return conv_ftl->rmap[pgidx];
}

/* set rmap[page_no(ppa)] -> lpn */
static inline void set_rmap_ent(struct conv_ftl *conv_ftl, uint64_t lpn, struct ppa *ppa)
{
	uint64_t pgidx = ppa2pgidx(conv_ftl, ppa);
	//NVMEV_INFO("set rmap[%llu] to %llu", pgidx, lpn);

	conv_ftl->rmap[pgidx] = lpn;
}

static inline int victim_line_cmp_pri(pqueue_pri_t next, pqueue_pri_t curr)
{
	return (next > curr);
}

static inline pqueue_pri_t victim_line_get_pri(void *a)
{

	return ((struct line *)a)->vpc;
}

static inline void victim_line_set_pri(void *a, pqueue_pri_t pri)
{
	((struct line *)a)->vpc = pri;
}

static inline size_t victim_line_get_pos(void *a)
{
	return ((struct line *)a)->pos;
}

static inline void victim_line_set_pos(void *a, size_t pos)
{
	((struct line *)a)->pos = pos;
}

static inline void consume_write_credit(struct conv_ftl *conv_ftl)
{
	conv_ftl->wfc.write_credits--;
}

static void foreground_gc(struct conv_ftl *conv_ftl);

static inline void check_and_refill_write_credit(struct conv_ftl *conv_ftl)
{
	struct write_flow_control *wfc = &(conv_ftl->wfc);
	if (wfc->write_credits <= 0) {
		foreground_gc(conv_ftl);
		wfc->write_credits += wfc->credits_to_refill;
	}
}

static void init_lines(struct conv_ftl *conv_ftl)
{
	struct ssdparams *spp = &conv_ftl->ssd->sp;
	struct line_mgmt *lm = &conv_ftl->lm;
	struct line_mgmt *slm = &conv_ftl->slm;
	struct line *line;
	int i, offset = 0;

	if (conv_ftl->slc_mode == SLC_MODE) {
		lm->tt_lines = spp->tt_lines_tlc;
		slm->tt_lines = spp->tt_lines_slc;
		NVMEV_ASSERT(lm->tt_lines + slm->tt_lines == spp->tt_lines);
		lm->lines = vmalloc(sizeof(struct line) * lm->tt_lines);
		slm->lines = vmalloc(sizeof(struct line) * slm->tt_lines);
	} else {
		lm->tt_lines = spp->blks_per_pl;
		NVMEV_ASSERT(lm->tt_lines == spp->tt_lines);
		lm->lines = vmalloc(sizeof(struct line) * lm->tt_lines);
	}
	

	INIT_LIST_HEAD(&lm->free_line_list);
	INIT_LIST_HEAD(&lm->full_line_list);
	
	if (conv_ftl->slc_mode == SLC_MODE) {
		lm->victim_line_pq = pqueue_init(spp->tt_lines_tlc, victim_line_cmp_pri, victim_line_get_pri,
				victim_line_set_pri, victim_line_get_pos,
				victim_line_set_pos);
	} else {
		lm->victim_line_pq = pqueue_init(spp->tt_lines, victim_line_cmp_pri, victim_line_get_pri,
				victim_line_set_pri, victim_line_get_pos,
				victim_line_set_pos);
	}
	

	lm->free_line_cnt = 0;

	if (conv_ftl->slc_mode == SLC_MODE) {
		offset = slm->tt_lines; // lm_lines are after slm_lines
	}

	for (i = 0; i < lm->tt_lines; i++) {
		lm->lines[i] = (struct line){
			.id = i + offset,
			.ipc = 0,
			.vpc = 0,
			.pos = 0,
			.entry = LIST_HEAD_INIT(lm->lines[i].entry),
		};

		/* initialize all the lines as free lines */
		list_add_tail(&lm->lines[i].entry, &lm->free_line_list);
		lm->free_line_cnt++;
	}

	NVMEV_ASSERT(lm->free_line_cnt == lm->tt_lines);
	lm->victim_line_cnt = 0;
	lm->full_line_cnt = 0;
	
	if (conv_ftl->slc_mode == SLC_MODE) {
		INIT_LIST_HEAD(&slm->full_line_list);
		INIT_LIST_HEAD(&slm->free_line_list);

		slm->victim_line_pq = pqueue_init(spp->tt_lines_slc, victim_line_cmp_pri, victim_line_get_pri,
						victim_line_set_pri, victim_line_get_pos,
						victim_line_set_pos);
		slm->free_line_cnt = 0;

		for (i = 0; i < slm->tt_lines; i++) {
			slm->lines[i] = (struct line) {
				.id = i,
				.ipc = 0,
				.vpc = 0,
				.pos = 0,
				.entry = LIST_HEAD_INIT(slm->lines[i].entry),
			};
			list_add_tail(&slm->lines[i].entry, &slm->free_line_list);
			slm->free_line_cnt++;
		}

		/* initialize all the lines as free lines */
		
		NVMEV_ASSERT(slm->free_line_cnt == slm->tt_lines);
		slm->victim_line_cnt = 0;
		slm->full_line_cnt = 0;
	}
}

static void remove_lines(struct conv_ftl *conv_ftl)
{
	pqueue_free(conv_ftl->lm.victim_line_pq);
	vfree(conv_ftl->lm.lines);
	if (conv_ftl->slc_mode == SLC_MODE) {
		pqueue_free(conv_ftl->slm.victim_line_pq);
		vfree(conv_ftl->slm.lines);
	}
}

static void init_write_flow_control(struct conv_ftl *conv_ftl)
{
	struct write_flow_control *wfc = &(conv_ftl->wfc);
	struct ssdparams *spp = &conv_ftl->ssd->sp;

	if (conv_ftl->slc_mode == SLC_MODE) {
		wfc->write_credits = spp->pgs_per_line_slc;
		wfc->credits_to_refill = spp->pgs_per_line_slc;
	} else {
		wfc->write_credits = spp->pgs_per_line;
		wfc->credits_to_refill = spp->pgs_per_line;
	}
}

static inline void check_addr(int a, int max)
{
	NVMEV_ASSERT(a >= 0 && a < max);
}

static struct line *get_next_free_line(struct conv_ftl *conv_ftl)
{
	struct line_mgmt *lm;
	if (conv_ftl->slc_mode == SLC_MODE && conv_ftl->dyn_slc_mode == SLC_MODE) {
		lm = &conv_ftl->slm;
	} else {
		lm = &conv_ftl->lm;
	}
	
	struct line *curline = list_first_entry_or_null(&lm->free_line_list, struct line, entry);

	if (!curline) {
		NVMEV_ERROR("No free line left in VIRT !!!!\n");
		return NULL;
	}

	list_del_init(&curline->entry);
	lm->free_line_cnt--;
	NVMEV_INFO("%s: free_line_cnt %d\n", __func__, lm->free_line_cnt);
	return curline;
}

static struct write_pointer *__get_wp(struct conv_ftl *ftl, uint32_t io_type)
{
	if (io_type == USER_IO) {
		return &ftl->wp;
	} else if (io_type == GC_IO) {
		return &ftl->gc_wp;
	}
	
	return NULL;
}

static void prepare_write_pointer(struct conv_ftl *conv_ftl, uint32_t io_type)
{
	struct write_pointer *wp = __get_wp(conv_ftl, io_type);

	if (conv_ftl->slc_mode == SLC_MODE && io_type == USER_IO) {
		conv_ftl->dyn_slc_mode = SLC_MODE;
	} else if (io_type == GC_IO) {
		conv_ftl->dyn_slc_mode = TLC_MODE;
	} else {
		conv_ftl->dyn_slc_mode = TLC_MODE;
	}

	struct line *curline = get_next_free_line(conv_ftl);

	NVMEV_ASSERT(wp);
	NVMEV_ASSERT(curline);

	/* wp->curline is always our next-to-write super-block */
	*wp = (struct write_pointer) {
		.curline = curline,
		.ch = 0,
		.lun = 0,
		.pg = 0,
		.blk = curline->id,
		.pl = 0,
	};
}

static void advance_write_pointer(struct conv_ftl *conv_ftl, uint32_t io_type)
{
	struct ssdparams *spp = &conv_ftl->ssd->sp;
	struct line_mgmt *lm;
	struct write_pointer *wpp = __get_wp(conv_ftl, io_type);
	int cur_pgs_per_blk = 0;
	int cur_pgs_per_oneshotpg = 0;
	int cur_pgs_per_line = 0;
	
	if (conv_ftl->slc_mode == SLC_MODE) {
		if (io_type == GC_IO) {
			lm = &conv_ftl->lm;
			cur_pgs_per_blk = spp->pgs_per_blk;
			cur_pgs_per_oneshotpg = spp->pgs_per_oneshotpg;
			cur_pgs_per_line = spp->pgs_per_line;
		} else if (io_type == USER_IO) {
			lm = &conv_ftl->slm;
			cur_pgs_per_blk = spp->pgs_per_blk_slc;
			cur_pgs_per_oneshotpg = spp->pgs_per_oneshotpg_slc;
			cur_pgs_per_line = spp->pgs_per_line_slc;
		}
	} else {
		lm = &conv_ftl->lm;
		cur_pgs_per_blk = spp->pgs_per_blk;
		cur_pgs_per_oneshotpg = spp->pgs_per_oneshotpg;
		cur_pgs_per_line = spp->pgs_per_line;
	}

	NVMEV_DEBUG_VERBOSE("current wpp: ch:%d, lun:%d, pl:%d, blk:%d, pg:%d\n",
			wpp->ch, wpp->lun, wpp->pl, wpp->blk, wpp->pg);

	check_addr(wpp->pg, cur_pgs_per_blk);
	wpp->pg++;
	if ((wpp->pg % cur_pgs_per_oneshotpg) != 0)
		goto out;

	wpp->pg -= cur_pgs_per_oneshotpg;
	check_addr(wpp->ch, spp->nchs);
	wpp->ch++;
	if (wpp->ch != spp->nchs)
		goto out;

	wpp->ch = 0;
	check_addr(wpp->lun, spp->luns_per_ch);
	wpp->lun++;
	/* in this case, we should go to next lun */
	if (wpp->lun != spp->luns_per_ch)
		goto out;

	wpp->lun = 0;
	/* go to next wordline in the block */
	wpp->pg += cur_pgs_per_oneshotpg;
	if (wpp->pg != cur_pgs_per_blk)
		goto out;

	wpp->pg = 0;
	/* move current line to {victim,full} line list */
	if (wpp->curline->vpc == cur_pgs_per_line) {
		/* all pgs are still valid, move to full line list */
		NVMEV_ASSERT(wpp->curline->ipc == 0);
		list_add_tail(&wpp->curline->entry, &lm->full_line_list);
		lm->full_line_cnt++;
		NVMEV_DEBUG_VERBOSE("wpp: move line to full_line_list\n");
	} else {
		NVMEV_DEBUG_VERBOSE("wpp: line is moved to victim list\n");
		NVMEV_INFO("insert1/%d, %d, %d, %lu", wpp->curline->id, wpp->curline->ipc, wpp->curline->vpc, wpp->curline->pos);
		NVMEV_ASSERT(wpp->curline->vpc >= 0 && wpp->curline->vpc < cur_pgs_per_line);
		/* there must be some invalid pages in this line */
		NVMEV_ASSERT(wpp->curline->ipc > 0);
		if (wpp->curline->vpc + wpp->curline->ipc != cur_pgs_per_line)
			NVMEV_INFO("vpc: %d, ipc: %d, total: %d", wpp->curline->vpc, wpp->curline->ipc, cur_pgs_per_line);
		NVMEV_ASSERT(wpp->curline->vpc + wpp->curline->ipc == cur_pgs_per_line);
		pqueue_insert(lm->victim_line_pq, wpp->curline);
		lm->victim_line_cnt++;
	}
	/* current line is used up, pick another empty line */
	check_addr(wpp->blk, spp->blks_per_pl);
	if (io_type == USER_IO) {
		conv_ftl->dyn_slc_mode = SLC_MODE;
	} else {
		conv_ftl->dyn_slc_mode = TLC_MODE;
	}
	wpp->curline = get_next_free_line(conv_ftl);
	NVMEV_ASSERT(wpp->curline);
	NVMEV_DEBUG_VERBOSE("wpp: got new clean line %d\n", wpp->curline->id);

	wpp->blk = wpp->curline->id;
	check_addr(wpp->blk, spp->blks_per_pl);

	/* make sure we are starting from page 0 in the super block */
	NVMEV_ASSERT(wpp->pg == 0);
	NVMEV_ASSERT(wpp->lun == 0);
	NVMEV_ASSERT(wpp->ch == 0);
	/* TODO: assume # of pl_per_lun is 1, fix later */
	NVMEV_ASSERT(wpp->pl == 0);
out:
	NVMEV_DEBUG_VERBOSE("advanced wpp: ch:%d, lun:%d, pl:%d, blk:%d, pg:%d (curline %d)\n",
			wpp->ch, wpp->lun, wpp->pl, wpp->blk, wpp->pg, wpp->curline->id);
}

static struct ppa get_new_page(struct conv_ftl *conv_ftl, uint32_t io_type)
{
	struct ppa ppa;
	struct write_pointer *wp = __get_wp(conv_ftl, io_type);

	ppa.ppa = 0;
	ppa.g.ch = wp->ch;
	ppa.g.lun = wp->lun;
	ppa.g.pg = wp->pg;
	ppa.g.blk = wp->blk;
	ppa.g.pl = wp->pl;

	NVMEV_ASSERT(ppa.g.pl == 0);

	return ppa;
}

static void init_maptbl(struct conv_ftl *conv_ftl)
{
	int i;
	struct ssdparams *spp = &conv_ftl->ssd->sp;

	conv_ftl->maptbl = vmalloc(sizeof(struct ppa) * spp->tt_pgs);
	for (i = 0; i < spp->tt_pgs; i++) {
		conv_ftl->maptbl[i].ppa = UNMAPPED_PPA;
	}
}

static void remove_maptbl(struct conv_ftl *conv_ftl)
{
	vfree(conv_ftl->maptbl);
}

static void init_rmap(struct conv_ftl *conv_ftl)
{
	int i;
	struct ssdparams *spp = &conv_ftl->ssd->sp;

	conv_ftl->rmap = vmalloc(sizeof(uint64_t) * spp->tt_pgs);
	for (i = 0; i < spp->tt_pgs; i++) {
		conv_ftl->rmap[i] = INVALID_LPN;
	}
}

static void remove_rmap(struct conv_ftl *conv_ftl)
{
	vfree(conv_ftl->rmap);
}

static void conv_init_ftl(struct conv_ftl *conv_ftl, struct convparams *cpp, struct ssd *ssd)
{
	/*copy convparams*/
	conv_ftl->cp = *cpp;

	conv_ftl->ssd = ssd;
	if (ENABLE_SLC_CACHE) {
		conv_ftl->slc_mode = SLC_MODE;
	} else {
		conv_ftl->slc_mode = TLC_MODE;
	}

	/* initialize maptbl */
	init_maptbl(conv_ftl); // mapping table

	/* initialize rmap */
	init_rmap(conv_ftl); // reverse mapping table (?)

	/* initialize all the lines */
	init_lines(conv_ftl);

	/* initialize write pointer, this is how we allocate new pages for writes */
	prepare_write_pointer(conv_ftl, USER_IO);
	prepare_write_pointer(conv_ftl, GC_IO);

	init_write_flow_control(conv_ftl);

	NVMEV_INFO("Init FTL instance with %d channels (%ld pages)\n", conv_ftl->ssd->sp.nchs,
		   conv_ftl->ssd->sp.tt_pgs);

	return;
}

static void conv_remove_ftl(struct conv_ftl *conv_ftl)
{
	remove_lines(conv_ftl);
	remove_rmap(conv_ftl);
	remove_maptbl(conv_ftl);
}

static void conv_init_params(struct convparams *cpp)
{
	cpp->op_area_pcent = OP_AREA_PERCENT;
	cpp->gc_thres_lines = 2; /* Need only two lines.(host write, gc)*/
	cpp->gc_thres_lines_high = 2; /* Need only two lines.(host write, gc)*/
	cpp->enable_gc_delay = 1;
	cpp->pba_pcent = (int)((1 + cpp->op_area_pcent) * 100);
	/* for SLC cache */
	cpp->mig_thres_lines = 2;
	cpp->mig_thres_lines_high = 2;
}

void conv_init_namespace(struct nvmev_ns *ns, uint32_t id, uint64_t size, void *mapped_addr,
			 uint32_t cpu_nr_dispatcher)
{
	struct ssdparams spp;
	struct convparams cpp;
	struct conv_ftl *conv_ftls;
	struct ssd *ssd;
	uint32_t i;
	const uint32_t nr_parts = SSD_PARTITIONS;

	if (ENABLE_SLC_CACHE == 1) {
		ssd_init_params_slc(&spp, size, nr_parts);
	} else {
		ssd_init_params(&spp, size, nr_parts);
	}
	
	conv_init_params(&cpp);

	conv_ftls = kmalloc(sizeof(struct conv_ftl) * nr_parts, GFP_KERNEL);

	for (i = 0; i < nr_parts; i++) {
		ssd = kmalloc(sizeof(struct ssd), GFP_KERNEL);
		ssd_init(ssd, &spp, cpu_nr_dispatcher);
		conv_init_ftl(&conv_ftls[i], &cpp, ssd);
	}

	/* PCIe, Write buffer are shared by all instances*/
	for (i = 1; i < nr_parts; i++) {
		kfree(conv_ftls[i].ssd->pcie->perf_model);
		kfree(conv_ftls[i].ssd->pcie);
		kfree(conv_ftls[i].ssd->write_buffer);
		conv_ftls[i].ssd->pcie = conv_ftls[0].ssd->pcie;
		conv_ftls[i].ssd->write_buffer = conv_ftls[0].ssd->write_buffer;
	}

	ns->id = id;
	ns->csi = NVME_CSI_NVM;
	ns->nr_parts = nr_parts;
	ns->ftls = (void *)conv_ftls;
	ns->size = (uint64_t)((size * 100) / cpp.pba_pcent);
	ns->mapped = mapped_addr;
	/*register io command handler*/
	ns->proc_io_cmd = conv_proc_nvme_io_cmd;

	NVMEV_INFO("FTL physical space: %lld, logical space: %lld (physical/logical * 100 = %d)\n",
		   size, ns->size, cpp.pba_pcent);

	return;
}

void conv_remove_namespace(struct nvmev_ns *ns)
{
	struct conv_ftl *conv_ftls = (struct conv_ftl *)ns->ftls;
	const uint32_t nr_parts = SSD_PARTITIONS;
	uint32_t i;

	/* PCIe, Write buffer are shared by all instances*/
	for (i = 1; i < nr_parts; i++) {
		/*
		 * These were freed from conv_init_namespace() already.
		 * Mark these NULL so that ssd_remove() skips it.
		 */
		conv_ftls[i].ssd->pcie = NULL;
		conv_ftls[i].ssd->write_buffer = NULL;
	}

	for (i = 0; i < nr_parts; i++) {
		conv_remove_ftl(&conv_ftls[i]);
		ssd_remove(conv_ftls[i].ssd);
		kfree(conv_ftls[i].ssd);
	}

	kfree(conv_ftls);
	ns->ftls = NULL;
}

static inline bool valid_ppa(struct conv_ftl *conv_ftl, struct ppa *ppa)
{
	struct ssdparams *spp = &conv_ftl->ssd->sp;
	int ch = ppa->g.ch;
	int lun = ppa->g.lun;
	int pl = ppa->g.pl;
	int blk = ppa->g.blk;
	int pg = ppa->g.pg;
	//int sec = ppa->g.sec;

	if (ch < 0 || ch >= spp->nchs)
		return false;
	if (lun < 0 || lun >= spp->luns_per_ch)
		return false;
	if (pl < 0 || pl >= spp->pls_per_lun)
		return false;
	if (blk < 0 || blk >= spp->blks_per_pl)
		return false;
	if (conv_ftl->slc_mode == SLC_MODE) {
		if (blk < conv_ftl->slm.tt_lines) {
			if (pg < 0 || pg >= spp->pgs_per_blk_slc) {
				NVMEV_INFO("invalid ppa from SLC cache");
				return false;
			}
		} else {
			if (pg < 0 || pg >= spp->pgs_per_blk) {
				NVMEV_INFO("invalid ppa from TLC Area");
				return false;
			}
		}
	} else {
		if (pg < 0 || pg >= spp->pgs_per_blk)
			return false;
	}

	return true;
}

static inline bool valid_lpn(struct conv_ftl *conv_ftl, uint64_t lpn)
{
	if (lpn < conv_ftl->ssd->sp.tt_pgs) {
		return true;
	} else {
		//NVMEV_INFO("lpn: %llu, tt_pgs: %lu", lpn, conv_ftl->ssd->sp.tt_pgs);
		return false;
	}
}

static inline bool mapped_ppa(struct ppa *ppa)
{
	return !(ppa->ppa == UNMAPPED_PPA);
}

static inline struct line *get_line(struct conv_ftl *conv_ftl, struct ppa *ppa)
{
	if (conv_ftl->slc_mode == SLC_MODE) {
		int boundary_line = conv_ftl->slm.tt_lines;
		if (ppa->g.blk < boundary_line) {
			return &(conv_ftl->slm.lines[ppa->g.blk]);
		} else {
			return &(conv_ftl->lm.lines[ppa->g.blk - boundary_line]);
		}
	} else {
		return &(conv_ftl->lm.lines[ppa->g.blk]);
	}
}

/* update SSD status about one page from PG_VALID -> PG_VALID */
static void mark_page_invalid(struct conv_ftl *conv_ftl, struct ppa *ppa)
{
	struct ssdparams *spp = &conv_ftl->ssd->sp;
	struct nand_block *blk = NULL;
	struct nand_page *pg = NULL;
	bool was_full_line = false;
	struct line *line;

	struct line_mgmt *lm;
	int cur_pgs_per_blk;
	int cur_pgs_per_line;
	NVMEV_ASSERT(spp->tt_lines_slc == conv_ftl->slm.tt_lines);
	if (conv_ftl->slc_mode == SLC_MODE) {
		if (ppa->g.blk < spp->tt_lines_slc) { // this ppa is in SLC cache area
			lm = &conv_ftl->slm;
			cur_pgs_per_blk = spp->pgs_per_blk_slc;
			cur_pgs_per_line = spp->pgs_per_line_slc;
		} else {
			lm = &conv_ftl->lm;
			cur_pgs_per_blk = spp->pgs_per_blk;
			cur_pgs_per_line = spp->pgs_per_line;
		}
	} else {
		lm = &conv_ftl->lm;
		cur_pgs_per_blk = spp->pgs_per_blk;
		cur_pgs_per_line = spp->pgs_per_line;
	}
	

	/* update corresponding page status */
	pg = get_pg(conv_ftl->ssd, ppa);
	NVMEV_ASSERT(pg->status == PG_VALID);
	pg->status = PG_INVALID;

	/* update corresponding block status */
	blk = get_blk(conv_ftl->ssd, ppa);
	
	NVMEV_ASSERT(blk->ipc >= 0 && blk->ipc < cur_pgs_per_blk);
	blk->ipc++;
	NVMEV_ASSERT(blk->vpc > 0 && blk->vpc <= cur_pgs_per_blk);
	blk->vpc--;

	/* update corresponding line status */
	line = get_line(conv_ftl, ppa);
	NVMEV_ASSERT(line->ipc >= 0 && line->ipc < cur_pgs_per_line);
	if (line->vpc == cur_pgs_per_line) {
		NVMEV_ASSERT(line->ipc == 0);
		was_full_line = true;
	}
	line->ipc++;
	NVMEV_ASSERT(line->vpc > 0 && line->vpc <= cur_pgs_per_line);
	/* Adjust the position of the victime line in the pq under over-writes */
	if (line->pos) {
		NVMEV_INFO("change_priority/%d, %d, %d, %lu", line->id, line->ipc, line->vpc, line->pos);
		/* Note that line->vpc will be updated by this call */
		pqueue_change_priority(lm->victim_line_pq, line->vpc - 1, line);
	} else {
		line->vpc--;
	}

	if (was_full_line) {
		/* move line: "full" -> "victim" */
		list_del_init(&line->entry);
		lm->full_line_cnt--;
		if (line->vpc + line->ipc != cur_pgs_per_line)
			NVMEV_INFO("vpc: %d, ipc: %d, total: %d", line->vpc, line->ipc, cur_pgs_per_line);
		NVMEV_ASSERT(line->vpc + line->ipc == cur_pgs_per_line);
		NVMEV_INFO("insert2/%d, %d, %d, %lu", line->id, line->ipc, line->vpc, line->pos);
		pqueue_insert(lm->victim_line_pq, line);
		lm->victim_line_cnt++;
	}
}

static void mark_page_valid(struct conv_ftl *conv_ftl, struct ppa *ppa)
{
	struct ssdparams *spp = &conv_ftl->ssd->sp;
	struct nand_block *blk = NULL;
	struct nand_page *pg = NULL;
	struct line *line;

	int cur_pgs_per_blk;
	int cur_pgs_per_line;

	if (conv_ftl->slc_mode == SLC_MODE) {
		if (ppa->g.blk < spp->tt_lines_slc) {
			cur_pgs_per_blk = spp->pgs_per_blk_slc;
			cur_pgs_per_line = spp->pgs_per_line_slc;
		} else {
			cur_pgs_per_blk = spp->pgs_per_blk;
			cur_pgs_per_line = spp->pgs_per_line;
		}
	} else {
		cur_pgs_per_blk = spp->pgs_per_blk;
		cur_pgs_per_line = spp->pgs_per_line;
	}
	

	/* update page status */
	pg = get_pg(conv_ftl->ssd, ppa);
	NVMEV_ASSERT(pg->status == PG_FREE);
	pg->status = PG_VALID;

	/* update corresponding block status */
	blk = get_blk(conv_ftl->ssd, ppa);
	NVMEV_ASSERT(blk->vpc >= 0 && blk->vpc < cur_pgs_per_blk);
	blk->vpc++;

	/* update corresponding line status */
	line = get_line(conv_ftl, ppa);
	NVMEV_ASSERT(line->vpc >= 0 && line->vpc < cur_pgs_per_line);
	line->vpc++;
}

static void mark_block_free(struct conv_ftl *conv_ftl, struct ppa *ppa)
{
	struct ssdparams *spp = &conv_ftl->ssd->sp;
	struct nand_block *blk = get_blk(conv_ftl->ssd, ppa);
	struct nand_page *pg = NULL;
	int i;

	int cur_pgs_per_blk;
	if (conv_ftl->slc_mode == SLC_MODE) {
		if (ppa->g.blk < spp->tt_lines_slc) {
			cur_pgs_per_blk = spp->pgs_per_blk_slc;
		} else {
			cur_pgs_per_blk = spp->pgs_per_blk;
		}
	} else {
		cur_pgs_per_blk = spp->pgs_per_blk;
	}

	for (i = 0; i < cur_pgs_per_blk; i++) {
		/* reset page status */
		pg = &blk->pg[i];
		NVMEV_ASSERT(pg->nsecs == spp->secs_per_pg);
		pg->status = PG_FREE;
	}

	/* reset block status */
	NVMEV_ASSERT(blk->npgs == cur_pgs_per_blk);
	blk->ipc = 0;
	blk->vpc = 0;
	blk->erase_cnt++;
}

/* read one valid page to copy somewhere */
/* ..하는 척만 함(타이밍 모델) */
static void gc_read_page(struct conv_ftl *conv_ftl, struct ppa *ppa)
{
	struct ssdparams *spp = &conv_ftl->ssd->sp;
	struct convparams *cpp = &conv_ftl->cp;
	/* advance conv_ftl status, we don't care about how long it takes */
	if (cpp->enable_gc_delay) {
		struct nand_cmd gcr = {
			.type = GC_IO,
			.cmd = NAND_READ,
			.stime = 0,
			.xfer_size = spp->pgsz,
			.interleave_pci_dma = false,
			.ppa = ppa,
		};
		ssd_advance_nand(conv_ftl->ssd, &gcr);
	}
}

/* move valid page data (already in DRAM) from victim line to a new page */
/* gc를 하든 migration을 하든 TLC 영역에 적어야 하므로 이 함수는 변경점이 없음 */
static uint64_t gc_write_page(struct conv_ftl *conv_ftl, struct ppa *old_ppa)
{
	struct ssdparams *spp = &conv_ftl->ssd->sp;
	struct convparams *cpp = &conv_ftl->cp;
	struct ppa new_ppa;
	//NVMEV_INFO("%s: old_ppa's... ch:%d, lun:%d, pl:%d, blk:%d, pg:%d\n", __func__,
	//		old_ppa->g.ch, old_ppa->g.lun, old_ppa->g.pl, old_ppa->g.blk, old_ppa->g.pg);
	uint64_t lpn = get_rmap_ent(conv_ftl, old_ppa);
	NVMEV_INFO("lpn: %llu, pgidx: %llu", lpn, ppa2pgidx(conv_ftl, old_ppa));
	NVMEV_ASSERT(valid_lpn(conv_ftl, lpn));

	new_ppa = get_new_page(conv_ftl, GC_IO);
	
	/* update maptbl */
	set_maptbl_ent(conv_ftl, lpn, &new_ppa);
	/* update rmap */
	set_rmap_ent(conv_ftl, lpn, &new_ppa);

	mark_page_valid(conv_ftl, &new_ppa);
	conv_ftl->copy_cnt++;

	/* need to advance the write pointer here */
	advance_write_pointer(conv_ftl, GC_IO);

	if (cpp->enable_gc_delay) {
		struct nand_cmd gcw = {
			.type = GC_IO,
			.cmd = NAND_NOP,
			.stime = 0,
			.interleave_pci_dma = false,
			.ppa = &new_ppa,
		};
		if (last_pg_in_wordline(conv_ftl, &new_ppa)) {
			gcw.cmd = NAND_WRITE;
			gcw.xfer_size = spp->pgsz * spp->pgs_per_oneshotpg;
		}

		ssd_advance_nand(conv_ftl->ssd, &gcw);
	}
	

	/* advance per-ch gc_endtime as well */
#if 0
	new_ch = get_ch(conv_ftl, &new_ppa);
	new_ch->gc_endtime = new_ch->next_ch_avail_time;

	new_lun = get_lun(conv_ftl, &new_ppa);
	new_lun->gc_endtime = new_lun->next_lun_avail_time;
#endif
	return 0;
}

#define US(a) (a * 1000 * 1000)
static inline int age_levelize(long long int age_us) {
	if (age_us < US(5)) {
		return 1;
	} else if (age_us < US(10)) {
		return 5;
	} else if (age_us < US(25)) {
		return 10;
	} else if (age_us < US(50)) {
		return 20;
	} else if (age_us < US(100)) {
		return 40;
	} else if (age_us < US(200)) {
		return 80;
	} else {
		return 250;
	}
}

#define SCALER 1000000000000LL // 1조
static inline long long int calculate_cost_benefit_val(struct ssdparams *spp, const struct line *cur_line) {
	long long int age_us = (ktime_get_ns() - cur_line->mtime) / 1000; // us
	int age;
	age = age_levelize(age_us);
	int vpc = cur_line->vpc;
	int ppl = spp->pgs_per_line;
	if (vpc == ppl) {
		return SCALER * 10;
	}
	return vpc * SCALER / ((ppl - vpc) * age);
}
#define CB
static struct line *select_victim_line(struct conv_ftl *conv_ftl, bool force)
{
	struct ssdparams *spp = &conv_ftl->ssd->sp;
	struct line_mgmt *lm;
	struct line *victim_line = NULL;
	int cur_pgs_per_line;

	lm = &conv_ftl->lm;
	cur_pgs_per_line = spp->pgs_per_line;
#if defined(CB) // cost-benefit with optimization using pqueue's array instead of looking-around all lines
	int min_idx = -1;
	long long int cb_min = __LONG_LONG_MAX__, cur_cbval;
	for (int i = 1; i < lm->victim_line_pq->size; i++) { // 1-based
		struct line *cur_line = (struct line *) (lm->victim_line_pq->d)[i];
		if (cur_line->ipc == 0)
			continue;
		if ((cur_cbval = calculate_cost_benefit_val(spp, cur_line)) < cb_min) {
			cb_min = cur_cbval;
			min_idx = i;
		}
	}
	if (min_idx == -1)
		return NULL;
	victim_line = (struct line *) (lm->victim_line_pq->d)[min_idx];
#elif defined(RD) // random
	int q_size = lm->victim_line_pq->size - 1;
	int rand_idx = get_random_u32() % q_size + 1;
	victim_line = (struct line *)lm->victim_line_pq->d[rand_idx];
#else // default, greedy
	victim_line = pqueue_peek(lm->victim_line_pq);
#endif
	if (!victim_line) {
		return NULL;
	}
#if defined(RD) || defined(CB)
#else
	if (!force && (victim_line->vpc > (spp->pgs_per_line / 8))) {
		// force는 강제로 시키는거같고
		// valid page 수가 (라인(슈퍼블럭) 당 페이지) / 8 보다 크면? -> 비효율적이니까 강제 아니면 놔둬라
		return NULL;
	}
#endif
#if defined(CB) || defined(RD)
	pqueue_remove(lm->victim_line_pq, victim_line);
#else
	pqueue_pop(lm->victim_line_pq);
#endif
	victim_line->pos = 0;
	lm->victim_line_cnt--;
	uint64_t age = (ktime_get_ns() - victim_line->mtime) / 1000000000;
	NVMEV_INFO("Age: %llu, VPC/IPC: %d/%d", age, victim_line->vpc, victim_line->ipc);
	if (age < 200)
		conv_ftl->age_cnt[age]++;
	else
		conv_ftl->age_cnt[200]++;

	/* victim_line is a danggling node now */
	return victim_line;
}

static struct line *select_victim_line_slc(struct conv_ftl *conv_ftl, bool force)
{
	struct ssdparams *spp = &conv_ftl->ssd->sp;
	struct line_mgmt *slm = &conv_ftl->slm;
	struct line *victim_line = NULL;
#if defined(CB2) // cost-benefit with optimization using pqueue's array instead of looking-around all lines
	int min_idx = -1;
	long long int cb_min = __LONG_LONG_MAX__, cur_cbval;
	for (int i = 1; i < slm->victim_line_pq->size; i++) {
		struct line *cur_line = (struct line *) (slm->victim_line_pq->d)[i];
		if (cur_line->ipc == 0)
			continue;
		if ((cur_cbval = calculate_cost_benefit_val(spp, cur_line)) < cb_min) {
			cb_min = cur_cbval;
			min_idx = i;
		}
	}
	if (min_idx == -1)
		return NULL;
	victim_line = (struct line *) (slm->victim_line_pq->d)[min_idx];
#elif defined(RD2) // random
	int q_size = slm->victim_line_pq->size - 1;
	int rand_idx = get_random_u32() % q_size + 1;
	victim_line = (struct line *)slm->victim_line_pq->d[rand_idx];
#else // default, greedy
	victim_line = pqueue_peek(slm->victim_line_pq);
#endif
	if (!victim_line) {
		victim_line = list_first_entry_or_null(&slm->full_line_list, struct line, entry);
		if (!victim_line)
			return NULL;
		list_del_init(&victim_line->entry);
		slm->full_line_cnt--;
		return victim_line;
	}
#if defined(RD2)
#else
	if (!force && (victim_line->vpc > (spp->pgs_per_line_slc / 8))) {
		// force는 강제로 시키는거같고
		// valid page 수가 (라인(슈퍼블럭) 당 페이지) / 8 보다 크면? -> 비효율적이니까 강제 아니면 놔둬라
		return NULL;
	}
#endif
#if defined(RD2) || defined(CB2)
	pqueue_remove(slm->victim_line_pq, victim_line);
#else
	pqueue_pop(slm->victim_line_pq);
#endif
	victim_line->pos = 0;
	slm->victim_line_cnt--;

	/* victim_line is a danggling node now */
	return victim_line;
}

/* here ppa identifies the block we want to clean */
/* duplicated, we use only clean_one_flashpg */
static void clean_one_block(struct conv_ftl *conv_ftl, struct ppa *ppa)
{
	struct ssdparams *spp = &conv_ftl->ssd->sp;
	struct nand_page *pg_iter = NULL;
	int cnt = 0;
	int pg;

	for (pg = 0; pg < spp->pgs_per_blk; pg++) {
		ppa->g.pg = pg;
		pg_iter = get_pg(conv_ftl->ssd, ppa);
		/* there shouldn't be any free page in victim blocks */
		NVMEV_ASSERT(pg_iter->status != PG_FREE);
		if (pg_iter->status == PG_VALID) {
			gc_read_page(conv_ftl, ppa);
			/* delay the maptbl update until "write" happens */
			NVMEV_INFO("call1");
			gc_write_page(conv_ftl, ppa);
			cnt++;
		}
	}

	NVMEV_ASSERT(get_blk(conv_ftl->ssd, ppa)->vpc == cnt);
}

/* here ppa identifies the block we want to clean */
static void clean_one_flashpg(struct conv_ftl *conv_ftl, struct ppa *ppa)
{
	struct ssdparams *spp = &conv_ftl->ssd->sp;
	struct convparams *cpp = &conv_ftl->cp;
	struct nand_page *pg_iter = NULL;
	int cnt = 0, i = 0;
	uint64_t completed_time = 0;
	struct ppa ppa_copy = *ppa;
	int cur_pgs_per_flashpg = spp->pgs_per_flashpg;
	int io_type = GC_IO;
	if (conv_ftl->slc_mode == SLC_MODE && conv_ftl->dyn_slc_mode == SLC_MODE) {
		NVMEV_ASSERT(ppa->g.blk < spp->tt_lines_slc);
		io_type = USER_IO;
	}
	
	for (i = 0; i < cur_pgs_per_flashpg; i++) {
		pg_iter = get_pg(conv_ftl->ssd, &ppa_copy);
		/* there shouldn't be any free page in victim blocks */
		NVMEV_ASSERT(pg_iter->status != PG_FREE);
		if (pg_iter->status == PG_VALID) {
			cnt++; // counting valid pages
		}
		ppa_copy.g.pg++;
	}

	ppa_copy = *ppa;

	if (cnt <= 0)
		return;

	if (cpp->enable_gc_delay) {
		struct nand_cmd gcr = {
			.type = io_type,
			.cmd = NAND_READ,
			.stime = 0,
			.xfer_size = spp->pgsz * cnt,
			.interleave_pci_dma = false,
			.ppa = &ppa_copy,
		};
		completed_time = ssd_advance_nand(conv_ftl->ssd, &gcr);
	}

	for (i = 0; i < cur_pgs_per_flashpg; i++) {
		pg_iter = get_pg(conv_ftl->ssd, &ppa_copy);

		/* there shouldn't be any free page in victim blocks */
		if (pg_iter->status == PG_VALID) {
			/* delay the maptbl update until "write" happens */
			// 여기서 지금 오류가 나고 있어
			gc_write_page(conv_ftl, &ppa_copy); // ppa_copy는 기존 ppa 주소
		}

		ppa_copy.g.pg++;
	}
}

static void mark_line_free(struct conv_ftl *conv_ftl, struct ppa *ppa)
{
	struct line_mgmt *lm;
	struct line *line = get_line(conv_ftl, ppa);

	if (conv_ftl->slc_mode == SLC_MODE) {
		if (ppa->g.blk < conv_ftl->ssd->sp.tt_lines_slc) {
			lm = &conv_ftl->slm;
		} else {
			lm = &conv_ftl->lm;
		}
	} else {
		lm = &conv_ftl->lm;
	}

	line->ipc = 0;
	line->vpc = 0;
	/* move this line to free line list */
	list_add_tail(&line->entry, &lm->free_line_list);
	lm->free_line_cnt++;
}

static int do_gc(struct conv_ftl *conv_ftl, bool force)
{
	struct line *victim_line = NULL;
	struct ssdparams *spp = &conv_ftl->ssd->sp;
	struct ppa ppa;
	int flashpg;
	int cur_flashpgs_per_blk;
	int io_type;

	if (conv_ftl->dyn_slc_mode == SLC_MODE) {
		victim_line = select_victim_line_slc(conv_ftl, force);
		cur_flashpgs_per_blk = spp->flashpgs_per_blk_slc;
		io_type = USER_IO;
	} else {
		victim_line = select_victim_line(conv_ftl, force);
		cur_flashpgs_per_blk = spp->flashpgs_per_blk;
		io_type = GC_IO;
		conv_ftl->gc_cnt++;
	}
	

	if (!victim_line) {
		return -1;
	}

	ppa.g.blk = victim_line->id;
	NVMEV_DEBUG_VERBOSE("GC-ing line:%d,ipc=%d(%d),victim=%d,full=%d,free=%d\n", ppa.g.blk,
		    victim_line->ipc, victim_line->vpc, conv_ftl->lm.victim_line_cnt,
		    conv_ftl->lm.full_line_cnt, conv_ftl->lm.free_line_cnt);

	if (conv_ftl->dyn_slc_mode == SLC_MODE)
		conv_ftl->wfc.credits_to_refill = spp->pgs_per_line_slc;
	else
		conv_ftl->wfc.credits_to_refill = victim_line->ipc;

	/* copy back valid data */
	for (flashpg = 0; flashpg < cur_flashpgs_per_blk; flashpg++) {
		int ch, lun;
		ppa.g.pg = flashpg * spp->pgs_per_flashpg;
		for (ch = 0; ch < spp->nchs; ch++) {
			for (lun = 0; lun < spp->luns_per_ch; lun++) {
				struct nand_lun *lunp;

				ppa.g.ch = ch;
				ppa.g.lun = lun;
				ppa.g.pl = 0;
				lunp = get_lun(conv_ftl->ssd, &ppa);
				clean_one_flashpg(conv_ftl, &ppa);

				if (flashpg == (cur_flashpgs_per_blk - 1)) {
					struct convparams *cpp = &conv_ftl->cp;

					mark_block_free(conv_ftl, &ppa);

					/*
						GC_IO, USER_IO 나눠보내야 하는가에 대한 간단한 고찰
						여기서 내리려는 명령은 erase 명령이다
						어디에 대한?
						-> 모든 블럭에 대해서..
						즉 victim line 내 모든 블럭 각각에 대한 erase 명령이다
						이 말은 erase 명령이 어느 영역에 이루어질지 모른다는 소리다
						따라서 io_type을 구분할 필요가 있다
						근데 어짜피 ppa로 구분되는거 아닌가?
						굳이인 것 같긴 하기도..
					*/
					if (cpp->enable_gc_delay) {
						struct nand_cmd gce = {
							.type = io_type,
							.cmd = NAND_ERASE,
							.stime = 0,
							.interleave_pci_dma = false,
							.ppa = &ppa,
						};
						ssd_advance_nand(conv_ftl->ssd, &gce);
					}

					lunp->gc_endtime = lunp->next_lun_avail_time;
				}
			}
		}
	}

	/* update line status */
	mark_line_free(conv_ftl, &ppa);

	return 0;
}

static void foreground_gc(struct conv_ftl *conv_ftl)
{
	if (should_migration_high(conv_ftl)) {
		/* perform GC here until !should_gc(conv_ftl) */
		/* there must free line in TLC before migration */
		if (should_gc_high(conv_ftl)) {
			NVMEV_INFO("GC occurred");
			conv_ftl->dyn_slc_mode = TLC_MODE;
			do_gc(conv_ftl, true);
		}
		if (conv_ftl->slc_mode == SLC_MODE) {
			conv_ftl->dyn_slc_mode = SLC_MODE;
			NVMEV_INFO("Mig occurred");
			do_gc(conv_ftl, true);
		}
	}
}

static bool is_same_flash_page(struct conv_ftl *conv_ftl, struct ppa ppa1, struct ppa ppa2)
{
	struct ssdparams *spp = &conv_ftl->ssd->sp;
	uint32_t ppa1_page = ppa1.g.pg / spp->pgs_per_flashpg;
	uint32_t ppa2_page = ppa2.g.pg / spp->pgs_per_flashpg;

	return (ppa1.h.blk_in_ssd == ppa2.h.blk_in_ssd) && (ppa1_page == ppa2_page);
}

static bool conv_read(struct nvmev_ns *ns, struct nvmev_request *req, struct nvmev_result *ret)
{
	struct conv_ftl *conv_ftls = (struct conv_ftl *)ns->ftls;
	struct conv_ftl *conv_ftl = &conv_ftls[0];
	/* spp are shared by all instances*/
	struct ssdparams *spp = &conv_ftl->ssd->sp;

	struct nvme_command *cmd = req->cmd;
	uint64_t lba = cmd->rw.slba;
	uint64_t nr_lba = (cmd->rw.length + 1);
	uint64_t start_lpn = lba / spp->secs_per_pg;
	uint64_t end_lpn = (lba + nr_lba - 1) / spp->secs_per_pg;
	uint64_t lpn;
	uint64_t nsecs_start = req->nsecs_start;
	uint64_t nsecs_completed, nsecs_latest = nsecs_start;
	uint32_t xfer_size, i;
	uint32_t nr_parts = ns->nr_parts;

	struct ppa prev_ppa;
	/*
		srd에서 USER_IO와 GC_IO를 구분할 필요가 있는가에 대한 간단한 고찰
		srd로는 READ request를 보내게 된다
		어디를 읽을지는 ppa로 알게 되지
		ppa로 구분이 가능하기도 함
		근데 USER_IO, GC_IO로 구분을 미리 할 수 있는가? 하면
		ppa를 알게 된 시점에서 가능하지
		GC_IO라는게 뭐지?
		내가 srd를 GC_IO로 바꾸면 side effect가 없나?
		ssd_advance_nand 에서, READ할 때 io_type이 어떻게 쓰이는지 보자
		-> 아예 안쓰이는데?
		-> 굳이 바꾸지 말자
	*/
	struct nand_cmd srd = {
		.type = USER_IO,
		.cmd = NAND_READ,
		.stime = nsecs_start,
		.interleave_pci_dma = true,
	};

	NVMEV_ASSERT(conv_ftls);
	NVMEV_DEBUG_VERBOSE("%s: start_lpn=%lld, len=%lld, end_lpn=%lld", __func__, start_lpn, nr_lba, end_lpn);
	if ((end_lpn / nr_parts) >= spp->tt_pgs) {
		NVMEV_ERROR("%s: lpn passed FTL range (start_lpn=%lld > tt_pgs=%ld)\n", __func__,
			    end_lpn / nr_parts, spp->tt_pgs);
		return false;
	}

	if (LBA_TO_BYTE(nr_lba) <= (KB(4) * nr_parts)) {
		srd.stime += spp->fw_4kb_rd_lat;
	} else {
		srd.stime += spp->fw_rd_lat;
	}

	for (i = 0; (i < nr_parts) && (start_lpn <= end_lpn); i++, start_lpn++) {
		conv_ftl = &conv_ftls[start_lpn % nr_parts];
		xfer_size = 0;
		prev_ppa = get_maptbl_ent(conv_ftl, start_lpn / nr_parts);

		/* normal IO read path */
		// wei edit : read하는 영역이 SLC 영역인지, TLC 영역인지 구분해서 시간 계산해야 함
		// -> 이건 ssd.c에서 할 일, 여기선 ppa만 srd에 숨겨서 넘겨주고 ssd_advance_nand에서 판단
		// page 단위기 때문에 읽는 행위 자체는 바뀌는 것이 없음
		for (lpn = start_lpn; lpn <= end_lpn; lpn += nr_parts) {
			uint64_t local_lpn;
			struct ppa cur_ppa;

			local_lpn = lpn / nr_parts;
			cur_ppa = get_maptbl_ent(conv_ftl, local_lpn);
			if (!mapped_ppa(&cur_ppa) || !valid_ppa(conv_ftl, &cur_ppa)) {
				NVMEV_DEBUG_VERBOSE("lpn 0x%llx not mapped to valid ppa\n", local_lpn);
				NVMEV_DEBUG_VERBOSE("Invalid ppa,ch:%d,lun:%d,blk:%d,pl:%d,pg:%d\n",
					    cur_ppa.g.ch, cur_ppa.g.lun, cur_ppa.g.blk,
					    cur_ppa.g.pl, cur_ppa.g.pg);
				continue;
			}

			// aggregate read io in same flash page
			if (mapped_ppa(&prev_ppa) &&
			    is_same_flash_page(conv_ftl, cur_ppa, prev_ppa)) {
				xfer_size += spp->pgsz;
				continue; // 다음 읽기에 짬 때림
			}

			if (xfer_size > 0) {
				srd.xfer_size = xfer_size;
				srd.ppa = &prev_ppa; // 읽으려는 ppa가 여기서 정해짐
				nsecs_completed = ssd_advance_nand(conv_ftl->ssd, &srd);
				nsecs_latest = max(nsecs_completed, nsecs_latest);
			}

			xfer_size = spp->pgsz;
			prev_ppa = cur_ppa;
		}

		// issue remaining io
		if (xfer_size > 0) {
			srd.xfer_size = xfer_size;
			srd.ppa = &prev_ppa; // 읽으려는 ppa가 여기서 정해짐2
			nsecs_completed = ssd_advance_nand(conv_ftl->ssd, &srd);
			nsecs_latest = max(nsecs_completed, nsecs_latest); // 결국 가장 늦는 놈 기준
		}
	}

	ret->nsecs_target = nsecs_latest;
	ret->status = NVME_SC_SUCCESS;
	return true;
}

static bool conv_write(struct nvmev_ns *ns, struct nvmev_request *req, struct nvmev_result *ret)
{
	struct conv_ftl *conv_ftls = (struct conv_ftl *)ns->ftls;
	struct conv_ftl *conv_ftl = &conv_ftls[0];

	/* wbuf and spp are shared by all instances */
	struct ssdparams *spp = &conv_ftl->ssd->sp;
	struct buffer *wbuf = conv_ftl->ssd->write_buffer; // GLOBAL_WB_SIZE를 갖는, oneshot-page * 칩 개수만큼 모아서 한 번에 뿌려주는 버퍼

	struct nvme_command *cmd = req->cmd;
	uint64_t lba = cmd->rw.slba;
	uint64_t nr_lba = (cmd->rw.length + 1);
	uint64_t start_lpn = lba / spp->secs_per_pg;
	uint64_t end_lpn = (lba + nr_lba - 1) / spp->secs_per_pg;

	uint64_t lpn;
	uint32_t nr_parts = ns->nr_parts;

	uint64_t nsecs_latest;
	uint64_t nsecs_xfer_completed;
	uint32_t allocated_buf_size;

	/*
		TODO:
		xfer_size는 당연히 달라질거고
		USER_IO, GC_IO 달라져야 하는지 판단해봐야 함
		판단은 ppa로 하겠지?
	*/
	struct nand_cmd swr = { // 유저 요청으로, write를 하고, DMA(Direct Memory Access)는 안하며, 원샷 페이지만큼 전송하라는 커맨드
		.type = USER_IO,
		.cmd = NAND_WRITE,
		.interleave_pci_dma = false,
		.xfer_size = spp->pgsz * spp->pgs_per_oneshotpg,
	};

	NVMEV_DEBUG_VERBOSE("%s: start_lpn=%lld, len=%lld, end_lpn=%lld", __func__, start_lpn, nr_lba, end_lpn);
	if ((end_lpn / nr_parts) >= spp->tt_pgs) {
		NVMEV_ERROR("%s: lpn passed FTL range (start_lpn=%lld > tt_pgs=%ld)\n",
				__func__, start_lpn, spp->tt_pgs);
		return false;
	}

	allocated_buf_size = buffer_allocate(wbuf, LBA_TO_BYTE(nr_lba)); // 할당된 버퍼 사이즈를 저장. 실제로 버퍼가 할당되냐?..X
	if (allocated_buf_size < LBA_TO_BYTE(nr_lba)) 
		return false;

	nsecs_latest =
		ssd_advance_write_buffer(conv_ftl->ssd, req->nsecs_start, LBA_TO_BYTE(nr_lba)); // 시간 계산 함수, 실제 복사 행위는 일어나지 않음
	nsecs_xfer_completed = nsecs_latest;

	swr.stime = nsecs_latest;

	for (lpn = start_lpn; lpn <= end_lpn; lpn++) { // write할 페이지 수만큼, 각 페이지에 대해 반복
		uint64_t local_lpn;
		uint64_t nsecs_completed = 0;
		struct ppa ppa;

		conv_ftl = &conv_ftls[lpn % nr_parts];
		local_lpn = lpn / nr_parts;
		ppa = get_maptbl_ent(
			conv_ftl, local_lpn); // Check whether the given LPN has been written before
		if (mapped_ppa(&ppa)) {
			/* update old page information first */
			mark_page_invalid(conv_ftl, &ppa);
			set_rmap_ent(conv_ftl, INVALID_LPN, &ppa);
			NVMEV_DEBUG("%s: %lld is invalid, ", __func__, ppa2pgidx(conv_ftl, &ppa));
			/* update mtime */
			struct line *updated_line = get_line(conv_ftl, &ppa);
			updated_line->mtime = ktime_get_ns();
		}

		/* new write */
		ppa = get_new_page(conv_ftl, USER_IO);
		/* update maptbl */
		set_maptbl_ent(conv_ftl, local_lpn, &ppa);
		NVMEV_DEBUG("%s: got new ppa %lld, ", __func__, ppa2pgidx(conv_ftl, &ppa));
		/* update rmap */
		set_rmap_ent(conv_ftl, local_lpn, &ppa);

		mark_page_valid(conv_ftl, &ppa);

		/* need to advance the write pointer here */
		

		/* Aggregate write io in flash page */
		if (conv_ftl->slc_mode == SLC_MODE) {
			advance_write_pointer(conv_ftl, USER_IO);
			if (last_pg_in_wordline_slc(conv_ftl, &ppa)) { // 한 라인을 다 모았니?
				swr.ppa = &ppa;

				nsecs_completed = ssd_advance_nand(conv_ftl->ssd, &swr); 
				nsecs_latest = max(nsecs_completed, nsecs_latest);

				schedule_internal_operation(req->sq_id, nsecs_completed, wbuf,
								spp->pgs_per_oneshotpg_slc * spp->pgsz); // 모아둔 라인에 대한 I/O 요청을 SQ에 넣음(나중에 스케쥴링되어 실행될 예정)
			}
		} else {
			advance_write_pointer(conv_ftl, USER_IO);
			if (last_pg_in_wordline(conv_ftl, &ppa)) { // 한 라인을 다 모았니?
				swr.ppa = &ppa;

				nsecs_completed = ssd_advance_nand(conv_ftl->ssd, &swr); 
				nsecs_latest = max(nsecs_completed, nsecs_latest);

				schedule_internal_operation(req->sq_id, nsecs_completed, wbuf,
								spp->pgs_per_oneshotpg * spp->pgsz); // 모아둔 라인에 대한 I/O 요청을 SQ에 넣음(나중에 스케쥴링되어 실행될 예정)
			}
		}

		consume_write_credit(conv_ftl); // write pointer에 page를 하나 썼다고 생각하고, 라인 내 작성 가능한 page 카운트를 1 감소시킴
		check_and_refill_write_credit(conv_ftl); // 그 page 카운트가 0이 됐는지 확인하고 true라면 새로운 free line을 할당받고 write_credit을 충전함
	}

	if ((cmd->rw.control & NVME_RW_FUA) || (spp->write_early_completion == 0)) {
		/* Wait all flash operations */
		ret->nsecs_target = nsecs_latest;
	} else {
		/* Early completion */
		ret->nsecs_target = nsecs_xfer_completed;
	}
	ret->status = NVME_SC_SUCCESS;

	return true;
}

static void conv_flush(struct nvmev_ns *ns, struct nvmev_request *req, struct nvmev_result *ret)
{
	uint64_t start, latest;
	uint32_t i;
	struct conv_ftl *conv_ftls = (struct conv_ftl *)ns->ftls;

	start = local_clock();
	latest = start;
	for (i = 0; i < ns->nr_parts; i++) {
		latest = max(latest, ssd_next_idle_time(conv_ftls[i].ssd));
	}

	NVMEV_DEBUG_VERBOSE("%s: latency=%llu\n", __func__, latest - start);
	uint64_t gc_cnts = 0, copy_cnts = 0;
	uint32_t age_cnts[201];
	for (i = 0; i < ns->nr_parts; i++) {
		gc_cnts += conv_ftls[i].gc_cnt;
		copy_cnts += conv_ftls[i].copy_cnt;
		for (int j = 0; j < 201; j++) {
			age_cnts[j] += conv_ftls[i].age_cnt[j];
		}
	}
	NVMEV_INFO("total gc: %llu, total copy: %llu", gc_cnts, copy_cnts);
	for (i = 0; i < 201; i++) {
		if (age_cnts[i] > 0) {
			NVMEV_INFO("Age %d's copy: %d", i, age_cnts[i]);
		}
	}
	ret->status = NVME_SC_SUCCESS;
	ret->nsecs_target = latest;
	return;
}

bool conv_proc_nvme_io_cmd(struct nvmev_ns *ns, struct nvmev_request *req, struct nvmev_result *ret)
{
	struct nvme_command *cmd = req->cmd;

	NVMEV_ASSERT(ns->csi == NVME_CSI_NVM);

	switch (cmd->common.opcode) {
	case nvme_cmd_write:
		if (!conv_write(ns, req, ret))
			return false;
		break;
	case nvme_cmd_read:
		if (!conv_read(ns, req, ret))
			return false;
		break;
	case nvme_cmd_flush:
		conv_flush(ns, req, ret);
		break;
	default:
		NVMEV_ERROR("%s: command not implemented: %s (0x%x)\n", __func__,
				nvme_opcode_string(cmd->common.opcode), cmd->common.opcode);
		break;
	}

	return true;
}
