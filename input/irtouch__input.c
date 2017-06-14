#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/cdev.h>
#include <linux/input.h>
#include <linux/input/mt.h>
#include <linux/mutex.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/device.h>

#define TOUCH_WIDTH_ENABLE 1
#define PER_POINT 6
#define MAX_POINT 20
#if TOUCH_WIDTH_ENABLE == 1
  #define PER_TOUCH_DATA_SIZE    62
#else 
  #define PER_TOUCH_DATA_SIZE    38
#endif

#define FINGER_STATE_UP   0
#define FINGER_STATE_DN   1

#define TOUCH_STATE_NL      0
#define TOUCH_STATE_DN_UP   4
#define TOUCH_STATE_MV      7

#define DEBUG 0
#if DEBUG==1
  #define DBG_PRINTK(args...) printk("irtouch-input.c[DBG]: "args)
#else
  #define DBG_PRINTK(args...) do {} while (0)
#endif
typedef struct _IRTOUCH_TOUCH_DATA_S
{
    unsigned char  state;
    unsigned char  id;
    unsigned short X;
    unsigned short Y;
#if TOUCH_WIDTH_ENABLE == 1
    unsigned short width;
    unsigned short height;
#endif
} IRTOUCH_TOUCH_DATA_S, *PTR_IRTOUCH_TOUCH_DATA_S;

typedef struct _IRTOUCH_INPUT_S {
	struct input_dev      *ptouch_dev;
	struct mutex          io_mutex;
	IRTOUCH_TOUCH_DATA_S  irtouch_data[MAX_POINT];
	int                   irtouch_pack_cnt;
} IRTOUCH_INPUT_S, *PTR_IRTOUCH_INPUT_S;

PTR_IRTOUCH_INPUT_S G_ptr_irtouch_input_dev;

static void report_touch_event(PTR_IRTOUCH_INPUT_S pDev, const PTR_IRTOUCH_TOUCH_DATA_S ptouch_data, int point_cnt) 
{
	struct input_dev *ptouch_dev = pDev->ptouch_dev;
    int i;
    int upfingercnt=0;
    int fingerflag[MAX_POINT]={0};  
    int position[MAX_POINT]={0}; 
		
    if (ptouch_dev != NULL) {
        for (i=0; i<point_cnt; i++){
            if (ptouch_data[i].state == TOUCH_STATE_MV){
               fingerflag[ptouch_data[i].id] = FINGER_STATE_DN; 
               position[ptouch_data[i].id] = i;
            }
        }
        for (i=0; i<point_cnt; i++){
            input_mt_slot(ptouch_dev, i);
            if (fingerflag[i]==FINGER_STATE_DN) {
                input_mt_report_slot_state(ptouch_dev, MT_TOOL_FINGER, true);  
                input_report_abs(ptouch_dev, ABS_MT_POSITION_X, ptouch_data[position[i]].X);   
                input_report_abs(ptouch_dev, ABS_MT_POSITION_Y, ptouch_data[position[i]].Y);    
            #if TOUCH_WIDTH_ENABLE == 1
                if (ptouch_data[position[i]].height > ptouch_data[position[i]].width) {
	                input_report_abs(ptouch_dev, ABS_MT_TOUCH_MAJOR, ptouch_data[position[i]].height/2);     
	                input_report_abs(ptouch_dev, ABS_MT_TOUCH_MINOR, ptouch_data[position[i]].width/2);
                } else {
                    input_report_abs(ptouch_dev, ABS_MT_TOUCH_MAJOR, ptouch_data[position[i]].width/2);     
	                input_report_abs(ptouch_dev, ABS_MT_TOUCH_MINOR, ptouch_data[position[i]].height/2);
                } 
	        #endif
            } else {
                upfingercnt++;
                input_mt_report_slot_state(ptouch_dev, MT_TOOL_FINGER, false); 
            }
        }
        if (upfingercnt == point_cnt) {
			DBG_PRINTK("BTN_TOUCH up\n");
            input_report_key(ptouch_dev, BTN_TOUCH, 0);
        } else {
			DBG_PRINTK("BTN_TOUCH down\n");
            input_report_key(ptouch_dev, BTN_TOUCH, 1);	
        }
        input_sync(ptouch_dev);
    }
}

