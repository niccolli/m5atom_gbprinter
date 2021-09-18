# M5Atom GBPrinter Emulation

GBPrinter emulating implementation on M5Atom.

This implementation is based on these repositories.

- https://github.com/Dhole/gb-link-stm32f411
- https://github.com/Dhole/gb-link-host

## Demo

[![Demonstoration](http://img.youtube.com/vi/Gxu6QUd5LTE/0.jpg)](https://www.youtube.com/watch?v=Gxu6QUd5LTE)

## Require

- M5Atom w/ [ATOMIC DIY Proto Kit](https://shop.m5stack.com/products/atomic-proto-kit)
- PlatformIO
  - Arduino framework

### Proto Kit PCB Schematic

Please make this circuit on Proto Kit PCB. Resistor value is not important beacuse it's voltage divider.

![](https://raw.githubusercontent.com/niccolli/m5atom-gbprinter/master/assets/schematic.png)

## Build

1. Open ```m5atom_app``` directory on PlatformIO.
1. Run PlatformiIO commands "Build" and "Upload" on VSCode command palette.

## Usage

1. Run M5Atom by USB Power.
1. Start photo data transmission.
1. After transmission, connect "GBPrinter" access point via Wi-Fi.
1. Access 192.168.4.1 on web browser.
1. The picture last transmitted is shown.

## Limitation

- After transmission, M5Atom cannot receive data again. Please push RESET button on M5Atom.
- This implementation requires cutting link cable. Now I'm planning to develop receptacle socket PCB.
