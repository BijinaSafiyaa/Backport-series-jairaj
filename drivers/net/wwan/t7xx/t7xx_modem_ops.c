// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2021, MediaTek Inc.
 * Copyright (c) 2021, Intel Corporation.
 *
 * Authors:
 *  Haijun Liu <haijun.liu@mediatek.com>
 *  Eliot Lee <eliot.lee@intel.com>
 *  Moises Veleta <moises.veleta@intel.com>
 *  Ricardo Martinez<ricardo.martinez@linux.intel.com>
 *
 * Contributors:
 *  Amir Hanania <amir.hanania@intel.com>
 *  Chiranjeevi Rapolu <chiranjeevi.rapolu@intel.com>
 *  Sreehari Kancharla <sreehari.kancharla@intel.com>
 */

#include <linux/acpi.h>
#include <linux/dev_printk.h>
#include <linux/device.h>
#include <linux/delay.h>
#include <linux/gfp.h>
#include <linux/io.h>
#include <linux/irqreturn.h>
#include <linux/kthread.h>
#include <linux/skbuff.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/types.h>
#include <linux/workqueue.h>

#include "t7xx_hif_cldma.h"
#include "t7xx_mhccif.h"
#include "t7xx_modem_ops.h"
#include "t7xx_pci.h"
#include "t7xx_pcie_mac.h"
#include "t7xx_reg.h"
#include "t7xx_state_monitor.h"

#define RGU_RESET_DELAY_MS	10
#define PORT_RESET_DELAY_MS	2000
#define EX_HS_TIMEOUT_MS	5000
#define EX_HS_POLL_DELAY_MS	10

static inline unsigned int t7xx_get_interrupt_status(struct t7xx_pci_dev *t7xx_dev)
{
	return t7xx_mhccif_read_sw_int_sts(t7xx_dev) & D2H_SW_INT_MASK;
}

/**
 * t7xx_pci_mhccif_isr() - Process MHCCIF interrupts.
 * @t7xx_dev: MTK device.
 *
 * Check the interrupt status and queue commands accordingly.
 *
 * Returns:
 ** 0		- Success.
 ** -EINVAL	- Failure to get FSM control.
 */
int t7xx_pci_mhccif_isr(struct t7xx_pci_dev *t7xx_dev)
{
	struct t7xx_modem *md = t7xx_dev->md;
	struct t7xx_fsm_ctl *ctl;
	unsigned int int_sta;
	u32 mask;

	ctl = md->fsm_ctl;
	if (!ctl) {
		dev_err_ratelimited(&t7xx_dev->pdev->dev,
				    "MHCCIF interrupt received before initializing MD monitor\n");
		return -EINVAL;
	}

	spin_lock_bh(&md->exp_lock);
	int_sta = t7xx_get_interrupt_status(t7xx_dev);

	md->exp_id |= int_sta;
	if (md->exp_id & D2H_INT_PORT_ENUM) {
		md->exp_id &= ~D2H_INT_PORT_ENUM;

		if (ctl->curr_state == FSM_STATE_INIT || ctl->curr_state == FSM_STATE_PRE_START ||
		    ctl->curr_state == FSM_STATE_STOPPED)
			t7xx_fsm_recv_md_intr(ctl, MD_IRQ_PORT_ENUM);
	}

	if (md->exp_id & D2H_INT_EXCEPTION_INIT) {
		if (ctl->md_state == MD_STATE_INVALID ||
		    ctl->md_state == MD_STATE_WAITING_FOR_HS1 ||
		    ctl->md_state == MD_STATE_WAITING_FOR_HS2 ||
		    ctl->md_state == MD_STATE_READY) {
			md->exp_id &= ~D2H_INT_EXCEPTION_INIT;
			t7xx_fsm_recv_md_intr(ctl, MD_IRQ_CCIF_EX);
		}
	} else if (ctl->md_state == MD_STATE_WAITING_FOR_HS1) {
		mask = t7xx_mhccif_mask_get(t7xx_dev);
		if ((md->exp_id & D2H_INT_ASYNC_MD_HK) && !(mask & D2H_INT_ASYNC_MD_HK)) {
			md->exp_id &= ~D2H_INT_ASYNC_MD_HK;
			queue_work(md->handshake_wq, &md->handshake_work);
		}
	}

	spin_unlock_bh(&md->exp_lock);

	return 0;
}

