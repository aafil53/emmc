// SPDX-License-Identifier: GPL-2.0+
/*
 * (C) Copyright 2000
 * Wolfgang Denk, DENX Software Engineering, wd@denx.de.
 */

/* #define	DEBUG	*/

#include <autoboot.h>
#include <button.h>
#include <bootstage.h>
#include <bootstd.h>
#include <cli.h>
#include <command.h>
#include <console.h>
#include <env.h>
#include <fdtdec.h>
#include <init.h>
#include <net.h>
#include <version_string.h>
#include <efi_loader.h>
#include <linux/delay.h>
#include <asm/gpio.h>
#include <i2c.h>
#include <dm.h>

#define IMX_GPIO_NR(bank, nr)   (((bank) - 1) * 32 + (nr))

/* #define GPIO2_IO19   IMX_GPIO_NR(1,19)
#define GPIO3_IO00   IMX_GPIO_NR(2,0)
#define GPIO4_IO28   IMX_GPIO_NR(3,28)
#define GPIO2_IO08   IMX_GPIO_NR(1,8)
#define GPIO2_IO09   IMX_GPIO_NR(1,9) */
#define GPIO2_IO28   IMX_GPIO_NR(1,28)
/*#define GPIO3_IO23   IMX_GPIO_NR(2,23)*/
#define GPIO4_IO04   IMX_GPIO_NR(3,4)
#define GPIO4_IO02   IMX_GPIO_NR(3,2)


#define I2C_BUS   1
#define I2C_BUS_0   0
#define IO_EXPANDER_2  0x75
#define IO_EXPANDER_3  0x74
#define BATTERY   0x6b

static void run_preboot_environment_command(void)
{
	char *p;

	p = env_get("preboot");
	if (p != NULL) {
		int prev = 0;

		if (IS_ENABLED(CONFIG_AUTOBOOT_KEYED))
			prev = disable_ctrlc(1); /* disable Ctrl-C checking */

		run_command_list(p, -1, 0);

		if (IS_ENABLED(CONFIG_AUTOBOOT_KEYED))
			disable_ctrlc(prev);	/* restore Ctrl-C checking */
	}
}

