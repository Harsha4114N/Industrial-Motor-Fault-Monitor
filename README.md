
# Industrial Motor Fault Detection System

**Author:** N Harsha, Electronics and Communication Engineering (ECE)
**Status:** Completed & Deployed

## Overview
This project is a real-time, high-speed industrial safety monitor built to detect mechanical faults and dangerous imbalances in high-RPM motors. Utilizing an ESP32-S3 and an MPU6050 accelerometer, the system continuously analyzes physical vibration (Peak-to-Peak amplitude) and triggers an instantaneous mechanical lockout if the hardware crosses a critical resonance threshold.

## The Engineering Pivot: From ML to Deterministic Edge Computing
Initially, this system was designed to use a cloud-trained Machine Learning anomaly detection model (K-Means/Neural Network). However, real-world physical testing revealed that high-RPM TT motors generate extreme harmonic resonance on plastic chassis components. This resonance scrambled the frequency domain data, blinding the AI and causing inverted phase-shift predictions.

To solve this hardware limitation, the architecture was pivoted away from fragile ML inference and rebuilt using a **Deterministic High-Speed Edge-Computing Filter**. 

The current C++ firmware pulls 100 rapid-fire physics samples per second directly from the Z-Axis via I2C, calculates the physical peak-to-peak variance, and uses hard math to bypass the harmonic noise.

## Technical Architecture
* **Microcontroller:** ESP32-S3 
* **Sensors:** MPU6050 (6-Axis Gyro/Accelerometer)
* **Actuators:** L298N Motor Driver, High-RPM TT Motor
* **Software Stack:** C++, FreeRTOS, ESP-IDF I2C Drivers
* **Interface:** Asynchronous Web Server with 250ms AJAX polling for real-time UI updates.

## System States
1. **System Healthy:** Motor runs smoothly; web dashboard logs baseline amplitude (e.g., `Shake: 2400`).
2. **System Fault Latched:** The instant an imbalanced load spikes the amplitude beyond the safe threshold, the ESP32 cuts motor power, engages the red hardware LED, and flashes a critical warning on the live dashboard. The system remains safely locked until a manual reset is issued.
