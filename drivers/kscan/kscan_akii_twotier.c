/*
 * AKII two-tier kscan driver -- Apple Keyboard II replica.
 *
 * XIAO nRF52840 + 2x MCP23017 I2C GPIO expanders, driven over RAW I2C
 * (this driver owns the chips; the Zephyr mcp230xx gpio driver is not used).
 *
 * Diodes are physically COL->ROW ("COL2ROW"). MCP23017 has internal pull-UPS
 * only, so we scan QMK-style: rows are outputs driven LOW one at a time, the
 * 18 columns are inputs with pull-ups and read active-low.
 *
 *   mcp_r @ addr-rows : GPA0-4 = ROW0..ROW4 (outputs)
 *                       GPB7..GPB0 = COL0..COL7 (inputs, pull-up)
 *   mcp_c @ reg       : GPA0-7 = COL8..COL15 (inputs, pull-up)
 *                       GPB7   = COL16, GPB6 = COL17 (inputs, pull-up)
 *
 * Two tiers:
 *   ACTIVE -- full matrix scan every active-poll-period-ms (= stock feel).
 *   IDLE   -- after idle-timeout-ms with no key down, drive ALL rows low and
 *             do a cheap "is any column low?" summary read every
 *             idle-poll-period-ms. Any key down -> back to ACTIVE.
 */

#define DT_DRV_COMPAT zmk_kscan_akii_twotier

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/kscan.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(kscan_akii, CONFIG_ZMK_LOG_LEVEL);

#define AKII_ROWS 5
#define AKII_COLS 18

/* MCP23017 register addresses (IOCON.BANK = 0). */
#define REG_IODIRA 0x00
#define REG_IODIRB 0x01
#define REG_GPPUA  0x0C
#define REG_GPPUB  0x0D
#define REG_GPIOA  0x12
#define REG_GPIOB  0x13
#define REG_OLATA  0x14
#define REG_IOCON  0x0A

/* Which read byte a column lives in. */
enum col_src { SRC_R_GPIOB = 0, SRC_C_GPIOA = 1, SRC_C_GPIOB = 2 };

struct col_def {
    uint8_t src;
    uint8_t bit;
};

/* Column wiring -- mirrors the verified board netlist / original overlay. */
static const struct col_def cols[AKII_COLS] = {
    {SRC_R_GPIOB, 7}, {SRC_R_GPIOB, 6}, {SRC_R_GPIOB, 5}, {SRC_R_GPIOB, 4}, /* COL0-3  */
    {SRC_R_GPIOB, 3}, {SRC_R_GPIOB, 2}, {SRC_R_GPIOB, 1}, {SRC_R_GPIOB, 0}, /* COL4-7  */
    {SRC_C_GPIOA, 0}, {SRC_C_GPIOA, 1}, {SRC_C_GPIOA, 2}, {SRC_C_GPIOA, 3}, /* COL8-11 */
    {SRC_C_GPIOA, 4}, {SRC_C_GPIOA, 5}, {SRC_C_GPIOA, 6}, {SRC_C_GPIOA, 7}, /* COL12-15*/
    {SRC_C_GPIOB, 7}, {SRC_C_GPIOB, 6},                                     /* COL16-17*/
};

struct akii_config {
    struct i2c_dt_spec mcp_c; /* columns 8-17 */
    struct i2c_dt_spec mcp_r; /* rows + columns 0-7 */
    uint16_t idle_period_ms;
    uint16_t active_period_ms;
    uint16_t idle_timeout_ms;
    uint8_t debounce_samples;
};

struct key_state {
    bool pressed;
    uint8_t cnt;
};

struct akii_data {
    const struct device *dev;
    kscan_callback_t callback;
    struct k_work_delayable work;
    bool enabled;
    bool idle;
    int pressed_count;
    int64_t last_activity;
    struct key_state keys[AKII_ROWS * AKII_COLS];
};

