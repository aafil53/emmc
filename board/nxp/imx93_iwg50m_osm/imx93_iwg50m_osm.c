// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2025 iWave Global Pvt Ltd.
 */

#include <env.h>
#include <efi_loader.h>
#include <init.h>
#include <asm/arch/sys_proto.h>
#include "../common/tcpc.h"
#include <dm/device.h>
#include <dm/uclass.h>
#include <usb.h>
#include <asm/gpio.h>
#include <i2c.h>

#include <adc.h>
#include <asm/bootm.h>
#include <asm/setup.h>
#include <asm/mach-imx/ele_api.h>

#if CONFIG_IS_ENABLED(EFI_HAVE_CAPSULE_SUPPORT)
#define IMX_BOOT_IMAGE_GUID \
	EFI_GUID(0xbc550d86, 0xda26, 0x4b70, 0xac, 0x05, \
		 0x2a, 0x44, 0x8e, 0xda, 0x6f, 0x21)

struct efi_fw_image fw_images[] = {
	{
		.image_type_id = IMX_BOOT_IMAGE_GUID,
		.fw_name = u"IMX93-IWG50M-OSM-RAW",
		.image_index = 1,
	},
};

struct efi_capsule_update_info update_info = {
	.dfu_string = "mmc 0=flash-bin raw 0 0x2000 mmcpart 1",
	.num_images = ARRAY_SIZE(fw_images),
	.images = fw_images,
};
#endif /* EFI_HAVE_CAPSULE_SUPPORT */

#ifdef CONFIG_USB_TCPC
struct tcpc_port port1;
struct tcpc_port port2;
struct tcpc_port portpd;

static int setup_pd_switch(uint8_t i2c_bus, uint8_t addr)
{
	struct udevice *bus;
	struct udevice *i2c_dev = NULL;
	int ret;
	uint8_t valb;

	ret = uclass_get_device_by_seq(UCLASS_I2C, i2c_bus, &bus);
	if (ret) {
		printf("%s: Can't find bus\n", __func__);
		return -EINVAL;
	}

	ret = dm_i2c_probe(bus, addr, 0, &i2c_dev);
	if (ret) {
		printf("%s: Can't find device id=0x%x\n",
			__func__, addr);
		return -ENODEV;
	}

	ret = dm_i2c_read(i2c_dev, 0xB, &valb, 1);
	if (ret) {
		printf("%s dm_i2c_read failed, err %d\n", __func__, ret);
		return -EIO;
	}
	valb |= 0x4; /* Set DB_EXIT to exit dead battery mode */
	ret = dm_i2c_write(i2c_dev, 0xB, (const uint8_t *)&valb, 1);
	if (ret) {
		printf("%s dm_i2c_write failed, err %d\n", __func__, ret);
		return -EIO;
	}

	/* Set OVP threshold to 23V */
	valb = 0x6;
	ret = dm_i2c_write(i2c_dev, 0x8, (const uint8_t *)&valb, 1);
	if (ret) {
		printf("%s dm_i2c_write failed, err %d\n", __func__, ret);
		return -EIO;
	}

	return 0;
}

int pd_switch_snk_enable(struct tcpc_port *port)
{
	if (port == &port1) {
		debug("Setup pd switch on port 1\n");
		return setup_pd_switch(2, 0x71);
	} else if (port == &port2) {
		debug("Setup pd switch on port 2\n");
		return setup_pd_switch(2, 0x73);
	} else
		return -EINVAL;
}

struct tcpc_port_config portpd_config = {
	.i2c_bus = 2, /*i2c3*/
	.addr = 0x52,
	.port_type = TYPEC_PORT_UFP,
	.max_snk_mv = 20000,
	.max_snk_ma = 3000,
	.max_snk_mw = 15000,
	.op_snk_mv = 9000,
};

struct tcpc_port_config port1_config = {
	.i2c_bus = 2, /*i2c3*/
	.addr = 0x50,
	.port_type = TYPEC_PORT_UFP,
	.max_snk_mv = 5000,
	.max_snk_ma = 3000,
	.max_snk_mw = 40000,
	.op_snk_mv = 9000,
	.switch_setup_func = &pd_switch_snk_enable,
	.disable_pd = true,
};

