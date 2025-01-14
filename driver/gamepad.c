// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2021 Severin von Wnuck-Lipinski <severinvonw@outlook.de>
 */

#include <linux/module.h>
#include <linux/uuid.h>
#include <linux/timer.h>

#include "common.h"
#include "../auth/auth.h"

#define GIP_GP_NAME "Microsoft Xbox Controller"

#define GIP_VENDOR_MICROSOFT 0x045e
#define GIP_PRODUCT_ELITE_SERIES_2 0x0b00
#define GIP_PRODUCT_ELITE 0x02e3
// Various versions of the Elite Series 2 firmware have changed the way paddle
// states are sent. Paddle support is only reported up to this firmware
// version.
#define GIP_ELITE_SERIES_2_4X_FIRMWARE 0x04FF
#define GIP_ELITE_SERIES_2_510_FIRMWARE 0x050A

#define GIP_GP_RUMBLE_DELAY msecs_to_jiffies(10)
#define GIP_GP_RUMBLE_MAX 100

/* button offset from end of packet */
#define GIP_GP_BTN_SHARE_OFFSET 18

#define gip_dbg(client, ...) dev_dbg(&(client)->adapter->dev, __VA_ARGS__)

static const guid_t gip_gamepad_guid_share =
	GUID_INIT(0xecddd2fe, 0xd387, 0x4294,
		  0xbd, 0x96, 0x1a, 0x71, 0x2e, 0x3d, 0xc7, 0x7d);

static const guid_t gip_gamepad_guid_dli =
	GUID_INIT(0x87f2e56b, 0xc3bb, 0x49b1,
		  0x82, 0x65, 0xff, 0xff, 0xf3, 0x77, 0x99, 0xee);

enum gip_gamepad_button {
	GIP_GP_BTN_MENU = BIT(2),
	GIP_GP_BTN_VIEW = BIT(3),		
	GIP_GP_BTN_A = BIT(4),
	GIP_GP_BTN_B = BIT(5),
	GIP_GP_BTN_X = BIT(6),
	GIP_GP_BTN_Y = BIT(7),
	GIP_GP_BTN_DPAD_U = BIT(8),
	GIP_GP_BTN_DPAD_D = BIT(9),
	GIP_GP_BTN_DPAD_L = BIT(10),
	GIP_GP_BTN_DPAD_R = BIT(11),
	GIP_GP_BTN_BUMPER_L = BIT(12),
	GIP_GP_BTN_BUMPER_R = BIT(13),
	GIP_GP_BTN_STICK_L = BIT(14),
	GIP_GP_BTN_STICK_R = BIT(15),
};

enum gip_gamepad_paddle {
	GIP_GP_BTN_P1 = BIT(0),
	GIP_GP_BTN_P2 = BIT(1),
	GIP_GP_BTN_P3 = BIT(2),
	GIP_GP_BTN_P4 = BIT(3),
};

enum gip_gamepad_motor {
	GIP_GP_MOTOR_R = BIT(0),
	GIP_GP_MOTOR_L = BIT(1),
	GIP_GP_MOTOR_RT = BIT(2),
	GIP_GP_MOTOR_LT = BIT(3),
};

struct gip_gamepad_pkt_firmware {
    // Remember, xpad keeps the 4 bytes.
    // Paddles are at [18] in xpad, so, [14] here.
    // Pad 14 bytes.
    u8 unknown[14];
    u8 paddles;
    u8 profile;
} __packed;

struct gip_gamepad_pkt_input {
	__le16 buttons;
	__le16 trigger_left;
	__le16 trigger_right;
	__le16 stick_left_x;
	__le16 stick_left_y;
	__le16 stick_right_x;
	__le16 stick_right_y;
} __packed;

struct gip_gamepad_pkt_dli {
	u32 counter_us1;
	u32 counter_us2;
} __packed;

struct gip_gamepad_pkt_rumble {
	u8 unknown;
	u8 motors;
	u8 left_trigger;
	u8 right_trigger;
	u8 left;
	u8 right;
	u8 duration;
	u8 delay;
	u8 repeat;
} __packed;

typedef enum {
	PADDLE_NONE,
	PADDLE_ELITE,
	PADDLE_ELITE2_4X, // Still in the same packet
	PADDLE_ELITE2_510, // Same packet, different location
    PADDLE_ELITE2_511 // Different packet entirely.
} PaddleCapability;

