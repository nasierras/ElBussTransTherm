## State of the Art: Thermal Management in Electric Public Transport
## Electrification of Urban Transit and Energy Constraints
## Vehicular HVAC Systems and Heat Pump Integration
## Human Thermal Comfort in Transient and Localized Environments
## Experimental Modalities: From Simulation to Physical Emulation

# State of the Art (Literature Review & Gap Analysis)
This chapter justifies the necessity of your research by systematically examining current advancements and exposing the lack of empirical, controlled baseline frameworks in electric vehicle thermal management.

## Electrification of Urban Public Transit and Energy Constraints
**The Energy Drain Dilemma:**
Analysis of auxiliary energy consumption in battery electric buses (BEBs), focusing on how HVAC loads severely degrade driving range during extreme winter and summer conditions.
**Operational Profile Analysis:** 
Review of urban transit characteristics—frequent door openings, passenger ingress/egress, stop-and-go driving cycles, and varying passenger densities—and how they disrupt steady-state cabin climates.

## Vehicle Thermal Management Systems (VTMS)
**Heat Pump Integration in Commercial Heavy Vehicles:**
State of the art in vapor-compression cycles, reversible heat pumps, and secondary fluid loops deployed in modern electric city buses (evaluating current commercial benchmarks like heavy transit fleets).
**Coupled Battery and Cabin Thermal Systems:**
Review of integrated thermal architectures that balance Battery Thermal Management Systems (BTMS for optimal charging and cell longevity) with passenger space heating and cooling demands.

## Human Thermal Comfort in Vehicular Environments
**From Static to Dynamic Comfort:**
Critical evaluation of classical steady-state models—such as Fanger’s Predicted Mean Vote ($PMV$) and Predicted Percentage of Dissatisfied ($PPD$)—and their limitations when applied to highly transient, non-uniform vehicular spaces.
**Localized Comfort Metrics:**
Review of advanced evaluation methodologies tailored for automotive cabins, specifically Equivalent Temperature ($T_{eq}$), local skin temperature gradients, draft ratings, and the influence of asymmetric solar radiation through large windows.

## Experimental Modalities: Simulation vs. Physical Emulation
**Computational Modeling:**
The role and current limitations of coupled Computational Fluid Dynamics (CFD) and human thermoregulation simulation tools (such as SolidWorks Flow Simulation or IDA ICE) in cabin design.
**Field Testing Challenges:**
The logistical and data-noise complexities of collecting thermal comfort data in active, commercial transit routes under uncontrolled environmental conditions.
**The Emulator Gap:**
A critical synthesis highlighting the scarcity of physical, modular testing apparatuses (such as controlled cold-room cabin mock-ups utilizing thermal manikins) that serve as a reproducible middle ground between pure software simulation and full-scale field deployment.

# Theoretical Framework (Governing Principles & Models)
This chapter outlines the core physics, thermodynamic equations, and mechatronic data architectures that form the foundation of your emulator methodology, sensor processing, and energy audits.

## Thermodynamics of Vehicular Heat Pumps and Cabin Envelopes
**Heat Transfer through Composite Structures:**
Governing equations for multidimensional heat conduction through modular insulated panels (polyurethane cores), convective boundary layers, and solar radiative transmission through glass surfaces.
**Vapor-Compression Thermodynamics:**
Application of the first and second laws of thermodynamics to heat pump cycles. Formulation of the Coefficient of Performance ($COP$) under varying heat sink and heat source temperatures.
**Reverse-Calculating Thermal Loads:** 
The mathematical model linking the electrical power consumption and enthalpy changes of the heat pump to the net thermal load of the cabin, establishing the basis for your emulator's validation algorithm.

## Human Heat Balance and Manikin Telemetry
**The Human Heat Balance Equation:**
Mathematical articulation of metabolic heat production and heat losses:
$$S = M - W - E - R - C$$
Where $S$ is heat storage, $M$ is metabolic rate, $W$ is external work, and $E$, $R$, and $C$ represent evaporative, radiative, and convective heat transfer, respectively.
**Operative and Mean Radiant Temperature:** 
Derivation of operative temperature ($T_{op}$) and mean radiant temperature ($T_{mrt}$) from sensor arrays and multi-segment thermal dummy data to quantify localized sensible heat loss.

## Mechatronics, Telemetry, and In-Vehicle Networks
**CANbus Communication Architectures:**
Theoretical principles of Controller Area Network (CAN) protocols in heavy vehicles, focusing on physical layer transmission, signal decoding via Database CAN ($DBC$) files, and input/output ($I/O$) actuator mapping.
**Kinematic and Environmental Data Aggregation:**
Mathematical correlation frameworks used to merge real-time vehicle telemetry (GPS positioning, speed, solar orientation, and door-opening frequency) with internal environmental sensor grids (air velocity gradients, temperature, and relative humidity).

## Energy Optimization and Control Strategies
**Energy Balance Equations in Transit Operations:**
Formulating the total vehicle energy consumption as a system of coupled differential equations balancing traction loads against HVAC energy draws.
**Optimization Algorithms for Climate Control:**
Theoretical basis for predictive or rule-based control strategies designed to minimize auxiliary energy consumption while maintaining passenger thermal comfort within acceptable thresholds.