struct tcpc_port_config port2_config = {
	.i2c_bus = 2, /*i2c3*/
	.addr = 0x51,
	.port_type = TYPEC_PORT_UFP,
	.max_snk_mv = 9000,
	.max_snk_ma = 3000,
	.max_snk_mw = 40000,
	.op_snk_mv = 9000,
	.switch_setup_func = &pd_switch_snk_enable,
	.disable_pd = true,
};

static int setup_typec(void)
{
	int ret;

	debug("tcpc_init port pd\n");
	ret = tcpc_init(&portpd, portpd_config, NULL);
	if (ret) {
		printf("%s: tcpc portpd init failed, err=%d\n",
		       __func__, ret);
	}

	debug("tcpc_init port 2\n");
	ret = tcpc_init(&port2, port2_config, NULL);
	if (ret) {
		printf("%s: tcpc port2 init failed, err=%d\n",
		       __func__, ret);
	}

	debug("tcpc_init port 1\n");
	ret = tcpc_init(&port1, port1_config, NULL);
	if (ret) {
		printf("%s: tcpc port1 init failed, err=%d\n",
		       __func__, ret);
	}

	return ret;
}

int board_usb_init(int index, enum usb_init_type init)
{
	int ret = 0;
	struct tcpc_port *port_ptr;

	debug("board_usb_init %d, type %d\n", index, init);

	if (index == 0)
		port_ptr = &port1;
	else
		port_ptr = &port2;

	if (init == USB_INIT_HOST)
		ret = tcpc_setup_dfp_mode(port_ptr);
	else
		ret = tcpc_setup_ufp_mode(port_ptr);

	return ret;
}

int board_usb_cleanup(int index, enum usb_init_type init)
{
	int ret = 0;

	debug("board_usb_cleanup %d, type %d\n", index, init);

	if (init == USB_INIT_HOST) {
		if (index == 0)
			ret = tcpc_disable_src_vbus(&port1);
		else
			ret = tcpc_disable_src_vbus(&port2);
	}

	return ret;
}

int board_ehci_usb_phy_mode(struct udevice *dev)
{
	int ret = 0;
	enum typec_cc_polarity pol;
	enum typec_cc_state state;
	struct tcpc_port *port_ptr;

	debug("%s %d\n", __func__, dev_seq(dev));

	if (dev_seq(dev) == 0)
		port_ptr = &port1;
	else
		port_ptr = &port2;

	tcpc_setup_ufp_mode(port_ptr);

	ret = tcpc_get_cc_status(port_ptr, &pol, &state);

	tcpc_print_log(port_ptr);
	if (!ret) {
		if (state == TYPEC_STATE_SRC_RD_RA || state == TYPEC_STATE_SRC_RD)
			return USB_INIT_HOST;
	}

	return USB_INIT_DEVICE;
}
#endif

static int iw_get_bom_revision(char *bom_rev, unsigned int raw, int bom_rev_flag)
{
    if (!bom_rev_flag) {
        switch (raw) {
            case 0 ... 100:
                *bom_rev = '0';
                break;
            case 300 ... 450:
                *bom_rev = '1';
                break;
            case 600 ... 850:
                *bom_rev = '2';
                break;
            case 1150 ... 1450:
                *bom_rev = '3';
                break;
            case 1500 ... 1800:
                *bom_rev = '4';
                break;
            case 1900 ... 2200:
                *bom_rev = '5';
                break;
            case 2300 ... 2600:
                *bom_rev = '6';
                break;
            case 2650 ... 2950:
                *bom_rev = '7';
                break;
            case 3200 ... 3450:
                *bom_rev = '8';
                break;
            case 3500 ... 3650:
                *bom_rev = '9';
                break;
            case 4000 ... 4150:
                *bom_rev = 'A';
                break;
            default:
                *bom_rev = 'x';
        }
    } else {
        switch (raw) {
            case 0 ... 100:
                *bom_rev = '0';
                break;
            case 300 ... 450:
                *bom_rev = '1';
                break;
            case 600 ... 850:
                *bom_rev = '2';
                break;
            case 1150 ... 1450:
                *bom_rev = '3';
                break;
            case 1500 ... 1800:
                *bom_rev = '4';
                break;
            case 1900 ... 2200:
                *bom_rev = '5';
                break;
            case 2300 ... 2600:
                *bom_rev = '6';
                break;
            case 2650 ... 2950:
                *bom_rev = '7';
                break;
            case 3200 ... 3450:
                *bom_rev = '8';
                break;
            case 3500 ... 3650:
                *bom_rev = '9';
                break;
            case 4000 ... 4150:
                *bom_rev = 'A';
                break;
            default:
                *bom_rev = 'x';
        }
    }

    return 0;
}

