#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/kref.h>
#include <linux/uaccess.h>
#include <linux/usb.h>
#include <linux/mutex.h>
#include <linux/input.h>
#include <linux/kthread.h>
#include <linux/freezer.h>
#include <linux/kfifo.h>
#include <linux/workqueue.h>
#include <linux/completion.h>
#include <linux/spinlock.h>
#include <linux/sysfs.h>
#include <linux/input.h>
#include <linux/input/mt.h>

#define DRIVER_VERSION	   "V1.0.2-20170614"

#define USB_IRTOUCH_VENDOR_ID		  0x1FF7
#define USB_IRTOUCH_PRODUCT_ID		  0x0013

#define USB_IRTOUCH_A8_VENDOR_ID             0x1FF7
#define USB_IRTOUCH_A8_PRODUCT_ID            0x0001

#define USB_INF_NUM_TOUCH			0x00 /*bInterfaceNumber: Number of Interface*/
#define USB_INF_NUM_ALGO			0x01 /*bInterfaceNumber: Number of Interface*/

#define BULK_TIMEOUT_READ	(HZ/20)  //50ms
#define BULK_TIMEOUT_WRITE       (HZ/20)

#define DEBUG 0
#if DEBUG==1
  #define DBG_PRINTK(args...) printk("irtouch-algo.c[DBG]: "args)
#else
  #define DBG_PRINTK(args...) do {} while (0)
#endif

#define ERR_PRINTK(args...) printk("irtouch-algo.c[ERR]: "args)

#define USE_IRTOUCH_INPUT_DEVICE 0
#define USE_IRTOUCH_ALGO_DRIVER 0
/*----------------------------------------------*
 * constants									*
 *----------------------------------------------*/

enum endpoint_in
{
	ENP_IN_NUM1,
	ENP_IN_NUM2,
};

/* table of devices that work with this driver */
static const struct usb_device_id irtouch_table[] = 
{
	{
		.match_flags	= USB_DEVICE_ID_MATCH_VENDOR | \
						  USB_DEVICE_ID_MATCH_PRODUCT | \
						  USB_DEVICE_ID_MATCH_INT_NUMBER,
		.idVendor		= USB_IRTOUCH_VENDOR_ID,
		.idProduct		= USB_IRTOUCH_PRODUCT_ID,
		.bInterfaceNumber = USB_INF_NUM_ALGO,
	},
	{
        .match_flags    = USB_DEVICE_ID_MATCH_VENDOR | \
						  USB_DEVICE_ID_MATCH_PRODUCT | \
						  USB_DEVICE_ID_MATCH_INT_NUMBER,
		.idVendor       = USB_IRTOUCH_A8_VENDOR_ID,
		.idProduct      = USB_IRTOUCH_A8_PRODUCT_ID,
        .bInterfaceNumber = USB_INF_NUM_ALGO,
	},

	{},
};
MODULE_DEVICE_TABLE(usb, irtouch_table);

typedef struct _IRTOUCH_DEV_S 
{
	struct usb_device		*udev;				/* the usb device for this device */
	struct usb_interface	*interface;			/* the interface for this device */
	struct urb				*bulk_in_urb;		 /* the urb to read data with */
	struct urb				*bulk_out_urb;		 /* the urb to write data with */
	unsigned char			*pInputBuf;			/* data from irtouch */
	unsigned char			*pOutputBuf;		/* data to irtouch */
	unsigned int            bulk_in_size;
	unsigned int            bulk_out_size;
	unsigned int 			bulk_in_filled;
	unsigned int 			bulk_out_filled;
	
	__u8					u8InputEPAddr;		/* the address of the int in endpoint */
	__u8					u8OutputEPAddr;		/* the address of the int out endpoint */
	
	struct completion		complete_read;		/* read complete */
	struct completion		complete_write;		/* write complete */
	
	struct kref				refcount;
	struct mutex		io_mutex_bulk;
} IRTOUCH_DEV_S, *PTR_IRTOUCH_DEV_S;

/*----------------------------------------------*
 * internal routine prototypes					*
 *----------------------------------------------*/

/*----------------------------------------------*
 * project-wide global variables				*
 *----------------------------------------------*/

