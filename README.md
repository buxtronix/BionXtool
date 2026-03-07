# BionXtool

<p align="center">
  <img width="420" height="187" src="images/logo.png">
</p>

**BionXtool** is a tool for diagnostics, configuration, and real-time monitoring of BionX e-bike
components (Console, Battery, and Motor). Together with a cheap (~$10) USB-CAN interface,
it can replace almost all of the functionality of the official BBI Software and hardware
interface (~$100).

It is a fork of the original [BigXionFlasher](https://bigxionflasher.org) project,
with the following enhancements:

- Uses a cheap off the shelf USB Canbus adapter and standard Linux CAN networking
- Read and write any bike registers
- Real-time sniffing of the CANbus for diagnostics and more
- Sniff from PCAP files for offline analysis

I write this fork as I had dusted off my old mothballed BionX system, and needed to adjust some
limits to comply with updated legislation in my jurisdiction. With the company defunct, I couldn't
get a dealer to reset it, and wanted to use a cheap Aliexpress CAN adapter.

> [!WARNING]
> A non-compliant system must not be ridden in public spaces, any limits changed with this tool
> must be compliant with local regulations. As well as legal trouble, raising speed limits can
> put yourself, others, and your bike in danger.

> [!WARNING]
> Setting certain registers may leave your system unusable or difficult to recover. Please take
> extra care when manually setting registers.

---

## 🚀 Key Features

- **System Diagnostics**: View detailed hardware/software versions, serial numbers, and health statistics (cycle counts, temperature, voltage).
- **Configuration**: Modify speed limits (Assist, Throttle, and Min Speed), console preferences, accessory voltage and more
- **Advanced Sniffing**: Real-time or file-based (PCAP) CAN-bus monitoring with automatic register decoding into human-readable names and values.
- **Direct Register Access**: Read and write any specific BionX register from any node using hex IDs or friendly names.

For a detailed description of the BionX CANBus protocol, see [PROTOCOL.md](PROTOCOL.md)..

---

## 🛠 Installation

### Hardware prerequisites
- A BionX e-bike system using the CANBus protocol (post 2009 with G1/G2 console)
- A USB CANBus interface running Candlelight (e.g CANable 2.0)
- A way to physically connect to the bike CANBus (see below).

### Software Prerequisites
- A Linux system with **SocketCAN** support.
- `gcc` and `make` build tools.
- `libncurses` for monitor mode.

### Building from Source
```bash
git clone https://github.com/buxtronix/BionXtool.git
cd BionXtool
make
```

---

## 🔌 Hardware Setup

There are a few ways to interface with the CANBus on the bike. For reference,
see this [wiring diagram](images/pinout.png).

### Connect to the existing plugs

The official way to connect (which is what the dealer interface does) is to
use a tap cable inline where the console connector plug is. To do this, you
need a pair of Hirose connectors (HR30-6J-6P and HR30-6P-6S), wire all 6 pins
up in a straight through configuration, and tap off the GND, CAN_L and CAN_H
connections into the CANBus adapter.

The connectors are however fairly expensive (about $20/pair).

### Tap off the battery connector.

The cheapest way is to tap lines off the battery connector by just soldering
wires to them.

To access this, you need to remove the battery bracket off the bike, unscrew
the two Torx screws underneath, remove the metal bracket and you will get
access to the back of the plug. You may need to remove some hot glue first.
Then you just solder wires to tap off pins 2 (CAN_H), 5 (CAN_L) and GND. You
need to take care not to short or unsolder the wrong wires as there are
high currents and moderate voltages (50v) present.

### Console shim

I have been developing a slim flex PCB that slides under the console where
it taps off the pogo pins. This is quite cheap, about $5 delivered from
JLCPCB, then once mounted you solder wires to the flex PCB that sticks out
from under the console. I'll update here when it's ready.

## 🔌 Software Setup

Most CANable adapters come from the factory with _slcan_ firmware, so you
will need to flash the _candlelight_ firmware onto it instead. This is really
simple, you just plug it into a computer and click a couple of buttons on
the [updater page](https://canable.io/updater/canable2.html).

BionX systems communicate at a bitrate of **125,000 bits/s**. Before running the
tool, you must bring up your CAN interface with the correct settings.

```bash
# Bring up can0 with the BionX bitrate (125k)
sudo ip link set can0 type can bitrate 125000
sudo ip link set can0 up
```

If you are testing without hardware, you can use a virtual CAN interface:
```bash
sudo modprobe vcan
sudo ip link add dev vcan0 type vcan
sudo ip link set vcan0 up
./BionXtool -d vcan0 -S
```
---

## 📖 Usage

BionXtool provides several modes of operation, from high-level configuration to low-level protocol analysis.

### 1. System Overview & Diagnostics
Retrieve a comprehensive report of all connected components, including software versions, battery health, and odometer.

```bash
# Print all system settings and statistics
$ ./BionXtool -s
BionXtool V 0.1 (BionXtool)
 (c) 2026 by Ben Buxton <bbuxton@gmail.com>

console already in slave mode. good!



Console information:
 hardware version ........: 0x15
 software version ........: 0x64
 manufacture date ........: 13/01/2014
 assistance level ........: 3 (0x03)
 part number .............: 03205
 item number .............: 00342
 max limit enabled .......: yes
 speed limit .............: 26.0 km/h
 min limit enabled .......: yes
 min speed limit .........: 0.2 km/h
 throttle limit enabled ..: yes
 throttle speed limit ....: 6.0 km/h
 wheel circumference .....: 2075
 mountain cap ............: 54.69%
 odo .....................: 14258.40 km

Battery information:
 hardware version ........: 0x63
 software version ........: 0x112
 manufacture date ........: 07/09/2017
 part number .............: 05560
 item number .............: 00342
 voltage .................: 53.13 V
 battery level ...........: 100.00%
 maximum voltage .........: 112.08%
 minimum voltage .........: 95.42%
 mean voltage ............: 110.00%
 resets ..................: 0
 lmd .....................: 10.40 Ah
 cell capacity ...........: 2.15 Ah
 charge time worst .......: 113
 charge time mean ........: 0
 charge cycles ...........: 268
 full charge cycles ......: 0
 power cycles ............: 969
 battery temp max ........: 80°C
 battery temp min ........: 239°C
 accessory voltage config.: 8.20 V

 charge level @ 010% : 0015
 charge level @ 020% : 0015
 charge level @ 030% : 0072
 charge level @ 040% : 0088
 charge level @ 050% : 0023
 charge level @ 060% : 0028
 charge level @ 070% : 0020
 charge level @ 080% : 0004
 charge level @ 090% : 0003
 charge level @ 100% : 0003
 total # of charges .: 0271

 balancer enabled ...: no

 voltage cell #01 ...: 4.085V
 voltage cell #02 ...: 4.083V
 voltage cell #03 ...: 4.080V
 voltage cell #04 ...: 4.086V
 voltage cell #05 ...: 4.081V
 voltage cell #06 ...: 4.088V
 voltage cell #07 ...: 4.086V
 voltage cell #08 ...: 4.091V
 voltage cell #09 ...: 4.098V
 voltage cell #10 ...: 4.083V
 voltage cell #11 ...: 4.066V
 voltage cell #12 ...: 4.114V
 voltage cell #13 ...: 4.088V
 temperature pack #01: 29°C
 temperature pack #02: 29°C
 temperature pack #03: 29°C
 temperature pack #04: 29°C
 temperature pack #05: 14°C

Motor information:
 hardware version ........: 0x32
 software version ........: 0x102
 manufacture date ........: 09/11/2017
 temperature .............: 37°C
 speed limit .............: 26 km/h
 wheel circumference .....: 2075
 part number .............: 05989
 item number .............: 00342

```

### 2. Configuring System Limits & Geometry
Adjust the behavior of the bike to match local regulations or your preferences.

```bash
# Set assist speed limit to 32.0 km/h (0 for unlimited)
./BionXtool -l 32.0

# Set throttle speed limit to 20.0 km/h
./BionXtool -t 20.0

# Set minimum assist speed (speed at which motor kicks in)
./BionXtool -m 2.0

# Update wheel circumference (in mm) for accurate speed/distance
./BionXtool -c 2073

# Set initial assist level on power-up (0-4)
./BionXtool -a 2

# Set mountain cap level (assist percentage)
./BionXtool -o 55
```

### 3. Battery & Accessory Configuration
Configure battery-specific settings like accessory port voltage.

```bash
# Set accessory port voltage (6.0V to 14.0V)
./BionXtool -v 12.0

# Enable/Disable accessory power (1=On, 0=Off)
./BionXtool -A 1
```

### 4. CAN Bus Monitoring & Sniffing
Analyze the live communication between the console, battery, and motor.

```bash
# Real-time TUI dashboard (Top-like interface)
./BionXtool -M

# Stream every CAN packet with human-readable decoding
./BionXtool -S

# Stream only when a register value changes
./BionXtool -C

# Analyze a previously captured PCAP file
./BionXtool -f capture.pcap
```

Included in this repo are two captures. `bike-on-off.pcap` is a raw PCAP sniff
from my bike being turned on, then turned off shortly after. `bike-on-off-txt`
is the same but decoded.

### 5. Direct Register Access (Expert)
Read or write individual registers on any node. Useful for advanced features or deep troubleshooting.

```bash
# Read Battery Hardware Revision (0x3B)
./BionXtool -R battery 0x3B

# Set config to enable accessory power at bike turn-on.
./BionXtool -W console 0x80 0x1
```

An almost complete list of all registers is in [registers.h](registers.h). Note
that interacting with the console requires first setting it into slave mode (-z),
however this is not required for other components.

### 6. Power & State Control
Force system states or perform remote operations.

```bash
# Power off the entire system
./BionXtool -p

# Force console into slave mode (for external control)
./BionXtool -z

# Run commands without attempting to trigger slave mode
./BionXtool -n -S
```

---

## 📋 Command Line Options

| Option | Argument | Description |
| :--- | :--- | :--- |
| **Global** | | |
| `-d` | `<device>` | CAN device interface (default: `can0`) |
| `-h` | | Print help screen |
| **Information** | | |
| `-s` | | Print system settings overview |
| `-i` | | Hide private serial and part numbers |
| **Configuration** | | |
| `-l` | `<speed>` | Set assist speed limit (0-70 km/h, 0=unlimited) |
| `-t` | `<speed>` | Set throttle speed limit (0-70 km/h) |
| `-m` | `<speed>` | Set minimum assist speed (0-30 km/h) |
| `-c` | `<mm>` | Set wheel circumference (in mm) |
| `-a` | `<level>` | Set initial assist level (0-4) |
| `-o` | `<pct>` | Set mountain cap assist level (0-100%) |
| `-v` | `<volt>` | Set battery accessory voltage (6.0 - 14.0V) |
| `-A` | `<0\|1>` | Set battery accessory power (0=off, 1=on) |
| **Sniffing** | | |
| `-M` | | **Monitor Mode**: Interactive TUI dashboard |
| `-S` | | **Sniff Mode**: Stream decoded packets |
| `-C` | | **Change Sniff**: Stream only changed values |
| `-f` | `<file>` | Sniff from a PCAP file |
| **Low-level** | | |
| `-R` | `<node> <reg>`| Read specific register (hex) from node (name or hex) |
| `-W` | `<node> <reg> <val>`| Write value to specific register |
| `-z` | | Force console into slave mode |
| `-n` | | Do NOT attempt to trigger slave mode |
| `-p` | | Power off system |

---

## ⚖️ License & Attribution

**BionXtool** is licensed under the **GPLv3**.

This project is a fork of **BigXionFlasher**, originally developed by **Thomas König**. 
It includes software developed by the BigXionFlasher Project (http://www.bigxionflasher.org/).

Gemini CLI is used to assist coding tasks. Actual hardware engineering lovingly
done by hand :)

**Author of this fork:** Ben Buxton <bbuxton@gmail.com> (2026)