struct gip_gamepad {
	struct gip_client *client;
	struct gip_battery battery;
	struct gip_auth auth;
	struct gip_led led;
	struct gip_input input;

	bool supports_share;
	bool supports_dli;
	PaddleCapability paddle_support;

	struct gip_gamepad_rumble {
		/* serializes access to rumble packet */
		spinlock_t lock;
		unsigned long last;
		struct timer_list timer;
		struct gip_gamepad_pkt_rumble pkt;
	} rumble;
};

static void gip_gamepad_send_rumble(struct timer_list *timer)
{
	struct gip_gamepad_rumble *rumble = from_timer(rumble, timer, timer);
	struct gip_gamepad *gamepad = container_of(rumble, typeof(*gamepad),
						   rumble);
	unsigned long flags;

	spin_lock_irqsave(&rumble->lock, flags);

	gip_send_rumble(gamepad->client, &rumble->pkt, sizeof(rumble->pkt));
	rumble->last = jiffies;

	spin_unlock_irqrestore(&rumble->lock, flags);
}

static int gip_gamepad_queue_rumble(struct input_dev *dev, void *data,
				    struct ff_effect *effect)
{
	struct gip_gamepad_rumble *rumble = input_get_drvdata(dev);
	u32 mag_left = effect->u.rumble.strong_magnitude;
	u32 mag_right = effect->u.rumble.weak_magnitude;
	unsigned long flags;

	if (effect->type != FF_RUMBLE)
		return 0;

	spin_lock_irqsave(&rumble->lock, flags);

	rumble->pkt.left = (mag_left * GIP_GP_RUMBLE_MAX + S16_MAX) / U16_MAX;
	rumble->pkt.right = (mag_right * GIP_GP_RUMBLE_MAX + S16_MAX) / U16_MAX;

	/* delay rumble to work around firmware bug */
	if (!timer_pending(&rumble->timer))
		mod_timer(&rumble->timer, rumble->last + GIP_GP_RUMBLE_DELAY);

	spin_unlock_irqrestore(&rumble->lock, flags);

	return 0;
}

static int gip_gamepad_init_rumble(struct gip_gamepad *gamepad)
{
	struct gip_gamepad_rumble *rumble = &gamepad->rumble;
	struct input_dev *dev = gamepad->input.dev;

	spin_lock_init(&rumble->lock);
	timer_setup(&rumble->timer, gip_gamepad_send_rumble, 0);

	/* stop rumble (required for some exotic gamepads to start input) */
	rumble->pkt.motors = GIP_GP_MOTOR_R | GIP_GP_MOTOR_L |
			     GIP_GP_MOTOR_RT | GIP_GP_MOTOR_LT;
	rumble->pkt.duration = 0xff;
	rumble->pkt.repeat = 0xeb;
	gip_gamepad_send_rumble(&rumble->timer);

	input_set_capability(dev, EV_FF, FF_RUMBLE);
	input_set_drvdata(dev, rumble);

	return input_ff_create_memless(dev, NULL, gip_gamepad_queue_rumble);
}

static int gip_gamepad_init_extra_data(struct gip_gamepad *gamepad)
{
	return gip_init_extra_data(gamepad->client);
}