/*----------------------------------------------*
 * extern variables & functions                                 *
 *----------------------------------------------*/
#if USE_IRTOUCH_ALGO_DRIVER == 1
typedef int (* Operation_Device_Function)(void *pDevice,unsigned char *pDataBuf,int nDataLength,uint8_t nType);
extern uint32_t InitIRTouchModule(Operation_Device_Function pfnOperationDeviceFunction, void *pDevice);
extern uint32_t ExitIRTouchModule(void);
extern uint8_t * GetIRTouchModuleInfo(void);
#endif
#if USE_IRTOUCH_INPUT_DEVICE == 1
extern int irtouch_input_init(void);
extern void irtouch_input_exit(void);
#endif

/*----------------------------------------------*
 * module-wide global variables					*
 *----------------------------------------------*/
static struct usb_driver irtouch_driver;

//==============================================================================
static void irtouch_notifier_write_int_callback(struct urb *urb)
{
	PTR_IRTOUCH_DEV_S  pDev = urb->context;

	/* sync/async unlink faults aren't errors */
	if (urb->status)
	{
		if (!(urb->status == -ENOENT \
			|| urb->status == -ECONNRESET \
			|| urb->status == -ESHUTDOWN))
		{
			dev_err(&pDev->interface->dev,
				"%s - error: %d\n",
				__func__, urb->status);
		}
		pDev->bulk_out_filled = 0;
	} else {
		pDev->bulk_out_filled = urb->actual_length;
		complete(&pDev->complete_write);
	}
}

//============================== read thread START =============================
static void irtouch_read_urb_callback(struct urb *urb)
{
	PTR_IRTOUCH_DEV_S	   pDev	   = urb->context;

	/* sync/async unlink faults aren't errors */
	if (urb->status) 
	{
		if (!(urb->status == -ENOENT \
			|| urb->status == -ECONNRESET \
			|| urb->status == -ESHUTDOWN))
		{
			dev_err(&pDev->interface->dev,
				"%s - error: %d\n",
				__func__, urb->status);
		}
		pDev->bulk_in_filled = 0;
	}
	else
	{
		pDev->bulk_in_filled = urb->actual_length;
		complete(&pDev->complete_read);
	}
}


static int irtouch_read_data(PTR_IRTOUCH_DEV_S	pDev, unsigned int size)
{
	int retval = 0;

	if (!pDev->interface)	 // disconnect() was called
	{	   
		retval = -ENODEV;
		return retval;
	}
	
	usb_fill_bulk_urb(pDev->bulk_in_urb,
			pDev->udev,
			usb_rcvbulkpipe(pDev->udev, pDev->u8InputEPAddr),
			pDev->pInputBuf,
			min(pDev->bulk_in_size, size),
			irtouch_read_urb_callback,
			pDev);
			
	pDev->bulk_in_urb->transfer_flags |= URB_NO_TRANSFER_DMA_MAP;
	/* do it */
	retval = usb_submit_urb(pDev->bulk_in_urb, GFP_KERNEL);

	if (retval < 0)
	{
		dev_err(&pDev->interface->dev,
			"%s - failed submitting read urb, error %d\n",
			__func__, retval);
		retval = (retval == -ENOMEM) ? retval : -EIO;
	}

	return retval;
}

static int irtouch_write_data(PTR_IRTOUCH_DEV_S	 pDev, unsigned int size)
{
	int retval = 0;	

	if (!pDev->interface)	 // disconnect() was called
	{	   
		retval = -ENODEV;
		return retval;
	}
	usb_fill_bulk_urb(pDev->bulk_out_urb,
				pDev->udev, 
				usb_sndbulkpipe(pDev->udev, pDev->u8OutputEPAddr),
				pDev->pOutputBuf,
				min(pDev->bulk_out_size, size),
				irtouch_notifier_write_int_callback,\
				pDev);

	retval = usb_submit_urb(pDev->bulk_out_urb, GFP_KERNEL);
	
	if (retval)
	{
		dev_err(&pDev->interface->dev,
			"%s - failed submitting write urb, error %d\n",
			__func__, retval);
	}

	return retval;
}


