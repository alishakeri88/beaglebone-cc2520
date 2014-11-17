#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/fs.h>
#include <asm/uaccess.h>
#include <linux/types.h>
#include <linux/semaphore.h>
#include <linux/slab.h>
#include <linux/sched.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/interrupt.h>
#include <linux/spi/spi.h>
#include <linux/gpio.h>
#include <linux/delay.h>
#include <linux/poll.h>

#include "debug.h"

#define DRIVER_AUTHOR  "Neal Jackson <nealjack@umich.edu>"
#define DRIVER_DESC    "A driver for the GAP Beaglebone Black \"Zigbeag\" cape SPI control."
#define DRIVER_VERSION "0.1"

static void gapspi_cs_mux(int id);
void gapspi_gpio_free(void);

const char gapspi_name[] = "gapspi";

// Device and Pin config
#define GAPSPI_NUM_DEVICES 3
#define GAPSPI_NUM_DEMUX_CTRL_PINS 2

// These pins control the DEMUX between the single SPI0 CS pin and
// the CS line for each radio on GAP
#define GAPSPI_DEMUX_CTRL_PIN0 45
#define GAPSPI_DEMUX_CTRL_PIN1 44
// Array of CS enable pins
static unsigned int GAPSPI_DEMUX_CTRL_PINS[] = {GAPSPI_DEMUX_CTRL_PIN0,
                                                GAPSPI_DEMUX_CTRL_PIN1};

// Defines the level of debug output
uint8_t debug_print = DEBUG_PRINT_DBG;

// SPI
#define SPI_BUS 1
#define SPI_BUS_CS0 0
#define SPI_BUS_SPEED 8000000

struct spi_device* gapspi_spi_device;

// static u8 spi_command_buffer[128];
// static u8 spi_data_buffer[128];

// static struct spi_transfer spi_tsfers[4];
// static struct spi_message spi_msg;

// static spinlock_t spi_spin_lock;
// static unsigned long spi_spin_lock_flags;
// static bool spi_pending = false;


// Use this function to set the DEMUX
static void gapspi_cs_mux(int id)
{
	int i;
	for (i = 0; i < GAPSPI_NUM_DEMUX_CTRL_PINS; ++i) {
		if (id & (1<<i)) {
			gpio_set_value(GAPSPI_DEMUX_CTRL_PINS[i], 1);
		} else {
			gpio_set_value(GAPSPI_DEMUX_CTRL_PINS[i], 0);
		}
	}
}

int gap_spi_async(struct spi_message * message, int dev_id)
{
	// Save the device id (which CS line to use) in the state
	// variable of the message.
	message->state = (void*) dev_id;
	return spi_async(gapspi_spi_device, message);
}

EXPORT_SYMBOL(gap_spi_async);

int gap_spi_sync(struct spi_message * message, int dev_id)
{
	message->state = (void*) dev_id;
	return spi_sync(gapspi_spi_device, message);
}

EXPORT_SYMBOL(gap_spi_sync);

/////////////////////
// SPI
/////////////////////

static int gapspi_spi_probe(struct spi_device *spi_device)
{
    ERR(KERN_INFO, "Inserting SPI protocol driver.\n");
    gapspi_spi_device = spi_device;
    return 0;
}

static int gapspi_spi_remove(struct spi_device *spi_device)
{
    ERR(KERN_INFO, "Removing SPI protocol driver.");
    gapspi_spi_device = NULL;
    return 0;
}

// Configure SPI
static struct spi_driver gapspi_spi_driver = {
	.driver = {
		.name = gapspi_name,
		.owner = THIS_MODULE,
	},
	.probe = gapspi_spi_probe,
	.remove = gapspi_spi_remove,
};

// Intercept the "transfer message" function to allow for
// setting the CS MUX before the message goes out.
int (*real_transfer_one_message)(struct spi_master *master,
                                 struct spi_message *mesg);

void our_transfer_one_message (struct spi_master *master,
                               struct spi_message *mesg) {
	int cs_device_id;

	INFO(KERN_INFO, "PREP DEVICE WOO %p %p", mesg, real_transfer_one_message);

	// use the stored value in mesg to set the mux
	cs_device_id = (int) (mesg->state);
	gapspi_cs_mux(cs_device_id);

	real_transfer_one_message(master, mesg);
}