static int gip_gamepad_init_input(struct gip_gamepad *gamepad)
{
	struct input_dev *dev = gamepad->input.dev;
	struct gip_hardware hardware = gamepad->client->hardware;
	int err;

	gamepad->supports_share = gip_has_interface(gamepad->client,
						    &gip_gamepad_guid_share);
	gamepad->supports_dli = gip_has_interface(gamepad->client,
						  &gip_gamepad_guid_dli);

	gamepad->paddle_support = PADDLE_NONE;
	if(hardware.vendor == GIP_VENDOR_MICROSOFT) {
		if(hardware.product == GIP_PRODUCT_ELITE) {
			gamepad->paddle_support = PADDLE_ELITE;
		}
        else if (hardware.product == GIP_PRODUCT_ELITE_SERIES_2)
        {
			printk("Elite Series 2\n");
            if (hardware.version <= GIP_ELITE_SERIES_2_4X_FIRMWARE)
                gamepad->paddle_support = PADDLE_ELITE2_4X;
            else if (hardware.version <= GIP_ELITE_SERIES_2_510_FIRMWARE)
                gamepad->paddle_support = PADDLE_ELITE2_510;
            else if (hardware.version > GIP_ELITE_SERIES_2_510_FIRMWARE){ // If new revisions come, this should become LTE new max
				printk("Elite Series 2 > 5.10\n");
                gamepad->paddle_support = PADDLE_ELITE2_511;
			}
        }
	}

	if (gamepad->supports_share)
		input_set_capability(dev, EV_KEY, KEY_RECORD);

	if ((gamepad->paddle_support ==  PADDLE_ELITE) || (gamepad->paddle_support == PADDLE_ELITE2_4X) || (gamepad->paddle_support == PADDLE_ELITE2_510) || (gamepad->paddle_support == PADDLE_ELITE2_511)) {
		input_set_capability(dev, EV_KEY, BTN_TRIGGER_HAPPY5);
		input_set_capability(dev, EV_KEY, BTN_TRIGGER_HAPPY6);
		input_set_capability(dev, EV_KEY, BTN_TRIGGER_HAPPY7);
		input_set_capability(dev, EV_KEY, BTN_TRIGGER_HAPPY8);
	}

	input_set_capability(dev, EV_KEY, BTN_MODE);
	input_set_capability(dev, EV_KEY, BTN_START);
	input_set_capability(dev, EV_KEY, BTN_SELECT);
	input_set_capability(dev, EV_KEY, BTN_A);
	input_set_capability(dev, EV_KEY, BTN_B);
	input_set_capability(dev, EV_KEY, BTN_X);
	input_set_capability(dev, EV_KEY, BTN_Y);
	input_set_capability(dev, EV_KEY, BTN_TL);
	input_set_capability(dev, EV_KEY, BTN_TR);
	input_set_capability(dev, EV_KEY, BTN_THUMBL);
	input_set_capability(dev, EV_KEY, BTN_THUMBR);
	input_set_abs_params(dev, ABS_X, -32768, 32767, 16, 128);
	input_set_abs_params(dev, ABS_RX, -32768, 32767, 16, 128);
	input_set_abs_params(dev, ABS_Y, -32768, 32767, 16, 128);
	input_set_abs_params(dev, ABS_RY, -32768, 32767, 16, 128);
	input_set_abs_params(dev, ABS_Z, 0, 1023, 0, 0);
	input_set_abs_params(dev, ABS_RZ, 0, 1023, 0, 0);
	input_set_abs_params(dev, ABS_HAT0X, -1, 1, 0, 0);
	input_set_abs_params(dev, ABS_HAT0Y, -1, 1, 0, 0);

	err = gip_gamepad_init_rumble(gamepad);
	if (err) {
		dev_err(&gamepad->client->dev, "%s: init rumble failed: %d\n",
			__func__, err);
		goto err_delete_timer;
	}

	err = input_register_device(dev);
	if (err) {
		dev_err(&gamepad->client->dev, "%s: register failed: %d\n",
			__func__, err);
		goto err_delete_timer;
	}

	return 0;

err_delete_timer:
	del_timer_sync(&gamepad->rumble.timer);

	return err;
}

static int gip_gamepad_op_battery(struct gip_client *client,
				  enum gip_battery_type type,
				  enum gip_battery_level level)
{
	struct gip_gamepad *gamepad = dev_get_drvdata(&client->dev);

	gip_report_battery(&gamepad->battery, type, level);

	return 0;
}

static int gip_gamepad_op_authenticate(struct gip_client *client,
				       void *data, u32 len)
{
	struct gip_gamepad *gamepad = dev_get_drvdata(&client->dev);

	return gip_auth_process_pkt(&gamepad->auth, data, len);
}

static int gip_gamepad_op_guide_button(struct gip_client *client, bool down)
{
	struct gip_gamepad *gamepad = dev_get_drvdata(&client->dev);

	input_report_key(gamepad->input.dev, BTN_MODE, down);
	input_sync(gamepad->input.dev);

	return 0;
}