static int irtouch_open(struct inode *inode, struct file *file)
{
	PTR_IRTOUCH_DEV_S pDev;
	struct usb_interface *interface;
	int subminor;
	int retval = 0;

	subminor = iminor(inode);

	interface = usb_find_interface(&irtouch_driver, subminor);
	if (!interface) {
			ERR_PRINTK("%s - error, can't find device for minor %d\n",
					__func__, subminor);
			retval = -ENODEV;
			goto exit;
	}

	pDev = usb_get_intfdata(interface);
	if (!pDev) {
			retval = -ENODEV;
			goto exit;
	}
			
	file->private_data = pDev;
	return 0;
	
exit:
	return retval;
}

static int irtouch_release(struct inode *inode, struct file *file)
{
	DBG_PRINTK("%s OK", __func__);
	return 0;
}

#define DRIVER_IOCTL_TYPE_BULK_READ  	   0
#define DRIVER_IOCTL_TYPE_BULK_WRITE 	   1 
#define DRIVER_IOCTL_TYPE_TOUCH_SEND 	   2
#define DRIVER_IOCTL_TYPE_GET_SSID   	   3
extern int irtouch_data_into_input(char *buffer ,int count);
static int irtouch_ioctl_driver(void *pDEV, unsigned char *buffer, int length, unsigned char type)
{
	PTR_IRTOUCH_DEV_S pDev = (PTR_IRTOUCH_DEV_S)pDEV;
	int retval = 0;

	if (pDev == NULL) {
		return -ENODEV;
	}
	
	if (buffer == NULL) {
		ERR_PRINTK("irtouch_ioctl_data buffer null\n");
		return -ENOMEM;	 
	}

	switch(type) {
		case DRIVER_IOCTL_TYPE_BULK_WRITE:
			if (pDev->bulk_out_size < length)
				return -EINVAL;
			mutex_lock(&pDev->io_mutex_bulk);	
			memcpy(pDev->pOutputBuf, buffer, length);
			irtouch_write_data(pDev, length);
			if (wait_for_completion_killable_timeout(&pDev->complete_write, BULK_TIMEOUT_WRITE)) {
				retval = pDev->bulk_out_filled;
			} else {
				DBG_PRINTK("bulk writed time out\n");
                                usb_kill_urb(pDev->bulk_out_urb);
				retval = -ETIMEDOUT;
			}
			mutex_unlock(&pDev->io_mutex_bulk);
			break;
		case DRIVER_IOCTL_TYPE_BULK_READ:
			if (pDev->bulk_in_size < length)
                                return -EINVAL;
			mutex_lock(&pDev->io_mutex_bulk);	
			irtouch_read_data(pDev, length);
			if (wait_for_completion_killable_timeout(&pDev->complete_read, BULK_TIMEOUT_READ)) {
				retval = pDev->bulk_in_filled;
				memcpy(buffer, pDev->pInputBuf, pDev->bulk_in_filled);
			} else {
				DBG_PRINTK("bulk read time out\n");
				usb_kill_urb(pDev->bulk_in_urb);
				retval = -ETIMEDOUT;
			}
			mutex_unlock(&pDev->io_mutex_bulk);	
			break;
		case DRIVER_IOCTL_TYPE_TOUCH_SEND:
#if USE_IRTOUCH_INPUT_DEVICE == 1
			retval = irtouch_data_into_input(buffer, length);
#endif
			break;
		default:
			return -ENOTTY;
	}

	return retval;
}

static const struct file_operations irtouch_fops = {
	.owner =	THIS_MODULE,
	.open =		irtouch_open,
	.release =	irtouch_release,
};

/*
 * usb class driver info in order to get a minor number from the usb core,
 * and to have the device registered with the driver core
 */
static struct usb_class_driver irtouch_class = {
	.name =		"irtouch-bulk%d",
	.fops =		&irtouch_fops,
	.minor_base =	180,
};