void board_early_gpio_i2c_init(void)
{
    int ret;
    uint8_t data;
    struct udevice *bus, *dev;

    printf("=== Starting early GPIO/I2C initialization ===\n");

/*     // ------------------ MODEM UART-TX as GPIO ------------------
    ret = gpio_request(GPIO2_IO08, "GPIO2_IO08");
    if (!ret) {
        gpio_direction_output(GPIO2_IO08, 0);
        printf("GPIO set GPIO2_IO08 = LOW\n");
    }

     // ------------------ MODEM UART-TX as GPIO ------------------
    ret = gpio_request(GPIO2_IO09, "GPIO2_IO09");
    if (!ret) {
        gpio_direction_output(GPIO2_IO09, 0);
        printf("GPIO set GPIO2_IO09 = LOW\n");
    }

    // ------------------ MODEM SWITCH  ------------------
    ret = gpio_request(GPIO2_IO19, "GPIO2_IO19");
    if (ret) {
        printf("[WARN] gpio_request GPIO2_IO19 failed (%d)\n", ret);
    } else {
        gpio_direction_output(GPIO2_IO19, 1);
        printf("GPIO set GPIO2_IO19 = HIGH \n");
    }

    // ------------------ USB SWITCH ------------------
    ret = uclass_get_device_by_seq(UCLASS_I2C, I2C_BUS, &bus);
    if (ret) {
        printf("[ERR] Failed to get I2C bus %d (%d)\n", I2C_BUS, ret);
        return;
    }

    ret = dm_i2c_probe(bus, IO_EXPANDER_3, 0, &dev);
    if (ret) {
        printf("[ERR] Failed to probe I2C addr 0x%02x (%d)\n", IO_EXPANDER_3, ret);
        return;
    }

    // Write reg 0x06 -> 0x09
    data = 0x09;
    ret = dm_i2c_write(dev, 0x06, &data, 1);
    printf("I2C write 0x74 0x06=0x09 ret=%d\n", ret);

    // Write reg 0x02 -> 0x02
    data = 0x02;
    ret = dm_i2c_write(dev, 0x02, &data, 1);
    printf("I2C write 0x74 0x02=0x02 ret=%d\n", ret);

    // ------------------ RESET/HW Shutdown ------------------
    ret = gpio_request(GPIO3_IO00, "GPIO3_IO00");
    if (!ret) {
        gpio_direction_output(GPIO3_IO00, 0);
        printf("GPIO set GPIO3_IO00 = LOW\n");
    }

    // ------------------ PWR-KEY / ON-OFF------------------
    ret = gpio_request(GPIO4_IO28, "GPIO4_IO28");
    if (!ret) {
        gpio_direction_output(GPIO4_IO28, 0);
        printf("GPIO set GPIO4_IO28 = LOW \n");
        mdelay(1000);

        gpio_set_value(GPIO4_IO28, 1);
        printf("GPIO set GPIO4_IO28 = HIGH \n");
        mdelay(1000);

        gpio_set_value(GPIO4_IO28, 0);
        printf("GPIO set GPIO4_IO28 = LOW \n");
    } else {
        printf("[WARN] GPIO2_28 request failed (%d)\n", ret);
    }

     // ------------------ SIM Detection ------------------
    ret = uclass_get_device_by_seq(UCLASS_I2C, I2C_BUS, &bus);
    if (ret) {
        printf("[ERR] Failed to get I2C bus %d (%d)\n", I2C_BUS, ret);
        return;
    }

    ret = dm_i2c_probe(bus, IO_EXPANDER_2, 0, &dev);
    if (ret) {
        printf("[ERR] Failed to probe I2C addr 0x%02x (%d)\n", IO_EXPANDER_2, ret);
        return;
    }

    // Write reg 0x06 -> 0x05
    data = 0x05;
    ret = dm_i2c_write(dev, 0x06, &data, 1);
    printf("I2C write 0x75 0x06=0x09 ret=%d\n", ret);

    // Write reg 0x02 -> 0x016
    data = 0x16;
    ret = dm_i2c_write(dev, 0x02, &data, 1);
    printf("I2C write 0x75 0x02=0x02 ret=%d\n", ret);

    // ------------------ 3V3 Switch EN ------------------

    // Write reg 0x07 -> 0xfb
    data = 0xfb;
    ret = dm_i2c_write(dev, 0x07, &data, 1);
    printf("I2C write 0x75 0x07=0xfb ret=%d\n", ret);

    printf("=== Early GPIO/I2C init done ===\n"); */

// -----------3V3 Rall Switch-------------
    ret = gpio_request(GPIO2_IO28, "GPIO2_IO28");
    if (ret) {
        printf("[WARN] gpio_request GPIO2_IO28 failed (%d)\n", ret);
    } else {
        gpio_direction_output(GPIO2_IO28, 1);
        printf("GPIO set GPIO2_IO28 = HIGH \n");
    }

/* --------- OE Pin for SPI -------------
    ret = gpio_request(GPIO3_IO23, "GPIO3_IO23");
    if (ret) {
        printf("[WARN] gpio_request GPIO3_IO23 failed (%d)\n", ret);
    } else {
        gpio_direction_output(GPIO3_IO23, 1);
        printf("GPIO set GPIO3_IO23 = HIGH \n");
    } */

// ------------------ Ethernet ------------------
	ret = uclass_get_device_by_seq(UCLASS_I2C, 0, &bus);
	if (ret) {
	    printf("[ERR] Failed to get I2C bus 0 (%d)\n", ret);
	    return;
	}

	ret = dm_i2c_probe(bus, 0x21, 0, &dev);
	if (ret) {
	    printf("[ERR] Failed to probe I2C addr 0x21 (%d)\n", ret);
	    return;
	}

// i2cset -f -y 0 0x21 0x03 0xef
	data = 0xEF;
	ret = dm_i2c_write(dev, 0x03, &data, 1);
	printf("I2C write 0x21 0x03=0xEF ret=%d\n", ret);

// i2cset -f -y 0 0x21 0x01 0x10
	data = 0x10;
	ret = dm_i2c_write(dev, 0x01, &data, 1);
	printf("I2C write 0x21 0x01=0x10 ret=%d\n", ret);

// --------- Make DOUT LOW -------------
// DOUT1
    ret = gpio_request(GPIO4_IO04, "GPIO4_IO04");
    if (ret) {
        printf("[WARN] gpio_request GPIO4_IO04 failed (%d)\n", ret);
    } else {
        gpio_direction_output(GPIO4_IO04, 0);
        printf("GPIO set GPIO4_IO04 = LOW \n");
    }

//DOUT2
    ret = gpio_request(GPIO4_IO02, "GPIO4_IO02");
    if (ret) {
        printf("[WARN] gpio_request GPIO4_IO02 failed (%d)\n", ret);
    } else {
        gpio_direction_output(GPIO4_IO02, 0);
        printf("GPIO set GPIO4_IO02 = LOW \n");
    }


// --------- Related to Battery in 2.0 ------------
	/* Get I2C bus 0 */
	ret = uclass_get_device_by_seq(UCLASS_I2C, 0, &bus);
	if (ret) {
	    printf("[ERR] Failed to get I2C bus 0 (%d)\n", ret);
	    return;
	}

	/* Probe device at address 0x6b */ 
	ret = dm_i2c_probe(bus, 0x6b, 0, &dev);
	if (ret) {
	    printf("[ERR] Failed to probe I2C addr 0x6b (%d)\n", ret);
	    return;
	}

	/* Write register 0x00 = 0x7F */
	data = 0x7F;
	ret = dm_i2c_write(dev, 0x00, &data, 1);
	if (ret) {
	    printf("[ERR] I2C write failed (%d)\n", ret);
	} else {
	    printf("I2C write: addr=0x6b reg=0x00 val=0x7F OK\n");
	} 
}