/* Read the three column bytes: mcp_r GPIOB, mcp_c GPIOA, mcp_c GPIOB. */
static int read_cols(const struct akii_config *cfg, uint8_t out[3])
{
    uint8_t cbuf[2];
    int rc = i2c_reg_read_byte_dt(&cfg->mcp_r, REG_GPIOB, &out[0]);
    if (rc) {
        return rc;
    }
    rc = i2c_burst_read_dt(&cfg->mcp_c, REG_GPIOA, cbuf, 2);
    if (rc) {
        return rc;
    }
    out[1] = cbuf[0]; /* mcp_c GPIOA (COL8-15) */
    out[2] = cbuf[1]; /* mcp_c GPIOB (COL16-17) */
    return 0;
}

static inline bool col_is_pressed(const uint8_t v[3], int c)
{
    return !((v[cols[c].src] >> cols[c].bit) & 1U); /* active low */
}

static int drive_row(const struct akii_config *cfg, int r)
{
    return i2c_reg_write_byte_dt(&cfg->mcp_r, REG_OLATA, (uint8_t)~BIT(r));
}

static int drive_all_rows_low(const struct akii_config *cfg)
{
    return i2c_reg_write_byte_dt(&cfg->mcp_r, REG_OLATA, 0x00);
}

static int mcp_init(const struct akii_config *cfg)
{
    int rc = 0;

    /* mcp_r: GPA0-4 outputs (rows), GPB0-7 inputs+pullup (COL0-7). */
    rc |= i2c_reg_write_byte_dt(&cfg->mcp_r, REG_IOCON, 0x00);
    rc |= i2c_reg_write_byte_dt(&cfg->mcp_r, REG_IODIRA, 0xE0);
    rc |= i2c_reg_write_byte_dt(&cfg->mcp_r, REG_IODIRB, 0xFF);
    rc |= i2c_reg_write_byte_dt(&cfg->mcp_r, REG_GPPUA, 0x00);
    rc |= i2c_reg_write_byte_dt(&cfg->mcp_r, REG_GPPUB, 0xFF);
    rc |= i2c_reg_write_byte_dt(&cfg->mcp_r, REG_OLATA, 0xFF); /* rows idle high */

    /* mcp_c: all inputs + pullup (COL8-17). */
    rc |= i2c_reg_write_byte_dt(&cfg->mcp_c, REG_IOCON, 0x00);
    rc |= i2c_reg_write_byte_dt(&cfg->mcp_c, REG_IODIRA, 0xFF);
    rc |= i2c_reg_write_byte_dt(&cfg->mcp_c, REG_IODIRB, 0xFF);
    rc |= i2c_reg_write_byte_dt(&cfg->mcp_c, REG_GPPUA, 0xFF);
    rc |= i2c_reg_write_byte_dt(&cfg->mcp_c, REG_GPPUB, 0xFF);

    return rc;
}

static void report_change(struct akii_data *data, int r, int c, bool pressed)
{
    if (data->callback) {
        data->callback(data->dev, r, c, pressed);
    }
}

/* Full matrix scan with debounce + reporting. Returns true if any key down. */
static bool active_scan(const struct device *dev)
{
    const struct akii_config *cfg = dev->config;
    struct akii_data *data = dev->data;
    bool any_pressed = false;

    for (int r = 0; r < AKII_ROWS; r++) {
        uint8_t v[3];

        if (drive_row(cfg, r) || read_cols(cfg, v)) {
            LOG_WRN("scan i2c error on row %d", r);
            continue;
        }

        for (int c = 0; c < AKII_COLS; c++) {
            struct key_state *ks = &data->keys[r * AKII_COLS + c];
            bool raw = col_is_pressed(v, c);

            if (raw != ks->pressed) {
                if (++ks->cnt >= cfg->debounce_samples) {
                    ks->pressed = raw;
                    ks->cnt = 0;
                    data->pressed_count += raw ? 1 : -1;
                    data->last_activity = k_uptime_get();
                    report_change(data, r, c, raw);
                }
            } else {
                ks->cnt = 0;
            }

            any_pressed |= ks->pressed;
        }
    }

    return any_pressed;
}

/* Cheap idle check -- rows already all low, one column read. */
static bool idle_any_pressed(const struct device *dev)
{
    const struct akii_config *cfg = dev->config;
    uint8_t v[3];

    if (read_cols(cfg, v)) {
        LOG_WRN("idle i2c error");
        return false;
    }
    for (int c = 0; c < AKII_COLS; c++) {
        if (col_is_pressed(v, c)) {
            return true;
        }
    }
    return false;
}