static void irtouch_delete(struct kref *kref)
{
	PTR_IRTOUCH_DEV_S  pDev	 = (PTR_IRTOUCH_DEV_S)container_of(kref, IRTOUCH_DEV_S, refcount);
	
	DBG_PRINTK("%s Line:%d", __func__, __LINE__);
	
	if (pDev->bulk_in_urb)
	{
		usb_kill_urb(pDev->bulk_in_urb);
		usb_free_urb(pDev->bulk_in_urb);
	}
	
	if (pDev->bulk_out_urb)
	{
		usb_kill_urb(pDev->bulk_out_urb);
		usb_free_urb(pDev->bulk_out_urb);
	}
	
	if (pDev->udev)
	{
		usb_put_dev(pDev->udev);
	}
		
	if (pDev->pInputBuf)
	{
		usb_free_coherent(pDev->udev, pDev->bulk_in_size,
                          pDev->pInputBuf, pDev->bulk_in_urb->transfer_dma);
	}
	
	if (pDev->pOutputBuf)
	{
		kfree(pDev->pOutputBuf);
	}
	
	kfree(pDev);
	DBG_PRINTK("%s Line:%d", __func__, __LINE__);
}

//==============================================================================
static int irtouch_probe(struct usb_interface *interface,
							const struct usb_device_id *id)
{
	PTR_IRTOUCH_DEV_S				   pDev		   = NULL;
	struct usb_host_interface			*pInfDesc	= NULL;
	struct usb_endpoint_descriptor		*pEndPoint	= NULL;
	int retval										= -ENOMEM;
	int i;
	
	DBG_PRINTK("%s Start!", __func__);
	
	/* allocate memory for our device state and initialize it */
	pDev = kzalloc(sizeof(IRTOUCH_DEV_S), GFP_KERNEL);
	if (!pDev)
	{
		dev_err(&interface->dev, "Out of memory\n");
		retval = -ENOMEM;
		goto error;
	}
	
	kref_init(&pDev->refcount);
	mutex_init(&pDev->io_mutex_bulk);
 
	init_completion(&pDev->complete_read);
	init_completion(&pDev->complete_write);
	
	// bind interface.
	pDev->udev = usb_get_dev(interface_to_usbdev(interface));
	pDev->interface = interface;

	/* set up the pEndPoint information */
	/* use only support one-and-last input and output endpoints */
	pInfDesc = interface->cur_altsetting;
	DBG_PRINTK("NumEndpoints:%d\n", pInfDesc->desc.bNumEndpoints);
	for (i = 0; i < pInfDesc->desc.bNumEndpoints; ++i)
	{
		pEndPoint = &pInfDesc->endpoint[i].desc;

		// INPUT endpoint
		if (!pDev->u8InputEPAddr && usb_endpoint_is_bulk_in(pEndPoint)) 
		{
			/* we found a int in pEndPoint */
			pDev->bulk_in_size = usb_endpoint_maxp(pEndPoint);
			DBG_PRINTK("usb_endpoint_maxp(bulk_in) = %d\n", pDev->bulk_in_size);
			pDev->u8InputEPAddr = pEndPoint->bEndpointAddress;
			pDev->bulk_in_urb	 = usb_alloc_urb(0, GFP_KERNEL);
			if (!pDev->bulk_in_urb)
			{
				dev_err(&interface->dev, "Could not allocate in-pEndPoint urb\n");
				retval = -ENOMEM;
				goto error;
			}
			pDev->pInputBuf	= usb_alloc_coherent(pDev->udev, pDev->bulk_in_size, GFP_KERNEL,
                                 &pDev->bulk_in_urb->transfer_dma);
			if (!pDev->pInputBuf) {
				dev_err(&interface->dev, "Could not allocate in-pEndPoint buffer\n");
                retval = -ENOMEM;
                goto error;
            }
		}

		// OUTPUT endpoint
		if (!pDev->u8OutputEPAddr && usb_endpoint_is_bulk_out(pEndPoint))
		{
			/* we found a int out pEndPoint */
			pDev->bulk_out_size = usb_endpoint_maxp(pEndPoint);
			DBG_PRINTK("usb_endpoint_maxp(bulk_out) = %d\n", pDev->bulk_out_size);
			pDev->pOutputBuf	= kzalloc(pDev->bulk_out_size, GFP_KERNEL);
			pDev->bulk_out_urb	 = usb_alloc_urb(0, GFP_KERNEL);
			if (!pDev->bulk_out_urb)
			{
				dev_err(&interface->dev, "Could not allocate out-pEndPoint buffer\n");
				goto error;
			}
			pDev->u8OutputEPAddr = pEndPoint->bEndpointAddress;
		}
	}
  
	
	if (!(pDev->u8InputEPAddr && pDev->u8OutputEPAddr))
	{
		dev_err(&interface->dev, "Could not find both int-in and int-out endpoints\n");
		goto error;
	}
		
	/* save our data pointer in this interface device */
	usb_set_intfdata(interface, pDev);

	/* we can register the device now, as it is ready */
	retval = usb_register_dev(interface, &irtouch_class);
	if (retval) 
	{
		/* something prevented us from registering this driver */
		dev_err(&interface->dev, "Not able to get a minor for this device.\n");
		usb_set_intfdata(interface, NULL);
		goto error;
	}

	/* let the user know what node this device is now attached to */
	dev_info(&interface->dev,
		 "USB device now attached to USBirtouch-%d, drv ver:%s\n",
		 interface->minor, DRIVER_VERSION);
		
#if USE_IRTOUCH_INPUT_DEVICE == 1
	retval = irtouch_input_init();
	if (retval) {
		dev_err(&interface->dev, "input-dev can not init.\n");
		goto input_error;
	}
#endif

#if USE_IRTOUCH_ALGO_DRIVER == 1
	InitIRTouchModule(irtouch_ioctl_driver, (void *)pDev);
#endif

	return 0;

input_error:
	usb_deregister_dev(interface, &irtouch_class);
error:
	if (pDev)
	{
		/* this frees allocated memory */
		kref_put(&pDev->refcount, irtouch_delete);
	}
	return retval;
}