void enable_battery_ship_mode(void)
{
	int ret = -1;
        uint8_t data;
        struct udevice *bus, *dev;
	printf("Battery ship mode init...\n");
	ret = uclass_get_device_by_seq(UCLASS_I2C, I2C_BUS_0, &bus);
	if (ret) {
		printf("[ERR] Failed to get I2C bus %d (%d)\n", I2C_BUS_0, ret);
        	return;
	}

	ret = dm_i2c_probe(bus, BATTERY, 0, &dev);
	if (ret) {
        	printf("[ERR] Failed to probe I2C addr 0x%02x (%d)\n", BATTERY, ret);
	        return;
	}
    // Write reg 0x06 -> 0x09
	data = 0x64;
	ret = dm_i2c_write(dev, 0x09, &data, 1);
	printf("I2C write 0x6B 0x09=0x64 ret=%d\n", ret);

	return;
}

/* We come here after U-Boot is initialised and ready to process commands */
void main_loop(void)
{
	const char *s;
	int ret = 0;
	char command[50] = {0};

	/* The below changes will reduce the Battery Charging Current to 300mA */
	sprintf(command, "i2c dev 0");
	ret = run_command(command, 0);
	if(ret)
	{
		printf("Failed to select the i2c line\n");
	}
	else
	{
		sprintf(command, "i2c probe");
		ret = run_command(command, 0);
		if(ret)
		{
			printf("i2c lines probe error\n");
		}
		else
		{
			/* i2c command to change the battery charging current to 300mA */
			sprintf(command, "i2c mw 0x6b 0x04 0x85");
			ret = run_command(command, 0);
			if(ret)
			{
				printf("Error while setting the battery current\n");
			}
			else
			{
				// Do Nothing
			}
		}
	}

	board_early_gpio_i2c_init();

	bootstage_mark_name(BOOTSTAGE_ID_MAIN_LOOP, "main_loop");

	enable_battery_ship_mode();

 	bootstage_mark_name(BOOTSTAGE_ID_MAIN_LOOP, "main_loop");


	if (IS_ENABLED(CONFIG_VERSION_VARIABLE))
		env_set("ver", version_string);  /* set version variable */

	cli_init();

	if (IS_ENABLED(CONFIG_USE_PREBOOT))
		run_preboot_environment_command();

	if (IS_ENABLED(CONFIG_UPDATE_TFTP))
		update_tftp(0UL, NULL, NULL);

	if (IS_ENABLED(CONFIG_EFI_CAPSULE_ON_DISK_EARLY)) {
		/* efi_init_early() already called */
		if (efi_init_obj_list() == EFI_SUCCESS)
			efi_launch_capsules();
	}

	process_button_cmds();

	s = bootdelay_process();
	if (cli_process_fdt(&s))
		cli_secure_boot_cmd(s);

	autoboot_command(s);

	/* if standard boot if enabled, assume that it will be able to boot */
	if (IS_ENABLED(CONFIG_BOOTSTD_PROG)) {
		int ret;

		ret = bootstd_prog_boot();
		printf("Standard boot failed (err=%dE)\n", ret);
		panic("Failed to boot");
	}

	cli_loop();

	panic("No CLI available");
}