static void t7xx_clr_device_irq_via_pcie(struct t7xx_pci_dev *t7xx_dev)
{
	struct t7xx_addr_base *pbase_addr = &t7xx_dev->base_addr;
	void __iomem *reset_pcie_reg;
	u32 val;

	reset_pcie_reg = pbase_addr->pcie_ext_reg_base + TOPRGU_CH_PCIE_IRQ_STA -
			  pbase_addr->pcie_dev_reg_trsl_addr;
	val = ioread32(reset_pcie_reg);
	iowrite32(val, reset_pcie_reg);
}

void t7xx_clear_rgu_irq(struct t7xx_pci_dev *t7xx_dev)
{
	/* Clear L2 */
	t7xx_clr_device_irq_via_pcie(t7xx_dev);
	/* Clear L1 */
	t7xx_pcie_mac_clear_int_status(t7xx_dev, SAP_RGU_INT);
}

static int t7xx_acpi_reset(struct t7xx_pci_dev *t7xx_dev, char *fn_name)
{
#ifdef CONFIG_ACPI
	struct acpi_buffer buffer = { ACPI_ALLOCATE_BUFFER, NULL };
	struct device *dev = &t7xx_dev->pdev->dev;
	acpi_status acpi_ret;
	acpi_handle handle;

	handle = ACPI_HANDLE(dev);
	if (!handle) {
		dev_err(dev, "ACPI handle not found\n");
		return -EFAULT;
	}

	if (!acpi_has_method(handle, fn_name)) {
		dev_err(dev, "%s method not found\n", fn_name);
		return -EFAULT;
	}

	acpi_ret = acpi_evaluate_object(handle, fn_name, NULL, &buffer);
	if (ACPI_FAILURE(acpi_ret)) {
		dev_err(dev, "%s method fail: %s\n", fn_name, acpi_format_exception(acpi_ret));
		return -EFAULT;
	}

#endif
	return 0;
}

int t7xx_acpi_fldr_func(struct t7xx_pci_dev *t7xx_dev)
{
	return t7xx_acpi_reset(t7xx_dev, "_RST");
}

static void t7xx_reset_device_via_pmic(struct t7xx_pci_dev *t7xx_dev)
{
	u32 val;

	val = ioread32(IREG_BASE(t7xx_dev) + PCIE_MISC_DEV_STATUS);
	if (val & MISC_RESET_TYPE_PLDR)
		t7xx_acpi_reset(t7xx_dev, "MRST._RST");
	else if (val & MISC_RESET_TYPE_FLDR)
		t7xx_acpi_fldr_func(t7xx_dev);
}

static irqreturn_t t7xx_rgu_isr_thread(int irq, void *data)
{
	struct t7xx_pci_dev *t7xx_dev = data;

	msleep(RGU_RESET_DELAY_MS);
	t7xx_reset_device_via_pmic(t7xx_dev);
	return IRQ_HANDLED;
}

static irqreturn_t t7xx_rgu_isr_handler(int irq, void *data)
{
	struct t7xx_pci_dev *t7xx_dev = data;
	struct t7xx_modem *modem;

	t7xx_clear_rgu_irq(t7xx_dev);
	if (!t7xx_dev->rgu_pci_irq_en)
		return IRQ_HANDLED;

	modem = t7xx_dev->md;
	modem->rgu_irq_asserted = true;
	t7xx_pcie_mac_clear_int(t7xx_dev, SAP_RGU_INT);
	return IRQ_WAKE_THREAD;
}

static void t7xx_pcie_register_rgu_isr(struct t7xx_pci_dev *t7xx_dev)
{
	/* Registers RGU callback ISR with PCIe driver */
	t7xx_pcie_mac_clear_int(t7xx_dev, SAP_RGU_INT);
	t7xx_pcie_mac_clear_int_status(t7xx_dev, SAP_RGU_INT);

	t7xx_dev->intr_handler[SAP_RGU_INT] = t7xx_rgu_isr_handler;
	t7xx_dev->intr_thread[SAP_RGU_INT] = t7xx_rgu_isr_thread;
	t7xx_dev->callback_param[SAP_RGU_INT] = t7xx_dev;
	t7xx_pcie_mac_set_int(t7xx_dev, SAP_RGU_INT);
}

static void t7xx_md_exception(struct t7xx_modem *md, enum hif_ex_stage stage)
{
	struct t7xx_pci_dev *t7xx_dev = md->t7xx_dev;

	if (stage == HIF_EX_CLEARQ_DONE) {
		/* Give DHL time to flush data */
		msleep(PORT_RESET_DELAY_MS);
	}

	t7xx_cldma_exception(md->md_ctrl[ID_CLDMA1], stage);

	if (stage == HIF_EX_INIT)
		t7xx_mhccif_h2d_swint_trigger(t7xx_dev, H2D_CH_EXCEPTION_ACK);
	else if (stage == HIF_EX_CLEARQ_DONE)
		t7xx_mhccif_h2d_swint_trigger(t7xx_dev, H2D_CH_EXCEPTION_CLEARQ_ACK);
}

