#include <linux/printk.h>
#include <linux/types.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/tty.h>
#include <linux/tty_ldisc.h>
#include <linux/kernel.h>
#include <linux/jiffies.h>
#include <linux/mutex.h>
#include <linux/uaccess.h>

#define N_PROTO 67
#define CRC8_POLYNOMIAL 0x31
#define SENTINEL_BYTE 0xAA
#define CMD_FRAME_LENGTH 6 // includes the crc + sentinel
#define RES_FRAME_LENGTH 5 // ditto

typedef enum {
    STATUS_OK = 0x00,
    STATUS_ERR_CRC = 0x01,
    STATUS_ERR_INVAL = 0x02,
    STATUS_ERR_TIMEOUT = 0x03,
} res_status_t;

typedef struct atmega_request {
    __u16 argument;
    __u8 status;
    __u8 reserved;
    __u16 value;
} atmega_request_t;

#define ATMEGA_IOC_MAGIC 'A'
#define ATMEGA_PING _IOR(ATMEGA_IOC_MAGIC, 0, atmega_request_t)
#define ATMEGA_ADC _IOWR(ATMEGA_IOC_MAGIC, 1, atmega_request_t)
#define ATMEGA_LED _IOWR(ATMEGA_IOC_MAGIC, 2, atmega_request_t)
#define ATMEGA_TIMEOUT msecs_to_jiffies(1000)

typedef enum {
    PROTO_STATE_IDLE,
    PROTO_STATE_PROCESSING,
    PROTO_STATE_TIMEOUT,
} protocol_state_t;

typedef enum {
    CMD_PING = 0x00,
    CMD_ADC = 0x01,
    CMD_LED = 0x02,
} command_type_t;

typedef struct ldisc_state {
    protocol_state_t proto_state;
    size_t bytes_processed;
    u8* input_buffer;
    atmega_request_t result;
    bool result_ready;
    bool command_pending;

    struct mutex command_lock; // ensure only one command is sent and waited for at once
    spinlock_t lock;
    wait_queue_head_t read_wait; // i'm told this is roughly equivalent to condition variables
} ldisc_state_t;

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

static u8* create_command(command_type_t cmd, u16 arg1, u8 arg2) {
    u8* buffer = kmalloc(6, GFP_KERNEL);
    buffer[0] = SENTINEL_BYTE;
    buffer[1] = cmd;
    buffer[2] = (u8)(arg1 & 0xFF);
    buffer[3] = (u8)((arg1 >> 8) & 0xFF);
    buffer[4] = arg2;
    buffer[5] = calculate_crc8(buffer, 5);

    return buffer;
}

// actually write the command into the tty device for processing
static int send_command(struct tty_struct* tty, command_type_t cmd, u16 arg1, u8 arg2) {
    u8* buffer;
    int ret;
    buffer = create_command(cmd, arg1, arg2);
    if (!buffer) {
        return -ENOMEM;
    }

    // do the write
    ret = tty->ops->write(tty, buffer, CMD_FRAME_LENGTH);
    kfree(buffer);

    if (ret < 0) {
        return ret;
    }

    if (ret != CMD_FRAME_LENGTH) {
        return -EIO;
    }

    return 0;
}

static void process_response(struct tty_struct* tty) {
    ldisc_state_t* data = tty->disc_data;
    u8 crc = calculate_crc8(data->input_buffer, RES_FRAME_LENGTH - 1);

    spin_lock_irq(&data->lock);

    if (data->command_pending) {
        if (crc != data->input_buffer[RES_FRAME_LENGTH - 1]) {
            data->result.status = STATUS_ERR_CRC;
            data->result.value = 0;
        } else {
            data->result.status = (res_status_t)data->input_buffer[1];
            // join the two bytes together
            data->result.value = data->input_buffer[2] |
                                 ((u16)data->input_buffer[3] << 8);
        }

        data->result_ready = true;
        data->command_pending = false;
    }

    data->proto_state = PROTO_STATE_IDLE;
    data->bytes_processed = 0;

    spin_unlock_irq(&data->lock);
    wake_up_interruptible(&data->read_wait);
}

// Allocate and initialise per-tty state, and associate it with the tty
static int disc_open(struct tty_struct* tty) {
    // kzalloc to allocate memory (guaranteed to be physically contiguous) and zeroes
    ldisc_state_t* state;
    state = kzalloc(sizeof(ldisc_state_t), GFP_KERNEL);
    if (!state) {
        return -ENOMEM;
    }

    state->input_buffer = kzalloc(RES_FRAME_LENGTH, GFP_KERNEL);
    if (!state->input_buffer) {
        kfree(state);
        return -ENOMEM;
    }

    state->proto_state = PROTO_STATE_IDLE;
    spin_lock_init(&state->lock);
    mutex_init(&state->command_lock);
    init_waitqueue_head(&state->read_wait);

    tty->disc_data = state;

    return 0;
}