static int gip_gamepad_op_firmware(struct gip_client *client, void *data, u32 len)
{
    // First, ensure the data is of the correct size.
    // This will probably footgun us later.
    struct gip_gamepad_pkt_firmware *pkt = data;
    if (len < sizeof (*pkt))
        return -EINVAL;

    // Grab our controller
    struct gip_gamepad *gamepad = dev_get_drvdata(&client->dev);
    struct input_dev *dev = gamepad->input.dev;

	input_report_key(dev, BTN_TRIGGER_HAPPY5, pkt->paddles & GIP_GP_BTN_P1);
	input_report_key(dev, BTN_TRIGGER_HAPPY6, pkt->paddles & GIP_GP_BTN_P2);
	input_report_key(dev, BTN_TRIGGER_HAPPY7, pkt->paddles & GIP_GP_BTN_P3);
	input_report_key(dev, BTN_TRIGGER_HAPPY8, pkt->paddles & GIP_GP_BTN_P4);

    input_sync(dev);

    return 0;
}

static int gip_gamepad_op_input(struct gip_client *client, void *data, u32 len)
{
	struct gip_gamepad *gamepad = dev_get_drvdata(&client->dev);
	struct gip_gamepad_pkt_input *pkt = data;
	struct input_dev *dev = gamepad->input.dev;
	u16 buttons;
	u8 share_offset = GIP_GP_BTN_SHARE_OFFSET;

	if (len < sizeof(*pkt))
		return -EINVAL;

	buttons = le16_to_cpu(pkt->buttons);

	/* share button byte is always at fixed offset from end of packet */
	if (gamepad->supports_share) {
		if (gamepad->supports_dli)
			share_offset += sizeof(struct gip_gamepad_pkt_dli);

		if (len < share_offset)
			return -EINVAL;

		input_report_key(dev, KEY_RECORD,
				 ((u8 *)data)[len - share_offset]);
	}

	input_report_key(dev, BTN_START, buttons & GIP_GP_BTN_MENU);
	input_report_key(dev, BTN_SELECT, buttons & GIP_GP_BTN_VIEW);
	input_report_key(dev, BTN_A, buttons & GIP_GP_BTN_A);
	input_report_key(dev, BTN_B, buttons & GIP_GP_BTN_B);
	input_report_key(dev, BTN_X, buttons & GIP_GP_BTN_X);
	input_report_key(dev, BTN_Y, buttons & GIP_GP_BTN_Y);
	input_report_key(dev, BTN_TL, buttons & GIP_GP_BTN_BUMPER_L);
	input_report_key(dev, BTN_TR, buttons & GIP_GP_BTN_BUMPER_R);
	input_report_key(dev, BTN_THUMBL, buttons & GIP_GP_BTN_STICK_L);
	input_report_key(dev, BTN_THUMBR, buttons & GIP_GP_BTN_STICK_R);


	// For anyone comparing to xpad's paddle handling source,
    // xone strips four bytes of header off of the beginning that xpad doesn't, so all offsets are 4 less
    // later revisions put paddle support in the firmware packet, check gip_gamepad_op_WTFEVER
    if ((gamepad->paddle_support == PADDLE_ELITE2_510) && (len > 18)) { // Assume the controller might not send profile data.
        // On the Elite Series 2 with newer-ISH firmware (<=5.10) paddles are stored at byte 18 (22)
        u8 paddles = ((u8 *) data)[18];

        // But first, ensure a profile is not applied, like xpad.
        if ((len > 19) && ((u8 *) data)[19] != 0)
            paddles = 0;

        input_report_key(dev, BTN_TRIGGER_HAPPY5, paddles & GIP_GP_BTN_P1);
        input_report_key(dev, BTN_TRIGGER_HAPPY6, paddles & GIP_GP_BTN_P2);
        input_report_key(dev, BTN_TRIGGER_HAPPY7, paddles & GIP_GP_BTN_P3);
        input_report_key(dev, BTN_TRIGGER_HAPPY8, paddles & GIP_GP_BTN_P4);
    } else if ((gamepad->paddle_support == PADDLE_ELITE2_4X) && (len > 14)) {
		// On the Elite Series 2 with older firmware (<5.11) paddles are stored at byte 14 (18)
		u8 paddles = ((u8 *) data)[14];

        // But first, ensure a profile is not applied, like xpad.
        if ((len > 15) && ((u8 *) data)[15] != 0)
            paddles = 0;

		input_report_key(dev, BTN_TRIGGER_HAPPY5, paddles & GIP_GP_BTN_P1);
		input_report_key(dev, BTN_TRIGGER_HAPPY6, paddles & GIP_GP_BTN_P2);
		input_report_key(dev, BTN_TRIGGER_HAPPY7, paddles & GIP_GP_BTN_P3);
		input_report_key(dev, BTN_TRIGGER_HAPPY8, paddles & GIP_GP_BTN_P4);
	} else if ((gamepad->paddle_support == PADDLE_ELITE) && (len > 28)){
			// On the original Elite, paddles are stored at byte 28
		u8 paddles = ((u8 *) data)[28];
		input_report_key(dev, BTN_TRIGGER_HAPPY5, paddles & GIP_GP_BTN_P2);
		input_report_key(dev, BTN_TRIGGER_HAPPY6, paddles & GIP_GP_BTN_P4);
		input_report_key(dev, BTN_TRIGGER_HAPPY7, paddles & GIP_GP_BTN_P1);
		input_report_key(dev, BTN_TRIGGER_HAPPY8, paddles & GIP_GP_BTN_P3);
	}

	input_report_abs(dev, ABS_X, (s16)le16_to_cpu(pkt->stick_left_x));
	input_report_abs(dev, ABS_RX, (s16)le16_to_cpu(pkt->stick_right_x));
	input_report_abs(dev, ABS_Y, ~(s16)le16_to_cpu(pkt->stick_left_y));
	input_report_abs(dev, ABS_RY, ~(s16)le16_to_cpu(pkt->stick_right_y));
	input_report_abs(dev, ABS_Z, le16_to_cpu(pkt->trigger_left));
	input_report_abs(dev, ABS_RZ, le16_to_cpu(pkt->trigger_right));
	input_report_abs(dev, ABS_HAT0X, !!(buttons & GIP_GP_BTN_DPAD_R) -
					 !!(buttons & GIP_GP_BTN_DPAD_L));
	input_report_abs(dev, ABS_HAT0Y, !!(buttons & GIP_GP_BTN_DPAD_D) -
					 !!(buttons & GIP_GP_BTN_DPAD_U));
	input_sync(dev);

	return 0;
}

