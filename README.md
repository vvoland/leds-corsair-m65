# leds-corsair-m65
leds-corsair-m65 is a Linux kernel driver for controlling the LED zones color on the Corsair M65 Pro mouse.

The driver exposes the following sysfs led nodes:
- /sys/class/leds/corsair\_m65::dpi
- /sys/class/leds/corsair\_m65::logo
- /sys/class/leds/corsair\_m65::wheel

You can control the LED via the **brightness** file. The valid values are binary from range 0 to 0xFFFFFF which translate to RGB colors, so for example red is 0xFF0000.

## Build and load
```
make  
sudo insmod ./leds-corsair-m65.ko
```

## Why not do it in userspace via /dev/hidrawX?
I did at first but it wasn't that much fun, so I also did it in the kernel. ;-)
Also I like the sysfs interface and the hidraw device number can change, so I prefer the kernel driver solution.

## Examples
### Set the logo led to red color (RGB: 0xFF0000)
`printf '\xff0000' | sudo tee /sys/class/leds/corsair_m65::logo`
### Set the logo led to pink color (RGB: 0xFF00FF)
`printf '\xff00ff' | sudo tee /sys/class/leds/corsair_m65::logo`

## Demo
rainbow.py is a simple Python script which utilizes the driver interface to animate the mouse leds through all hues.

https://user-images.githubusercontent.com/5046555/127766130-2f43ab44-c4f2-4bc4-86e7-a86a5c87a3ed.mov

## TODO
 - [ ] Find out the command (if there is any) to read the zone color to avoid hardcoding the default color
 - [ ] Think how to make the nodes writable as a non-root user