// Stop deferred work and release per-tty state
static void disc_close(struct tty_struct* tty) {
    ldisc_state_t* state = tty->disc_data;

    if (!state) {
        // don't know how this could happen
        return;
    }

    kfree(state->input_buffer);
    kfree(state);

    tty->disc_data = NULL;
}

// handle the structured commands sent from userspace, and send them to the tty device
static int disc_ioctl(struct tty_struct* tty, unsigned int cmd, unsigned long arg) {
    ldisc_state_t* state = tty->disc_data;
    atmega_request_t request;
    atmega_request_t result;
    unsigned long remaining;
    int ret;

    if (cmd != ATMEGA_PING && cmd != ATMEGA_ADC && cmd != ATMEGA_LED) {
        return -ENOTTY;
    }

    if (copy_from_user(&request, (void __user*)arg, sizeof(request))) {
        return -EFAULT;
    }

    ret = mutex_lock_interruptible(&state->command_lock);
    if (ret) {
        return ret;
    }

    spin_lock_irq(&state->lock);
    state->command_pending = true;
    state->result_ready = false;
    spin_unlock_irq(&state->lock);

    switch (cmd) {
    case ATMEGA_PING: {
        ret = send_command(tty, CMD_PING, 0, 0);
        break;
    }
    case ATMEGA_ADC: {
        ret = send_command(tty, CMD_ADC, request.argument, 0);
        break;
    }
    case ATMEGA_LED: {
        ret = send_command(tty, CMD_LED, request.argument, 0);
        break;
    }
    default:
        ret = -ENOTTY;
        break;
    }

    if (ret) {
        goto cancel_command;
    }

    remaining = wait_event_interruptible_timeout(state->read_wait, READ_ONCE(state->result_ready), ATMEGA_TIMEOUT);
    if (remaining == 0) {
        spin_lock_irq(&state->lock);
        state->command_pending = false;
        state->result_ready = false;
        state->result.status = STATUS_ERR_TIMEOUT;
        state->result.value = 0;
        spin_unlock_irq(&state->lock);

        result.status = STATUS_ERR_TIMEOUT;
        result.value = 0;
        ret = 0;
        goto copy_result;
    }

    if (remaining < 0) {
        ret = remaining;
        goto cancel_command;
    }

    spin_lock_irq(&state->lock);
    result = state->result;
    state->command_pending = false;
    state->result_ready = false;
    spin_unlock_irq(&state->lock);

copy_result:
    request.status = result.status;
    request.value = result.value;

    if (copy_to_user((void __user*)arg, &request, sizeof(request))) {
        ret = -EFAULT;
        goto unlock;
    }

cancel_command:
    spin_lock_irq(&state->lock);
    state->command_pending = false;
    state->result_ready = false;
    spin_unlock_irq(&state->lock);

unlock:
    mutex_unlock(&state->command_lock);

    return ret;
}

// Consume input, advance protocol state
static void receive_buf(struct tty_struct* tty, const u8* cp, const u8* fp, size_t count) {
    ldisc_state_t* data = tty->disc_data;
    size_t buf_idx = 0;

    // if we're not receiving, look for the sentinel byte in the received data
    if (data->proto_state == PROTO_STATE_IDLE) {
        while (buf_idx < count) {
            if (cp[buf_idx] == SENTINEL_BYTE) {
                data->proto_state = PROTO_STATE_PROCESSING;
                data->input_buffer[0] = SENTINEL_BYTE;
                data->bytes_processed = 1;

                break;
            }

            buf_idx++;
        }
    }

    // if we are (now) receiving, continue to read bytes until full frame is read
    if (data->proto_state == PROTO_STATE_PROCESSING) {
        while (buf_idx < count && data->bytes_processed < RES_FRAME_LENGTH) {
            data->input_buffer[data->bytes_processed] = cp[buf_idx];
            data->bytes_processed++;
            buf_idx++;
        }

        if (data->bytes_processed == RES_FRAME_LENGTH) {
            return process_response(tty);
        }
    }

    // TODO: timeout handling
}

static struct tty_ldisc_ops ldisc = {
    .owner = THIS_MODULE,
    .num = N_PROTO,
    .name = "atmega328p-ldisc",
    .open = disc_open,
    .close = disc_close,
    .ioctl = disc_ioctl,
    .receive_buf = receive_buf,
};

static int __init disc_init(void) {
    return tty_register_ldisc(&ldisc);
}

static void __exit disc_exit(void) {
    tty_unregister_ldisc(&ldisc);
}

module_init(disc_init);
module_exit(disc_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("ATMega328P driver");
