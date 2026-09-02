# Generic I2C "Ignore NAK" Framework Hook — Design

## Problem

Some I2C slave devices legitimately don't ACK certain transactions (e.g.
during reset/boot, or by design for specific registers), even though the
transaction should be treated as successful. Today, `I2C_M_IGNORE_NAK` and
`I2C_M_NO_RD_ACK` are defined in `include/i2c.h` (`enum dm_i2c_msg_flags`)
but are not implemented by the generic framework — the comment at
`include/i2c.h:129-131` explicitly says "Not all of these flags are
implemented in the U-Boot API." Only two drivers reference
`I2C_M_IGNORE_NAK` today:

- `drivers/i2c/mt7621_i2c.c` — checks the flag itself and returns success.
- `drivers/i2c/tegra186_bpmp_i2c.c` — allow-lists the flag and forwards it
  to BPMP firmware, but does not act on it locally.

No generic mechanism lets an arbitrary I2C bus driver support ignore-NAK
without reimplementing the logic itself, and no per-chip configuration
exists (a device driver can't declare "this chip may not ACK" once and
have it apply to all `dm_i2c_read`/`dm_i2c_write`/`dm_i2c_probe` calls).

## Goal

Add a generic hook in the I2C uclass (`drivers/i2c/i2c-uclass.c`) so that:

1. A slave-device driver (e.g. a bridge/sensor driver) can mark its chip as
   "ignore NAK" once, and have that apply automatically to all reads/writes/
   probes issued through the standard `dm_i2c_*` API.
2. A caller building raw `struct i2c_msg` lists via `dm_i2c_xfer()` can set
   `I2C_M_IGNORE_NAK` per-transaction for the same effect.
3. Any existing bus driver benefits automatically, with no driver-side code
   changes required, as long as it uses the already-dominant convention of
   returning `-EREMOTEIO` to signal "peer did not ACK" (used by ~35 of the
   in-tree I2C bus drivers, e.g. `designware_i2c.c`, `omap24xx_i2c.c`,
   `rcar_i2c.c`, `mxc_i2c.c`, `npcm_i2c.c`, etc.).

## Non-goals