int irtouch_data_into_input(char *buffer, int count) {
	PTR_IRTOUCH_TOUCH_DATA_S point_data;
	int point_count;
	
	if (buffer == NULL)
		return -1;
	
	if (count != PER_TOUCH_DATA_SIZE)
		return -3;

	mutex_lock(&G_ptr_irtouch_input_dev->io_mutex);
	point_data = G_ptr_irtouch_input_dev->irtouch_data;
	point_count = buffer[PER_TOUCH_DATA_SIZE-1];
	
	if (buffer[PER_TOUCH_DATA_SIZE-1] > PER_POINT) {
		memset((void *)point_data, 0, sizeof(point_data));
		memcpy((void *)point_data, (const void *)(buffer+1), sizeof(IRTOUCH_TOUCH_DATA_S)*PER_POINT);
		G_ptr_irtouch_input_dev->irtouch_pack_cnt = 1;
	} else if (buffer[PER_TOUCH_DATA_SIZE-1] == 0){
		int pack_cnt = G_ptr_irtouch_input_dev->irtouch_pack_cnt;
		if (pack_cnt < MAX_POINT/PER_POINT) {
			memcpy((void *)(point_data+pack_cnt), (const void *)(buffer+1), sizeof(IRTOUCH_TOUCH_DATA_S)*PER_POINT);
		} else if (pack_cnt == MAX_POINT/PER_POINT){
			memcpy((void *)(point_data+pack_cnt), (const void *)(buffer+1), sizeof(IRTOUCH_TOUCH_DATA_S)*(MAX_POINT%PER_POINT));
			report_touch_event(G_ptr_irtouch_input_dev, point_data, MAX_POINT);
		} else {
			mutex_unlock(&G_ptr_irtouch_input_dev->io_mutex);
			return -2;
		}
		G_ptr_irtouch_input_dev->irtouch_pack_cnt++;
	} else {
		memset((void *)point_data, 0, sizeof(point_data));
		memcpy((void *)point_data, (const char *)(buffer+1), sizeof(IRTOUCH_TOUCH_DATA_S)*PER_POINT);
		report_touch_event(G_ptr_irtouch_input_dev, point_data, MAX_POINT);
	}
	mutex_unlock(&G_ptr_irtouch_input_dev->io_mutex);
	
	return 0;
}

int irtouch_input_init(void)
{
	int retval=0;
	PTR_IRTOUCH_INPUT_S pDev;
	
	G_ptr_irtouch_input_dev = kzalloc(sizeof(IRTOUCH_INPUT_S), GFP_KERNEL);
	if (!G_ptr_irtouch_input_dev) {
        DBG_PRINTK("IRtouch_input_dev_driver Out of memory.\n");
        retval = -ENOMEM;
        goto error;
    }
	pDev = G_ptr_irtouch_input_dev;
	
	mutex_init(&pDev->io_mutex);
	
	pDev->ptouch_dev= input_allocate_device();
	if(!pDev->ptouch_dev) {
		DBG_PRINTK("IRtouch_input_dev_driver failed to allocate input device.\n");
		retval = -1;
		goto error;
	}
	
 	pDev->ptouch_dev->name = "IRtouch-algo";
 	pDev->ptouch_dev->phys = "IRtouch-algo/touch";

    pDev->ptouch_dev->evbit[0] = BIT_MASK(EV_SYN) | BIT_MASK(EV_KEY) | BIT_MASK(EV_ABS);
    pDev->ptouch_dev->keybit[BIT_WORD(BTN_TOUCH)] = BIT_MASK(BTN_TOUCH);

    input_mt_init_slots(pDev->ptouch_dev, MAX_POINT, INPUT_MT_DIRECT);
	input_set_abs_params(pDev->ptouch_dev, ABS_MT_POSITION_X, 0, 32767, 0, 0);
	input_set_abs_params(pDev->ptouch_dev, ABS_MT_POSITION_Y, 0, 32767, 0, 0);
    #if TOUCH_WIDTH_ENABLE == 1
	input_set_abs_params(pDev->ptouch_dev, ABS_MT_TOUCH_MAJOR, 0, 32767, 0, 0);
	input_set_abs_params(pDev->ptouch_dev, ABS_MT_TOUCH_MINOR, 0, 32767, 0, 0);
	#endif
    if (input_register_device(pDev->ptouch_dev) < 0) {
		DBG_PRINTK("Failed to register IRtouch-algo device\n");
		input_free_device(pDev->ptouch_dev);
		goto error;
    }

    return 0; 
error:
	return retval;     
}

void irtouch_input_exit(void)
{
	PTR_IRTOUCH_INPUT_S pDev = G_ptr_irtouch_input_dev;
	
	input_unregister_device(pDev->ptouch_dev);
	kfree(G_ptr_irtouch_input_dev);
}

