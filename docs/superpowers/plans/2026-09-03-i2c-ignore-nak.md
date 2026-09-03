# Generic I2C "Ignore NAK" Framework Hook Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Let any I2C slave-device driver mark its chip as "ignore NAK" (via the existing `i2c_set_chip_flags()` API) or let a caller set `I2C_M_IGNORE_NAK` per-transaction, so that `-EREMOTEIO` (peer did not ACK) is treated as success automatically by the uclass, with zero changes required in existing bus drivers.

**Architecture:** Add `DM_I2C_CHIP_IGNORE_NAK` to `enum dm_i2c_chip_flags` (`include/i2c.h`) and propagate it into `I2C_M_IGNORE_NAK` on the messages the uclass builds. Introduce one static wrapper, `i2c_xfer()`, in `drivers/i2c/i2c-uclass.c` that calls `ops->xfer()` and suppresses `-EREMOTEIO` when every message in the batch allows it; route all existing `ops->xfer()` call sites through it, and apply the same suppression to the `ops->probe_chip()` path inline.

**Tech Stack:** U-Boot driver model (DM), C, U-Boot sandbox unit tests (`test/dm/i2c.c`, `./test/py` sandbox test runner via `pytest`/`test/dm` UT framework).

## Global Constraints

- Spec source of truth: `docs/superpowers/specs/2026-09-02-i2c-ignore-nak-design.md`. Follow it exactly; do not add DT bindings, do not modify `mt7621_i2c.c`/`tegra186_bpmp_i2c.c`, do not attempt true continue-after-NAK bit-level semantics.
- Suppression triggers only on `-EREMOTEIO`, and only when **every** message in the batch has `I2C_M_IGNORE_NAK` set.
- No new public API functions — reuse `i2c_set_chip_flags()` / `i2c_get_chip_flags()` and the existing `I2C_M_IGNORE_NAK` enum value (already defined at `include/i2c.h:138`, just unused today).
- All code in `drivers/i2c/i2c-uclass.c` follows existing style in that file (tabs for indentation, `Return:` doc-comment convention already used, e.g. at `i2c_probe_chip()`'s doc comment around line 259-266).

---

### Task 1: Sandbox test infrastructure to force a NACK (`-EREMOTEIO`)

**Files:**
- Modify: `arch/sandbox/include/asm/test.h:85-92` (add enum value + doc)
- Modify: `drivers/misc/i2c_eeprom_emul.c:72-145` (`sandbox_i2c_eeprom_xfer`)
- Test: `test/dm/i2c.c` (new test function)

**Interfaces:**
- Consumes: existing `sandbox_i2c_eeprom_set_test_mode(struct udevice *dev, enum sandbox_i2c_eeprom_test_mode mode)` (`drivers/misc/i2c_eeprom_emul.c:35`), existing `i2c_get_chip()`, `dm_i2c_read()`.
- Produces: new enum value `SIE_TEST_MODE_NAK` that later tasks (Task 2) rely on to force `dm_i2c_read()`/`dm_i2c_write()` to observe `-EREMOTEIO` from the emulator.

- [ ] **Step 1: Write the failing test**

Add to `test/dm/i2c.c`, after `dm_test_i2c_bytewise()` (after its `DM_TEST(...)` line, i.e. after line 167):

```c
static int dm_test_i2c_ignore_nak(struct unit_test_state *uts)
{
	struct udevice *bus, *dev, *eeprom;
	uint8_t buf[5];

	ut_assertok(uclass_get_device_by_seq(UCLASS_I2C, busnum, &bus));
	ut_assertok(i2c_get_chip(bus, chip, 1, &dev));
	ut_assertok(uclass_first_device_err(UCLASS_I2C_EMUL, &eeprom));
	ut_assertnonnull(eeprom);

	/* Baseline: normal read works before we force a NACK */
	ut_assertok(dm_i2c_read(dev, 0, buf, 5));

	/* Force every transaction on this emulator to NACK */
	sandbox_i2c_eeprom_set_test_mode(eeprom, SIE_TEST_MODE_NAK);
	ut_asserteq(-EREMOTEIO, dm_i2c_read(dev, 0, buf, 5));
	ut_asserteq(-EREMOTEIO, dm_i2c_write(dev, 0, (uint8_t *)"A", 1));

	/* Restore defaults */
	sandbox_i2c_eeprom_set_test_mode(eeprom, SIE_TEST_MODE_NONE);
	ut_assertok(dm_i2c_read(dev, 0, buf, 5));

	return 0;
}
DM_TEST(dm_test_i2c_ignore_nak, UTF_SCAN_PDATA | UTF_SCAN_FDT);
```

- [ ] **Step 2: Run test to verify it fails**

Run:
```bash
cd /home/sj/u-boot
./tools/buildman/buildman -b sii9022x_bridge_driver sandbox 2>/dev/null; \
make O=build-sandbox sandbox_defconfig >/dev/null && \
make O=build-sandbox -j"$(nproc)" >/dev/null && \
./build-sandbox/u-boot -T -c "ut dm i2c_ignore_nak"
```
Expected: build failure — `error: 'SIE_TEST_MODE_NAK' undeclared` (the enum value doesn't exist yet). This confirms the test is exercising code that isn't implemented.

- [ ] **Step 3: Implement the minimal test infrastructure**

In `arch/sandbox/include/asm/test.h`, change lines 85-89 from:
```c
enum sandbox_i2c_eeprom_test_mode {
	SIE_TEST_MODE_NONE,
	/* Permits read/write of only one byte per I2C transaction */
	SIE_TEST_MODE_SINGLE_BYTE,
};
```
to:
```c
enum sandbox_i2c_eeprom_test_mode {
	SIE_TEST_MODE_NONE,
	/* Permits read/write of only one byte per I2C transaction */
	SIE_TEST_MODE_SINGLE_BYTE,
	/* Makes every transaction fail as if the slave did not ACK */
	SIE_TEST_MODE_NAK,
};
```

In `drivers/misc/i2c_eeprom_emul.c`, in `sandbox_i2c_eeprom_xfer()` (line 72), add the NACK check right after the existing debug/priv->prev_addr bookkeeping (after line 83, before the `for` loop at line 85):

```c
static int sandbox_i2c_eeprom_xfer(struct udevice *emul, struct i2c_msg *msg,
				  int nmsgs)
{
	struct sandbox_i2c_flash *priv = dev_get_priv(emul);
	struct sandbox_i2c_flash_plat_data *plat = dev_get_plat(emul);
	uint offset = msg->addr & plat->chip_addr_offset_mask;

	debug("\n%s\n", __func__);
	debug_buffer(0, priv->data, 1, 16, 0);

	/* store addr for testing visibity */
	priv->prev_addr = msg->addr;

	if (plat->test_mode == SIE_TEST_MODE_NAK)
		return -EREMOTEIO;

	for (; nmsgs > 0; nmsgs--, msg++) {
```
(the rest of the function is unchanged — only the new `if` block is inserted between the existing `priv->prev_addr = msg->addr;` line and the `for` loop).

- [ ] **Step 4: Run test to verify it passes**

Run:
```bash
cd /home/sj/u-boot
make O=build-sandbox -j"$(nproc)" >/dev/null && \
./build-sandbox/u-boot -T -c "ut dm i2c_ignore_nak"
```
Expected: `Test: dm_test_i2c_ignore_nak: i2c.c` ... `Failures: 0`

- [ ] **Step 5: Commit**

```bash
cd /home/sj/u-boot
git add arch/sandbox/include/asm/test.h drivers/misc/i2c_eeprom_emul.c test/dm/i2c.c
git commit -m "test: dm: i2c: add sandbox test mode to force a simulated NACK"
```

---

### Task 2: Add `DM_I2C_CHIP_IGNORE_NAK` flag and generic suppression wrapper

**Files:**
- Modify: `include/i2c.h:29-33` (`enum dm_i2c_chip_flags`), `include/i2c.h:129-131` (stale comment)
- Modify: `drivers/i2c/i2c-uclass.c` (new `i2c_xfer()` helper; edits at `i2c_setup_offset()` ~line 67, `dm_i2c_read()` ~line 152, `i2c_read_bytewise()` ~line 101, `i2c_write_bytewise()` ~line 125, `dm_i2c_write()` ~line 207, `dm_i2c_xfer()` ~line 221, `i2c_probe_chip()` ~lines 274-289)
- Test: `test/dm/i2c.c` (extend `dm_test_i2c_ignore_nak` from Task 1)

**Interfaces:**
- Consumes: `SIE_TEST_MODE_NAK` and `dm_test_i2c_ignore_nak()` from Task 1; existing `i2c_set_chip_flags(struct udevice *dev, uint flags)` / `i2c_get_chip_flags()` (`drivers/i2c/i2c-uclass.c:518,535`); existing `i2c_get_ops(bus)` helper already used throughout `i2c-uclass.c`.
- Produces: `DM_I2C_CHIP_IGNORE_NAK` (new value in `enum dm_i2c_chip_flags`, `include/i2c.h`) and the static function `static int i2c_xfer(struct udevice *bus, struct i2c_msg *msg, int nmsgs)` in `drivers/i2c/i2c-uclass.c`, used only within that file.

- [ ] **Step 1: Extend the failing test**

Replace the `dm_test_i2c_ignore_nak()` body written in Task 1 with the full version below (same file, `test/dm/i2c.c`):

```c
static int dm_test_i2c_ignore_nak(struct unit_test_state *uts)
{
	struct udevice *bus, *dev, *eeprom;
	uint8_t buf[5];

	ut_assertok(uclass_get_device_by_seq(UCLASS_I2C, busnum, &bus));
	ut_assertok(i2c_get_chip(bus, chip, 1, &dev));
	ut_assertok(uclass_first_device_err(UCLASS_I2C_EMUL, &eeprom));
	ut_assertnonnull(eeprom);

	/* Baseline: normal read works before we force a NACK */
	ut_assertok(dm_i2c_read(dev, 0, buf, 5));

	/* Force every transaction on this emulator to NACK */
	sandbox_i2c_eeprom_set_test_mode(eeprom, SIE_TEST_MODE_NAK);

	/* Without the flag, the NACK still propagates as an error */
	ut_asserteq(-EREMOTEIO, dm_i2c_read(dev, 0, buf, 5));
	ut_asserteq(-EREMOTEIO, dm_i2c_write(dev, 0, (uint8_t *)"A", 1));
	ut_asserteq(-EREMOTEIO, dm_i2c_probe(bus, chip, 0, &dev));

	/* With the flag set, the same NACK is suppressed and treated as success */
	ut_assertok(i2c_get_chip(bus, chip, 1, &dev));
	ut_assertok(i2c_set_chip_flags(dev, DM_I2C_CHIP_IGNORE_NAK));
	ut_assertok(dm_i2c_read(dev, 0, buf, 5));
	ut_assertok(dm_i2c_write(dev, 0, (uint8_t *)"A", 1));
	ut_assertok(dm_i2c_probe(bus, chip, DM_I2C_CHIP_IGNORE_NAK, &dev));

	/* Restore defaults */
	ut_assertok(i2c_set_chip_flags(dev, 0));
	sandbox_i2c_eeprom_set_test_mode(eeprom, SIE_TEST_MODE_NONE);
	ut_assertok(dm_i2c_read(dev, 0, buf, 5));

	return 0;
}
DM_TEST(dm_test_i2c_ignore_nak, UTF_SCAN_PDATA | UTF_SCAN_FDT);
```

- [ ] **Step 2: Run test to verify it fails**

Run:
```bash
cd /home/sj/u-boot
make O=build-sandbox -j"$(nproc)" >/dev/null 2>build-sandbox/err.log; \
grep -q "error" build-sandbox/err.log && cat build-sandbox/err.log
```
Expected: build failure — `error: 'DM_I2C_CHIP_IGNORE_NAK' undeclared` (flag doesn't exist yet). If the build somehow succeeds (it shouldn't), the immediate next check is running the test and confirming it fails with `ut_assertok(i2c_set_chip_flags(dev, DM_I2C_CHIP_IGNORE_NAK))` — either way, confirm failure before proceeding.

- [ ] **Step 3: Add the chip flag**

In `include/i2c.h`, change lines 29-33 from:
```c
enum dm_i2c_chip_flags {
	DM_I2C_CHIP_10BIT	= 1 << 0, /* Use 10-bit addressing */
	DM_I2C_CHIP_RD_ADDRESS	= 1 << 1, /* Send address for each read byte */
	DM_I2C_CHIP_WR_ADDRESS	= 1 << 2, /* Send address for each write byte */
};
```
to:
```c
enum dm_i2c_chip_flags {
	DM_I2C_CHIP_10BIT	= 1 << 0, /* Use 10-bit addressing */
	DM_I2C_CHIP_RD_ADDRESS	= 1 << 1, /* Send address for each read byte */
	DM_I2C_CHIP_WR_ADDRESS	= 1 << 2, /* Send address for each write byte */
	DM_I2C_CHIP_IGNORE_NAK	= 1 << 3, /* Treat a NAK from this chip as success */
};
```

Also update the stale comment at `include/i2c.h:129-131`. Change:
```c
/*
 * Not all of these flags are implemented in the U-Boot API
 */
enum dm_i2c_msg_flags {
```
to:
```c
/*
 * Not all of these flags are implemented in the U-Boot API. I2C_M_IGNORE_NAK
 * is implemented generically by the uclass (see dm_i2c_xfer()); the rest are
 * only honoured by drivers that explicitly check for them.
 */
enum dm_i2c_msg_flags {
```

- [ ] **Step 4: Add the `i2c_xfer()` suppression wrapper**

In `drivers/i2c/i2c-uclass.c`, add this new static function immediately before `i2c_read_bytewise()` (i.e. right after the closing brace of `i2c_setup_offset()`, before line 80):

```c
/**
 * i2c_xfer() - perform an I2C transfer, honoring I2C_M_IGNORE_NAK
 *
 * Wraps ops->xfer(). If the driver reports -EREMOTEIO (peer did not ACK)
 * and every message in this transfer permits ignoring NAK, the failure
 * is suppressed and treated as success.
 *
 * @bus:	I2C bus device
 * @msg:	Array of messages to transfer
 * @nmsgs:	Number of messages
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

- [ ] **Step 5: Propagate the chip flag into message flags**

In `drivers/i2c/i2c-uclass.c`, `i2c_setup_offset()` (line 67), change:
```c
	msg->flags = chip->flags & DM_I2C_CHIP_10BIT ? I2C_M_TEN : 0;
```
to:
```c
	msg->flags = chip->flags & DM_I2C_CHIP_10BIT ? I2C_M_TEN : 0;
	msg->flags |= chip->flags & DM_I2C_CHIP_IGNORE_NAK ? I2C_M_IGNORE_NAK : 0;
```

In `dm_i2c_read()` (line 152), change:
```c
		ptr->flags = chip->flags & DM_I2C_CHIP_10BIT ? I2C_M_TEN : 0;
		ptr->flags |= I2C_M_RD;
```
to:
```c
		ptr->flags = chip->flags & DM_I2C_CHIP_10BIT ? I2C_M_TEN : 0;
		ptr->flags |= chip->flags & DM_I2C_CHIP_IGNORE_NAK ? I2C_M_IGNORE_NAK : 0;
		ptr->flags |= I2C_M_RD;
```

In `i2c_probe_chip()` (line 285), change:
```c
	msg->flags = chip_flags & DM_I2C_CHIP_10BIT ? I2C_M_TEN : 0;
```
to:
```c
	msg->flags = chip_flags & DM_I2C_CHIP_10BIT ? I2C_M_TEN : 0;
	msg->flags |= chip_flags & DM_I2C_CHIP_IGNORE_NAK ? I2C_M_IGNORE_NAK : 0;
```

- [ ] **Step 6: Route all `ops->xfer()` call sites through `i2c_xfer()`**

In `i2c_read_bytewise()` (line 101), change:
```c
		ret = ops->xfer(bus, msg, ptr - msg);
```
to:
```c
		ret = i2c_xfer(bus, msg, ptr - msg);
```

In `i2c_write_bytewise()` (line 125), change:
```c
		ret = ops->xfer(bus, msg, 1);
```
to:
```c
		ret = i2c_xfer(bus, msg, 1);
```

In `dm_i2c_read()` (line 160), change:
```c
	return ops->xfer(bus, msg, msg_count);
```
to:
```c
	return i2c_xfer(bus, msg, msg_count);
```

In `dm_i2c_write()` (line 207), change:
```c
	ret = ops->xfer(bus, msg, 1);
```
to:
```c
	ret = i2c_xfer(bus, msg, 1);
```

In `dm_i2c_xfer()` (lines 213-222), change:
```c
int dm_i2c_xfer(struct udevice *dev, struct i2c_msg *msg, int nmsgs)
{
	struct udevice *bus = dev_get_parent(dev);
	struct dm_i2c_ops *ops = i2c_get_ops(bus);

	if (!ops->xfer)
		return -ENOSYS;

	return ops->xfer(bus, msg, nmsgs);
}
```
to:
```c
int dm_i2c_xfer(struct udevice *dev, struct i2c_msg *msg, int nmsgs)
{
	struct udevice *bus = dev_get_parent(dev);
	struct dm_i2c_ops *ops = i2c_get_ops(bus);

	if (!ops->xfer)
		return -ENOSYS;

	return i2c_xfer(bus, msg, nmsgs);
}
```

In `i2c_probe_chip()` (lines 267-290), change:
```c
static int i2c_probe_chip(struct udevice *bus, uint chip_addr,
			  enum dm_i2c_chip_flags chip_flags)
{
	struct dm_i2c_ops *ops = i2c_get_ops(bus);
	struct i2c_msg msg[1];
	int ret;

	if (ops->probe_chip) {
		ret = ops->probe_chip(bus, chip_addr, chip_flags);
		if (ret != -ENOSYS)
			return ret;
	}

	if (!ops->xfer)
		return -ENOSYS;

	/* Probe with a zero-length message */
	msg->addr = chip_addr;
	msg->flags = chip_flags & DM_I2C_CHIP_10BIT ? I2C_M_TEN : 0;
	msg->flags |= chip_flags & DM_I2C_CHIP_IGNORE_NAK ? I2C_M_IGNORE_NAK : 0;
	msg->len = 0;
	msg->buf = NULL;

	return ops->xfer(bus, msg, 1);
}
```
to:
```c
static int i2c_probe_chip(struct udevice *bus, uint chip_addr,
			  enum dm_i2c_chip_flags chip_flags)
{
	struct dm_i2c_ops *ops = i2c_get_ops(bus);
	struct i2c_msg msg[1];
	int ret;

	if (ops->probe_chip) {
		ret = ops->probe_chip(bus, chip_addr, chip_flags);
		if (ret != -ENOSYS) {
			if (ret == -EREMOTEIO &&
			    (chip_flags & DM_I2C_CHIP_IGNORE_NAK))
				ret = 0;
			return ret;
		}
	}

	if (!ops->xfer)
		return -ENOSYS;

	/* Probe with a zero-length message */
	msg->addr = chip_addr;
	msg->flags = chip_flags & DM_I2C_CHIP_10BIT ? I2C_M_TEN : 0;
	msg->flags |= chip_flags & DM_I2C_CHIP_IGNORE_NAK ? I2C_M_IGNORE_NAK : 0;
	msg->len = 0;
	msg->buf = NULL;

	return i2c_xfer(bus, msg, 1);
}
```

- [ ] **Step 7: Run test to verify it passes**

Run:
```bash
cd /home/sj/u-boot
make O=build-sandbox -j"$(nproc)" && \
./build-sandbox/u-boot -T -c "ut dm i2c_ignore_nak" && \
./build-sandbox/u-boot -T -c "ut dm i2c"
```
Expected: both commands report `Failures: 0` (the second command runs the full `test/dm/i2c.c` suite — `dm_test_i2c_find`, `dm_test_i2c_read_write`, `dm_test_i2c_speed`, `dm_test_i2c_offset_len`, `dm_test_i2c_probe_empty`, `dm_test_i2c_bytewise`, `dm_test_i2c_offset`, `dm_test_i2c_ignore_nak`, etc. — to confirm no regressions).

- [ ] **Step 8: Commit**

```bash
cd /home/sj/u-boot
git add include/i2c.h drivers/i2c/i2c-uclass.c test/dm/i2c.c
git commit -m "i2c: add generic DM_I2C_CHIP_IGNORE_NAK / I2C_M_IGNORE_NAK support"
```

---

## Self-Review Notes

- **Spec coverage:** Section 1 (data model) → Task 2 Step 3. Section 2 (propagation) → Task 2 Step 5. Section 3 (wrapper + call sites) → Task 2 Steps 4 and 6. Section 4 (semantics: all-messages rule) → implemented in the wrapper itself (Step 4) and exercised by the test. Section 5 (backward compat with `mt7621_i2c.c`/`tegra186_bpmp_i2c.c`) → no code change needed, verified by reasoning in the spec (their own handling returns 0 before the wrapper would see an error); no action item. Testing section of the spec → Tasks 1 and 2 together implement exactly the sandbox test described.
- **No placeholders:** every step shows full, exact code — no "add appropriate handling" language.
- **Type/signature consistency:** `i2c_xfer(struct udevice *bus, struct i2c_msg *msg, int nmsgs)` is defined once in Task 2 Step 4 and used identically (same name, same argument order) at every call site touched in Step 6. `SIE_TEST_MODE_NAK` is defined in Task 1 and consumed unchanged in Task 2's test.
