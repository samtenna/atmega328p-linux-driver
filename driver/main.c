#include <linux/printk.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/tty.h>
#include <linux/tty_ldisc.h>
#include <linux/kernel.h>

#define N_PROTO 67
#define CRC8_POLYNOMIAL 0x31
#define SENTINEL_BYTE 0xAA
#define CMD_FRAME_LENGTH 6 // includes the crc + sentinel

#define ATMEGA_IOC_MAGIC 'A'
#define ATMEGA_PING _IO(ATMEGA_IOC_MAGIC, 0)
#define ATMEGA_ADC _IOWR(ATMEGA_IOC_MAGIC, 1, __u16)
#define ATMEGA_LED _IOW(ATMEGA_IOC_MAGIC, 2, __u16)

typedef enum {
    PROTO_STATE_IDLE,
    PROTO_STATE_PROCESSING,
    PROTO_STATE_TIMEOUT,
} protocol_state_t;

typedef struct ldisc_state {
    protocol_state_t state;
    size_t bytes_processed;
} ldisc_state_t;

typedef enum {
    CMD_PING = 0x00,
    CMD_ADC = 0x01,
    CMD_LED = 0x02,
} command_type_t;

typedef enum {
    STATUS_OK,
    STATUS_ERR_CRC,
    STATUS_ERR_INVAL,
    STATUS_ERR_TIMEOUT,
} res_status_t;

static u8 calculate_crc8(u8* bytes, u8 length) {
    u8 crc = 0;

    for (u8 i = 0; i < length; i++) {
        crc ^= bytes[i];

        for (u8 bit = 0; bit < 8; bit++) {
            if (crc & 0x80) {
                crc = (crc << 1) ^ CRC8_POLYNOMIAL;
            } else {
                crc = (crc << 1);
            }
        }
    }

    return crc;
}

static const u8* create_command(command_type_t cmd, u16 arg1, u8 arg2) {
    u8* buffer = kmalloc(6, GFP_KERNEL);
    buffer[0] = SENTINEL_BYTE;
    buffer[1] = cmd;
    buffer[2] = (u8)(arg1 & 0xFF);
    buffer[3] = (u8)((arg1 >> 8) & 0xFF);
    buffer[4] = arg2;
    buffer[5] = calculate_crc8(buffer, 5);

    return buffer;
}

static int send_command(struct tty_struct* tty, command_type_t cmd, u16 arg1, u8 arg2) {
    const u8* buffer = create_command(cmd, arg1, arg2);
    tty->ops->write(tty, buffer, CMD_FRAME_LENGTH);
    kfree(buffer);

    return 0;
}

// Allocate and initialise per-tty state, and associate it with the tty
static int disc_open(struct tty_struct* tty) {
    // kzalloc to allocate memory (guaranteed to be physically contiguous) and zeroes
    ldisc_state_t* state = kzalloc(sizeof(ldisc_state_t), GFP_KERNEL);
    state->state = PROTO_STATE_IDLE;

    tty->disc_data = state;

    return 0;
}

// Stop deferred work and release per-tty state
static void disc_close(struct tty_struct* tty) {
    kfree(tty->disc_data);
}

static int disc_ioctl(struct tty_struct* tty, unsigned int cmd, unsigned long arg) {
    switch (cmd) {
    case ATMEGA_PING: {
        return send_command(tty, CMD_PING, 0, 0);
    }
    case ATMEGA_ADC: {
        // do stuff
        break;
    }
    case ATMEGA_LED: {
        // do stuff
        break;
    }
    }

    return -ENOTTY;
}

// Consume input, advance protocol state
static void receive_buf(struct tty_struct* tty, const u8* cp, const u8* fp, size_t count) {
}

static struct tty_ldisc_ops my_ldisc = {
    .owner = THIS_MODULE,
    .num = N_PROTO,
    .name = "atmega328p-ldisc",
    .open = disc_open,
    .close = disc_close,
    .ioctl = disc_ioctl,
    .receive_buf = receive_buf,
};

static int __init my_init(void) {
    return tty_register_ldisc(&my_ldisc);
}

static void __exit my_exit(void) {
    tty_unregister_ldisc(&my_ldisc);
}

module_init(my_init);
module_exit(my_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("ATMega328P driver");
