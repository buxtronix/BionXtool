# BionX CANBus Protocol Specification

The BionX system communicates over CANBus (125kbps), with the Console typically acting as the Bus Master.

![BionX pinout](images/pinout.png)

## Node Network

| Node Name | Node ID (Master) | Node ID (Slave) | Description |
| :--- | :--- | :--- | :--- |
| **Console** | `0x08` | `0x48` | The master controller and display unit. |
| **Battery** | `0x10` | `0x50` | The Battery Management System (BMS). |
| **Motor**   | `0x20` | `0x60` | The motor controller (integrated in the hub). |
| **BIB**     | `0x58` | `0x58` | BionX Interface Box (for PC diagnostic tools). |

---

## Packet Format

BionX uses standard CAN 2.0A frames with 11-bit identifiers. In normal operation
all Query and Set packets are sent from the console. The battery and motor never
initiate communication.

### 1. Query Register (REQ)
Requests the value of a specific register from a node.
- **CAN ID**: Target Node ID (e.g., `0x10` for Battery)
- **DLC**: `2`
- **Data**: `[0x00, Register_ID]`

### 2. Reply Register (RESP)
Sent by a node in response to a Query.
- **CAN ID**: Requester Node ID (typically `0x08` for Console)
- **DLC**: `4`
- **Data**: `[0x00, Register_ID, 0x00, Value]`

### 3. Set Register (SET)
Command to change a register value on a target node.
- **CAN ID**: Target Node ID
- **DLC**: `4`
- **Data**: `[0x00, Register_ID, 0x00, New_Value]`

---

## Key Registers

For full details, see [registers.h](registers.h).

### Console Registers (`0x08` / `0x48`)
| ID | Name | Description | Unit/Format |
| :--- | :--- | :--- | :--- |
| `0x64-67` | `REG_CONSOLE_STATISTIC_ODOMETER_*` | Total system distance. | 0.1 km |
| `0x81-82` | `REG_CONSOLE_GEOMETRY_CIRC_*` | Wheel circumference for speed calculation. | mm |
| `0x84-85` | `REG_CONSOLE_ASSIST_MAXSPEED_*` | Assist speed limit. | 0.1 km/h |
| `0x87-88` | `REG_CONSOLE_THROTTLE_MAXSPEED_*`| Throttle speed limit. | 0.1 km/h |
| `0x8A`    | `REG_CONSOLE_ASSIST_MINSPEED` | Minimum speed required for assist. | 0.1 km/h |
| `0xB4`    | `REG_CONSOLE_ASSIST_INITLEVEL` | Default assist level on power-up (0-4). | Raw |
| `0xD1`    | `REG_CONSOLE_STATUS_SLAVE` | Write `1` to force console into slave mode. | Boolean |

### Battery Registers (`0x10` / `0x50`)
| ID | Name | Description | Unit/Format |
| :--- | :--- | :--- | :--- |
| `0x1E-1F` | `REG_BATTERY_STATUS_CELLPACK_CURRENT_*` | Real-time current flow (signed). | 0.001 A |
| `0x25`    | `REG_BATTERY_CONFIG_SHUTDOWN` | Write `1` to trigger system power-off. | Trigger |
| `0x28`    | `REG_BATTERY_CONFIG_ACCESSORY_VOLTAGE` | Accessory port output voltage. | 0.1 V |
| `0x32`    | `REG_BATTERY_STATUS_BATTERY_VOLTAGE_NORMALIZED` | Normalized battery voltage (3.7V/cell ref). | % |
| `0x61`    | `REG_BATTERY_STATUS_CHARGE_LEVEL` | Current State of Charge (SoC). | 6.66 % |
| `0x66-69` | `REG_BATTERY_STATUS_TEMPERATURE_SENSOR_*` | Temperature sensors 1-4. | °C |
| `0xA6-A7` | `REG_BATTERY_STATUS_BATTERY_VOLTAGE_HI` | Absolute battery pack voltage. | 0.001 V |

### Motor Registers (`0x20` / `0x60`)
| ID | Name | Description | Unit/Format |
| :--- | :--- | :--- | :--- |
| `0x09`    | `REG_MOTOR_ASSIST_LEVEL` | Current motor assistance level. | 1.5625 % |
| `0x11`    | `REG_MOTOR_STATUS_SPEED` | Motor rotational speed. | rpm |
| `0x14`    | `REG_MOTOR_STATUS_POWER_METER` | Power output being delivered. | 1.5625 % |
| `0x16`    | `REG_MOTOR_STATUS_TEMPERATURE` | Internal motor temperature. | °C |
| `0x21`    | `REG_MOTOR_TORQUE_GAUGE_VALUE` | Raw signal from the torque sensor. | 1.5625 % |
| `0x8B`    | `REG_MOTOR_ASSIST_MAXSPEED` | Speed limit of the motor. | kph |

---

## Register Protection
Critical registers (like speed limits or battery configuration) are often protected.
To modify a protected register:
1.  Write `0xAA` to the node's `UNLOCK` register (e.g., `0xA5` on Motor, `0x71` on Battery).
2.  Write the new value to the target register.
3.  Write `0x00` to the `UNLOCK` register to re-lock.