static void irtouch_disconnect(struct usb_interface *interface) 
{
	PTR_IRTOUCH_DEV_S pDev	= NULL;
	int minor = interface->minor;
	
	DBG_PRINTK("%s Line:%d\n", __func__, __LINE__);

#if USE_IRTOUCH_ALGO_DRIVER == 1
	ExitIRTouchModule();
#endif

#if USE_IRTOUCH_INPUT_DEVICE == 1
	irtouch_input_exit();
#endif

	pDev = usb_get_intfdata(interface);
	usb_set_intfdata(interface, NULL);

	/* give back our minor */
	usb_deregister_dev(interface, &irtouch_class);
	
	/* prevent more I/O from starting */
	mutex_lock(&pDev->io_mutex_bulk);
	pDev->interface = NULL;
	mutex_unlock(&pDev->io_mutex_bulk);

	/* decrement our usage count */
	kref_put(&pDev->refcount, irtouch_delete);
		
	dev_err(&interface->dev, "USB Mcutouch #%d now disconnected", minor);
}

static struct usb_driver irtouch_driver = {
	.name					= "seewo-irtouch",
	.probe					= irtouch_probe,
	.disconnect				= irtouch_disconnect,
	.id_table				= irtouch_table,
	.supports_autosuspend	= 1,
};

static ssize_t get_drvinfo(struct device_driver *_drv, char *_buf)
{
	int length = 0;
#if USE_IRTOUCH_ALGO_DRIVER
	unsigned char *source = GetIRTouchModuleInfo();

	length = strlen(source);
	memcpy(_buf, source, length);
#endif
	return length;
}
static DRIVER_ATTR(drvinfo, 0440, get_drvinfo, NULL);

static int usb_driver_irtouch_init(struct usb_driver *driver) {
	int retval;
	usb_register_driver(driver, THIS_MODULE, KBUILD_MODNAME);
	retval = driver_create_file(&driver->drvwrap.driver, &driver_attr_drvinfo);
        if (retval) {
		printk("seewo-irtouch usb-driver create sys file error.\n");
		return retval;
        }
	return 0;
}

static void usb_driver_irtouch_exit(struct usb_driver *driver) {
	/* remove driver attr file */
        driver_remove_file(&driver->drvwrap.driver, &driver_attr_drvinfo);
	usb_deregister(driver);
}
module_driver(irtouch_driver, usb_driver_irtouch_init, usb_driver_irtouch_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("HeXin <hexin@cvte.com>");
MODULE_DESCRIPTION("Irtouch analog driver");
MODULE_VERSION(DRIVER_VERSION);



