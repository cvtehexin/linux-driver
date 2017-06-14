/****************************************************************************************
 **Copyright       :SEEWO Inc
 **Author          :Hexin(hexin@cvte.com)
 **Date            :2016-03-10
 **This driver used for create virtual board event by sysfile.
 *********************************************************************************************/

#include <linux/module.h>
#include <linux/string.h>
#include <linux/input.h>
#include <linux/platform_device.h>
#include <linux/version.h>

#define SYS_INPUT_MAX_BUF_SIZE 1024

static struct input_dev *g_dev_keyboard;

static ssize_t get_virtual_board(struct device_driver *_drv, char *_buf)
{
    int ret;
    ret = snprintf(_buf, SYS_INPUT_MAX_BUF_SIZE, "It's a virtual device,can not return anything!\n");
    return ret;
}

static ssize_t set_virtual_board(struct device_driver *_drv, const char *_buf, size_t _count)
{
    int input_num = 0;
    char input_char = 0;
    if (_count > SYS_INPUT_MAX_BUF_SIZE)
	return _count;
    
    sscanf(_buf, "%d%c", &input_num, &input_char);
    if ((input_num < KEY_RESERVED) || (input_num > KEY_MAX))
	return _count;

    if ((input_char == 'D') || (input_char == 'd')) {
        input_event(g_dev_keyboard, EV_KEY, input_num, 1);   //key down
        input_sync(g_dev_keyboard);
    } else if ((input_char == 'U') || (input_char == 'u')) {
        input_event(g_dev_keyboard, EV_KEY, input_num, 0);   //key up
        input_sync(g_dev_keyboard);
    }
    
    return _count;
}

static DRIVER_ATTR(virtual_board, 0666, get_virtual_board, set_virtual_board);

void alloc_and_register_device(void)
{
    int error;
    struct input_dev *tmp;
    
    g_dev_keyboard = 0;

    //allocate and register a mouse device
    tmp = input_allocate_device();
    if(tmp) {
        int i;
        tmp->name = "virtual_key_board";
        tmp->phys = "virtual_key_board/input0";
        tmp->id.bustype = BUS_HOST;
        tmp->id.vendor = 0x0088;
        tmp->id.product = 0x0003;
        tmp->id.version = 0x0100;
        tmp->dev.parent = 0;
		

        for(i = KEY_RESERVED + 1; i < KEY_MAX; i++)
            set_bit(i, tmp->keybit);
        set_bit(EV_KEY, tmp->evbit);
	set_bit(EV_REP, tmp->evbit);

        error = input_register_device(tmp);
        if (error) {
            input_free_device(tmp);
        } else {
            g_dev_keyboard = tmp;
        }
    }

}

static struct platform_driver virtual_board_platform_driver = {  //platform driver for sysfile control
        .driver         = {
                .name   = "virtual_board",
                .owner  = THIS_MODULE,
        }
};

static int __init virtual_board_init(void)
{
    int ret;
    ret = platform_driver_register(&virtual_board_platform_driver);
    if(ret)
        return ret;

    alloc_and_register_device();

    ret = driver_create_file(&(virtual_board_platform_driver.driver), &driver_attr_virtual_board);
    return 0;
}

static void __exit virtual_board_exit(void)
{
    driver_remove_file(&(virtual_board_platform_driver.driver), &driver_attr_virtual_board);

    if(g_dev_keyboard)
    {
        input_unregister_device(g_dev_keyboard);
        input_free_device(g_dev_keyboard);
    }
    platform_driver_unregister(&virtual_board_platform_driver);
}


#define DRV_VERSION "V1.0"

MODULE_DESCRIPTION("SEEWO Virtual Board Device Driver");
MODULE_LICENSE("GPL");
MODULE_VERSION(DRV_VERSION);

module_init(virtual_board_init);
module_exit(virtual_board_exit);