- True bit-level "continue clocking past a NACK mid-message" semantics
  (as in the Linux kernel's `I2C_FUNC_PROTOCOL_MANGLING` contract). U-Boot's
  driver model doesn't expose per-byte transfer control generically, so this
  design treats "ignore NAK" as "don't fail the overall transfer/probe when
  the peer didn't ACK," not literal continued clocking.
- Fixing the minority of drivers (`mt7621_i2c.c`, `i2c-microchip.c`,
  `sun8i_rsb.c`, and a few others) that report NACK via `-EIO`/`-ENXIO`
  instead of `-EREMOTEIO`. Migrating them to the `-EREMOTEIO` convention is
  a valid follow-up but out of scope here.
- New Device Tree bindings. Per-chip configuration reuses the existing
  `i2c_set_chip_flags()`/`i2c_get_chip_flags()` API, the same mechanism
  already used by `drivers/rtc/ds1307.c`, `drivers/rtc/rv3028.c`, etc. for
  `DM_I2C_CHIP_RD_ADDRESS`/`DM_I2C_CHIP_WR_ADDRESS`.

## Design

### 1. New chip flag

`include/i2c.h`, `enum dm_i2c_chip_flags`:

```c
enum dm_i2c_chip_flags {
	DM_I2C_CHIP_10BIT	= 1 << 0, /* Use 10-bit addressing */
	DM_I2C_CHIP_RD_ADDRESS	= 1 << 1, /* Send address for each read byte */
	DM_I2C_CHIP_WR_ADDRESS	= 1 << 2, /* Send address for each write byte */
	DM_I2C_CHIP_IGNORE_NAK	= 1 << 3, /* Treat NAK as success for this chip */
};
```

No new setter/getter functions are needed — `i2c_set_chip_flags(dev,
DM_I2C_CHIP_IGNORE_NAK)` and `i2c_get_chip_flags()` already exist
(`drivers/i2c/i2c-uclass.c:518,535`) and are called by device drivers from
their own `probe()`.

### 2. Propagate chip flag into message flag

Mirror the existing `DM_I2C_CHIP_10BIT → I2C_M_TEN` pattern at the three
places in `drivers/i2c/i2c-uclass.c` where message flags are derived from
chip flags:

- `i2c_setup_offset()` (~line 67) — used by `dm_i2c_write()` and the
  bytewise read/write helpers.
- `dm_i2c_read()` (~line 152).
- `i2c_probe_chip()` (~line 285).

At each site, add:

```c
msg->flags |= chip->flags & DM_I2C_CHIP_IGNORE_NAK ? I2C_M_IGNORE_NAK : 0;
```

(using `chip_flags` instead of `chip->flags` in `i2c_probe_chip()`, matching
that function's existing parameter). `i2c_read_bytewise()` and
`i2c_write_bytewise()` inherit the flag automatically since they build their
per-byte messages from `i2c_setup_offset()`'s output.

### 3. Centralized suppression wrapper

Add one static helper in `drivers/i2c/i2c-uclass.c`:

```c
/**
 * i2c_xfer() - perform an I2C transfer, honoring I2C_M_IGNORE_NAK
 *
 * Wraps ops->xfer(). If the driver reports -EREMOTEIO (peer did not ACK)
 * and every message in this transfer permits ignoring NAK, the failure
 * is suppressed and treated as success.
 *
 * @bus: I2C bus device
 * @msg: Array of messages to transfer
 * @nmsgs: Number of messages
 * Return: 0 on success (including suppressed NAK), -ve on other errors
 */
static int i2c_xfer(struct udevice *bus, struct i2c_msg *msg, int nmsgs)
{
	struct dm_i2c_ops *ops = i2c_get_ops(bus);
	int ret = ops->xfer(bus, msg, nmsgs);
	int i;

	if (ret == -EREMOTEIO) {
		for (i = 0; i < nmsgs; i++) {
			if (!(msg[i].flags & I2C_M_IGNORE_NAK))
				return ret;
		}
		ret = 0;
	}

	return ret;
}
```

Replace direct `ops->xfer(...)` calls with `i2c_xfer(...)` in:

- `i2c_read_bytewise()`
- `i2c_write_bytewise()`
- `dm_i2c_read()`
- `dm_i2c_write()`
- `dm_i2c_xfer()`
- `i2c_probe_chip()`'s zero-length-message fallback branch

For `i2c_probe_chip()`'s custom `ops->probe_chip()` branch (which bypasses
`xfer` entirely), apply the same suppression inline:

```c
if (ops->probe_chip) {
	ret = ops->probe_chip(bus, chip_addr, chip_flags);
	if (ret != -ENOSYS) {
		if (ret == -EREMOTEIO && (chip_flags & DM_I2C_CHIP_IGNORE_NAK))
			ret = 0;
		return ret;
	}
}
```

### 4. Suppression semantics

- Suppression only occurs when **every** message in the batch carries
  `I2C_M_IGNORE_NAK`. This avoids silently swallowing a real NACK failure
  on an unrelated message within a mixed multi-message transfer.
- Since the per-chip flag is propagated uniformly to every message the
  uclass itself builds, this "all messages" rule is transparent for the
  common per-chip use case.
- A caller manually constructing a message list for `dm_i2c_xfer()` must
  set `I2C_M_IGNORE_NAK` on all messages in that call to get suppression.

### 5. Backward compatibility

- `mt7621_i2c.c` and `tegra186_bpmp_i2c.c` already have their own
  `I2C_M_IGNORE_NAK` handling and return success themselves when the flag
  is set — the new wrapper is a no-op in that case (it never observes an
  error).
- Drivers not using `-EREMOTEIO` for NACK are unaffected (no regression);
  they simply don't yet benefit from the new hook (see Non-goals).

## Usage example

A bridge driver (e.g. sii902x) that knows its chip won't ACK a specific
transaction would, in its `probe()`:

```c
i2c_set_chip_flags(dev, DM_I2C_CHIP_IGNORE_NAK);
```

After that, all `dm_i2c_read()`/`dm_i2c_write()`/`dm_i2c_probe()` calls for
that device automatically tolerate `-EREMOTEIO` (NACK) as success.

## Testing

- Extend `test/dm/i2c.c` (sandbox I2C emulation) with a case that:
  - Sets `DM_I2C_CHIP_IGNORE_NAK` on a sandbox emulated chip.
  - Forces the emulator's `xfer` to return `-EREMOTEIO`.
  - Asserts `dm_i2c_read()`/`dm_i2c_write()` return 0 instead of the error.
  - Asserts that without the flag set, the same forced `-EREMOTEIO` still
    propagates as an error.
- No hardware-specific driver changes are required for this test since the
  suppression lives entirely in the uclass layer.