static int t7xx_wait_hif_ex_hk_event(struct t7xx_modem *md, int event_id)
{
	unsigned int waited_time_ms = 0;

	do {
		if (md->exp_id & event_id)
			return 0;

		waited_time_ms += EX_HS_POLL_DELAY_MS;
		msleep(EX_HS_POLL_DELAY_MS);
	} while (waited_time_ms < EX_HS_TIMEOUT_MS);

	return -EFAULT;
}

static void t7xx_md_sys_sw_init(struct t7xx_pci_dev *t7xx_dev)
{
	/* Register the MHCCIF ISR for MD exception, port enum and
	 * async handshake notifications.
	 */
	t7xx_mhccif_mask_set(t7xx_dev, D2H_SW_INT_MASK);
	t7xx_dev->mhccif_bitmask = D2H_SW_INT_MASK;
	t7xx_mhccif_mask_clr(t7xx_dev, D2H_INT_PORT_ENUM);

	/* Register RGU IRQ handler for sAP exception notification */
	t7xx_dev->rgu_pci_irq_en = true;
	t7xx_pcie_register_rgu_isr(t7xx_dev);
}

static void t7xx_md_hk_wq(struct work_struct *work)
{
	struct t7xx_modem *md = container_of(work, struct t7xx_modem, handshake_work);
	struct t7xx_fsm_ctl *ctl = md->fsm_ctl;

	t7xx_cldma_switch_cfg(md->md_ctrl[ID_CLDMA1]);
	t7xx_cldma_start(md->md_ctrl[ID_CLDMA1]);
	t7xx_fsm_broadcast_state(ctl, MD_STATE_WAITING_FOR_HS2);
	md->core_md.ready = true;
	wake_up(&ctl->async_hk_wq);
}

void t7xx_md_event_notify(struct t7xx_modem *md, enum md_event_id evt_id)
{
	struct t7xx_fsm_ctl *ctl = md->fsm_ctl;
	void __iomem *mhccif_base;
	unsigned int int_sta;
	unsigned long flags;

	switch (evt_id) {
	case FSM_PRE_START:
		t7xx_mhccif_mask_clr(md->t7xx_dev, D2H_INT_PORT_ENUM);
		break;

	case FSM_START:
		t7xx_mhccif_mask_set(md->t7xx_dev, D2H_INT_PORT_ENUM);
		spin_lock_irqsave(&md->exp_lock, flags);
		int_sta = t7xx_get_interrupt_status(md->t7xx_dev);

		md->exp_id |= int_sta;
		if (md->exp_id & D2H_INT_EXCEPTION_INIT) {
			ctl->exp_flg = true;
			md->exp_id &= ~D2H_INT_EXCEPTION_INIT;
			md->exp_id &= ~D2H_INT_ASYNC_MD_HK;
		} else if (ctl->exp_flg) {
			md->exp_id &= ~D2H_INT_ASYNC_MD_HK;
		} else if (md->exp_id & D2H_INT_ASYNC_MD_HK) {
			queue_work(md->handshake_wq, &md->handshake_work);
			md->exp_id &= ~D2H_INT_ASYNC_MD_HK;
			mhccif_base = md->t7xx_dev->base_addr.mhccif_rc_base;
			iowrite32(D2H_INT_ASYNC_MD_HK, mhccif_base + REG_EP2RC_SW_INT_ACK);
			t7xx_mhccif_mask_set(md->t7xx_dev, D2H_INT_ASYNC_MD_HK);
		} else {
			t7xx_mhccif_mask_clr(md->t7xx_dev, D2H_INT_ASYNC_MD_HK);
		}

		spin_unlock_irqrestore(&md->exp_lock, flags);

		t7xx_mhccif_mask_clr(md->t7xx_dev,
				     D2H_INT_EXCEPTION_INIT |
				     D2H_INT_EXCEPTION_INIT_DONE |
				     D2H_INT_EXCEPTION_CLEARQ_DONE |
				     D2H_INT_EXCEPTION_ALLQ_RESET);
		break;

	case FSM_READY:
		t7xx_mhccif_mask_set(md->t7xx_dev, D2H_INT_ASYNC_MD_HK);
		break;

	default:
		break;
	}
}