/////////////////
// init/free
///////////////////

int init_module(void)
{
	int result;

	struct spi_master *spi_master = NULL;
	struct spi_device *spi_device = NULL;
	struct device *pdev;
	char buff[64];
	int i;

	//
	// Setup GPIO SPI mux pins
	//

	// Setup the GPIOs
	for(i = 0; i < GAPSPI_NUM_DEMUX_CTRL_PINS; ++i){
		result = gpio_request_one(GAPSPI_DEMUX_CTRL_PINS[i], GPIOF_DIR_OUT, NULL);
		if (result) goto error;
	}

	// Initialize mux to device 0
	gapspi_cs_mux(0);

	//
	// SPI Device setup
	//

	// Init the SPI lock
	// spin_lock_init(&spi_spin_lock);

	spi_master = spi_busnum_to_master(SPI_BUS);
	if (!spi_master) {
		ERR(KERN_ALERT, "spi_busnum_to_master(%d) returned NULL\n", SPI_BUS);
		//ERR((KERN_ALERT "Missing modprobe spi-bcm2708?\n"));
		goto error;
	}


	// Splice in our transfer one message function so that we can
	// set the mux before the packet is transfered.
	real_transfer_one_message = spi_master->transfer_one_message;
	spi_master->transfer_one_message = our_transfer_one_message;


	INFO(KERN_INFO, "Setup transfer func %p", real_transfer_one_message);

	spi_device = spi_alloc_device(spi_master);
	if (!spi_device) {
		put_device(&spi_master->dev);
		ERR(KERN_ALERT, "spi_alloc_device() failed\n");
		goto error;
	}

	spi_device->chip_select = SPI_BUS_CS0;

	/* Check whether this SPI bus.cs is already claimed */
	snprintf(buff,
		     sizeof(buff),
		     "%s.%u",
	         dev_name(&spi_device->master->dev),
	         spi_device->chip_select);

	pdev = bus_find_device_by_name(spi_device->dev.bus, NULL, buff);

	if (pdev) {
		if (pdev->driver != NULL) {
			ERR(KERN_INFO,
			"Driver [%s] already registered for %s. \
			Nuking from orbit.\n",
			pdev->driver->name, buff);
		} else {
			ERR(KERN_INFO,
			"Previous driver registered with no loaded module. \
			Nuking from orbit.\n");
		}

		device_unregister(pdev);
	}

	spi_device->max_speed_hz = SPI_BUS_SPEED;
	spi_device->mode = SPI_MODE_0;
	spi_device->bits_per_word = 8;
	spi_device->irq = -1;

	spi_device->controller_state = NULL;
	spi_device->controller_data = NULL;
	strlcpy(spi_device->modalias, gapspi_name, SPI_NAME_SIZE);

	result = spi_add_device(spi_device);
	if (result < 0) {
		spi_dev_put(spi_device);
		ERR(KERN_ALERT, "spi_add_device() failed: %d\n", result);
	}

	put_device(&spi_master->dev);

	// Register SPI
	result = spi_register_driver(&gapspi_spi_driver);
    if (result < 0) {
    	ERR(KERN_INFO, "Could not register SPI driver.\n");
    	goto error;
    }

    return 0;

	error:

	gapspi_gpio_free();

	if (spi_master) {
		spi_master->transfer_one_message = real_transfer_one_message;
	}

	spi_unregister_device(spi_device);
    spi_unregister_driver(&gapspi_spi_driver);

	return -1;
}

void cleanup_module(void)
{
	gapspi_gpio_free();

	if (gapspi_spi_device) {
		gapspi_spi_device->master->transfer_one_message = real_transfer_one_message;
		spi_unregister_device(gapspi_spi_device);
	}

	spi_unregister_driver(&gapspi_spi_driver);
}

void gapspi_gpio_free()
{
	int i;
	for (i = 0; i < GAPSPI_NUM_DEMUX_CTRL_PINS; ++i) {
		gpio_free(GAPSPI_DEMUX_CTRL_PINS[i]);
	}
}

MODULE_LICENSE("GPL");
MODULE_AUTHOR(DRIVER_AUTHOR);
MODULE_DESCRIPTION(DRIVER_DESC);