static void akii_work(struct k_work *work)
{
    struct k_work_delayable *dwork = k_work_delayable_from_work(work);
    struct akii_data *data = CONTAINER_OF(dwork, struct akii_data, work);
    const struct device *dev = data->dev;
    const struct akii_config *cfg = dev->config;
    uint32_t next_ms;

    if (!data->enabled) {
        return;
    }

    if (data->idle) {
        if (idle_any_pressed(dev)) {
            data->idle = false;
            data->last_activity = k_uptime_get();
            active_scan(dev); /* begin debouncing the waking key (reported after
                               * debounce_samples active scans; == 1 by default) */
            next_ms = cfg->active_period_ms;
        } else {
            next_ms = cfg->idle_period_ms;
        }
    } else {
        active_scan(dev);
        if (data->pressed_count <= 0 &&
            (k_uptime_get() - data->last_activity) > cfg->idle_timeout_ms) {
            data->idle = true;
            drive_all_rows_low(cfg);
            next_ms = cfg->idle_period_ms;
        } else {
            next_ms = cfg->active_period_ms;
        }
    }

    k_work_reschedule(&data->work, K_MSEC(next_ms));
}

static int akii_configure(const struct device *dev, kscan_callback_t callback)
{
    struct akii_data *data = dev->data;

    if (!callback) {
        return -EINVAL;
    }
    data->callback = callback;
    return 0;
}

static int akii_enable(const struct device *dev)
{
    struct akii_data *data = dev->data;

    data->enabled = true;
    data->idle = false;
    data->last_activity = k_uptime_get();
    k_work_reschedule(&data->work, K_NO_WAIT);
    return 0;
}

static int akii_disable(const struct device *dev)
{
    struct akii_data *data = dev->data;
    static struct k_work_sync sync;

    data->enabled = false;
    /* Synchronous cancel: blocks until the handler is idle and rejects any
     * in-flight self-reschedule, so no scan runs after disable returns.
     * Safe because disable is called from the layout-switch context, never
     * from the system workqueue thread that runs akii_work. */
    k_work_cancel_delayable_sync(&data->work, &sync);
    return 0;
}

static int akii_init(const struct device *dev)
{
    const struct akii_config *cfg = dev->config;
    struct akii_data *data = dev->data;
    int rc;

    data->dev = dev;

    if (!device_is_ready(cfg->mcp_c.bus)) {
        LOG_ERR("I2C bus not ready");
        return -ENODEV;
    }

    rc = mcp_init(cfg);
    if (rc) {
        LOG_ERR("MCP23017 init failed (%d)", rc);
        return -EIO;
    }

    k_work_init_delayable(&data->work, akii_work);
    LOG_INF("AKII two-tier kscan ready (idle %u ms / active %u ms)",
            cfg->idle_period_ms, cfg->active_period_ms);
    return 0;
}

static const struct kscan_driver_api akii_api = {
    .config = akii_configure,
    .enable_callback = akii_enable,
    .disable_callback = akii_disable,
};

#define AKII_DEBOUNCE_SAMPLES                                                   \
    MAX(1, DIV_ROUND_UP(DT_INST_PROP(0, debounce_press_ms),                    \
                        DT_INST_PROP(0, active_poll_period_ms)))

static const struct akii_config akii_cfg = {
    .mcp_c = I2C_DT_SPEC_INST_GET(0),
    .mcp_r = {
        .bus = DEVICE_DT_GET(DT_INST_BUS(0)),
        .addr = DT_INST_PROP(0, addr_rows),
    },
    .idle_period_ms = DT_INST_PROP(0, idle_poll_period_ms),
    .active_period_ms = DT_INST_PROP(0, active_poll_period_ms),
    .idle_timeout_ms = DT_INST_PROP(0, idle_timeout_ms),
    .debounce_samples = AKII_DEBOUNCE_SAMPLES,
};

static struct akii_data akii_data;

DEVICE_DT_INST_DEFINE(0, akii_init, NULL, &akii_data, &akii_cfg, POST_KERNEL,
                      CONFIG_KSCAN_INIT_PRIORITY, &akii_api);
