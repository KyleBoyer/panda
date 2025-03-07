const int SUBARU_L_MAX_STEER = 2047; // 1s
// real time torque limit to prevent controls spamming
// the real time limit is 1500/sec
const int SUBARU_L_MAX_RT_DELTA = 940;          // max delta torque allowed for real time checks
const uint32_t SUBARU_L_RT_INTERVAL = 250000;    // 250ms between real time checks
const int SUBARU_L_MAX_RATE_UP = 50;
const int SUBARU_L_MAX_RATE_DOWN = 70;
const int SUBARU_L_DRIVER_TORQUE_ALLOWANCE = 60;
const int SUBARU_L_DRIVER_TORQUE_FACTOR = 10;
const int SUBARU_L_STANDSTILL_THRSLD = 20;  // about 1kph
const uint32_t SUBARU_L_BRAKE_THRSLD = 2; // filter sensor noise, max_brake is 400

const CanMsg SUBARU_L_TX_MSGS[] = {{0x161, 0, 8}, {0x164, 0, 8}, {0x140, 2, 8}};
#define SUBARU_L_TX_MSGS_LEN (sizeof(SUBARU_L_TX_MSGS) / sizeof(SUBARU_L_TX_MSGS[0]))

// TODO: do checksum and counter checks after adding the signals to the outback dbc file
AddrCheckStruct subaru_l_addr_checks[] = {
  {.msg = {{0x140, 0, 8, .expected_timestep = 10000U}, { 0 }, { 0 }}},
  {.msg = {{0x371, 0, 8, .expected_timestep = 20000U}, { 0 }, { 0 }}},
  {.msg = {{0x144, 0, 8, .expected_timestep = 50000U}, { 0 }, { 0 }}},
};
#define SUBARU_L_ADDR_CHECK_LEN (sizeof(subaru_l_addr_checks) / sizeof(subaru_l_addr_checks[0]))
addr_checks subaru_l_rx_checks = {subaru_l_addr_checks, SUBARU_L_ADDR_CHECK_LEN};


// TODO add legacy checksum check

const uint16_t SUBARU_L_PARAM_FLIP_DRIVER_TORQUE = 1;
bool subaru_l_flip_driver_torque = false;

static int subaru_legacy_rx_hook(CAN_FIFOMailBox_TypeDef *to_push) {

  bool valid = addr_safety_check(to_push, &subaru_l_rx_checks,
                            NULL, NULL, NULL);

  if (valid && (GET_BUS(to_push) == 0)) {
    int addr = GET_ADDR(to_push);
    if (addr == 0x371) {
      int torque_driver_new;
      torque_driver_new = (GET_BYTE(to_push, 3) >> 5) + (GET_BYTE(to_push, 4) << 3);
      torque_driver_new = to_signed(torque_driver_new, 11);
      if (subaru_l_flip_driver_torque) {
        torque_driver_new = -1 * torque_driver_new;
      }
      update_sample(&torque_driver, torque_driver_new);
    }

    // enter controls on rising edge of ACC, exit controls on ACC off
    if (addr == 0x144) {
      int cruise_engaged = ((GET_BYTE(to_push, 6) >> 1) & 1);
      if (cruise_engaged && !cruise_engaged_prev) {
        controls_allowed = 1;
      }
      if (!cruise_engaged) {
        controls_allowed = 0;
      }
      cruise_engaged_prev = cruise_engaged;
    }

    // sample wheel speed, averaging opposite corners
    if (addr == 0xD4) {
      int subaru_speed = (GET_BYTES_04(to_push) >> 16) & 0xFFFF;  // FR
      subaru_speed += GET_BYTES_48(to_push) & 0xFFFF;  // RL
      subaru_speed /= 2;
      vehicle_moving = subaru_speed > SUBARU_L_STANDSTILL_THRSLD;
    }

    if (addr == 0xD1) {
      brake_pressed = GET_BYTE(to_push, 2) > SUBARU_L_BRAKE_THRSLD;
    }

    if (addr == 0x140) {
      gas_pressed = GET_BYTE(to_push, 0) != 0;
    }

    generic_rx_checks((addr == 0x164));
  }
  return valid;
}