/* Read the adc value based on the dts pin configuration */
static int adc_measurement(ofnode node, int adc_count, char *pcb_rev, char *bom_rev)
{
    struct ofnode_phandle_args adc_args;
    struct udevice *adc;
    unsigned int raw;
    int ret, uV;
    int i;
    int bom_rev_flag = -1;

    for (i = 0; i < adc_count; i++) {

        if (ofnode_parse_phandle_with_args(node, "iwg50m-adc",
                                           "#io-channel-cells", 0, i,
                                           &adc_args)) {
            printf("!!! Failed to parse phandle args for index %d\n", i);
            return 0;
  	}

        ret = uclass_get_device_by_ofnode(UCLASS_ADC, adc_args.node, &adc);
        if (ret) {
            printf("!!! Can't get ADC device for node %s (ret=%d)\n",
                   ofnode_get_name(adc_args.node), ret);
            return ret;
        }

        ret = adc_channel_single_shot(adc->name, adc_args.args[0], &raw);
        if (ret) {
            printf("!!! Single-shot read failed for %s[%d] (ret=%d)\n",
                   adc->name, adc_args.args[0], ret);
            return ret;
        }

        if (adc_raw_to_uV(adc, raw, &uV)) {


            if (i == 0) {
                switch (raw) {
                    case 0 ... 100:
                        *pcb_rev = '1';
                        bom_rev_flag = 0;
                        break;
                    case 300 ... 450:
                        *pcb_rev = '1';
                        bom_rev_flag = 1;
                        break;
                    case 600 ... 850:
                        *pcb_rev = '2';
                        bom_rev_flag = 0;
                        break;
                    case 1150 ... 1450:
                        *pcb_rev = '2';
                        bom_rev_flag = 1;
                        break;
                    case 1500 ... 1800:
                        *pcb_rev = '3';
                        bom_rev_flag = 0;
                        break;
                    case 1900 ... 2200:
                        *pcb_rev = '3';
                        bom_rev_flag = 1;
                        break;
                    case 2300 ... 2600:
                        *pcb_rev = '4';
                        bom_rev_flag = 0;
                        break;
                    case 2650 ... 2950:
                        *pcb_rev = '4';
                        bom_rev_flag = 1;
                        break;
                    case 3200 ... 3450:
                        *pcb_rev = '5';
                        bom_rev_flag = 0;
                        break;
                    case 3500 ... 3650:
                        *pcb_rev = '5';
                        bom_rev_flag = 1;
                        break;
                    case 3950 ... 4150:
                        *pcb_rev = '6';
                        bom_rev_flag = 0;
                        break;
                    default:
                        *pcb_rev = 'x';
                        bom_rev_flag = -1;
	}

            } else {
                
                iw_get_bom_revision(bom_rev, raw, bom_rev_flag);
}
        } else {
            printf("!!! Failed to convert raw ADC to uV for %s[%d]\n",
                   adc->name, adc_args.args[0]);
        }
    }

    return 0;
}

int ahab_get_all_events_string(char *buf, size_t buf_sz)
{
    u32 events[8];
    u32 cnt = ARRAY_SIZE(events);
    u32 fw_ver = 0, sha1 = 0, resp = 0;
    int ret;
    int offset = 0;

    if (!buf || !buf_sz)
        return -EINVAL;

    buf[0] = '\0';

    ret = ele_get_fw_version(&fw_ver, &sha1, &resp);
    if (ret) {
        snprintf(buf, buf_sz, "ELE:not_ready");
        return 0;
    }

    offset += snprintf(buf + offset, buf_sz - offset,
                       "ELE_FW=0x%08x ", fw_ver);

    ret = ele_get_events(events, &cnt, NULL);
    if (ret) {
        snprintf(buf + offset, buf_sz - offset,
                 "EVENTS=read_failed");
        return 0;
    }

    if (!cnt) {
        snprintf(buf + offset, buf_sz - offset,
                 "EVENTS=none");
        return 0;
    }

    offset += snprintf(buf + offset, buf_sz - offset,
                       "COUNT=%u ", cnt);

    for (u32 i = 0; i < cnt && offset < buf_sz; i++)
        offset += snprintf(buf + offset, buf_sz - offset,
                           "[%u]=0x%08x ", i, events[i]);

    return 0;
}

