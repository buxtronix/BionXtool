# BionX CANBus Protocol Specification

The BionX system communicates over CANBus (125kbps), with the Console typically acting as the Bus Master.

![BionX pinout](pinout.png)

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
| `0x64-67` | `ODOMETER` | Total system distance. | 0.1 km |
| `0x81-82` | `CIRCUMFERENCE` | Wheel circumference for speed calculation. | mm |
| `0x84-85` | `ASSIST_SPEED` | Assist speed limit. | 0.1 km/h |
| `0x87-88` | `THROTTLE_SPEED`| Throttle speed limit. | 0.1 km/h |
| `0x8A`    | `MIN_SPEED` | Minimum speed required for assist. | 0.1 km/h |
| `0xA3`    | `SW_VERSION` | Console software version. | Hex |
| `0xB4`    | `INIT_LEVEL` | Default assist level on power-up (0-4). | Raw |
| `0xC6`    | `MOUNTAIN_CAP` | Assistance cap in Mountain Mode. | 1.5625 % |
| `0xD1`    | `SLAVE_MODE` | Write `1` to force console into slave mode. | Boolean |

### Battery Registers (`0x10` / `0x50`)
| ID | Name | Description | Unit/Format |
| :--- | :--- | :--- | :--- |
| `0x1E-1F` | `CURRENT` | Real-time current flow (signed). | 0.001 A |
| `0x25`    | `SHUTDOWN` | Write `1` to trigger system power-off. | Trigger |
| `0x28`    | `ACC_VOLTAGE` | Accessory port output voltage. | 0.1 V |
| `0x32`    | `VOLTAGE_NORM` | Normalized battery voltage (3.7V/cell ref). | % |
| `0x3B`    | `HW_VERSION` | Battery hardware version. | Hex |
| `0x61`    | `CHARGE_LEVEL` | Current State of Charge (SoC). | 6.66 % |
| `0x66-69` | `TEMPERATURE` | Temperature sensors 1-4. | °C |
| `0xA6-A7` | `VOLTAGE` | Absolute battery pack voltage. | 0.001 V |

### Motor Registers (`0x20` / `0x60`)
| ID | Name | Description | Unit/Format |
| :--- | :--- | :--- | :--- |
| `0x09`    | `ASSIST_LEVEL` | Current motor assistance level. | 1.5625 % |
| `0x11`    | `SPEED` | Motor rotational speed. | rpm |
| `0x14`    | `POWER_METER` | Power output being delivered. | 1.5625 % |
| `0x16`    | `TEMPERATURE` | Internal motor temperature. | °C |
| `0x21`    | `TORQUE_GAUGE` | Raw signal from the torque sensor. | 1.5625 % |
| `0xA5`    | `UNLOCK` | Write `0xAA` to unlock protected registers. | Key |

---

## Register Protection
Critical registers (like speed limits or battery configuration) are often protected.
To modify a protected register:
1.  Write `0xAA` to the node's `UNLOCK` register (e.g., `0xA5` on Motor, `0x71` on Battery).
2.  Write the new value to the target register.
3.  Write `0x00` to the `UNLOCK` register to re-lock.