static int subaru_legacy_tx_hook(CAN_FIFOMailBox_TypeDef *to_send) {
  int tx = 1;
  int addr = GET_ADDR(to_send);

  if (!msg_allowed(to_send, SUBARU_L_TX_MSGS, SUBARU_L_TX_MSGS_LEN)) {
    tx = 0;
  }

  if (relay_malfunction) {
    tx = 0;
  }

  // steer cmd checks
  if (addr == 0x164) {
    int desired_torque = ((GET_BYTES_04(to_send) >> 8) & 0x1FFF);
    bool violation = 0;
    uint32_t ts = microsecond_timer_get();

    desired_torque = -1 * to_signed(desired_torque, 13);

    if (controls_allowed) {

      // *** global torque limit check ***
      violation |= max_limit_check(desired_torque, SUBARU_L_MAX_STEER, -SUBARU_L_MAX_STEER);

      // *** torque rate limit check ***
      violation |= driver_limit_check(desired_torque, desired_torque_last, &torque_driver,
        SUBARU_L_MAX_STEER, SUBARU_L_MAX_RATE_UP, SUBARU_L_MAX_RATE_DOWN,
        SUBARU_L_DRIVER_TORQUE_ALLOWANCE, SUBARU_L_DRIVER_TORQUE_FACTOR);

      // used next time
      desired_torque_last = desired_torque;

      // *** torque real time rate limit check ***
      violation |= rt_rate_limit_check(desired_torque, rt_torque_last, SUBARU_L_MAX_RT_DELTA);

      // every RT_INTERVAL set the new limits
      uint32_t ts_elapsed = get_ts_elapsed(ts, ts_last);
      if (ts_elapsed > SUBARU_L_RT_INTERVAL) {
        rt_torque_last = desired_torque;
        ts_last = ts;
      }
    }

    // no torque if controls is not allowed
    if (!controls_allowed && (desired_torque != 0)) {
      violation = 1;
    }

    // reset to 0 if either controls is not allowed or there's a violation
    if (violation || !controls_allowed) {
      desired_torque_last = 0;
      rt_torque_last = 0;
      ts_last = ts;
    }

    if (violation) {
      tx = 0;
    }

  }
  return tx;
}

static int subaru_legacy_fwd_hook(int bus_num, CAN_FIFOMailBox_TypeDef *to_fwd) {
  int bus_fwd = -1;
  int addr = GET_ADDR(to_fwd);

  if (!relay_malfunction) {
    if (bus_num == 0) {
      // Preglobal platform
      // 0x140 is Throttle
      int block_msg = (addr == 0x140);
      if (!block_msg) {
        bus_fwd = 2;  // Camera CAN
      }
    }
    if (bus_num == 2) {
      // Preglobal platform
      // 0x161 is ES_CruiseThrottle
      // 0x164 is ES_LKAS
      int block_msg = ((addr == 0x161) || (addr == 0x164));
      if (!block_msg) {
        bus_fwd = 0;  // Main CAN
      }
    }
  }
  // fallback to do not forward
  return bus_fwd;
}

static const addr_checks* subaru_legacy_init(int16_t param) {
  controls_allowed = false;
  relay_malfunction_reset();
  // Checking for flip driver torque from safety parameter
  subaru_l_flip_driver_torque = GET_FLAG(param, SUBARU_L_PARAM_FLIP_DRIVER_TORQUE);
  return &subaru_l_rx_checks;
}

const safety_hooks subaru_legacy_hooks = {
  .init = subaru_legacy_init,
  .rx = subaru_legacy_rx_hook,
  .tx = subaru_legacy_tx_hook,
  .tx_lin = nooutput_tx_lin_hook,
  .fwd = subaru_legacy_fwd_hook,
};