static int board_get_som_revision(char *pcb_rev, char *bom_rev)
{
    ofnode node;
    int adc_count, ret;

    node = ofnode_path("/config");
    if (!ofnode_valid(node)) {
        printf("no /config node?\n");
        return -ENOENT;
    }

    adc_count = ofnode_count_phandle_with_args(node, "iwg50m-adc",
                                               "#io-channel-cells", 0);
    if (adc_count < 0) {
        if (adc_count == -ENOENT)
            return 0;

        printf("Can't find adc channel (%d)\n", adc_count);
        return adc_count;
    }

    return adc_measurement(node, adc_count, pcb_rev, bom_rev);
}

static void print_board_info(void)
{
    struct tag_serialnr serialnr;
    ofnode node;
    char pcb_rev = 0, bom_rev = 0;

    get_board_serial(&serialnr);

    board_get_som_revision(&pcb_rev, &bom_rev); // update your board_get_som_revision accordingly

    printf("Board Info:\n");
    printf("\tBSP Version               : %s\n", BSP_VERSION);
    printf("\tCPU Unique ID             : 0x%08X%08X%08X%08X\n",
           serialnr.low, serialnr.second, serialnr.third, serialnr.high);
    printf("\n");
}

int ft_board_setup(void *blob, struct bd_info *bd)
{
	int ret = 0;
	char pcb_rev = 0, bom_rev = 0;
	int pcb_rev_int = 0, bom_rev_int = 0;

	board_get_som_revision(&pcb_rev, &bom_rev);

	if (pcb_rev >= '0' && pcb_rev <= '9')
		pcb_rev_int = pcb_rev - '0';
	else if (pcb_rev >= 'A' && pcb_rev <= 'F')
		pcb_rev_int = pcb_rev - 'A' + 10;
	else
		pcb_rev_int = -1;

	if (bom_rev >= '0' && bom_rev <= '9')
		bom_rev_int = bom_rev - '0';
	else if (bom_rev >= 'A' && bom_rev <= 'F')
		bom_rev_int = bom_rev - 'A' + 10;
	else
		bom_rev_int = -1;

	if (pcb_rev_int >= 0)
		ret |= fdt_setprop_u32(blob, fdt_path_offset(blob, "/iwg50m_common"), "pcb_rev", pcb_rev_int);
	if (bom_rev_int >= 0)
		ret |= fdt_setprop_u32(blob, fdt_path_offset(blob, "/iwg50m_common"), "bom_rev", bom_rev_int);

	if (ret) {
		printf("Failed to pass SOM revision to kernel FDT file\n");
	}

	return 0;
}


int board_init(void)
{
#if IS_ENABLED(CONFIG_USB_TCPC)
	setup_typec();
#endif



	return 0;
}

int board_late_init(void)
{
	char ahab_buf[256];
#if CONFIG_IS_ENABLED(ENV_IS_IN_MMC) || CONFIG_IS_ENABLED(ENV_IS_NOWHERE)
	board_late_mmc_env_init();
#endif
	print_board_info();
	env_set("sec_boot", "no");
#ifdef CONFIG_AHAB_BOOT
	env_set("sec_boot", "yes");
#endif

#ifdef CONFIG_ENV_VARS_UBOOT_RUNTIME_CONFIG
	env_set("board_name", "i.MX93 IWG50M OSM");
	env_set("board_rev", "iMX93");
#endif
	if (ahab_get_all_events_string(ahab_buf, sizeof(ahab_buf)) == 0)
		env_set("ahab_status", ahab_buf);
    	else
		env_set("ahab_status", "unknown");

	return 0;
}

#ifdef CONFIG_FSL_FASTBOOT
#ifdef CONFIG_ANDROID_RECOVERY
int is_recovery_key_pressing(void)
{
	return 0;
}
#endif /*CONFIG_ANDROID_RECOVERY*/
#endif /*CONFIG_FSL_FASTBOOT*/