static int gip_gamepad_probe(struct gip_client *client)
{
	struct gip_gamepad *gamepad;
	int err;

	gamepad = devm_kzalloc(&client->dev, sizeof(*gamepad), GFP_KERNEL);
	if (!gamepad)
		return -ENOMEM;

	gamepad->client = client;

	err = gip_set_power_mode(client, GIP_PWR_ON);
	if (err)
		return err;

	err = gip_gamepad_init_extra_data(gamepad);
	if (err)
		return err;

	err = gip_init_battery(&gamepad->battery, client, GIP_GP_NAME);
	if (err)
		return err;

	err = gip_init_led(&gamepad->led, client);
	if (err)
		return err;

	err = gip_auth_start_handshake(&gamepad->auth, client);
	if (err)
		return err;

	err = gip_init_input(&gamepad->input, client, GIP_GP_NAME);
	if (err)
		return err;

	err = gip_gamepad_init_input(gamepad);
	if (err)
		return err;



	dev_set_drvdata(&client->dev, gamepad);

	return 0;
}

static void gip_gamepad_remove(struct gip_client *client)
{
	struct gip_gamepad *gamepad = dev_get_drvdata(&client->dev);

	del_timer_sync(&gamepad->rumble.timer);
}

static struct gip_driver gip_gamepad_driver = {
	.name = "xone-gip-gamepad",
	.class = "Windows.Xbox.Input.Gamepad",
	.ops = {
		.battery = gip_gamepad_op_battery,
		.authenticate = gip_gamepad_op_authenticate,
		.guide_button = gip_gamepad_op_guide_button,
		.input = gip_gamepad_op_input,
        .firmware = gip_gamepad_op_firmware,
	},
	.probe = gip_gamepad_probe,
	.remove = gip_gamepad_remove,
};
module_gip_driver(gip_gamepad_driver);

MODULE_ALIAS("gip:Windows.Xbox.Input.Gamepad");
MODULE_AUTHOR("Severin von Wnuck-Lipinski <severinvonw@outlook.de>");
MODULE_DESCRIPTION("xone GIP gamepad driver");
MODULE_VERSION("#VERSION#");
MODULE_LICENSE("GPL");