void t7xx_md_exception_handshake(struct t7xx_modem *md)
{
	struct device *dev = &md->t7xx_dev->pdev->dev;
	int ret;

	t7xx_md_exception(md, HIF_EX_INIT);
	ret = t7xx_wait_hif_ex_hk_event(md, D2H_INT_EXCEPTION_INIT_DONE);
	if (ret)
		dev_err(dev, "EX CCIF HS timeout, RCH 0x%lx\n", D2H_INT_EXCEPTION_INIT_DONE);

	t7xx_md_exception(md, HIF_EX_INIT_DONE);
	ret = t7xx_wait_hif_ex_hk_event(md, D2H_INT_EXCEPTION_CLEARQ_DONE);
	if (ret)
		dev_err(dev, "EX CCIF HS timeout, RCH 0x%lx\n", D2H_INT_EXCEPTION_CLEARQ_DONE);

	t7xx_md_exception(md, HIF_EX_CLEARQ_DONE);
	ret = t7xx_wait_hif_ex_hk_event(md, D2H_INT_EXCEPTION_ALLQ_RESET);
	if (ret)
		dev_err(dev, "EX CCIF HS timeout, RCH 0x%lx\n", D2H_INT_EXCEPTION_ALLQ_RESET);

	t7xx_md_exception(md, HIF_EX_ALLQ_RESET);
}

static struct t7xx_modem *t7xx_md_alloc(struct t7xx_pci_dev *t7xx_dev)
{
	struct device *dev = &t7xx_dev->pdev->dev;
	struct t7xx_modem *md;

	md = devm_kzalloc(dev, sizeof(*md), GFP_KERNEL);
	if (!md)
		return NULL;

	md->t7xx_dev = t7xx_dev;
	t7xx_dev->md = md;
	md->core_md.ready = false;
	spin_lock_init(&md->exp_lock);
	md->handshake_wq = alloc_workqueue("%s", WQ_UNBOUND | WQ_MEM_RECLAIM | WQ_HIGHPRI,
					   0, "md_hk_wq");
	if (!md->handshake_wq)
		return NULL;

	INIT_WORK(&md->handshake_work, t7xx_md_hk_wq);
	return md;
}

void t7xx_md_reset(struct t7xx_pci_dev *t7xx_dev)
{
	struct t7xx_modem *md = t7xx_dev->md;

	md->md_init_finish = false;
	md->exp_id = 0;
	spin_lock_init(&md->exp_lock);
	t7xx_fsm_reset(md);
	t7xx_cldma_reset(md->md_ctrl[ID_CLDMA1]);
	md->md_init_finish = true;
}

/**
 * t7xx_md_init() - Initialize modem.
 * @t7xx_dev: MTK device.
 *
 * Allocate and initialize MD control block, and initialize data path.
 * Register MHCCIF ISR and RGU ISR, and start the state machine.
 *
 * Return:
 ** 0		- Success.
 ** -ENOMEM	- Allocation failure.
 */
int t7xx_md_init(struct t7xx_pci_dev *t7xx_dev)
{
	struct t7xx_modem *md;
	int ret;

	md = t7xx_md_alloc(t7xx_dev);
	if (!md)
		return -ENOMEM;

	ret = t7xx_cldma_alloc(ID_CLDMA1, t7xx_dev);
	if (ret)
		goto err_destroy_hswq;

	ret = t7xx_fsm_init(md);
	if (ret)
		goto err_destroy_hswq;

	ret = t7xx_cldma_init(md, md->md_ctrl[ID_CLDMA1]);
	if (ret)
		goto err_uninit_fsm;

	t7xx_fsm_append_cmd(md->fsm_ctl, FSM_CMD_START, 0);
	t7xx_md_sys_sw_init(t7xx_dev);
	md->md_init_finish = true;
	return 0;

err_uninit_fsm:
	t7xx_fsm_uninit(md);

err_destroy_hswq:
	destroy_workqueue(md->handshake_wq);
	dev_err(&t7xx_dev->pdev->dev, "Modem init failed\n");
	return ret;
}

void t7xx_md_exit(struct t7xx_pci_dev *t7xx_dev)
{
	struct t7xx_modem *md = t7xx_dev->md;

	t7xx_pcie_mac_clear_int(t7xx_dev, SAP_RGU_INT);

	if (!md->md_init_finish)
		return;

	t7xx_fsm_append_cmd(md->fsm_ctl, FSM_CMD_PRE_STOP, FSM_CMD_FLAG_WAIT_FOR_COMPLETION);
	t7xx_cldma_exit(md->md_ctrl[ID_CLDMA1]);
	t7xx_fsm_uninit(md);
	destroy_workqueue(md->handshake_wq);
}
