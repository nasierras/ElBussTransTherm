# ElBussTransTherm
*ElBussTransTherm* is a Swedish research project funded by the Swedish Energy Agency (Energimyndigheten) that investigates how the energy used for heating, ventilation, and air conditioning (HVAC) in electric buses interacts with battery performance and passenger comfort.

## Main Objective
The primary objective is to develop operational strategies that minimize auxiliary energy consumption without compromising comfort.

## Core Areas of Focus
### Climate and Comfort: 
Analyzing how thermal comfort is perceived during various weather conditions in urban transit.
### Battery Management: 
Evaluating the link between powertrain thermal management and the cabin's heating or cooling needs.
### Field Studies & Guidelines: 
Conducting real-world tests, passenger surveys, and simulations to create actionable guidelines for bus manufacturers and public transport operators.

## Context and Impact
The project is highly relevant as cities scale up the electrification of public transport to reduce reliance on biofuels. By collaborating with public transport authorities (such as SL in the Stockholm region) and operators, the research helps identify specific measures to optimize fleet efficiency, especially during harsh winter conditions where heating demands can significantly drain battery capacity.

## PhD Main Objective
The project investigates the relationship between passenger thermal comfort, drivetrain and battery thermal management, and energy consumption for heating/cooling in electric city buses. The goal is to minimize energy  consumption for electric buses in public transport through optimization of climate strategies and operational  strategies. Through field studies, simulations, passenger surveys, and pilot studies, guidelines for operators and manufacturers are developed. Specifically, the project can, together with the Traffic Administration and Operators, identify strategies and measures that can enhance the value of conducted Energy Audits. The project is expected to contribute to increased energy efficiency and accelerate the transition to sustainable public transport, in line with the call's aim to promote applicable solutions for the transport system's energy transition.
## Main Research question:
How can thermal management and operational strategies be optimized to reduce total energy consumption in electric city buses while maintaining passenger thermal comfort and preserving battery/drivetrain performance?

## Data and Methods:
### Operator's Fleet:
This includes vehicle speed, cumulated energy consumption, HVAC power drawn, HVAC setpoints and output state, ambient temperature, passenger occupancy (APC-Automatic Passenger Counting), door states and state of charge (SoC).

### ElBussTransTherm Sensor Grid:
#### WP2.1.i:Stratified Cabin Air Temperature Node
This node measures vertical air temperature stratification inside the bus cabin at three longitudinal positions (front, middle, rear), capturing the classic head/seated/abdomen/ankle gradient relevant to passenger thermal comfort. Each position uses four T-type thermocouples (multiplexed through an ADG704 into a MAX31856 amplifier) to sample head-height (standing and seated), abdomen, and ankle-level air temperature, plus a 3-wire PT100 RTD (via MAX31865) as a stable reference/control channel. An ESP32-S3 (T-ETH-Lite) reads all channels at 1Hz over SPI communication protocol, performing local hardware-fault and out-of-bounds validation independent of network state, and publishes the readings as JSON over MQTT via wired Ethernet to a central Raspberry Pi broker, feeding directly into the project's field-measurement dataset on how cabin thermal gradients relate to HVAC energy demand and passenger comfort in electric city buses.


#### WP2.2.i: Cabin Air Quality & Micro-Environment Node
This node characterizes the cabin's local air quality and micro-climate at three lateral positions (front, right, left), combining indoor air velocity, CO₂, particulate matter, humidity, and solar gain (irradiance) into a single environmental picture per zone. A PAV1005V hot-wire anemometer captures air velocity along both X and Y axes, an SCD41 provides CO₂ concentration alongside its own temperature/RH reading, an SPS30 reports particulate mass concentration across four size bins (PM1.0–PM10), and an SPS-215-SS pyranometer tracks solar irradiance entering through the windows. An ESP32-S3 (T-ETH-Lite) polls all sensors over I2C at 1Hz, validates each reading via I2C transaction status and CRC checks (SCD41/SPS30) independent of network state, and publishes JSON over MQTT via wired Ethernet to a central Raspberry Pi broker supporting the project's analysis of how ventilation, solar gain, and occupant-generated CO₂ interact with climate control energy use.


#### WP2.3.i: 

#### WP2.5: 
Central Node and Publisher (API and/or DB access)

#### WP2.6: 
Thermal Dummy Physical Sensor Network 

#### WP2.TM: Thermal Dummy
