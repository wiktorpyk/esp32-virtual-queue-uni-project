# ESP32 Virtual Queue System

This project implements a virtual queue system using an ESP32 microcontroller. It hosts a Wi-Fi Access Point (AP) with a captive portal. Users connecting to this AP are redirected to a web page where they can join a queue. A physical button on the ESP32 allows an administrator to signal that the next person in the queue can proceed. The user is then notified on their device.

Features:
- Wi-Fi AP mode
- Captive portal with DNS hijacking
- Web-based queue joining
- Physical button to advance the queue
- Client notification for their turn
- LED status indicator
```
